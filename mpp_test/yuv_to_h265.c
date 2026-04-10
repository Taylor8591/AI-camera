/*
 * Simple H.265/HEVC encoder for YUV solid color test images
 * Based on mpi_enc_test.c from Rockchip MPP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#include "rk_mpi.h"

/* Alignment macro */
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
    
    FILE            *fp_output;
} H265EncoderCtx;

/* Generate solid color YUV frame */
static void generate_solid_color_yuv(RK_U8 *buf, RK_U32 width, RK_U32 height,
                                     RK_U32 hor_stride, RK_U32 ver_stride,
                                     RK_U8 y, RK_U8 u, RK_U8 v)
{
    RK_U32 i, j;
    RK_U8 *data = buf;
    
    /* Fill Y plane - each pixel gets a Y value */
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++)
            data[j] = y;
        data += hor_stride;
    }
    
    /* Fill UV planes (NV12: semi-planar format)
     * UV plane contains interleaved U and V values
     * Each 2x2 block of Y pixels shares one U and one V value
     */
    RK_U32 uv_height = height / 2;
    RK_U32 uv_width = width / 2;
    
    data = buf + hor_stride * ver_stride;
    
    for (i = 0; i < uv_height; i++) {
        /* Each row in UV plane has width bytes (interleaved UVUV...) */
        for (j = 0; j < uv_width; j++) {
            data[2 * j] = u;
            data[2 * j + 1] = v;
        }
        /* UV plane uses same stride as Y plane */
        data += hor_stride;
    }
}

