#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "v4l2_preview_sdl.h"
#include <rk_mpi.h>

#define VIDEO_DEVICE "/dev/video11"
#define OUTPUT_FIFO "/tmp/live.h265"
#define OUTPUT_FIFO_MP4 "/tmp/live_mp4.h265"
#define WIDTH 3264
#define HEIGHT 2448
#define FPS 15
#define BITRATE 4000000
#define BUFFER_COUNT 4
#define MAX_PLANES 2
#define MAX_OPEN_RETRY_US 500000

#ifndef MPP_ALIGN
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

// 摄像头缓冲区表
struct buffer {
    void *start[MAX_PLANES];        // mmap 后的用户态地址
    size_t length[MAX_PLANES];      // 每个 plane 对应的长度
    int dma_fd;                     // 导出的 dma-buf fd
    MppBuffer mpp_buf;              // dma-buf 对应的 MPP 缓冲区句柄
};

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup buf_grp;
    MppBuffer pkt_buf;
    int headers_written;
    RK_U32 width;
    RK_U32 height;
    RK_U32 hor_stride;
    RK_U32 ver_stride;
    RK_U32 frame_size;
    MppFrameFormat fmt;
} EncoderCtx;

typedef struct {
    unsigned int index;     // 表示是哪个 V4L2 buffer 
    RK_U32 frame_idx;       // 表示这是第几帧，主要给编码时间戳/日志用
    int64_t capture_us;     // SDL 显示完成时刻
} EncodeJob;

static int64_t now_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// 环形队列
typedef struct {
    EncodeJob jobs[BUFFER_COUNT];   // 固定大小的任务数组，BUFFER_COUNT 定义了队列的容量
    unsigned int head;              // 指向队列头部的索引，消费者从这里取任务
    unsigned int tail;              // 指向队列尾部的索引，生产者从这里放任务
    unsigned int count;             // 当前队列中的任务数量，范围是 0 到 BUFFER_COUNT
    int stop;
    pthread_mutex_t mutex;          // 保护队列访问的互斥锁
    pthread_cond_t not_empty;       // 队列非空时通知消费者线程
    pthread_cond_t not_full;        // 队列非满时通知生产者线程
} EncodeQueue;

static int camera_fd = -1;
static int stream_fifo_fd = -1;
static int store_fifo_fd = -1;
static const char *stream_fifo_path = OUTPUT_FIFO;
static const char *store_fifo_path = OUTPUT_FIFO_MP4;
static struct buffer *camera_buffers = NULL;
static int camera_running = 1;
static unsigned int camera_buffer_count = 0;
static unsigned int camera_num_planes = 0;
static unsigned int camera_width = 0;
static unsigned int camera_height = 0;
static unsigned int camera_y_stride = 0;    // Y 平面一行有多少字节
static unsigned int camera_uv_stride = 0;
static unsigned int camera_uv_offset = 0;
static pthread_mutex_t g_v4l2_mutex = PTHREAD_MUTEX_INITIALIZER;

static void encode_queue_init(EncodeQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void encode_queue_destroy(EncodeQueue *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static int encode_queue_push(EncodeQueue *q, EncodeJob job)
{
    pthread_mutex_lock(&q->mutex);
    while (!q->stop && q->count == BUFFER_COUNT) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (q->stop) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->jobs[q->tail] = job;                     // 从队列尾部放入任务         
    q->tail = (q->tail + 1) % BUFFER_COUNT;     // 环形队列：到头后自动回到 0
    q->count++;                                 // 任务数量 +1
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int encode_queue_pop(EncodeQueue *q, EncodeJob *job)
{
    pthread_mutex_lock(&q->mutex);
    while (!q->stop && q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    if (q->count == 0 && q->stop) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    *job = q->jobs[q->head];                    // 从队列头部取出任务
    q->head = (q->head + 1) % BUFFER_COUNT;     // 环形队列：到头后自动回到 0
    q->count--;                                 // 任务数量 -1
    pthread_cond_signal(&q->not_full);          // 通知生产者：队列有空位了
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static void encode_queue_stop(EncodeQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->stop = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}

static int ensure_fifo_exists(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "path exists but is not FIFO: %s\n", path);
            return -1;
        }
        return 0;
    }

    if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo failed");
        return -1;
    }

    return 0;
}

static int open_fifo_writer_blocking(const char *path)
{
    int fd;

    for (;;) {
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            printf("fifo writer connected: %s\n", path);
            return fd;
        }

        if (!camera_running) {
            return -1;
        }

        perror("open fifo writer failed");
        usleep(MAX_OPEN_RETRY_US);
    }
}

