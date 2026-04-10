#ifndef JCAVCODECHANDLER_H
#define JCAVCODECHANDLER_H

#include <QObject>
#include <mutex>
#include <iostream>
#include <atomic>
#include <vector>
#include "JCDataDefine.h"
#include "CCYUVDataDefine.h"
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

enum MediaPlayStatus {
    MEDIAPLAY_STATUS_PLAYING = 0,
    MEDIAPLAY_STATUS_PAUSE,
    MEDIAPLAY_STATUS_SEEK,
    MEDIAPLAY_STATUS_STOP
};

typedef void (*UpdateVideo2GUI_Callback)(H264YUV_Frame* yuv, unsigned long userData);



class JCAVCodecHandler
{
public:
    explicit JCAVCodecHandler();
    virtual ~JCAVCodecHandler();

    void SetVideoFilePath(const QString& path);
    QString GetVideoFilePath();

    QSize GetMediaWidthHeight();

    int InitVideoStream();
    void unInitVideoStream();

    void StartPlayVideo();
    void PausePlayVideo();
    void StopPlayVideo();
    void SeekMedia(float nPos);

    void SetupUpdateVideoCallback(UpdateVideo2GUI_Callback callback, unsigned long userData);

private:
    // 渲染与转换
    void convertAndRenderVideo(AVFrame* decodeFrame, long long videoPts);
    void convertAndPlayAudio(AVFrame* decodeFrame);

    // 线程控制
    void StartMediaProcessThread();
    void StopMediaProcessThread();

    // 线程入口
    void doReadMediaFrameThread();
    void doVideoDecodeThread();
    void doAudioDecodeThread();

    // 工具函数
    void stdThreadSleep(int milliseconds);
    void readMediaPacket();
    void freePacket(AVPacket* packet);
    void copyDecodeFrame420(uint8_t* src, uint8_t* dst, int linesize, int width, int height);

    // 音视频同步
    float getAudioTimeStampFromPTS(qint64 pts);
    float getVideoTimeStampFromPTS(qint64 pts);
    void updateAudioClock(int64_t pts);
    int syncVideoByAudioClock(int64_t videoPts);

private:
    // 路径与尺寸
    QString m_videoPathString;
    int     m_videoWidth  = 0;
    int     m_videoHeight = 0;

    // FFmpeg 核心上下文
    AVFormatContext*    m_pFormatCtx     = NULL;
    AVCodecContext*     m_pVideoCodecCtx = NULL;
    AVCodecContext*     m_pAudioCodecCtx = NULL;

    int                 m_videoStreamIdx = -1;
    int                 m_audioStreamIdx = -1;
    AVRational          m_videoRational  = {0, 0};
    AVRational          m_audioRational  = {0, 0};

    // 数据包队列
    JCTSQueue<AVPacket*> m_videoPacketQueue;
    JCTSQueue<AVPacket*> m_audioPacketQueue;

    // 播放状态
    MediaPlayStatus      PlayStatus = MEDIAPLAY_STATUS_STOP;
    bool                 m_bReadFileEOF     = false;
    bool                 m_hasLoggedEOF     = false;
    float                m_nSeekingPos      = 0.0f;

    // 帧与转换
    AVFrame*             m_pYUVFrame      = NULL;
    AVFrame*             m_pVideoFrame    = NULL;
    AVFrame*             m_pAudioFrame    = NULL;
    SwrContext*          m_pAudioSwrCtx   = NULL;
    SwsContext*          m_pVideoSwsCtx   = NULL;
    uint8_t*             m_pYUV420Buffer  = NULL;

    // 音频参数
    int                  m_sampleRate     = 44100;
    int                  m_sampleSize     = 16;
    int                  m_channel        = 2;
    int                  m_volume         = 100;
    uint8_t*             m_pSwrBuffer     = NULL;
    int                  m_swrBufferSize  = 0;

    // 音视频同步时钟
    float                m_nCurrAudioTimeStamp = 0.0f;

    // 线程
    std::thread          m_readThread;
    std::thread          m_videoThread;
    std::thread          m_audioThread;

    // 线程锁
    std::mutex           m_yuvFrameMutex;

    // 视频回调
    unsigned long        m_userdataVideo      = 0;
    UpdateVideo2GUI_Callback m_updateVideoCallback = NULL;
};

#endif // JCAVCODECHANDLER_H
