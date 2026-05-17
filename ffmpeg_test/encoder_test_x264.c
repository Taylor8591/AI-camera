#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

int main(int argc, char **argv) {
    AVFormatContext *input_fmt_ctx = NULL;
    AVFormatContext *output_fmt_ctx = NULL;
    AVDictionary *input_options = NULL;
    AVCodecContext *encoder_ctx = NULL;
    const AVCodec *encoder = NULL;
    AVStream *out_stream = NULL;
    AVPacket *pkt = NULL;
    AVPacket *input_pkt = NULL;
    AVFrame *frame = NULL;
    int video_stream_index = -1;
    int ret;
    int64_t pts_counter = 0;
    
    const char *input_device = "/dev/video11";
    int width = 1632, height = 1224;
    int framerate = 30;
    int bitrate = 4000000;
    int gop_size = 30;
    const char *output_url = "output.h264";
    
    printf("=== H.264 编码推流程序 ===\n");
    printf("输入设备: %s\n", input_device);
    printf("分辨率: %dx%d\n", width, height);
    printf("帧率: %d\n", framerate);
    printf("码率: %d Mbps\n", bitrate / 1000000);
    printf("输出文件: %s\n", output_url);
    printf("按 Ctrl+C 停止\n\n");
    printf("使用说明：\n");
    printf("1. 此程序生成 H.264 裸流文件\n");
    printf("2. 然后可用 ffmpeg 进行 RTSP 推流：\n");
    printf("   ffmpeg -re -i output.h264 -c copy -f rtsp -rtsp_transport tcp rtsp://server:8554/live\n");
    printf("3. 或用 ffplay 播放：\n");
    printf("   ffplay output.h264\n\n");
    
    signal(SIGINT, signal_handler);
    
    // 初始化FFmpeg
    avdevice_register_all();
    avformat_network_init();
    
    // 打开摄像头
    av_dict_set(&input_options, "video_size", "1632x1224", 0);
    av_dict_set(&input_options, "framerate", "30", 0);
    av_dict_set(&input_options, "pixel_format", "nv12", 0);
    
    ret = avformat_open_input(&input_fmt_ctx, input_device, 
                              av_find_input_format("video4linux2"), &input_options);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "打开摄像头失败: %s\n", errbuf);
        return -1;
    }
    printf("摄像头打开成功\n");
    
    // 查找视频流
    for (int i = 0; i < input_fmt_ctx->nb_streams; i++) {
        if (input_fmt_ctx->streams[i]->codecpar && 
            input_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            width = input_fmt_ctx->streams[i]->codecpar->width;
            height = input_fmt_ctx->streams[i]->codecpar->height;
            break;
        }
    }
    if (video_stream_index == -1) {
        video_stream_index = 0;
    }
    printf("视频尺寸: %dx%d\n", width, height);
    
    // 初始化编码器
    encoder = avcodec_find_encoder_by_name("h264_rkmpp");
    if (!encoder) {
        encoder = avcodec_find_encoder_by_name("libx264");
    }
    if (!encoder) {
        fprintf(stderr, "未找到H264编码器\n");
        return -1;
    }
    printf("使用编码器: %s\n", encoder->name);
    
    encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        fprintf(stderr, "无法分配编码器上下文\n");
        return -1;
    }
    
    // 设置编码参数
    encoder_ctx->width = width;
    encoder_ctx->height = height;
    encoder_ctx->time_base = (AVRational){1, framerate};
    encoder_ctx->framerate = (AVRational){framerate, 1};
    encoder_ctx->pix_fmt = AV_PIX_FMT_NV12;
    encoder_ctx->bit_rate = bitrate;
    encoder_ctx->gop_size = gop_size;
    encoder_ctx->max_b_frames = 0;
    
    // 打开编码器
    ret = avcodec_open2(encoder_ctx, encoder, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "打开编码器失败: %s\n", errbuf);
        return -1;
    }
    printf("编码器打开成功\n");
    
    // 打开输出文件（H.264 裸流）
    FILE *output_fp = fopen(output_url, "wb");
    if (!output_fp) {
        fprintf(stderr, "无法打开输出文件: %s\n", output_url);
        return -1;
    }
    printf("输出文件打开成功: %s\n\n", output_url);
    
    // 准备帧缓冲
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "无法分配帧\n");
        return -1;
    }
    frame->format = encoder_ctx->pix_fmt;
    frame->width = width;
    frame->height = height;
    ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        fprintf(stderr, "无法分配帧缓冲区\n");
        return -1;
    }
    
    pkt = av_packet_alloc();
    input_pkt = av_packet_alloc();
    if (!pkt || !input_pkt) {
        fprintf(stderr, "无法分配数据包\n");
        return -1;
    }
    
    // 主循环
    printf("开始编码并保存到文件...\n");
    int64_t start_time = av_gettime_relative();
    int frames_encoded = 0;
    int frames_read = 0;
    
    while (running) {
        // 读取摄像头帧
        ret = av_read_frame(input_fmt_ctx, input_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                usleep(1000);
                continue;
            }
            break;
        }
        
        frames_read++;
        
        // 填充帧数据（NV12格式）
        frame->data[0] = input_pkt->data;
        frame->data[1] = input_pkt->data + width * height;
        frame->linesize[0] = width;
        frame->linesize[1] = width;
        frame->pts = pts_counter++;
        
        // 编码
        ret = avcodec_send_frame(encoder_ctx, frame);
        if (ret < 0) {
            av_packet_unref(input_pkt);
            continue;
        }
        
        // 接收编码包
        while (1) {
            ret = avcodec_receive_packet(encoder_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                break;
            }
            
            // 写入文件
            size_t written = fwrite(pkt->data, 1, pkt->size, output_fp);
            if (written != pkt->size) {
                fprintf(stderr, "文件写入失败\n");
            }
            
            frames_encoded++;
            av_packet_unref(pkt);
        }
        
        av_packet_unref(input_pkt);
        
        // 统计信息
        static int64_t last_time = 0;
        int64_t now = av_gettime_relative();
        if (now - last_time >= 1000000) {
            double elapsed = (now - start_time) / 1000000.0;
            printf("\r[运行] 读取: %d | 编码: %d | 时长: %.1fs      ", 
                   frames_read, frames_encoded, elapsed);
            fflush(stdout);
            last_time = now;
        }
        
        // 帧率控制
        int64_t expected_pts = pts_counter * 1000000 / framerate;
        int64_t actual_time = av_gettime_relative() - start_time;
        if (expected_pts > actual_time) {
            usleep(expected_pts - actual_time);
        }
    }
    
    // 刷新编码器
    printf("\n\n正在刷新编码器...\n");
    avcodec_send_frame(encoder_ctx, NULL);
    while (1) {
        ret = avcodec_receive_packet(encoder_ctx, pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            break;
        
        size_t written = fwrite(pkt->data, 1, pkt->size, output_fp);
        if (written != pkt->size) {
            fprintf(stderr, "文件写入失败\n");
        }
        av_packet_unref(pkt);
    }
    
    printf("\n\n=== 编码统计 ===\n");
    printf("读取帧数: %d\n", frames_read);
    printf("编码帧数: %d\n", frames_encoded);
    printf("编码时长: %.2f 秒\n", (av_gettime_relative() - start_time) / 1000000.0);
    
    // 获取文件大小
    fclose(output_fp);
    FILE *check_fp = fopen(output_url, "rb");
    if (check_fp) {
        fseek(check_fp, 0, SEEK_END);
        long filesize = ftell(check_fp);
        fclose(check_fp);
        printf("输出文件大小: %.2f MB\n", filesize / 1024.0 / 1024.0);
    }
    
    printf("\n文件已保存到: %s\n", output_url);
    printf("可用 ffplay 播放: ffplay %s\n", output_url);
    printf("或转换为 RTSP 推流：\n");
    printf("  ffmpeg -re -i %s -c copy -f rtsp -rtsp_transport tcp rtsp://server:8554/live\n", output_url);
    
    // 清理资源
    av_packet_free(&pkt);
    av_packet_free(&input_pkt);
    av_frame_free(&frame);
    avcodec_free_context(&encoder_ctx);
    
    if (input_fmt_ctx) {
        avformat_close_input(&input_fmt_ctx);
    }
    
    avformat_network_deinit();
    
    printf("\n编码结束\n");
    return 0;
}