static int write_full_packet_to_fd(int fd, const void *data, size_t size)
{
    const unsigned char *ptr = (const unsigned char *)data;
    size_t written = 0;

    while (written < size) {
        ssize_t ret = write(fd, ptr + written, size - written);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)ret;
    }

    return 0;
}

static int write_packet_to_sink(int *fd, const char *path, const void *data, size_t size)
{
    if (*fd < 0) {
        *fd = open_fifo_writer_blocking(path);
        if (*fd < 0) {
            return -1;
        }
    }

    if (write_full_packet_to_fd(*fd, data, size) == 0) {
        return 0;
    }

    if (errno == EPIPE || errno == EAGAIN) {
        close(*fd);
        *fd = open_fifo_writer_blocking(path);
        if (*fd < 0) {
            return -1;
        }
        return write_full_packet_to_fd(*fd, data, size);
    }

    return -1;
}

static void handle_signal(int sig)
{
    (void)sig;
    camera_running = 0;
}

// 如果系统调用被信号中断，就自动重试
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

static int wait_for_frame(void)
{
    fd_set fds;
    struct timeval tv;
    int ret;

    FD_ZERO(&fds);
    FD_SET(camera_fd, &fds);

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    ret = select(camera_fd + 1, &fds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR && !camera_running)
            return -1;
        perror("select failed");
        return -1;
    }

    if (ret == 0) {
        fprintf(stderr, "wait frame timeout\n");
        return -1;
    }

    return 0;
}

static int open_device(const char *device)
{
    camera_fd = open(device, O_RDWR | O_NONBLOCK);
    if (camera_fd < 0) {
        perror("open failed");
        return -1;
    }
    return 0;
}

