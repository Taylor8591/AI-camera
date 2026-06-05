#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define VIDEO_DEVICE    "/dev/video11"
#define WIDTH           3264
#define HEIGHT          2448
#define BUFFER_COUNT    2

struct buffer {
    void *start[3];    // 多平面最多3个
    size_t length[3];
};

static int fd = -1;
static struct buffer *buffers;

static int wait_for_frame()
{
    fd_set fds;
    struct timeval tv;
    int ret;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret < 0) {
        perror("select failed");
        return -1;
    }

    if (ret == 0) {
        fprintf(stderr, "wait frame timeout\n");
        return -1;
    }

    return 0;
}

static int open_device()
{
    fd = open(VIDEO_DEVICE, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }
    return 0;
}

// ====================== 多平面格式设置 ======================
static int set_format()
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    // 这里必须用多平面类型！
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    fmt.fmt.pix_mp.width = WIDTH;
    fmt.fmt.pix_mp.height = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 2; // NV12 是 2 平面

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("set format failed");
        return -1;
    }

    printf("设置成功：width=%d, height=%d, format=NV12(MP)\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
    return 0;
}

// ====================== 多平面 MMAP ======================
static int init_mmap()
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("reqbufs failed");
        return -1;
    }

    buffers = calloc(req.count, sizeof(*buffers));

    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[3];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 2;
        buf.m.planes = planes;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("querybuf failed");
            return -1;
        }

        for (int p = 0; p < 2; p++) {
            buffers[i].start[p] = mmap(NULL, planes[p].length,
                PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
            buffers[i].length[p] = planes[p].length;
        }

        // 入队
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("qbuf failed");
            return -1;
        }
    }

    return 0;
}

static int start_capture()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("stream on failed");
        return -1;
    }
    return 0;
}

// ====================== 读取 NV12 帧 ======================
static int grab_frame(const char *filename)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[3];
    FILE *fp;

    if (wait_for_frame() < 0)
        return -1;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 2;
    buf.m.planes = planes;

    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            fprintf(stderr, "dqbuf failed: frame not ready yet\n");
        else
            perror("dqbuf failed");
        return -1;
    }

    // 合并 NV12 数据并保存
    fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen failed");
        ioctl(fd, VIDIOC_QBUF, &buf);
        return -1;
    }

    if (fwrite(buffers[buf.index].start[0], 1, planes[0].bytesused, fp) != planes[0].bytesused) {
        perror("write plane 0 failed");
        fclose(fp);
        ioctl(fd, VIDIOC_QBUF, &buf);
        return -1;
    }

    if (fwrite(buffers[buf.index].start[1], 1, planes[1].bytesused, fp) != planes[1].bytesused) {
        perror("write plane 1 failed");
        fclose(fp);
        ioctl(fd, VIDIOC_QBUF, &buf);
        return -1;
    }

    fclose(fp);

    printf("保存成功：%s\n", filename);

    ioctl(fd, VIDIOC_QBUF, &buf);
    return 0;
}

static void stop_capture()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
}

static void cleanup()
{
    for (int i = 0; i < BUFFER_COUNT; i++) {
        munmap(buffers[i].start[0], buffers[i].length[0]);
        munmap(buffers[i].start[1], buffers[i].length[1]);
    }
    free(buffers);
    close(fd);
}

int main()
{
    if (open_device() < 0) return -1;
    if (set_format() < 0) return -1;
    if (init_mmap() < 0) return -1;
    if (start_capture() < 0) return -1;

    grab_frame("out.yuv");

    stop_capture();
    cleanup();
    return 0;
}