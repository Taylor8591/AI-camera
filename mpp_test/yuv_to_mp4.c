/*
 * H.265/HEVC encoder directly to MP4
 * Combines: yuv_to_h265.c (MPP encoding) + h265_to_mp4.c (FFmpeg container)
 * No intermediate H.265 file needed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "rk_mpi.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifndef MPP_ALIGN
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

typedef struct {
    MppCtx          ctx;
    MppApi          *mpi;
    RK_U32          width;
    RK_U32          height;
    RK_U32          fmt;
    MppBuffer       frm_buf;
    MppBuffer       pkt_buf;
} EncoderCtx;

typedef struct {
    const char *name;
    RK_U8 y, u, v;
} ColorDef;

static const ColorDef colors[] = {
    {"black",    16,  128,  128},
    {"white",   235,  128,  128},
    {"red",      76,   85,  255},
    {"green",   149,   43,   21},
    {"blue",     29,  255,  107},
    {"yellow",  225,   16,  149},
    {"cyan",    178,  170,   15},
    {"magenta", 105,  212,  234},
    {"gray",    128,  128,  128},
    {NULL, 0, 0, 0}
};

/* Generate solid color YUV420SP frame */
static void generate_solid_color_yuv(RK_U8 *buf, RK_U32 width, RK_U32 height,
                                     RK_U32 hor_stride, RK_U32 ver_stride,
                                     RK_U8 y, RK_U8 u, RK_U8 v)
{
    RK_U32 i, j;
    RK_U8 *data = buf;
    
    /* Fill Y plane */
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++)
            data[j] = y;
        data += hor_stride;
    }
    
    /* Fill UV plane (NV12 format) */
    RK_U32 uv_height = height / 2;
    RK_U32 uv_width = width / 2;
    data = buf + hor_stride * ver_stride;
    
    for (i = 0; i < uv_height; i++) {
        for (j = 0; j < uv_width; j++) {
            data[2 * j] = u;
            data[2 * j + 1] = v;
        }
        data += hor_stride;
    }
}

