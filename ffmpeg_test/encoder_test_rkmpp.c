/*
 * RKMPP encoder - Working version without hardware frames
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>

int main(int argc, char **argv)
{
    int ret;
    const AVCodec *codec;
    AVCodecContext *enc_ctx = NULL;
    AVFrame *sw_frame = NULL;
    AVPacket *pkt = NULL;
    int width = 640, height = 480;  // 先用小分辨率测试
    int i;
    
    printf("=== RKMPP Encoder Working Test ===\n");
    
    // 1. 查找编码器
    codec = avcodec_find_encoder_by_name("hevc_rkmpp");
    if (!codec) {
        fprintf(stderr, "Encoder not found\n");
        return -1;
    }
    printf("Found: %s\n", codec->name);
    
    // 2. 分配编码器上下文
    enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx) {
        fprintf(stderr, "Cannot allocate context\n");
        return -1;
    }
    
    // 3. 设置参数
    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->time_base = (AVRational){1, 30};
    enc_ctx->framerate = (AVRational){30, 1};
    enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    enc_ctx->bit_rate = 2000000;  // 2 Mbps
    enc_ctx->gop_size = 60;
    enc_ctx->max_b_frames = 0;
    
    // 4. 尝试打开编码器（不设置硬件设备）
    printf("Opening encoder...\n");
    ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Cannot open encoder: %s\n", errbuf);
        return -1;
    }
    printf("Encoder opened successfully\n");
    
    // 5. 分配软件帧
    sw_frame = av_frame_alloc();
    if (!sw_frame) {
        fprintf(stderr, "Cannot allocate frame\n");
        return -1;
    }
    
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = width;
    sw_frame->height = height;
    
    ret = av_frame_get_buffer(sw_frame, 32);
    if (ret < 0) {
        fprintf(stderr, "Cannot allocate frame buffer\n");
        return -1;
    }
    printf("Frame buffer allocated\n");
    
    // 6. 分配 packet
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Cannot allocate packet\n");
        return -1;
    }
    
    // 7. 编码测试帧
    printf("Encoding 5 frames...\n");
    for (i = 0; i < 5; i++) {
        // 填充绿色 (Y=149, U=43, V=21)
        // Y 平面
        for (int h = 0; h < height; h++) {
            memset(sw_frame->data[0] + h * sw_frame->linesize[0], 149, width);
        }
        // UV 平面
        for (int h = 0; h < height/2; h++) {
            for (int w = 0; w < width/2; w++) {
                sw_frame->data[1][h * sw_frame->linesize[1] + w*2] = 43;
                sw_frame->data[1][h * sw_frame->linesize[1] + w*2 + 1] = 21;
            }
        }
        
        sw_frame->pts = i;
        
        // 发送帧到编码器
        ret = avcodec_send_frame(enc_ctx, sw_frame);
        if (ret < 0) {
            fprintf(stderr, "Send frame %d error: %d\n", i, ret);
            break;
        }
        
        // 接收编码后的包
        while (1) {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr, "Receive packet error: %d\n", ret);
                break;
            }
            
            printf("Frame %d: %d bytes\n", i, pkt->size);
            av_packet_unref(pkt);
        }
    }
    
    // 8. 刷新编码器
    printf("Flushing encoder...\n");
    avcodec_send_frame(enc_ctx, NULL);
    while (1) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Flush error: %d\n", ret);
            break;
        }
        printf("Flush packet: %d bytes\n", pkt->size);
        av_packet_unref(pkt);
    }
    
    printf("✅ Encoding completed successfully!\n");
    
    // 9. 清理
    av_packet_free(&pkt);
    av_frame_free(&sw_frame);
    avcodec_free_context(&enc_ctx);
    
    return 0;
}