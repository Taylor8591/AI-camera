#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 封装格式处理（打开设备、读取帧）
#include <libavformat/avformat.h>
// 设备输入（V4L2摄像头）
#include <libavdevice/avdevice.h>
// 编解码器
#include <libavcodec/avcodec.h>
// 通用工具（错误处理、数学运算）
#include <libavutil/avutil.h>
// 图像工具（帧缓冲区分配）
#include <libavutil/imgutils.h>
// 像素格式转换（NV12→YUV420P）
#include <libswscale/swscale.h>

// 图形显示（窗口、纹理、事件）
#include <SDL2/SDL.h>

#include <unistd.h>

int main() {
    AVFormatContext *fmt_ctx = NULL;        // 格式上下文，存储摄像头设备和流信息
    AVDictionary *options = NULL;           // 参数选项，传递给设备的键值对
    struct SwsContext *sws_ctx = NULL;
    AVFrame *frame = NULL;                  // 临时帧，指向摄像头原始数据
    AVFrame *yuv_frame = NULL;              // 转换后帧，存储YUV420P格式数据
    AVPacket *pkt = NULL;                   // 数据包，存储从摄像头读取的一帧原始数据
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;          // 渲染器，负责将纹理绘制到窗口
    SDL_Texture *texture = NULL;            // 纹理，GPU中的图像数据
    SDL_Event e;
    int ret, video_stream_index = -1;
    int frame_count = 0;
    int running = 1;
    int width = 1632, height = 1224;
    
    printf("=== 摄像头预览程序 ===\n");
    
    // ========== 1. 初始化 FFmpeg ==========
    avdevice_register_all();    // 注册所有设备驱动（V4L2、ALSA等）
    
    // 设置摄像头参数（直接指定分辨率和格式）
    av_dict_set(&options, "video_size", "1632x1224", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "input_format", "nv12", 0);
    
    // 打开摄像头设备
    ret = avformat_open_input(&fmt_ctx, "/dev/video11", av_find_input_format("video4linux2"), &options);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "打开摄像头失败: %s\n", errbuf);
        return -1;
    }
    printf("摄像头打开成功\n");
    
    
    // 遍历所有流，找到视频流并获取宽高
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = i;
                width = fmt_ctx->streams[i]->codecpar->width;
                height = fmt_ctx->streams[i]->codecpar->height;
                break;
            }
        } else {
            // V4L2设备的codecpar可能为NULL，所以有else分支兜底
            video_stream_index = i;
            width = 1632;
            height = 1224;
            break;
        }
    }
    if (video_stream_index == -1) {
        // 如果没有找到流，使用默认值
        video_stream_index = 0;
        width = 1632;
        height = 1224;
    }
    printf("视频尺寸: %dx%d\n", width, height);
    
    // NV12 是原始格式，不需要解码，直接使用
    
    // ========== 4. 初始化 SDL2 ==========
    // 初始化SDL2视频子系统
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return -1;
    }
    // 创建窗口
    window = SDL_CreateWindow("Camera Preview",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              width, height,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "创建窗口失败: %s\n", SDL_GetError());
        return -1;
    }
    // 创建渲染器和纹理
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, // YUV420P格式
                                SDL_TEXTUREACCESS_STREAMING,
                                width, height);
    printf("SDL2 窗口初始化成功\n");
    
    // ========== 5. 准备格式转换 ==========
    yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = width;
    yuv_frame->height = height;
    av_frame_get_buffer(yuv_frame, 32);
    // 初始化转换上下文 (NV12 -> YUV420P)
    sws_ctx = sws_getContext(width, height, AV_PIX_FMT_NV12,        // 源格式
                             width, height, AV_PIX_FMT_YUV420P,     // 目标格式
                             SWS_BILINEAR,                          // 双线性插值算法
                             NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "无法创建格式转换上下文\n");
        return -1;
    }
    
    // ========== 6. 分配帧和包 ==========
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    
    // ========== 7. 主循环 ==========
    while (running) {
        // 从摄像头读取一帧
        ret = av_read_frame(fmt_ctx, pkt);      // 直接返回原始帧数据（NV12格式）
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                usleep(1000);
                continue;
            }
            break;
        }
        
        frame_count++;
        
        // 直接从 packet 中获取 NV12 数据
        // 创建一个临时 AVFrame 指向 packet 数据
        frame->format = AV_PIX_FMT_NV12;
        frame->width = width;
        frame->height = height;
        frame->data[0] = pkt->data;                     // Y平面数据
        frame->data[1] = pkt->data + width * height;    // UV平面数据
        frame->linesize[0] = width;                     // Y平面行间距
        frame->linesize[1] = width;                     // UV平面行间距
        
        // 格式转换：NV12 -> YUV420P
        sws_scale(sws_ctx,
                  (const uint8_t * const*)frame->data, 
                  frame->linesize, 0, height,
                  yuv_frame->data, yuv_frame->linesize);
        
        // 更新 SDL 纹理
        SDL_UpdateYUVTexture(texture, NULL,
                             yuv_frame->data[0], yuv_frame->linesize[0],
                             yuv_frame->data[1], yuv_frame->linesize[1],
                             yuv_frame->data[2], yuv_frame->linesize[2]);
        
        // 渲染
        SDL_RenderClear(renderer);                      // 清空渲染器
        SDL_RenderCopy(renderer, texture, NULL, NULL);  // 复制纹理到渲染器
        SDL_RenderPresent(renderer);                    // 交换缓冲区，显示画面
        
        // 帧率显示
        static int last_frame = 0;
        static Uint32 last_time = 0;
        last_frame++;
        Uint32 now = SDL_GetTicks();
        if (now - last_time >= 1000) {
            char title[256];
            snprintf(title, sizeof(title), 
                     "Camera Preview - %d fps", last_frame);
            SDL_SetWindowTitle(window, title);
            last_frame = 0;
            last_time = now;
        }
        
        av_packet_unref(pkt);
        
        // 处理事件
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                running = 0;
            }
        }
    }
    
    // ========== 8. 清理 ==========
    printf("\n共显示 %d 帧\n", frame_count);
    
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    
    printf("程序退出\n");
    return 0;
}