/* Initialize H.265 encoder */
static int encoder_init(EncoderCtx *ctx, RK_U32 width, RK_U32 height)
{
    MPP_RET ret = MPP_OK;
    MppEncCfg cfg = NULL;
    MppBufferGroup buf_grp = NULL;
    
    ctx->width = width;
    ctx->height = height;
    ctx->fmt = MPP_FMT_YUV420SP;
    
    RK_U32 hor_stride = MPP_ALIGN(width, 16);
    RK_U32 ver_stride = MPP_ALIGN(height, 16);
    RK_U32 frame_size = hor_stride * ver_stride * 3 / 2;
    
    printf("Encoder init: %dx%d, stride: %dx%d, frame_size: %d\n",
            width, height, hor_stride, ver_stride, frame_size);
    
    /* Create encoder context */
    ret = mpp_create(&ctx->ctx, &ctx->mpi);
    if (ret) {
        fprintf(stderr, "mpp_create failed\n");
        return -1;
    }
    
    /* Initialize for H.265 encoding */
    ret = mpp_init(ctx->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
    if (ret) {
        fprintf(stderr, "mpp_init failed\n");
        return -1;
    }
    
    /* Set output timeout to block mode */
    RK_U32 timeout = MPP_POLL_BLOCK;
    ctx->mpi->control(ctx->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    
    /* Get and configure encoder */
    ret = mpp_enc_cfg_init(&cfg);
    if (ret) {
        fprintf(stderr, "mpp_enc_cfg_init failed\n");
        return -1;
    }
    
    ret = ctx->mpi->control(ctx->ctx, MPP_ENC_GET_CFG, cfg);
    if (ret) {
        fprintf(stderr, "get enc cfg failed\n");
        return -1;
    }
    
    /* Set encoding parameters */
    mpp_enc_cfg_set_u32(cfg, "prep:width", width);
    mpp_enc_cfg_set_u32(cfg, "prep:height", height);
    mpp_enc_cfg_set_u32(cfg, "prep:hor_stride", hor_stride);
    mpp_enc_cfg_set_u32(cfg, "prep:ver_stride", ver_stride);
    mpp_enc_cfg_set_u32(cfg, "prep:fmt", ctx->fmt);
    
    /* Rate control */
    mpp_enc_cfg_set_u32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_u32(cfg, "rc:bps", width * height * 30 / 2);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_in_num", 30);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_in_den", 1);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_out_num", 30);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_out_den", 1);
    mpp_enc_cfg_set_u32(cfg, "gop:gop_len", 30);
    
    ret = ctx->mpi->control(ctx->ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        fprintf(stderr, "set enc cfg failed\n");
        return -1;
    }
    
    mpp_enc_cfg_deinit(cfg);
    
    /* Allocate buffers */
    ret = mpp_buffer_group_get_internal(&buf_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
    if (ret) {
        fprintf(stderr, "failed to get buffer group\n");
        return -1;
    }
    
    ret = mpp_buffer_get(buf_grp, &ctx->frm_buf, frame_size);
    if (ret) {
        fprintf(stderr, "failed to get frame buffer\n");
        return -1;
    }
    
    ret = mpp_buffer_get(buf_grp, &ctx->pkt_buf, frame_size);
    if (ret) {
        fprintf(stderr, "failed to get packet buffer\n");
        return -1;
    }
    
    mpp_buffer_group_put(buf_grp);
    
    printf("Encoder initialized successfully\n");
    return 0;
}

/* Encode one frame and return H.265 packet */
static int encode_frame(EncoderCtx *ctx, RK_U8 y, RK_U8 u, RK_U8 v, 
                       RK_U8 **out_buf, RK_U32 *out_len)
{
    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    RK_U32 hor_stride = MPP_ALIGN(ctx->width, 16);
    RK_U32 ver_stride = MPP_ALIGN(ctx->height, 16);
    
    *out_buf = NULL;
    *out_len = 0;
    
    /* Generate YUV data */
    void *buf = mpp_buffer_get_ptr(ctx->frm_buf);
    mpp_buffer_sync_begin(ctx->frm_buf);
    generate_solid_color_yuv(buf, ctx->width, ctx->height, hor_stride, ver_stride, y, u, v);
    mpp_buffer_sync_end(ctx->frm_buf);
    
    /* Create frame */
    ret = mpp_frame_init(&frame);
    if (ret) {
        fprintf(stderr, "mpp_frame_init failed\n");
        return -1;
    }
    
    mpp_frame_set_width(frame, ctx->width);
    mpp_frame_set_height(frame, ctx->height);
    mpp_frame_set_hor_stride(frame, hor_stride);
    mpp_frame_set_ver_stride(frame, ver_stride);
    mpp_frame_set_fmt(frame, ctx->fmt);
    mpp_frame_set_buffer(frame, ctx->frm_buf);
    
    /* Encode */
    ret = ctx->mpi->encode_put_frame(ctx->ctx, frame);
    if (ret) {
        fprintf(stderr, "encode_put_frame failed\n");
        mpp_frame_deinit(&frame);
        return -1;
    }
    
    mpp_frame_deinit(&frame);
    
    /* Get encoded packet */
    ret = ctx->mpi->encode_get_packet(ctx->ctx, &packet);
    if (ret) {
        fprintf(stderr, "encode_get_packet failed\n");
        return -1;
    }
    
    if (packet) {
        *out_buf = (RK_U8 *)mpp_packet_get_pos(packet);
        *out_len = mpp_packet_get_length(packet);
        mpp_packet_deinit(&packet);
    }
    
    return 0;
}

void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -w WIDTH      Video width (default: 1280)\n");
    printf("  -h HEIGHT     Video height (default: 720)\n");
    printf("  -n FRAMES     Number of frames (default: 100)\n");
    printf("  -o OUTPUT     Output MP4 file (default: output.mp4)\n");
    printf("  -c COLOR      Color: red, green, blue, white, black, yellow, cyan, magenta, gray (default: yellow)\n");
    printf("Example:\n");
    printf("  %s -w 1920 -h 1080 -n 200 -o video.mp4 -c red\n", prog);
}

