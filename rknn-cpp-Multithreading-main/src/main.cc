#include <stdio.h>
#include <memory>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "rkYolov5s.hpp"
#include "rknnPool.hpp"

// H.265 encoding
#include <rk_mpi.h>

// FFmpeg C library - needs extern "C" for C++ compilation
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

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

/* Convert OpenCV Mat (BGR) to YUV420SP (NV12) */
static void bgr_to_yuv420sp(const cv::Mat& bgr, RK_U8 *yuv, RK_U32 hor_stride, RK_U32 ver_stride)
{
    RK_U32 width = bgr.cols;
    RK_U32 height = bgr.rows;

    cv::Mat i420;
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);

    const RK_U8 *src_y = i420.ptr<RK_U8>(0);
    const RK_U8 *src_u = src_y + width * height;
    const RK_U8 *src_v = src_u + (width * height) / 4;

    RK_U8 *y_ptr = yuv;
    for (RK_U32 i = 0; i < height; i++) {
        memcpy(y_ptr, src_y + i * width, width);
        if (hor_stride > width)
            memset(y_ptr + width, 0, hor_stride - width);
        y_ptr += hor_stride;
    }

    RK_U8 *uv_ptr = yuv + hor_stride * ver_stride;
    for (RK_U32 i = 0; i < height / 2; i++) {
        for (RK_U32 j = 0; j < width / 2; j++) {
            uv_ptr[2 * j] = src_u[i * (width / 2) + j];
            uv_ptr[2 * j + 1] = src_v[i * (width / 2) + j];
        }
        if (hor_stride > width)
            memset(uv_ptr + width, 0, hor_stride - width);
        uv_ptr += hor_stride;
    }
}

static int write_video_packet(AVFormatContext *ofmt_ctx, AVStream *out_stream,
                              RK_U8 *pkt_data, RK_U32 pkt_len,
                              RK_U32 frame_index, double fps, int key_frame)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "alloc packet failed\n");
        return -1;
    }

    uint8_t *data = (uint8_t *)av_malloc(pkt_len);
    if (!data) {
        av_packet_free(&pkt);
        fprintf(stderr, "malloc packet data failed\n");
        return -1;
    }

    memcpy(data, pkt_data, pkt_len);

    pkt->data = data;
    pkt->size = pkt_len;
    pkt->stream_index = 0;
    pkt->pts = frame_index;
    pkt->dts = frame_index;
    pkt->duration = 1;
    if (key_frame)
        pkt->flags |= AV_PKT_FLAG_KEY;

    AVRational internal_tb = {1, (int)fps};
    av_packet_rescale_ts(pkt, internal_tb, out_stream->time_base);

    if (av_interleaved_write_frame(ofmt_ctx, pkt) < 0) {
        fprintf(stderr, "write frame failed\n");
        av_packet_free(&pkt);
        return -1;
    }

    av_packet_free(&pkt);
    return 0;
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
    
    /* Rate control - 30 fps */
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
    
    printf("H.265 Encoder initialized: %dx%d, stride: %dx%d\n", width, height, hor_stride, ver_stride);
    return 0;
}

