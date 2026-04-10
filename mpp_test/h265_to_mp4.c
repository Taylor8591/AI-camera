#include <stdio.h>
#include <libavformat/avformat.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: %s <input.h265> <output.mp4>\n", argv[0]);
        return -1;
    }

    const char *in_filename  = argv[1];
    const char *out_filename = argv[2];

    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    // 存储一帧视频数据
    AVPacket pkt;

    int ret;
    int64_t pts = 0;       // 手动生成正常时间戳
    const int fps = 30;     // 你的编码帧率

    // 打开输入
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, NULL, NULL)) < 0) {
        printf("open input failed\n");
        goto end;
    }
    avformat_find_stream_info(ifmt_ctx, NULL);

    AVStream *in_stream  = ifmt_ctx->streams[0];
    // 1. 编码格式名称
    printf("编码格式      : %s\n", avcodec_get_name(in_stream->codecpar->codec_id));
    // 2. 分辨率
    printf("分辨率        : %d x %d\n", in_stream->codecpar->width, in_stream->codecpar->height);
    // 3. 码流格式类型
    printf("媒体类型      : %s\n", ifmt_ctx->iformat->name);


    // 创建输出 MP4
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);

    // 复制编码参数
    avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    out_stream->codecpar->codec_tag = 0;

    // 手动设置时间基（关键！解决裸流无时间戳问题）
    out_stream->time_base.num = 1;
    out_stream->time_base.den = fps;
    in_stream->time_base.num   = 1;
    in_stream->time_base.den   = fps;

    // 打开输出并写头
    avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
    avformat_write_header(ofmt_ctx, NULL);

    // 逐帧封装
    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index != 0) {
            av_packet_unref(&pkt);
            continue;
        }

        // 【核心修复】手动赋予正常递增时间戳
        pkt.pts = pts;
        pkt.dts = pts;
        pkt.duration = 1;
        pts++;

        // 时间戳转换
        av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
        pkt.stream_index = 0;

        // 写入 MP4
        av_interleaved_write_frame(ofmt_ctx, &pkt);
        av_packet_unref(&pkt);
    }

    // 写入文件尾
    av_write_trailer(ofmt_ctx);
    printf("\n✅ Convert success: %s -> %s\n", in_filename, out_filename);

end:
    avformat_close_input(&ifmt_ctx);
    avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    return 0;
}