static int set_format(int width, int height)
{
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;             // 非隔行扫描，正常摄像头都用这个
    fmt.fmt.pix_mp.num_planes = MAX_PLANES;             // NV12 一般是 1 或 2 个平面

    // 执行后，fmt 会被驱动改写为实际生效的参数
    if (xioctl(camera_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT failed");
        return -1;
    }

    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 &&
        fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M) {
        fprintf(stderr, "device did not accept NV12/NV12M format\n");
        return -1;
    }

    camera_width = fmt.fmt.pix_mp.width;
    camera_height = fmt.fmt.pix_mp.height;
    camera_num_planes = fmt.fmt.pix_mp.num_planes;

    // 单平面 NV12：
    // +---------------------------------------------------+
    // | Y 区域 (共 H 行)                                   |
    // | row0: [0  ...  y_stride-1]                        |
    // | row1: [y_stride ... 2*y_stride-1]                 |
    // | ...                                               |
    // | rowH-1: [(H-1)*y_stride ... H*y_stride-1]         |
    // +---------------------------------------------------+
    // | UV 区域 (共 H/2 行，紧跟 Y 末尾)                    |
    // | row0: [H*y_stride ... H*y_stride + y_stride - 1]  |
    // | row1: [H*y_stride+y_stride ... ]                  |
    // | ...                                               |
    // +---------------------------------------------------+
    camera_y_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline ?
                 fmt.fmt.pix_mp.plane_fmt[0].bytesperline : camera_width;
    camera_uv_stride = camera_y_stride;                     // 单平面 NV12 的 UV 区域和 Y 区域行距相同
    camera_uv_offset = camera_y_stride * camera_height;     // 只有单平面时才有效
    if (camera_num_planes >= 2 && fmt.fmt.pix_mp.plane_fmt[1].bytesperline) {
        camera_uv_stride = fmt.fmt.pix_mp.plane_fmt[1].bytesperline;
    }

    printf("capture format: %ux%u %s, num_planes=%u, y_stride=%u, uv_stride=%u\n",
           camera_width, camera_height,
           fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M ? "NV12M" : "NV12",
           camera_num_planes, camera_y_stride, camera_uv_stride);
    for (unsigned int i = 0; i < fmt.fmt.pix_mp.num_planes; ++i) {
        // bytesperline：一行多少字节
        // sizeimage：这个 plane 的总大小（可能包含 padding）
        printf("plane[%u]: bytesperline=%u sizeimage=%u\n",
            i,
            fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
            fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
    }

    return 0;
}

static int init_mmap(void)
{
    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(camera_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS failed");
        return -1;
    }

    if (req.count < 2) {
        fprintf(stderr, "insufficient buffer memory\n");
        return -1;
    }

    camera_buffer_count = req.count;
    printf("buffer count = %u\n", camera_buffer_count);

    // 比 malloc 多一步：把内存全部填 0
    camera_buffers = calloc(camera_buffer_count, sizeof(*camera_buffers));
    if (!camera_buffers) {
        perror("calloc failed");
        return -1;
    }

    for (unsigned int i = 0; i < camera_buffer_count; ++i) {
        camera_buffers[i].dma_fd = -1;
    }

    for (unsigned int i = 0; i < camera_buffer_count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(camera_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF failed");
            return -1;
        }

        printf("buffer[%u]: returned planes=%u\n", i, buf.length);

        {
            struct v4l2_exportbuffer expbuf;
            MppBufferInfo info;
            memset(&expbuf, 0, sizeof(expbuf));
            expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            expbuf.index = i;       // 导出第 i 个采集 buffer
            expbuf.plane = 0;       // 的第 0 个 plane
            expbuf.flags = O_CLOEXEC;

            // 把这个采集 buffer 导出成一个可共享的 dma-buf fd
            if (xioctl(camera_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                fprintf(stderr, "EXPBUF failed for buffer[%u]: %s\n",
                        i, strerror(errno));
                return -1;
            } else {
                printf("EXPBUF success: buffer[%u] dma_fd=%d\n", i, expbuf.fd);
                camera_buffers[i].dma_fd = expbuf.fd;
            }

            memset(&info, 0, sizeof(info));
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.fd = camera_buffers[i].dma_fd;
            info.size = planes[0].length;
            info.index = (int)i;

            // 把这个 dma-buf fd 导入成 MPP 可以识别的缓冲区句柄，后续编码时就可以直接使用了
            if (mpp_buffer_import(&camera_buffers[i].mpp_buf, &info) != MPP_OK) {
                fprintf(stderr, "  mpp_buffer_import failed for buffer[%u]\n", i);
                return -1;
            } else {
                printf("  mpp_buffer_import success: buffer[%u]\n", i);
            }
        }

        for (unsigned int p = 0; p < buf.length && p < MAX_PLANES; ++p) {
            printf("  plane[%u]: length=%u offset=%u bytesused=%u\n",
                   p, planes[p].length, planes[p].m.mem_offset, planes[p].bytesused);
            camera_buffers[i].length[p] = planes[p].length;
            camera_buffers[i].start[p] = mmap(NULL, planes[p].length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         camera_fd, planes[p].m.mem_offset);
            if (camera_buffers[i].start[p] == MAP_FAILED) {
                fprintf(stderr, "mmap failed on buffer %u plane %u: %s\n",
                        i, p, strerror(errno));
                return -1;
            }
        }

        if (xioctl(camera_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF failed");
            return -1;
        }
    }

    return 0;
}

static int start_capture(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(camera_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON failed");
        return -1;
    }

    return 0;
}

static void stop_capture(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (camera_fd >= 0) {
        xioctl(camera_fd, VIDIOC_STREAMOFF, &type);
    }
}

static void cleanup(void)
{
    if (camera_buffers) {
        for (unsigned int i = 0; i < camera_buffer_count; ++i) {
            if (camera_buffers[i].mpp_buf) {
                mpp_buffer_put(camera_buffers[i].mpp_buf);
                camera_buffers[i].mpp_buf = NULL;
            }
            if (camera_buffers[i].dma_fd >= 0) {
                close(camera_buffers[i].dma_fd);
                camera_buffers[i].dma_fd = -1;
            }
            for (int p = 0; p < MAX_PLANES; ++p) {
                if (camera_buffers[i].start[p] && camera_buffers[i].start[p] != MAP_FAILED) {
                    munmap(camera_buffers[i].start[p], camera_buffers[i].length[p]);
                }
            }
        }
        free(camera_buffers);
        camera_buffers = NULL;
    }

    if (camera_fd >= 0) {
        close(camera_fd);
        camera_fd = -1;
    }
}

static int encoder_init(EncoderCtx *enc, unsigned int width, unsigned int height)
{
    MPP_RET ret;
    MppEncPrepCfg prep_cfg;     // 管“输入帧怎么喂给编码器”
    MppEncRcCfg rc_cfg;         // 管“编码器怎么控码率”
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;
    enc->hor_stride = MPP_ALIGN(width, 16);
    enc->ver_stride = MPP_ALIGN(height, 16);
    enc->frame_size = enc->hor_stride * enc->ver_stride * 3 / 2;
    enc->fmt = MPP_FMT_YUV420SP;

    printf("encoder init w %d h %d stride %d x %d frame_size %d\n",
            enc->width, enc->height, enc->hor_stride, enc->ver_stride, enc->frame_size);

    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret) {
        fprintf(stderr, "mpp_create failed ret=%d\n", ret);
        return -1;
    }

    /* Initialize as encoder for H.265 */
    ret = mpp_init(enc->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
    if (ret) {
        fprintf(stderr, "mpp_init failed ret=%d\n", ret);
        return -1;
    }

    /* Set output timeout to block mode */
    RK_S64 timeout = MPP_POLL_NON_BLOCK;
    ret = enc->mpi->control(enc->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ret) {
        fprintf(stderr, "MPP_SET_OUTPUT_TIMEOUT failed ret=%d\n", ret);
        return -1;
    }

    memset(&prep_cfg, 0, sizeof(prep_cfg));
    prep_cfg.change = MPP_ENC_PREP_CFG_CHANGE_INPUT | MPP_ENC_PREP_CFG_CHANGE_FORMAT;   // 变更掩码
    prep_cfg.width = enc->width;
    prep_cfg.height = enc->height;
    prep_cfg.hor_stride = enc->hor_stride;
    prep_cfg.ver_stride = enc->ver_stride;
    prep_cfg.format = enc->fmt;

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_PREP_CFG, &prep_cfg);
    if (ret) {
        fprintf(stderr, "MPP_ENC_SET_PREP_CFG failed ret=%d\n", ret);
        return -1;
    }

    memset(&rc_cfg, 0, sizeof(rc_cfg));
    rc_cfg.change = MPP_ENC_RC_CFG_CHANGE_RC_MODE |
                    MPP_ENC_RC_CFG_CHANGE_BPS |
                    MPP_ENC_RC_CFG_CHANGE_FPS_IN |
                    MPP_ENC_RC_CFG_CHANGE_FPS_OUT |
                    MPP_ENC_RC_CFG_CHANGE_GOP;
    rc_cfg.rc_mode = MPP_ENC_RC_MODE_CBR;   // 恒定码率模式
    rc_cfg.bps_target = BITRATE;
    rc_cfg.bps_max = BITRATE * 5 / 4;
    rc_cfg.bps_min = BITRATE * 3 / 4;
    rc_cfg.fps_in_flex = 0;                 // 输入帧率固定，不是可变帧率
    rc_cfg.fps_in_num = FPS;
    rc_cfg.fps_in_denom = 1;
    rc_cfg.fps_out_flex = 0;                // 表示输出帧率也是固定的
    rc_cfg.fps_out_num = FPS;
    rc_cfg.fps_out_denom = 1;
    rc_cfg.gop = FPS;

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_RC_CFG, &rc_cfg);
    if (ret) {
        fprintf(stderr, "MPP_ENC_SET_RC_CFG failed ret=%d\n", ret);
        return -1;
    }

    // 让编码器在每一个 I 帧（关键帧）前面，都带上 SPS + PPS 头信息
    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret) {
        fprintf(stderr, "MPP_ENC_SET_HEADER_MODE failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);    // 缓冲区管理池
    if (ret) {
        fprintf(stderr, "mpp_buffer_group_get_internal failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf, enc->frame_size);
    if (ret) {
        fprintf(stderr, "mpp_buffer_get pkt_buf failed ret=%d\n", ret);
        return -1;
    }

    printf("mpp encoder initialized: %ux%u stride=%ux%u\n",
           enc->width, enc->height, enc->hor_stride, enc->ver_stride);
    return 0;
}

static void encoder_deinit(EncoderCtx *enc)
{
    if (enc->pkt_buf) {
        mpp_buffer_put(enc->pkt_buf);
        enc->pkt_buf = NULL;
    }

    if (enc->buf_grp) {
        mpp_buffer_group_put(enc->buf_grp);
        enc->buf_grp = NULL;
    }

    if (enc->ctx) {
        if (enc->mpi) {
            enc->mpi->reset(enc->ctx);
        }
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
    }
}

static int write_encoder_headers(EncoderCtx *enc)
{
    MPP_RET ret;
    MppPacket header = NULL;
    void *ptr = NULL;
    size_t len = 0;

    if (enc->headers_written) {
        return 0;
    }

    ret = mpp_packet_init_with_buffer(&header, enc->pkt_buf);
    if (ret) {
        fprintf(stderr, "mpp_packet_init_with_buffer failed ret=%d\n", ret);
        return -1;
    }

    mpp_packet_set_length(header, 0);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_HDR_SYNC, header);
    if (ret) {
        fprintf(stderr, "MPP_ENC_GET_HDR_SYNC failed ret=%d\n", ret);
        mpp_packet_deinit(&header);
        return -1;
    }

    if (!header) {
        fprintf(stderr, "encoder header packet is null\n");
        return -1;
    }

    ptr = mpp_packet_get_pos(header);
    len = mpp_packet_get_length(header);
    if (!ptr || len == 0) {
        fprintf(stderr, "encoder header packet is empty\n");
        mpp_packet_deinit(&header);
        return -1;
    }

    if (write_packet_to_sink(&stream_fifo_fd, stream_fifo_path, ptr, len) < 0) {
        perror("write stream header failed");
        mpp_packet_deinit(&header);
        return -1;
    }

    if (write_packet_to_sink(&store_fifo_fd, store_fifo_path, ptr, len) < 0) {
        perror("write store header failed");
        mpp_packet_deinit(&header);
        return -1;
    }

    mpp_packet_deinit(&header);
    enc->headers_written = 1;
    printf("wrote encoder headers: %zu bytes\n", len);
    return 0;
}

static int encode_one_frame(EncoderCtx *enc, MppBuffer input_buf, RK_U32 frame_idx, int *encoded_frames)
{
    MPP_RET ret;
    MppFrame frame = NULL;
    MppPacket packet = NULL;

    ret = mpp_frame_init(&frame);
    if (ret) {
        fprintf(stderr, "mpp_frame_init failed ret=%d\n", ret);
        return -1;
    }

    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, enc->fmt);
    mpp_frame_set_pts(frame, frame_idx);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_buffer(frame, input_buf);

    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) {
        fprintf(stderr, "encode_put_frame failed ret=%d\n", ret);
        return -1;
    }

    if (write_encoder_headers(enc) < 0) {
        return -1;
    }

    while (1) {
        ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
        if (ret == MPP_NOK || ret == MPP_ERR_TIMEOUT) {
            return 0;
        }

        if (ret) {
            fprintf(stderr, "encode_get_packet failed ret=%d\n", ret);
            return -1;
        }

        if (!packet) {
            return 0;
        }

        {
            void *ptr = mpp_packet_get_pos(packet);
            size_t len = mpp_packet_get_length(packet);

            if (len > 0) {
                if (write_packet_to_sink(&stream_fifo_fd, stream_fifo_path, ptr, len) < 0) {
                    perror("write stream fifo failed");
                    mpp_packet_deinit(&packet);
                    return -1;
                }

                if (write_packet_to_sink(&store_fifo_fd, store_fifo_path, ptr, len) < 0) {
                    perror("write store fifo failed");
                    mpp_packet_deinit(&packet);
                    return -1;
                }
                (*encoded_frames)++;
            }
        }

        mpp_packet_deinit(&packet);
    }
}

typedef struct {
    EncoderCtx *enc;
    EncodeQueue *queue;
    int *encoded_frames;
    int *submitted_frames;
    int64_t encode_wait_total_us;
    int64_t encode_fifo_total_us;
    int64_t samples;
} EncodeThreadCtx;

typedef struct {
    EncodeQueue *queue;
    int *frame_count;
    int *fps_count;
    Uint32 *fps_tick;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    EncodeThreadCtx *encode_stats;
} CaptureThreadCtx;

static void *encode_thread_main(void *arg)
{
    EncodeThreadCtx *ctx = (EncodeThreadCtx *)arg;

    while (camera_running) {
        EncodeJob job;
        struct v4l2_buffer buf;
        struct v4l2_plane planes[MAX_PLANES];

        if (encode_queue_pop(ctx->queue, &job) < 0) {
            break;
        }

        if (job.capture_us > 0) {
            ctx->encode_wait_total_us += now_monotonic_us() - job.capture_us;
            ctx->samples++;
        }

        if (!camera_buffers[job.index].mpp_buf) {
            fprintf(stderr, "missing imported mpp buffer for index %u\n", job.index);
            camera_running = 0;
            break;
        }

        {
            int64_t start_us = now_monotonic_us();
            if (encode_one_frame(ctx->enc, camera_buffers[job.index].mpp_buf,
                                 job.frame_idx, ctx->encoded_frames) < 0) {
                camera_running = 0;
                break;
            }
            ctx->encode_fifo_total_us += now_monotonic_us() - start_us;
        }

        (*ctx->submitted_frames)++;

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.index = job.index;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = MAX_PLANES;
        buf.m.planes = planes;

        pthread_mutex_lock(&g_v4l2_mutex);
        if (xioctl(camera_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF failed");
            camera_running = 0;
            pthread_mutex_unlock(&g_v4l2_mutex);
            break;
        }
        pthread_mutex_unlock(&g_v4l2_mutex);
    }

    return NULL;
}

static void *capture_display_thread_main(void *arg)
{
    CaptureThreadCtx *ctx = (CaptureThreadCtx *)arg;

    while (camera_running) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[MAX_PLANES];
        unsigned char *src_y;
        unsigned char *src_uv;

        if (wait_for_frame() < 0) {
            continue;
        }

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = MAX_PLANES;
        buf.m.planes = planes;

        pthread_mutex_lock(&g_v4l2_mutex);
        if (xioctl(camera_fd, VIDIOC_DQBUF, &buf) < 0) {
            pthread_mutex_unlock(&g_v4l2_mutex);
            if (errno == EAGAIN) {
                continue;
            }
            perror("VIDIOC_DQBUF failed");
            camera_running = 0;
            break;
        }
        pthread_mutex_unlock(&g_v4l2_mutex);

        src_y = (unsigned char *)camera_buffers[buf.index].start[0];
        if (camera_num_planes >= 2 && camera_buffers[buf.index].start[1]) {
            src_uv = (unsigned char *)camera_buffers[buf.index].start[1];
        } else {
            src_uv = src_y + camera_uv_offset;
        }

        if (SDL_UpdateNVTexture(ctx->texture, NULL,
                                src_y, camera_y_stride,
                                src_uv, camera_uv_stride) < 0) {
            fprintf(stderr, "SDL_UpdateNVTexture failed: %s\n", SDL_GetError());
            pthread_mutex_lock(&g_v4l2_mutex);
            xioctl(camera_fd, VIDIOC_QBUF, &buf);
            pthread_mutex_unlock(&g_v4l2_mutex);
            camera_running = 0;
            break;
        }

        SDL_RenderClear(ctx->renderer);
        SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, NULL);
        SDL_RenderPresent(ctx->renderer);

        if (encode_queue_push(ctx->queue, (EncodeJob){buf.index, (RK_U32)(*ctx->frame_count), now_monotonic_us()}) < 0) {
            pthread_mutex_lock(&g_v4l2_mutex);
            xioctl(camera_fd, VIDIOC_QBUF, &buf);
            pthread_mutex_unlock(&g_v4l2_mutex);
            camera_running = 0;
            break;
        }

        (*ctx->frame_count)++;
        (*ctx->fps_count)++;

        {
            Uint32 now = SDL_GetTicks();
            if (now - *ctx->fps_tick >= 1000) {
                char title[128];
                snprintf(title, sizeof(title),
                         "V4L2 Preview - %dx%d - %d fps",
                         camera_width, camera_height, *ctx->fps_count);
                SDL_SetWindowTitle(ctx->window, title);
                printf("fps=%d\n", *ctx->fps_count);
                if (ctx->encode_stats && ctx->encode_stats->samples > 0) {
                    printf("latency: sdl->encode_pop=%.2f ms encode->fifo=%.2f ms samples=%lld\n",
                           (double)ctx->encode_stats->encode_wait_total_us / ctx->encode_stats->samples / 1000.0,
                           (double)ctx->encode_stats->encode_fifo_total_us / ctx->encode_stats->samples / 1000.0,
                           (long long)ctx->encode_stats->samples);
                    ctx->encode_stats->encode_wait_total_us = 0;
                    ctx->encode_stats->encode_fifo_total_us = 0;
                    ctx->encode_stats->samples = 0;
                }
                *ctx->fps_count = 0;
                *ctx->fps_tick = now;
            }
        }
    }

    encode_queue_stop(ctx->queue);
    return NULL;
}

