#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

#define OUTPUT_FIFO "/tmp/live.h265"
#define MAX_OPEN_RETRY_US 500000

static int ensure_fifo_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "路径已存在但不是FIFO: %s\n", path);
            return -1;
        }
        return 0;
    }

    if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo 失败");
        return -1;
    }
    return 0;
}

static int open_fifo_writer_blocking(const char *path) {
    int fd;
    for (;;) {
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            printf("FIFO写端已连接: %s\n", path);
            return fd;
        }
        perror("open FIFO 写端失败");
        usleep(MAX_OPEN_RETRY_US);
    }
}

int main() {
    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *options = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFrame *frame = NULL;
    AVFrame *yuv_frame = NULL;
    AVPacket *pkt = NULL;

    const AVCodec *encoder = NULL;
    AVCodecContext *enc_ctx = NULL;
    AVFrame *enc_frame = NULL;
    AVPacket *enc_pkt = NULL;

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_Event e;

    int ret, video_stream_index = -1;
    int frame_count = 0;
    int running = 1;
    int width = 1632, height = 1224;
    int encoded_frames = 0;
    int fifo_fd = -1;

    signal(SIGPIPE, SIG_IGN);

    printf("=== 摄像头实时推流编码程序 (H.265 -> FIFO) ===\n");
    printf("输出FIFO: %s\n", OUTPUT_FIFO);

    if (ensure_fifo_exists(OUTPUT_FIFO) < 0) {
        return -1;
    }

    avdevice_register_all();

    av_dict_set(&options, "video_size", "1632x1224", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "input_format", "nv12", 0);

    ret = avformat_open_input(&fmt_ctx, "/dev/video11", av_find_input_format("video4linux2"), &options);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "打开摄像头失败: %s\n", errbuf);
        return -1;
    }
    printf("摄像头打开成功\n");

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar &&
            fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            width = fmt_ctx->streams[i]->codecpar->width;
            height = fmt_ctx->streams[i]->codecpar->height;
            break;
        }
    }
    if (video_stream_index == -1) {
        video_stream_index = 0;
        width = 1632;
        height = 1224;
    }
    printf("视频尺寸: %dx%d\n", width, height);

    encoder = avcodec_find_encoder_by_name("hevc_rkmpp");
    if (!encoder) {
        fprintf(stderr, "未找到 HEVC 编码器 hevc_rkmpp\n");
        return -1;
    }
    printf("找到编码器: %s\n", encoder->name);

    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        fprintf(stderr, "无法分配编码器上下文\n");
        return -1;
    }

    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->time_base = (AVRational){1, 30};
    enc_ctx->framerate = (AVRational){30, 1};
    enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    enc_ctx->bit_rate = 4000000;
    enc_ctx->gop_size = 30;
    enc_ctx->max_b_frames = 0;

    av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
    av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);

    ret = avcodec_open2(enc_ctx, encoder, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "无法打开编码器: %s\n", errbuf);
        return -1;
    }
    printf("编码器初始化成功\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow("Camera Live H265 -> FIFO (ESC to stop)",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              width, height,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "创建窗口失败: %s\n", SDL_GetError());
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                SDL_TEXTUREACCESS_STREAMING,
                                width, height);

    yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = width;
    yuv_frame->height = height;
    av_frame_get_buffer(yuv_frame, 32);

    enc_frame = av_frame_alloc();
    enc_frame->format = AV_PIX_FMT_NV12;
    enc_frame->width = width;
    enc_frame->height = height;
    av_frame_get_buffer(enc_frame, 32);

    sws_ctx = sws_getContext(width, height, AV_PIX_FMT_NV12,
                             width, height, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "无法创建格式转换上下文\n");
        return -1;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();

    printf("等待 live555/播放器连接 FIFO 读端...\n");
    fifo_fd = open_fifo_writer_blocking(OUTPUT_FIFO);

    printf("开始实时编码并写入 FIFO...\n");
    while (running) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                usleep(1000);
                continue;
            }
            break;
        }

        frame->format = AV_PIX_FMT_NV12;
        frame->width = width;
        frame->height = height;
        frame->data[0] = pkt->data;
        frame->data[1] = pkt->data + width * height;
        frame->linesize[0] = width;
        frame->linesize[1] = width;

        sws_scale(sws_ctx,
                  (const uint8_t * const*)frame->data,
                  frame->linesize, 0, height,
                  yuv_frame->data, yuv_frame->linesize);

        SDL_UpdateYUVTexture(texture, NULL,
                             yuv_frame->data[0], yuv_frame->linesize[0],
                             yuv_frame->data[1], yuv_frame->linesize[1],
                             yuv_frame->data[2], yuv_frame->linesize[2]);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        enc_frame->pts = frame_count;
        memcpy(enc_frame->data[0], frame->data[0], width * height);
        memcpy(enc_frame->data[1], frame->data[1], width * height / 2);

        ret = avcodec_send_frame(enc_ctx, enc_frame);
        if (ret < 0) {
            fprintf(stderr, "发送帧到编码器失败\n");
            av_packet_unref(pkt);
            break;
        }

        while (1) {
            ret = avcodec_receive_packet(enc_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr, "接收编码包失败\n");
                break;
            }

            ssize_t w = write(fifo_fd, enc_pkt->data, enc_pkt->size);
            if (w < 0) {
                if (errno == EPIPE || errno == EAGAIN) {
                    close(fifo_fd);
                    printf("FIFO 读端断开，等待重连...\n");
                    fifo_fd = open_fifo_writer_blocking(OUTPUT_FIFO);
                } else {
                    perror("写入 FIFO 失败");
                    running = 0;
                }
            } else if (w != enc_pkt->size) {
                fprintf(stderr, "部分写入 FIFO: %zd/%d\n", w, enc_pkt->size);
            } else {
                encoded_frames++;
            }

            av_packet_unref(enc_pkt);
        }

        frame_count++;
        av_packet_unref(pkt);

        static int last_frame = 0;
        static Uint32 last_time = 0;
        last_frame++;
        Uint32 now = SDL_GetTicks();
        if (now - last_time >= 1000) {
            char title[256];
            snprintf(title, sizeof(title),
                     "实时推流中 - %d fps | 编码: %d 帧",
                     last_frame, encoded_frames);
            SDL_SetWindowTitle(window, title);
            last_frame = 0;
            last_time = now;
        }

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                printf("\n用户结束实时推流\n");
                running = 0;
            }
        }
    }

    printf("\n=== 统计 ===\n");
    printf("采集帧数: %d\n", frame_count);
    printf("编码并写出帧数: %d\n", encoded_frames);

    if (fifo_fd >= 0) close(fifo_fd);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (sws_ctx) sws_freeContext(sws_ctx);
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (enc_frame) av_frame_free(&enc_frame);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);

    printf("程序退出\n");
    return 0;
}