static RK_S32 h265_encoder_init(H265EncoderCtx *ctx, RK_U32 width, RK_U32 height)
{
    MPP_RET ret = MPP_OK;
    
    ctx->width = width;
    ctx->height = height;
    ctx->fmt = MPP_FMT_YUV420SP;
    
    /* Calculate strides */
    RK_U32 hor_stride = MPP_ALIGN(width, 16);
    RK_U32 ver_stride = MPP_ALIGN(height, 16);
    RK_U32 frame_size = hor_stride * ver_stride * 3 / 2;
    
    printf("encoder init w %d h %d stride %d x %d frame_size %d\n",
            width, height, hor_stride, ver_stride, frame_size);
    
    /* Create encoder context */
    ret = mpp_create(&ctx->ctx, &ctx->mpi);
    if (ret) {
        fprintf(stderr, "mpp_create failed ret %d\n", ret);
        return ret;
    }
    
    /* Initialize as encoder for H.265 */
    ret = mpp_init(ctx->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
    if (ret) {
        fprintf(stderr, "mpp_init failed ret %d\n", ret);
        return ret;
    }
    
    printf("encoder initialized\n");
    
    /* Get default config */
    MppEncCfg cfg = NULL;
    ret = mpp_enc_cfg_init(&cfg);
    if (ret) {
        fprintf(stderr, "mpp_enc_cfg_init failed ret %d\n", ret);
        return ret;
    }
    
    /* Set output timeout to block mode */
    RK_U32 timeout = MPP_POLL_BLOCK;
    ret = ctx->mpi->control(ctx->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ret) {
        fprintf(stderr, "set output timeout failed ret %d\n", ret);
        return ret;
    }
    
    /* Get current config */
    ret = ctx->mpi->control(ctx->ctx, MPP_ENC_GET_CFG, cfg);
    if (ret) {
        fprintf(stderr, "get enc cfg failed ret %d\n", ret);
        return ret;
    }
    
    /* Set encoding parameters */
    mpp_enc_cfg_set_u32(cfg, "prep:width", width);
    mpp_enc_cfg_set_u32(cfg, "prep:height", height);
    mpp_enc_cfg_set_u32(cfg, "prep:hor_stride", hor_stride);
    mpp_enc_cfg_set_u32(cfg, "prep:ver_stride", ver_stride);
    mpp_enc_cfg_set_u32(cfg, "prep:fmt", ctx->fmt);
    
    /* Set RC mode and bit rate */
    mpp_enc_cfg_set_u32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_u32(cfg, "rc:bps", width * height * 30 / 2); /* 0.5 bits per pixel */
    mpp_enc_cfg_set_u32(cfg, "rc:fps_in_num", 30);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_in_den", 1);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_out_num", 30);
    mpp_enc_cfg_set_u32(cfg, "rc:fps_out_den", 1);
    
    /* Set GOP size */
    mpp_enc_cfg_set_u32(cfg, "gop:gop_len", 30);
    
    /* Apply config */
    ret = ctx->mpi->control(ctx->ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        fprintf(stderr, "set enc cfg failed ret %d\n", ret);
        return ret;
    }
    
    mpp_enc_cfg_deinit(cfg);
    
    /* Allocate buffers */
    MppBufferGroup buf_grp = NULL;
    ret = mpp_buffer_group_get_internal(&buf_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
    if (ret) {
        fprintf(stderr, "failed to get mpp buffer group ret %d\n", ret);
        return ret;
    }
    
    ret = mpp_buffer_get(buf_grp, &ctx->frm_buf, frame_size);
    if (ret) {
        fprintf(stderr, "failed to get frame buffer ret %d\n", ret);
        return ret;
    }
    
    ret = mpp_buffer_get(buf_grp, &ctx->pkt_buf, frame_size);
    if (ret) {
        fprintf(stderr, "failed to get packet buffer ret %d\n", ret);
        return ret;
    }
    
    mpp_buffer_group_put(buf_grp);
    
    printf("encoder initialized successfully\n");
    return MPP_OK;
}

static RK_S32 h265_encode_frame(H265EncoderCtx *ctx,
                                RK_U8 y, RK_U8 u, RK_U8 v,
                                RK_U32 frame_id)
{
    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    RK_U32 hor_stride = MPP_ALIGN(ctx->width, 16);
    RK_U32 ver_stride = MPP_ALIGN(ctx->height, 16);
    
    /* Generate solid color YUV data */
    void *buf = mpp_buffer_get_ptr(ctx->frm_buf);
    mpp_buffer_sync_begin(ctx->frm_buf);
    generate_solid_color_yuv(buf, ctx->width, ctx->height,
                            hor_stride, ver_stride, y, u, v);
    mpp_buffer_sync_end(ctx->frm_buf);
    
    /* Create frame and set properties */
    ret = mpp_frame_init(&frame);
    if (ret) {
        fprintf(stderr, "mpp_frame_init failed ret %d\n", ret);
        return ret;
    }
    
    mpp_frame_set_width(frame, ctx->width);
    mpp_frame_set_height(frame, ctx->height);
    mpp_frame_set_hor_stride(frame, hor_stride);
    mpp_frame_set_ver_stride(frame, ver_stride);
    mpp_frame_set_fmt(frame, ctx->fmt);
    mpp_frame_set_eos(frame, (frame_id == 0) ? 0 : 0); /* Last frame will set EOS */
    mpp_frame_set_buffer(frame, ctx->frm_buf);
    
    /* Put frame to encoder */
    ret = ctx->mpi->encode_put_frame(ctx->ctx, frame);
    if (ret) {
        fprintf(stderr, "encode put frame failed ret %d\n", ret);
        mpp_frame_deinit(&frame);
        return ret;
    }
    
    mpp_frame_deinit(&frame);
    
    /* Get encoded packet */
    ret = ctx->mpi->encode_get_packet(ctx->ctx, &packet);
    if (ret) {
        fprintf(stderr, "encode get packet failed ret %d\n", ret);
        return ret;
    }
    
    if (packet) {
        void *ptr = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);
        
        printf("frame %d encoded, packet size %zu bytes\n", frame_id, len);
        
        if (ctx->fp_output)
            fwrite(ptr, 1, len, ctx->fp_output);
        
        mpp_packet_deinit(&packet);
    }
    
    return ret;
}

void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -w WIDTH      Video width (default: 1280)\n");
    printf("  -h HEIGHT     Video height (default: 720)\n");
    printf("  -n FRAMES     Number of frames (default: 100)\n");
    printf("  -o OUTPUT     Output file (default: output.h265)\n");
    printf("  -c COLOR      Color (default: gray)\n");
    printf("               Colors: red, green, blue, gray, white, black\n");
    printf("  -y Y -u U -v V  Custom YUV color values\n");
    printf("\nExample:\n");
    printf("  %s -w 1920 -h 1080 -n 200 -o red_video.h265 -c red\n", prog);
}