int main(int argc, char **argv)
{
    EncoderCtx enc_ctx;
    RK_U32 width = 1280, height = 720, frame_count = 100;
    const char *output_file = "output.mp4";
    const char *color_name = "magenta";
    RK_U8 color_y = 128, color_u = 128, color_v = 128;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && i + 1 < argc) {
            switch (argv[i][1]) {
                case 'w': width = atoi(argv[++i]); break;
                case 'h':
                    if (argv[i][2] == '\0') height = atoi(argv[++i]);
                    else { print_usage(argv[0]); return 0; }
                    break;
                case 'n': frame_count = atoi(argv[++i]); break;
                case 'o': output_file = argv[++i]; break;
                case 'c': color_name = argv[++i]; break;
                default: fprintf(stderr, "Unknown option: %s\n", argv[i]); return -1;
            }
        }
    }
    
    /* Look up color */
    for (int i = 0; colors[i].name != NULL; i++) {
        if (strcmp(colors[i].name, color_name) == 0) {
            color_y = colors[i].y;
            color_u = colors[i].u;
            color_v = colors[i].v;
            break;
        }
    }
    
    printf("\n=== H.265 to MP4 Direct Encoder ===\n");
    printf("Resolution: %d x %d\n", width, height);
    printf("Frames: %d\n", frame_count);
    printf("Color: %s (Y=%d, U=%d, V=%d)\n", color_name, color_y, color_u, color_v);
    printf("Output: %s\n\n", output_file);
    
    /* Initialize encoder */
    memset(&enc_ctx, 0, sizeof(enc_ctx));
    if (encoder_init(&enc_ctx, width, height) < 0) {
        fprintf(stderr, "encoder init failed\n");
        return -1;
    }
    
    /* Initialize FFmpeg */
    AVFormatContext *ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, output_file);
    if (!ofmt_ctx) {
        fprintf(stderr, "create output context failed\n");
        return -1;
    }
    
    /* Create video stream */
    AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) {
        fprintf(stderr, "create stream failed\n");
        return -1;
    }
    
    /* Set codec parameters */
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    out_stream->codecpar->codec_id = AV_CODEC_ID_HEVC;
    out_stream->codecpar->width = width;
    out_stream->codecpar->height = height;
    out_stream->codecpar->codec_tag = 0;
    
    /* Set time base - crucial for correct duration calculation */
    out_stream->time_base.num = 1;
    out_stream->time_base.den = 30;
    out_stream->r_frame_rate.num = 30;
    out_stream->r_frame_rate.den = 1;
    out_stream->avg_frame_rate.num = 30;
    out_stream->avg_frame_rate.den = 1;
    
    /* Open output file */
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "open output file failed\n");
            return -1;
        }
    }
    
    /* Write header */
    if (avformat_write_header(ofmt_ctx, NULL) < 0) {
        fprintf(stderr, "write header failed\n");
        return -1;
    }
    
    printf("Encoding frames...\n");
    
    /* Encode frames directly to MP4 */
    int first_frame = 1;
    for (RK_U32 i = 0; i < frame_count; i++) {
        RK_U8 *pkt_data = NULL;
        RK_U32 pkt_len = 0;
        
        /* Encode frame */
        if (encode_frame(&enc_ctx, color_y, color_u, color_v, &pkt_data, &pkt_len) < 0) {
            fprintf(stderr, "encode frame %d failed\n", i);
            break;
        }
        
        if (pkt_len > 0) {
            /* For first frame, extract and set codec extradata (VPS/SPS/PPS for HEVC) */
            if (first_frame && i == 0) {
                /* HEVC packets start with 00 00 00 01 */
                /* Extract header info if available */
                out_stream->codecpar->extradata_size = 0;
                first_frame = 0;
            }
            
            /* Write to MP4 - must copy data as encoder buffer is reused */
            AVPacket *pkt = av_packet_alloc();
            if (!pkt) {
                fprintf(stderr, "alloc packet failed\n");
                break;
            }
            
            /* Copy encoded data to avoid buffer reuse issues */
            uint8_t *data = av_malloc(pkt_len);
            if (!data) {
                av_packet_free(&pkt);
                fprintf(stderr, "malloc packet data failed\n");
                break;
            }
            memcpy(data, pkt_data, pkt_len);
            
            pkt->data = data;
            pkt->size = pkt_len;
            pkt->stream_index = 0;
            pkt->pts = i;
            pkt->dts = i;
            pkt->duration = 1;
            
            /* Mark I-frames (every 60 frames per GOP) */
            if (i == 0 || i % 60 == 0)
                pkt->flags |= AV_PKT_FLAG_KEY;
            
            /* Rescale timestamps to stream time_base - CRUCIAL FIX */
            AVRational internal_tb = {1, 30};
            av_packet_rescale_ts(pkt, internal_tb, out_stream->time_base);
            
            int64_t pts_val = pkt->pts;  /* Save before write */
            
            if (av_interleaved_write_frame(ofmt_ctx, pkt) < 0) {
                fprintf(stderr, "write frame failed\n");
                av_packet_free(&pkt);
                break;
            }
            
            av_packet_free(&pkt);
            
            if (i < 5 || i % 50 == 0)
                printf("Frame %3d: %4d bytes (pts:%ld)\n", i, pkt_len, pts_val);
        }
        
        if ((i + 1) % 50 == 0) printf("  ... %d frames encoded\n", i + 1);
    }
    
    /* Flush encoder */
    printf("Flushing encoder...\n");
    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_eos(frame, 1);
    enc_ctx.mpi->encode_put_frame(enc_ctx.ctx, frame);
    mpp_frame_deinit(&frame);
    
    /* Get remaining packets */
    RK_S32 timeout_count = 0;
    while (timeout_count < 10) {
        MppPacket packet = NULL;
        if (enc_ctx.mpi->encode_get_packet(enc_ctx.ctx, &packet) < 0) {
            timeout_count++;
            continue;
        }
        
        timeout_count = 0;
        
        if (packet) {
            RK_U8 *ptr = (RK_U8 *)mpp_packet_get_pos(packet);
            size_t len = mpp_packet_get_length(packet);
            
            if (len > 0) {
                AVPacket *pkt = av_packet_alloc();
                if (!pkt) {
                    mpp_packet_deinit(&packet);
                    continue;
                }
                
                /* Copy data */
                uint8_t *data = av_malloc(len);
                if (!data) {
                    av_packet_free(&pkt);
                    mpp_packet_deinit(&packet);
                    continue;
                }
                memcpy(data, ptr, len);
                
                pkt->data = data;
                pkt->size = len;
                pkt->stream_index = 0;
                pkt->pts = frame_count;
                pkt->dts = frame_count;
                pkt->duration = 1;
                
                /* Rescale timestamps */
                AVRational internal_tb = {1, 30};
                av_packet_rescale_ts(pkt, internal_tb, out_stream->time_base);
                
                av_interleaved_write_frame(ofmt_ctx, pkt);
                av_packet_free(&pkt);
            }
            
            if (mpp_packet_get_eos(packet)) {
                printf("Received EOS, flushed\n");
                mpp_packet_deinit(&packet);
                break;
            }
            
            mpp_packet_deinit(&packet);
        }
    }
    
    /* Write trailer */
    av_write_trailer(ofmt_ctx);
    
    printf("\n✅ Success: MP4 file saved to %s\n", output_file);
    
    /* Cleanup */
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    if (ofmt_ctx) avformat_free_context(ofmt_ctx);
    
    if (enc_ctx.frm_buf) mpp_buffer_put(enc_ctx.frm_buf);
    if (enc_ctx.pkt_buf) mpp_buffer_put(enc_ctx.pkt_buf);
    if (enc_ctx.ctx) {
        enc_ctx.mpi->reset(enc_ctx.ctx);
        mpp_destroy(enc_ctx.ctx);
    }
    
    return 0;
}
