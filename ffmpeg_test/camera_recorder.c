#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

#define OUTPUT_FILE "recording.h265"
#define RECORD_DURATION 10  // 录制时长（秒）

int main() {
    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *options = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFrame *frame = NULL;
    AVFrame *yuv_frame = NULL;
    AVPacket *pkt = NULL;
    
    // 编码器相关
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
    Uint32 start_time, current_time;
    
    printf("=== 摄像头录制程序 (H.265) ===\n");
    printf("将录制 %d 秒视频到文件: %s\n", RECORD_DURATION, OUTPUT_FILE);
    
    // ========== 1. 初始化 FFmpeg 设备 ==========
    avdevice_register_all();
    
    // 设置摄像头参数
    av_dict_set(&options, "video_size", "1632x1224", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "input_format", "nv12", 0);
    
    // 打开摄像头设备
    ret = avformat_open_input(&fmt_ctx, 
                        "/dev/video11", 
                        av_find_input_format("video4linux2"), 
                        &options);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "打开摄像头失败: %s\n", errbuf);
        return -1;
    }
    printf("摄像头打开成功\n");
    
    // 获取视频流信息
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
    
    // ========== 2. 初始化 H.265 编码器 ==========
    encoder = avcodec_find_encoder_by_name("hevc_rkmpp");
    if (!encoder) {
        fprintf(stderr, "未找到 HEVC 编码器\n");
        return -1;
    }
    printf("找到编码器: %s\n", encoder->name);
    
    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        fprintf(stderr, "无法分配编码器上下文\n");
        return -1;
    }
    
    // 设置编码参数
    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->time_base = (AVRational){1, 30};
    enc_ctx->framerate = (AVRational){30, 1};
    enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    enc_ctx->bit_rate = 4000000;  // 4 Mbps，可根据需要调整
    enc_ctx->gop_size = 30;       // 每30帧一个关键帧
    enc_ctx->max_b_frames = 0;
    
    // 可选：设置编码质量
    av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
    av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);
    
    // 打开编码器
    ret = avcodec_open2(enc_ctx, encoder, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "无法打开编码器: %s\n", errbuf);
        return -1;
    }
    printf("编码器初始化成功\n");
    
    // ========== 3. 初始化 SDL2 ==========
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return -1;
    }
    
    window = SDL_CreateWindow("Camera Recording - Press ESC to stop early",
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
    printf("SDL2 窗口初始化成功\n");
    
    // ========== 4. 打开输出文件 ==========
    FILE *outfile = fopen(OUTPUT_FILE, "wb");
    if (!outfile) {
        fprintf(stderr, "无法创建输出文件: %s\n", OUTPUT_FILE);
        return -1;
    }
    printf("输出文件: %s\n", OUTPUT_FILE);
    
    // ========== 5. 准备帧和格式转换 ==========
    // 用于预览的帧（YUV420P）
    yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = width;
    yuv_frame->height = height;
    av_frame_get_buffer(yuv_frame, 32);
    
    // 用于编码的帧（NV12）
    enc_frame = av_frame_alloc();
    enc_frame->format = AV_PIX_FMT_NV12;
    enc_frame->width = width;
    enc_frame->height = height;
    av_frame_get_buffer(enc_frame, 32);
    
    // 格式转换上下文 (NV12 -> YUV420P 用于预览)
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
    
    // ========== 6. 写入 H.265 文件头 ==========
    // 写入简单的 H.265 文件头（可选，某些播放器需要）
    const uint8_t h265_header[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x96, 0xa0, 0x05};
    // 实际使用中，编码器会输出包含参数集的包，不需要手动添加
    
    // ========== 7. 主循环 ==========
    start_time = SDL_GetTicks();
    printf("开始录制...\n");
    
    while (running) {
        // 从摄像头读取一帧
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                usleep(1000);
                continue;
            }
            break;
        }
        
        // 直接从 packet 中获取 NV12 数据
        frame->format = AV_PIX_FMT_NV12;
        frame->width = width;
        frame->height = height;
        frame->data[0] = pkt->data;
        frame->data[1] = pkt->data + width * height;
        frame->linesize[0] = width;
        frame->linesize[1] = width;
        
        // 格式转换：NV12 -> YUV420P (用于预览)
        sws_scale(sws_ctx,
                  (const uint8_t * const*)frame->data, 
                  frame->linesize, 0, height,
                  yuv_frame->data, yuv_frame->linesize);
        
        // 更新预览纹理
        SDL_UpdateYUVTexture(texture, NULL,
                             yuv_frame->data[0], yuv_frame->linesize[0],
                             yuv_frame->data[1], yuv_frame->linesize[1],
                             yuv_frame->data[2], yuv_frame->linesize[2]);
        
        // 渲染预览
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        
        // ========== 编码当前帧 ==========
        // 复制数据到编码帧
        enc_frame->pts = frame_count;
        memcpy(enc_frame->data[0], frame->data[0], width * height);
        memcpy(enc_frame->data[1], frame->data[1], width * height / 2);
        
        // 发送帧到编码器
        ret = avcodec_send_frame(enc_ctx, enc_frame);
        if (ret < 0) {
            fprintf(stderr, "发送帧到编码器失败\n");
            break;
        }
        
        // 接收编码后的数据包
        while (1) {
            ret = avcodec_receive_packet(enc_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr, "接收编码包失败\n");
                break;
            }
            
            // 写入文件
            fwrite(enc_pkt->data, 1, enc_pkt->size, outfile);
            fflush(outfile);
            encoded_frames++;
            
            av_packet_unref(enc_pkt);
        }
        
        frame_count++;
        
        // 显示进度
        current_time = SDL_GetTicks() - start_time;
        if (current_time >= RECORD_DURATION * 1000) {
            printf("\n录制完成！\n");
            running = 0;
            break;
        }
        
        // 显示FPS和进度
        static int last_frame = 0;
        static Uint32 last_time = 0;
        last_frame++;
        Uint32 now = SDL_GetTicks();
        if (now - last_time >= 1000) {
            char title[256];
            snprintf(title, sizeof(title), 
                     "录制中: %d/%ds - %d fps | 编码: %d帧",
                     (int)(current_time/1000), RECORD_DURATION, last_frame, encoded_frames);
            SDL_SetWindowTitle(window, title);
            last_frame = 0;
            last_time = now;
        }
        
        av_packet_unref(pkt);
        
        // 处理事件（允许提前结束）
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                printf("\n用户提前结束录制\n");
                running = 0;
            }
        }
    }
    
    // ========== 8. 刷新编码器缓冲区 ==========
    printf("刷新编码器...\n");
    avcodec_send_frame(enc_ctx, NULL);
    while (1) {
        ret = avcodec_receive_packet(enc_ctx, enc_pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "刷新编码器失败\n");
            break;
        }
        fwrite(enc_pkt->data, 1, enc_pkt->size, outfile);
        encoded_frames++;
        av_packet_unref(enc_pkt);
    }
    
    // ========== 9. 统计和清理 ==========
    printf("\n=== 录制统计 ===\n");
    printf("总帧数: %d\n", frame_count);
    printf("编码帧数: %d\n", encoded_frames);
    printf("输出文件: %s\n", OUTPUT_FILE);
    
    // 获取文件大小
    fseek(outfile, 0, SEEK_END);
    long file_size = ftell(outfile);
    printf("文件大小: %.2f MB\n", file_size / (1024.0 * 1024.0));
    
    fclose(outfile);
    
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
    
    printf("程序正常退出\n");
    return 0;
}