int v4l2_preview_sdl_entry(int argc, char *argv[])
{
    const char *device = VIDEO_DEVICE;
    EncoderCtx enc;
    EncodeQueue queue;
    EncodeThreadCtx encode_ctx;
    CaptureThreadCtx capture_display_ctx;
    pthread_t encode_thread;
    pthread_t capture_display_thread;
    int encode_thread_started = 0;
    int capture_thread_started = 0;
    int width = WIDTH;
    int height = HEIGHT;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_Event event;
    int frame_count = 0;
    int encoded_frames = 0;
    int submitted_frames = 0;
    Uint32 fps_tick = 0;
    int fps_count = 0;

    if (argc > 1) {
        device = argv[1];
    }
    if (argc > 3) {
        width = atoi(argv[2]);
        height = atoi(argv[3]);
    }
    if (argc > 4) {
        stream_fifo_path = argv[4];
    }
    if (argc > 5) {
        store_fifo_path = argv[5];
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    memset(&enc, 0, sizeof(enc));
    memset(&encode_ctx, 0, sizeof(encode_ctx));
    memset(&capture_display_ctx, 0, sizeof(capture_display_ctx));
    encode_queue_init(&queue);

    if (open_device(device) < 0) {
        goto out;
    }

    if (set_format(width, height) < 0) {
        goto out;
    }

    if (init_mmap() < 0) {
        goto out;
    }

    if (start_capture() < 0) {
        goto out;
    }

    if (ensure_fifo_exists(stream_fifo_path) < 0) {
        goto out;
    }

    if (ensure_fifo_exists(store_fifo_path) < 0) {
        goto out;
    }

    if (encoder_init(&enc, camera_width, camera_height) < 0) {
        goto out;
    }

    printf("waiting for fifo readers...\n");
    stream_fifo_fd = open_fifo_writer_blocking(stream_fifo_path);
    if (stream_fifo_fd < 0) {
        goto out;
    }
    store_fifo_fd = open_fifo_writer_blocking(store_fifo_path);
    if (store_fifo_fd < 0) {
        goto out;
    }

    encode_ctx.enc = &enc;
    encode_ctx.queue = &queue;
    encode_ctx.encoded_frames = &encoded_frames;
    encode_ctx.submitted_frames = &submitted_frames;
    encode_ctx.encode_wait_total_us = 0;
    encode_ctx.encode_fifo_total_us = 0;
    encode_ctx.samples = 0;

    capture_display_ctx.queue = &queue;
    capture_display_ctx.frame_count = &frame_count;
    capture_display_ctx.fps_count = &fps_count;
    capture_display_ctx.fps_tick = &fps_tick;
    capture_display_ctx.window = window;
    capture_display_ctx.renderer = renderer;
    capture_display_ctx.texture = texture;
    capture_display_ctx.encode_stats = &encode_ctx;

    if (pthread_create(&encode_thread, NULL, encode_thread_main, &encode_ctx) != 0) {
        perror("pthread_create encode_thread failed");
        goto out;
    }
    encode_thread_started = 1;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        goto out;
    }
    window = SDL_CreateWindow("V4L2 Preview",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              width, height,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        goto out;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        goto out;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_NV12,
                                SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        goto out;
    }
    capture_display_ctx.window = window;
    capture_display_ctx.renderer = renderer;
    capture_display_ctx.texture = texture;
    fps_tick = SDL_GetTicks();

    if (pthread_create(&capture_display_thread, NULL, capture_display_thread_main, &capture_display_ctx) != 0) {
        perror("pthread_create capture_display_thread failed");
        goto out;
    }
    capture_thread_started = 1;

    while (camera_running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                camera_running = 0;
            } else if (event.type == SDL_KEYDOWN &&
                       event.key.keysym.sym == SDLK_ESCAPE) {
                camera_running = 0;
            }
        }
        usleep(10000);
    }

    printf("displayed frames: %d\n", frame_count);
    printf("submitted frames: %d\n", submitted_frames);
    printf("encoded packets: %d\n", encoded_frames);

out:
    encode_queue_stop(&queue);
    if (capture_thread_started) {
        pthread_join(capture_display_thread, NULL);
    }
    if (encode_thread_started) {
        pthread_join(encode_thread, NULL);
    }
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    if (stream_fifo_fd >= 0) {
        close(stream_fifo_fd);
        stream_fifo_fd = -1;
    }
    if (store_fifo_fd >= 0) {
        close(store_fifo_fd);
        store_fifo_fd = -1;
    }
    encoder_deinit(&enc);
    stop_capture();
    cleanup();
    encode_queue_destroy(&queue);
    return 0;
}