int main(int argc, char **argv)
{
    H265EncoderCtx ctx;
    RK_S32 ret = 0;
    RK_U32 width = 1280;
    RK_U32 height = 720;
    RK_U32 frame_count = 100;
    const char *output_file = "output.h265";
    const char *color_name = "cyan";
    
    /* Color presets in YUV420 (ITU-R BT.601) */
    struct {
        const char *name;
        RK_U8 y, u, v;
    } color_presets[] = {
        {"black",    16,  128,  128},    /* RGB(0, 0, 0) */
        {"white",   235,  128,  128},    /* RGB(255, 255, 255) */
        {"red",      76,   85,  255},    /* RGB(255, 0, 0) */
        {"green",   149,   43,   21},    /* RGB(0, 255, 0) */
        {"blue",     29,  255,  107},    /* RGB(0, 0, 255) */
        {"yellow",  225,   16,  149},    /* RGB(255, 255, 0) */
        {"cyan",    178,  170,   15},    /* RGB(0, 255, 255) */
        {"magenta", 105,  212,  234},    /* RGB(255, 0, 255) */
        {"gray",    128,  128,  128},    /* RGB(128, 128, 128) */
        {NULL, 0, 0, 0}
    };
    
    RK_U8 color_y = 128, color_u = 128, color_v = 128;
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && i + 1 < argc) {
            switch (argv[i][1]) {
                case 'w':
                    width = atoi(argv[++i]);
                    break;
                case 'h':
                    if (argv[i][2] == '\0') {  /* -h HEIGHT */
                        height = atoi(argv[++i]);
                    } else {  /* --help or -h */
                        print_usage(argv[0]);
                        return 0;
                    }
                    break;
                case 'n':
                    frame_count = atoi(argv[++i]);
                    break;
                case 'o':
                    output_file = argv[++i];
                    break;
                case 'c':
                    color_name = argv[++i];
                    break;
                case 'y':
                    color_y = atoi(argv[++i]);
                    break;
                case 'u':
                    color_u = atoi(argv[++i]);
                    break;
                case 'v':
                    color_v = atoi(argv[++i]);
                    break;
                default:
                    fprintf(stderr, "Unknown option: %s\n", argv[i]);
                    print_usage(argv[0]);
                    return -1;
            }
        }
    }
    
    /* Look up color preset */
    for (int i = 0; color_presets[i].name != NULL; i++) {
        if (strcmp(color_presets[i].name, color_name) == 0) {
            color_y = color_presets[i].y;
            color_u = color_presets[i].u;
            color_v = color_presets[i].v;
            break;
        }
    }
    
    printf("=== H.265 Encoder - Single Color Video ===\n");
    printf("Resolution: %d x %d\n", width, height);
    printf("Frames: %d\n", frame_count);
    printf("Color: %s (Y=%d, U=%d, V=%d)\n", color_name, color_y, color_u, color_v);
    printf("Output: %s\n\n", output_file);
    
    memset(&ctx, 0, sizeof(ctx));
    
    /* Initialize encoder */
    ret = h265_encoder_init(&ctx, width, height);
    if (ret) {
        fprintf(stderr, "encoder init failed\n");
        return -1;
    }
    
    /* Open output file */
    ctx.fp_output = fopen(output_file, "wb");
    if (!ctx.fp_output) {
        fprintf(stderr, "failed to open output file %s\n", output_file);
        return -1;
    }
    
    printf("encoding %d frames of %s...\n", frame_count, color_name);
    
    /* Encode frames with single color */
    for (RK_U32 i = 0; i < frame_count; i++) {
        ret = h265_encode_frame(&ctx, color_y, color_u, color_v, i);
        if (ret) {
            fprintf(stderr, "encode frame %d failed\n", i);
            break;
        }
        
        /* Progress indicator */
        if ((i + 1) % 10 == 0) {
            printf("  encoded %d frames...\n", i + 1);
        }
    }
    
    /* Flush encoder */
    printf("flushing encoder...\n");
    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_eos(frame, 1);
    ctx.mpi->encode_put_frame(ctx.ctx, frame);
    mpp_frame_deinit(&frame);
    
    /* Get remaining packets with timeout */
    MppPacket packet = NULL;
    RK_S32 timeout_count = 0;
    RK_S32 max_timeouts = 10;
    
    while (timeout_count < max_timeouts) {
        ret = ctx.mpi->encode_get_packet(ctx.ctx, &packet);
        if (ret) {
            timeout_count++;
            continue;
        }
        
        timeout_count = 0;
        
        if (packet) {
            void *ptr = mpp_packet_get_pos(packet);
            size_t len = mpp_packet_get_length(packet);
            
            if (len > 0 && ctx.fp_output)
                fwrite(ptr, 1, len, ctx.fp_output);
            
            if (mpp_packet_get_eos(packet)) {
                printf("received EOS, flushing complete\n");
                mpp_packet_deinit(&packet);
                break;
            }
            
            mpp_packet_deinit(&packet);
        }
    }
    
    /* Cleanup */
    if (ctx.fp_output)
        fclose(ctx.fp_output);
    
    if (ctx.frm_buf)
        mpp_buffer_put(ctx.frm_buf);
    
    if (ctx.pkt_buf)
        mpp_buffer_put(ctx.pkt_buf);
    
    if (ctx.ctx) {
        ctx.mpi->reset(ctx.ctx);
        mpp_destroy(ctx.ctx);
    }
    
    printf("encoding completed, output saved to %s\n", output_file);
    
    return 0;
}
