#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include "fifo_h265_to_mp4.h"

#define DEFAULT_INPUT_FIFO "/tmp/live_mp4.h265"
#define DEFAULT_OUTPUT_MP4 "/tmp/live_output.mp4"
#define DEFAULT_FPS 15
#define OPEN_RETRY_US 500000

static volatile sig_atomic_t g_running = 1;

#define INPUT_PROBE_SIZE "1048576"
#define INPUT_ANALYZE_DURATION "1000000"
#define INPUT_FPS_PROBE_SIZE "8"

static int find_start_code(const uint8_t *data, int size, int *offset, int *len)
{
    int i;

    for (i = 0; i + 3 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *offset = i;
                *len = 3;
                return 1;
            }
            if (i + 4 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *offset = i;
                *len = 4;
                return 1;
            }
        }
    }

    return 0;
}

static int packet_has_irap(const AVPacket *pkt)
{
    int pos = 0;
    int sc_len = 0;

    if (!pkt || !pkt->data || pkt->size < 6) {
        return 0;
    }

    if (!find_start_code(pkt->data, pkt->size, &pos, &sc_len)) {
        return 0;
    }

    while (pos + sc_len + 1 < pkt->size) {
        int nal_type = (pkt->data[pos + sc_len] & 0x7E) >> 1;
        int next_rel = 0;
        int next_sc_len = 0;
        if (nal_type >= 16 && nal_type <= 23) {
            return 1;
        }

        if (!find_start_code(pkt->data + pos + sc_len,
                             pkt->size - pos - sc_len,
                             &next_rel, &next_sc_len)) {
            break;
        }

        pos += sc_len + next_rel;
        sc_len = next_sc_len;
    }

    return 0;
}

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int mp4_muxer_entry(int argc, char *argv[])
{
    const char *in_filename = (argc > 1) ? argv[1] : DEFAULT_INPUT_FIFO;
    const char *out_filename = (argc > 2) ? argv[2] : DEFAULT_OUTPUT_MP4;
    int fps = (argc > 3) ? atoi(argv[3]) : DEFAULT_FPS;

    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    const AVInputFormat *hevc_fmt = NULL;
    AVStream *in_stream = NULL;
    AVStream *out_stream = NULL;
    AVBSFContext *bsf_ctx = NULL;
    AVDictionary *input_opts = NULL;
    AVPacket pkt;
    AVPacket filtered_pkt;
    AVPacket cached_pkt;
    int have_cached_pkt = 0;
    int started_writing = 0;
    int ret = 0;
    int64_t pts = 0;
    int64_t probe_start_us = 0;
    int64_t probe_elapsed_us = 0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("mp4 muxer input=%s output=%s fps=%d\n", in_filename, out_filename, fps);

    // 寻找 hevc 解封装器，直接打开 fifo 时 ffmpeg 可能无法自动识别格式
    hevc_fmt = av_find_input_format("hevc");
    if (!hevc_fmt) {
        fprintf(stderr, "cannot find hevc input format\n");
        return -1;
    }

    av_dict_set(&input_opts, "probesize", INPUT_PROBE_SIZE, 0);
    av_dict_set(&input_opts, "analyzeduration", INPUT_ANALYZE_DURATION, 0);
    av_dict_set(&input_opts, "fpsprobesize", INPUT_FPS_PROBE_SIZE, 0);
    av_dict_set(&input_opts, "fflags", "nobuffer", 0);
    av_dict_set(&input_opts, "avioflags", "direct", 0);

    while (g_running) {
        ret = avformat_open_input(&ifmt_ctx, in_filename, hevc_fmt, &input_opts);
        if (ret >= 0) {
            break;
        }

        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "open input retry: %s\n", errbuf);
        }
        usleep(OPEN_RETRY_US);
    }

    if (ret < 0) {
        return -1;
    }

    printf("input opened: %s\n", in_filename);
    
    probe_start_us = av_gettime_relative();
    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    probe_elapsed_us = av_gettime_relative() - probe_start_us;
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "find stream info failed after %.3f s: %s\n",
                probe_elapsed_us / 1000000.0, errbuf);
        goto out;
    }
    printf("stream info ready in %.3f s\n", probe_elapsed_us / 1000000.0);
    
    if (!ifmt_ctx->nb_streams) {
        fprintf(stderr, "no input streams found\n");
        ret = -1;
        goto out;
    }

    in_stream = ifmt_ctx->streams[0];
    printf("mux stream codec=%s size=%dx%d\n",
           avcodec_get_name(in_stream->codecpar->codec_id),
           in_stream->codecpar->width,
           in_stream->codecpar->height);
    if (in_stream->codecpar->width <= 0 || in_stream->codecpar->height <= 0) {
        fprintf(stderr, "stream parameters still incomplete after probe; width=%d height=%d\n",
                in_stream->codecpar->width, in_stream->codecpar->height);
        ret = -1;
        goto out;
    }

    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (ret < 0 || !ofmt_ctx) {
        fprintf(stderr, "alloc output context failed\n");
        ret = -1;
        goto out;
    }

    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) {
        fprintf(stderr, "create output stream failed\n");
        ret = -1;
        goto out;
    }

    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
        fprintf(stderr, "copy codec parameters failed\n");
        goto out;
    }
    out_stream->codecpar->codec_tag = 0;
    out_stream->time_base = (AVRational){1, fps};
    in_stream->time_base = (AVRational){1, fps};

    // extract_extradata 过滤器
    {
        const AVBitStreamFilter *extract_bsf = av_bsf_get_by_name("extract_extradata");
        if (!extract_bsf) {
            fprintf(stderr, "cannot find extract_extradata bitstream filter\n");
            ret = -1;
            goto out;
        }

        ret = av_bsf_alloc(extract_bsf, &bsf_ctx);
        if (ret < 0 || !bsf_ctx) {
            fprintf(stderr, "alloc extract_extradata bsf failed\n");
            goto out;
        }

        ret = avcodec_parameters_copy(bsf_ctx->par_in, in_stream->codecpar);
        if (ret < 0) {
            fprintf(stderr, "copy bsf input parameters failed\n");
            goto out;
        }
        bsf_ctx->time_base_in = in_stream->time_base;

        ret = av_bsf_init(bsf_ctx);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "init extract_extradata bsf failed: %s\n", errbuf);
            goto out;
        }

    }

    memset(&pkt, 0, sizeof(pkt));
    memset(&filtered_pkt, 0, sizeof(filtered_pkt));
    memset(&cached_pkt, 0, sizeof(cached_pkt));

    while (g_running && !have_cached_pkt) {
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "read first hevc packet failed: %s\n", errbuf);
            goto out;
        }

        if (pkt.stream_index != 0) {
            av_packet_unref(&pkt);
            continue;
        }

        pkt.pts = pts;
        pkt.dts = pts;
        pkt.duration = 1;
        pts++;

        av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
        pkt.stream_index = 0;

        ret = av_bsf_send_packet(bsf_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "send first packet to extract_extradata failed: %s\n", errbuf);
            goto out;
        }

        while ((ret = av_bsf_receive_packet(bsf_ctx, &filtered_pkt)) == 0) {
            ret = av_packet_ref(&cached_pkt, &filtered_pkt);
            av_packet_unref(&filtered_pkt);
            if (ret < 0) {
                fprintf(stderr, "cache first filtered packet failed\n");
                goto out;
            }
            have_cached_pkt = 1;
            break;
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "receive first packet from extract_extradata failed: %s\n", errbuf);
            goto out;
        }
    }

    ret = avcodec_parameters_copy(out_stream->codecpar, bsf_ctx->par_out);
    if (ret < 0) {
        fprintf(stderr, "copy bsf output parameters failed\n");
        goto out;
    }
    out_stream->codecpar->codec_tag = 0;
    if (out_stream->codecpar->extradata_size <= 0) {
        fprintf(stderr, "hevc extradata not extracted before mp4 header write\n");
        ret = -1;
        goto out;
    }

    ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "open output failed: %s\n", errbuf);
        goto out;
    }

    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "write header failed: %s\n", errbuf);
        goto out;
    }

    if (have_cached_pkt) {
        if (!packet_has_irap(&cached_pkt)) {
            av_packet_unref(&cached_pkt);
            have_cached_pkt = 0;
        } else {
            cached_pkt.flags |= AV_PKT_FLAG_KEY;
            started_writing = 1;
        }
    }

    if (have_cached_pkt) {
        cached_pkt.stream_index = 0;
        ret = av_interleaved_write_frame(ofmt_ctx, &cached_pkt);
        av_packet_unref(&cached_pkt);
        have_cached_pkt = 0;
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "write first cached mp4 frame failed: %s\n", errbuf);
            goto out;
        }
    }

    while (g_running && (ret = av_read_frame(ifmt_ctx, &pkt)) >= 0) {
        if (pkt.stream_index != 0) {
            av_packet_unref(&pkt);
            continue;
        }

        pkt.pts = pts;
        pkt.dts = pts;
        pkt.duration = 1;
        pts++;

        av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
        pkt.stream_index = 0;

        ret = av_bsf_send_packet(bsf_ctx, &pkt);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "send packet to extract_extradata failed: %s\n", errbuf);
            av_packet_unref(&pkt);
            goto out;
        }

        av_packet_unref(&pkt);

        while ((ret = av_bsf_receive_packet(bsf_ctx, &filtered_pkt)) == 0) {
            if (!started_writing) {
                if (!packet_has_irap(&filtered_pkt)) {
                    av_packet_unref(&filtered_pkt);
                    continue;
                }
                filtered_pkt.flags |= AV_PKT_FLAG_KEY;
                started_writing = 1;
            }
            filtered_pkt.stream_index = 0;
            ret = av_interleaved_write_frame(ofmt_ctx, &filtered_pkt);
            av_packet_unref(&filtered_pkt);
            if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "write mp4 frame failed: %s\n", errbuf);
                goto out;
            }
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "receive packet from extract_extradata failed: %s\n", errbuf);
            goto out;
        }
    }

    if (ret == AVERROR_EOF || !g_running) {
        ret = 0;
    } else if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "read fifo failed: %s\n", errbuf);
    }

    if (bsf_ctx) {
        av_bsf_send_packet(bsf_ctx, NULL);
        while ((ret = av_bsf_receive_packet(bsf_ctx, &filtered_pkt)) == 0) {
            if (!started_writing) {
                if (!packet_has_irap(&filtered_pkt)) {
                    av_packet_unref(&filtered_pkt);
                    continue;
                }
                filtered_pkt.flags |= AV_PKT_FLAG_KEY;
                started_writing = 1;
            }
            filtered_pkt.stream_index = 0;
            if (av_interleaved_write_frame(ofmt_ctx, &filtered_pkt) < 0) {
                av_packet_unref(&filtered_pkt);
                break;
            }
            av_packet_unref(&filtered_pkt);
        }
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            ret = 0;
        }
    }

    av_write_trailer(ofmt_ctx);
    printf("mp4 muxer finished: %s\n", out_filename);

out:
    av_dict_free(&input_opts);
    if (bsf_ctx) {
        av_bsf_free(&bsf_ctx);
    }
    if (ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
    }
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            avio_closep(&ofmt_ctx->pb);
        }
        avformat_free_context(ofmt_ctx);
    }
    return ret < 0 ? -1 : 0;
}