/* Encode one frame and return H.265 packet */
static int encode_frame(EncoderCtx *ctx, const cv::Mat& mat,
                       RK_U8 **out_buf, RK_U32 *out_len)
{
    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    RK_U32 hor_stride = MPP_ALIGN(ctx->width, 16);
    RK_U32 ver_stride = MPP_ALIGN(ctx->height, 16);
    
    *out_buf = NULL;
    *out_len = 0;
    
    /* Convert Mat to YUV420SP */
    void *buf = mpp_buffer_get_ptr(ctx->frm_buf);
    mpp_buffer_sync_begin(ctx->frm_buf);
    cv::Mat resized;
    if (mat.cols != (int)ctx->width || mat.rows != (int)ctx->height) {
        cv::resize(mat, resized, cv::Size(ctx->width, ctx->height));
    } else {
        resized = mat;
    }
    bgr_to_yuv420sp(resized, (RK_U8*)buf, hor_stride, ver_stride);
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
    mpp_frame_set_fmt(frame, (MppFrameFormat)ctx->fmt);
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
    printf("Usage: %s <rknn model> <video/camera> [output.mp4]\n", prog);
    printf("Examples:\n");
    printf("  %s model.rknn input.mp4              # Record with output.mp4\n", prog);
    printf("  %s model.rknn input.mp4 output.mp4   # Record with custom output\n", prog);
    printf("  %s model.rknn 0                      # Record from camera 0\n", prog);
}
int main(int argc, char **argv)
{
    char *model_name = NULL;
    if (argc < 3)
    {
        print_usage(argv[0]);
        return -1;
    }
    
    model_name = (char *)argv[1];
    char *vedio_name = argv[2];
    const char *output_file = argc >= 4 ? argv[3] : "output.mp4";

    /* Initialize encoder */
    EncoderCtx enc_ctx;
    memset(&enc_ctx, 0, sizeof(enc_ctx));
    
    /* Get video properties to determine frame size */
    cv::VideoCapture capture;
    if (strlen(vedio_name) == 1)
        capture.open((int)(vedio_name[0] - '0'));
    else
        capture.open(vedio_name);
    
    if (!capture.isOpened()) {
        printf("Failed to open video/camera: %s\n", vedio_name);
        return -1;
    }
    
    RK_U32 frame_width = (RK_U32)capture.get(cv::CAP_PROP_FRAME_WIDTH);
    RK_U32 frame_height = (RK_U32)capture.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps = capture.get(cv::CAP_PROP_FPS);
    if (fps <= 0 || fps > 120) fps = 30;  /* Default to 30 if invalid */
    bool enable_display = getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL;
    
    printf("\n=== YOLO v5 Real-time MP4 Recording ===\n");
    printf("Model: %s\n", model_name);
    printf("Input: %s (%dx%d @ %.1f fps)\n", vedio_name, frame_width, frame_height, fps);
    printf("Output: %s\n", output_file);
    printf("Display: %s\n", enable_display ? "enabled" : "disabled (headless)");
    
    if (encoder_init(&enc_ctx, frame_width, frame_height) < 0) {
        fprintf(stderr, "encoder init failed\n");
        return -1;
    }

    /* Initialize RKNN thread pool */
    int threadNum = 3;
    rknnPool<rkYolov5s, cv::Mat, cv::Mat> testPool(model_name, threadNum);
    if (testPool.init() != 0)
    {
        printf("rknnPool init fail!\n");
        return -1;
    }

    if (enable_display)
        cv::namedWindow("Camera FPS", cv::WINDOW_AUTOSIZE);

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
    out_stream->codecpar->width = frame_width;
    out_stream->codecpar->height = frame_height;
    out_stream->codecpar->codec_tag = 0;
    
    /* Set time base */
    out_stream->time_base.num = 1;
    out_stream->time_base.den = (int)fps;
    out_stream->r_frame_rate.num = (int)fps;
    out_stream->r_frame_rate.den = 1;
    out_stream->avg_frame_rate.num = (int)fps;
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

    struct timeval time;
    gettimeofday(&time, nullptr);
    auto startTime = time.tv_sec * 1000 + time.tv_usec / 1000;

    RK_U32 frames = 0;
    RK_U32 submitted_frames = 0;
    auto beforeTime = startTime;
    
    printf("Recording... (processing video and encoding to MP4)\n");
    
    while (capture.isOpened())
    {
        cv::Mat input_img;
        if (capture.read(input_img) == false)
            break;
        if (testPool.put(input_img) != 0)
            break;
        submitted_frames++;

        if (submitted_frames < (RK_U32)threadNum)
            continue;

        cv::Mat img;
        if (testPool.get(img) != 0)
            break;

        if (enable_display) {
            cv::imshow("Camera FPS", img);
            if (cv::waitKey(1) == 'q') {
                break;
            }
        }
        
        /* Encode frame to H.265 */
        RK_U8 *pkt_data = NULL;
        RK_U32 pkt_len = 0;
        
        if (encode_frame(&enc_ctx, img, &pkt_data, &pkt_len) < 0) {
            fprintf(stderr, "encode frame %d failed\n", frames);
            break;
        }
        
        if (pkt_len > 0 && write_video_packet(ofmt_ctx, out_stream, pkt_data, pkt_len,
                                              frames, fps, frames == 0 || frames % 30 == 0) < 0)
            break;
        
        frames++;
        
        if (frames % 120 == 0)
        {
            gettimeofday(&time, nullptr);
            auto currentTime = time.tv_sec * 1000 + time.tv_usec / 1000;
            printf("[%5d frames] FPS: %.2f\n", frames, 120.0 / float(currentTime - beforeTime) * 1000.0);
            beforeTime = currentTime;
        }
    }

    /* Drain remaining frames from thread pool */
    printf("\nDraining thread pool...\n");
    while (true)
    {
        cv::Mat img;
        if (testPool.get(img) != 0)
            break;

        if (enable_display) {
            cv::imshow("Camera FPS", img);
            if (cv::waitKey(1) == 'q') {
                break;
            }
        }
        
        /* Encode remaining frames */
        RK_U8 *pkt_data = NULL;
        RK_U32 pkt_len = 0;
        
        if (encode_frame(&enc_ctx, img, &pkt_data, &pkt_len) < 0) {
            break;
        }
        
        if (pkt_len > 0 && write_video_packet(ofmt_ctx, out_stream, pkt_data, pkt_len,
                                              frames, fps, frames % 30 == 0) < 0)
            break;
        
        frames++;
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
                if (write_video_packet(ofmt_ctx, out_stream, ptr, (RK_U32)len,
                                       frames, fps, 0) < 0) {
                    mpp_packet_deinit(&packet);
                    break;
                }
            }
            
            if (mpp_packet_get_eos(packet)) {
                printf("Encoder flushed\n");
                mpp_packet_deinit(&packet);
                break;
            }
            
            mpp_packet_deinit(&packet);
        }
    }

    /* Write trailer */
    av_write_trailer(ofmt_ctx);

    gettimeofday(&time, nullptr);
    auto endTime = time.tv_sec * 1000 + time.tv_usec / 1000;

    printf("\n✅ Recording Complete!\n");
    printf("Total frames: %d\n", frames);
    printf("Average FPS: %.2f\n", float(frames) / float(endTime - startTime) * 1000.0);
    printf("Output file: %s\n\n", output_file);

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
    if (enable_display)
        cv::destroyAllWindows();

    return 0;
}