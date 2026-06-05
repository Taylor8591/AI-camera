#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rk_mpi.h>

#define VIDEO_DEVICE "/dev/video11"
#define OUTPUT_FIFO "/tmp/live.h265"
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

struct buffer {
    void *start[MAX_PLANES];
    size_t length[MAX_PLANES];
    int dma_fd;
    MppBuffer mpp_buf;
};

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup buf_grp;
    MppBuffer pkt_buf;
    RK_U32 width;
    RK_U32 height;
    RK_U32 hor_stride;
    RK_U32 ver_stride;
    RK_U32 frame_size;
    MppFrameFormat fmt;
} EncoderCtx;

static int g_fd = -1;
static int g_fifo_fd = -1;
static struct buffer *g_buffers = NULL;
static int g_running = 1;
static unsigned int g_buffer_count = 0;
static unsigned int g_num_planes = 0;
static unsigned int g_width = 0;
static unsigned int g_height = 0;
static unsigned int g_y_stride = 0;
static unsigned int g_uv_stride = 0;
static unsigned int g_uv_offset = 0;

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

        if (!g_running) {
            return -1;
        }

        perror("open fifo writer failed");
        usleep(MAX_OPEN_RETRY_US);
    }
}

static int write_full_packet(const void *data, size_t size)
{
    const unsigned char *ptr = (const unsigned char *)data;
    size_t written = 0;

    while (written < size) {
        ssize_t ret = write(g_fifo_fd, ptr + written, size - written);
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

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
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
    FD_SET(g_fd, &fds);

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    ret = select(g_fd + 1, &fds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR && !g_running)
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
    g_fd = open(device, O_RDWR | O_NONBLOCK);
    if (g_fd < 0) {
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
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = MAX_PLANES;

    if (xioctl(g_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT failed");
        return -1;
    }

    printf("pixelformat = %c%c%c%c\n",
       fmt.fmt.pix_mp.pixelformat & 0xff,
       (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
       (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
       (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff);

    printf("num_planes = %u\n", fmt.fmt.pix_mp.num_planes);

    for (unsigned int i = 0; i < fmt.fmt.pix_mp.num_planes; ++i) {
        printf("plane[%u]: bytesperline=%u sizeimage=%u\n",
            i,
            fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
            fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
    }

    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 &&
        fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M) {
        fprintf(stderr, "device did not accept NV12/NV12M format\n");
        return -1;
    }

    g_width = fmt.fmt.pix_mp.width;
    g_height = fmt.fmt.pix_mp.height;
    g_num_planes = fmt.fmt.pix_mp.num_planes;
    g_y_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline ?
                 fmt.fmt.pix_mp.plane_fmt[0].bytesperline : g_width;
    g_uv_stride = g_y_stride;
    g_uv_offset = g_y_stride * g_height;

    if (g_num_planes >= 2 && fmt.fmt.pix_mp.plane_fmt[1].bytesperline) {
        g_uv_stride = fmt.fmt.pix_mp.plane_fmt[1].bytesperline;
    }

    printf("capture format: %ux%u %s, num_planes=%u, y_stride=%u, uv_stride=%u\n",
           g_width, g_height,
           fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M ? "NV12M" : "NV12",
           g_num_planes, g_y_stride, g_uv_stride);
    return 0;
}

static int init_mmap(void)
{
    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(g_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS failed");
        return -1;
    }

    if (req.count < 2) {
        fprintf(stderr, "insufficient buffer memory\n");
        return -1;
    }

    g_buffer_count = req.count;

    g_buffers = calloc(g_buffer_count, sizeof(*g_buffers));
    if (!g_buffers) {
        perror("calloc failed");
        return -1;
    }

    for (unsigned int i = 0; i < g_buffer_count; ++i) {
        g_buffers[i].dma_fd = -1;
    }

    for (unsigned int i = 0; i < g_buffer_count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF failed");
            return -1;
        }

        printf("buffer[%u]: returned planes=%u\n", i, buf.length);

        {
            struct v4l2_exportbuffer expbuf;
            MppBufferInfo info;
            memset(&expbuf, 0, sizeof(expbuf));
            expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            expbuf.index = i;
            expbuf.plane = 0;
            expbuf.flags = O_CLOEXEC;

            if (xioctl(g_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                fprintf(stderr, "  EXPBUF failed for buffer[%u]: %s\n",
                        i, strerror(errno));
                return -1;
            } else {
                printf("  EXPBUF success: buffer[%u] dma_fd=%d\n", i, expbuf.fd);
                g_buffers[i].dma_fd = expbuf.fd;
            }

            memset(&info, 0, sizeof(info));
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.fd = g_buffers[i].dma_fd;
            info.size = planes[0].length;
            info.index = (int)i;

            if (mpp_buffer_import(&g_buffers[i].mpp_buf, &info) != MPP_OK) {
                fprintf(stderr, "  mpp_buffer_import failed for buffer[%u]\n", i);
                return -1;
            } else {
                printf("  mpp_buffer_import success: buffer[%u]\n", i);
            }
        }

        for (unsigned int p = 0; p < buf.length && p < MAX_PLANES; ++p) {
            printf("  plane[%u]: length=%u offset=%u bytesused=%u\n",
                   p, planes[p].length, planes[p].m.mem_offset, planes[p].bytesused);
            g_buffers[i].length[p] = planes[p].length;
            g_buffers[i].start[p] = mmap(NULL, planes[p].length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         g_fd, planes[p].m.mem_offset);
            if (g_buffers[i].start[p] == MAP_FAILED) {
                fprintf(stderr, "mmap failed on buffer %u plane %u: %s\n",
                        i, p, strerror(errno));
                return -1;
            }
        }

        if (xioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF failed");
            return -1;
        }
    }

    return 0;
}

static int start_capture(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(g_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON failed");
        return -1;
    }

    return 0;
}

static void stop_capture(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (g_fd >= 0) {
        xioctl(g_fd, VIDIOC_STREAMOFF, &type);
    }
}

static void cleanup(void)
{
    if (g_buffers) {
        for (unsigned int i = 0; i < g_buffer_count; ++i) {
            if (g_buffers[i].mpp_buf) {
                mpp_buffer_put(g_buffers[i].mpp_buf);
                g_buffers[i].mpp_buf = NULL;
            }
            if (g_buffers[i].dma_fd >= 0) {
                close(g_buffers[i].dma_fd);
                g_buffers[i].dma_fd = -1;
            }
            for (int p = 0; p < MAX_PLANES; ++p) {
                if (g_buffers[i].start[p] && g_buffers[i].start[p] != MAP_FAILED) {
                    munmap(g_buffers[i].start[p], g_buffers[i].length[p]);
                }
            }
        }
        free(g_buffers);
        g_buffers = NULL;
    }

    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

static int encoder_init(EncoderCtx *enc, unsigned int width, unsigned int height)
{
    MPP_RET ret;
    MppEncPrepCfg prep_cfg;     // 管“输入帧怎么喂给编码器”
    MppEncRcCfg rc_cfg;         // 管“编码器怎么控码率”
    

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
                if (*encoded_frames < 5) {
                    printf("encoded packet size=%zu pts=%u\n", len, frame_idx);
                }
                if (write_full_packet(ptr, len) < 0) {
                    if (errno == EPIPE || errno == EAGAIN) {
                        close(g_fifo_fd);
                        printf("fifo reader disconnected, waiting for reconnect...\n");
                        g_fifo_fd = open_fifo_writer_blocking(OUTPUT_FIFO);
                        if (g_fifo_fd < 0 || write_full_packet(ptr, len) < 0) {
                            perror("write fifo failed");
                            mpp_packet_deinit(&packet);
                            return -1;
                        }
                    } else {
                        perror("write fifo failed");
                        mpp_packet_deinit(&packet);
                        return -1;
                    }
                }
                (*encoded_frames)++;
            }
        }

        mpp_packet_deinit(&packet);
    }
}

int main(int argc, char *argv[])
{
    const char *device = VIDEO_DEVICE;
    EncoderCtx enc;
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

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    memset(&enc, 0, sizeof(enc));

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

    if (ensure_fifo_exists(OUTPUT_FIFO) < 0) {
        goto out;
    }

    if (encoder_init(&enc, g_width, g_height) < 0) {
        goto out;
    }

    printf("waiting for fifo reader...\n");
    g_fifo_fd = open_fifo_writer_blocking(OUTPUT_FIFO);
    if (g_fifo_fd < 0) {
        goto out;
    }

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
    fps_tick = SDL_GetTicks();

    while (g_running) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[MAX_PLANES];
        unsigned char *src_y;
        unsigned char *src_uv;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_running = 0;
            } else if (event.type == SDL_KEYDOWN &&
                       event.key.keysym.sym == SDLK_ESCAPE) {
                g_running = 0;
            }
        }

        if (wait_for_frame() < 0) {
            continue;
        }

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(g_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            perror("VIDIOC_DQBUF failed");
            break;
        }

        src_y = (unsigned char *)g_buffers[buf.index].start[0];
        if (g_num_planes >= 2 && g_buffers[buf.index].start[1]) {
            src_uv = (unsigned char *)g_buffers[buf.index].start[1];
        } else {
            src_uv = src_y + g_uv_offset;
        }

        if (SDL_UpdateNVTexture(texture, NULL,
                                src_y, g_y_stride,
                                src_uv, g_uv_stride) < 0) {
            fprintf(stderr, "SDL_UpdateNVTexture failed: %s\n", SDL_GetError());
            xioctl(g_fd, VIDIOC_QBUF, &buf);
            break;
        }

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        if (!g_buffers[buf.index].mpp_buf) {
            fprintf(stderr, "missing imported mpp buffer for index %u\n", buf.index);
            xioctl(g_fd, VIDIOC_QBUF, &buf);
            break;
        }

        if (encode_one_frame(&enc, g_buffers[buf.index].mpp_buf,
                             (RK_U32)frame_count, &encoded_frames) < 0) {
            xioctl(g_fd, VIDIOC_QBUF, &buf);
            break;
        }
        submitted_frames++;

        if (xioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF failed");
            break;
        }

        frame_count++;
        fps_count++;

        {
            Uint32 now = SDL_GetTicks();
            if (now - fps_tick >= 1000) {
                char title[128];
                snprintf(title, sizeof(title),
                         "V4L2 Preview - %dx%d - %d fps - enc %d",
                         g_width, g_height, fps_count, encoded_frames);
                SDL_SetWindowTitle(window, title);
                printf("fps=%d encoded=%d\n", fps_count, encoded_frames);
                fps_count = 0;
                fps_tick = now;
            }
        }
    }

    printf("displayed frames: %d\n", frame_count);
    printf("submitted frames: %d\n", submitted_frames);
    printf("encoded packets: %d\n", encoded_frames);

out:
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
    if (g_fifo_fd >= 0) {
        close(g_fifo_fd);
        g_fifo_fd = -1;
    }
    encoder_deinit(&enc);
    stop_capture();
    cleanup();
    return 0;
}
