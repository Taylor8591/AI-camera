#include "JCAVCodecHandler.h"
#include "CCYUVDataDefine.h"
#include "JCAudioPlayer.h"
#include <QSize>
#include <QDebug>
#include <thread>
#include <mutex>
#include <iostream>
#include <atomic>


std::atomic<bool> allThreadRunning(false);
std::atomic<bool> ReadThreadRunning(false);
std::atomic<bool> VideoThreadRunning(false);
std::atomic<bool> AudioThreadRunning(false);

#if !defined(MIN)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

JCAVCodecHandler::JCAVCodecHandler()
    : m_videoPathString(""),
      m_videoWidth(0),
      m_videoHeight(0),
      m_pFormatCtx(NULL),
      m_pVideoCodecCtx(NULL),
      m_pAudioCodecCtx(NULL)
{
    av_register_all();
}

JCAVCodecHandler::~JCAVCodecHandler()
{

}

int JCAVCodecHandler::InitVideoStream() {

    if(m_videoPathString.isEmpty()) {
        qDebug() << "video path is empty...";
        return -1;
    }

    QByteArray pathBytesArray = m_videoPathString.toLocal8Bit();
    // convert QString to const char* 
    const char* filePath = pathBytesArray.data();
    if(avformat_open_input(&m_pFormatCtx,filePath,NULL,NULL) != 0) {
        qDebug() << "Open input stream failed...";
        return -1;
    }

    if(avformat_find_stream_info(m_pFormatCtx, NULL) < 0) {
        qDebug() << "avformat_find_stream_info failed...";
        return -1;
    }
    // qDebug() << "****************************************";
    // print detailed information
    // av_dump_format(m_pFormatCtx, 0, filePath ,0);
    // qDebug() << "****************************************";
    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;

    qDebug() << "nb_streams: " << m_pFormatCtx->nb_streams;
    for(int i=0; i<(int)m_pFormatCtx->nb_streams; i++) {

        AVCodecParameters* codecParameters = m_pFormatCtx->streams[i]->codecpar;
        if(codecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIdx = i;
            m_pVideoCodecCtx = m_pFormatCtx->streams[i]->codec;
            AVCodec* VideoCodec = avcodec_find_decoder(codecParameters->codec_id);
            if(VideoCodec == NULL) {
                qDebug() << "avcodec_find_decoder failed...";
                return -1;
            }
            if(avcodec_open2(m_pVideoCodecCtx, VideoCodec, NULL) < 0) {
                qDebug() << "avcodec_open2 failed...";
                return -1;
            }
            qDebug() << "find videoStreamIdx: " << m_videoStreamIdx;
        }
            
        if(codecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audioStreamIdx = i;
            m_pAudioCodecCtx = m_pFormatCtx->streams[i]->codec;
            AVCodec* AudioCodec = avcodec_find_decoder(codecParameters->codec_id);
            if(AudioCodec == NULL) {
                qDebug() << "avcodec_find_decoder failed...";
                return -1;
            }
            if(avcodec_open2(m_pAudioCodecCtx, AudioCodec, NULL) < 0) {
                qDebug() << "avcodec_open2 failed...";
                return -1;
            }
            qDebug() << "find audioStreamIdx: " << m_audioStreamIdx;
        }
            
    }
    
    m_sampleRate = m_pAudioCodecCtx->sample_rate;
    m_channel = m_pAudioCodecCtx->channels;
    switch(m_pAudioCodecCtx->sample_fmt) {
        case AV_SAMPLE_FMT_U8:
            m_sampleSize = 8;
            break;
        case AV_SAMPLE_FMT_S16:
            m_sampleSize = 16;
            break;
        case AV_SAMPLE_FMT_S32:
            m_sampleSize = 32;
            break;
        default:
            m_sampleSize = 16;
            break;
    }
    qDebug() << "audio sample rate: " << m_sampleRate << ", audio channel: " << m_channel;
    qDebug() << "audio sample size: " << m_sampleSize;

    JCAudioPlayer::GetInstance()->SetSampleRate(m_sampleRate);
    JCAudioPlayer::GetInstance()->SetSampleSize(m_sampleSize);
    JCAudioPlayer::GetInstance()->Setchannel(m_channel);
    
    if(m_pYUVFrame == NULL) {
        m_pYUVFrame = av_frame_alloc();
    }

    if(m_pYUV420Buffer == NULL) {
        // 从FFmpeg的内存管理系统申请内存块
        m_pYUV420Buffer = (uint8_t*)av_malloc(
            // 计算存储YUV420P格式图像需要的总字节数
            av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 
                                    m_pVideoCodecCtx->width, 
                                    m_pVideoCodecCtx->height, 
                                    1)
        );
    }
    
    // 函数内部做的事：
    // data[0] = m_pYUV420Buffer + 0;           // Y平面指针
    // data[1] = m_pYUV420Buffer + 921600;      // U平面指针
    // data[2] = m_pYUV420Buffer + 1152000;     // V平面指针
    // data[3] = NULL;                          // Alpha通道（无）

    // linesize[0] = 1280;    // Y平面每行1280字节
    // linesize[1] = 640;     // U平面每行640字节
    // linesize[2] = 640;     // V平面每行640字节
    // linesize[3] = 0;       // Alpha通道（无）
    av_image_fill_arrays(m_pYUVFrame->data,                                 // 输出：四个平面指针
                     m_pYUVFrame->linesize,                                 // 输出：四个平面的linesize
                     m_pYUV420Buffer,                                       // 输入：实际的内存缓冲区
                     AV_PIX_FMT_YUV420P,                                    // 输入：像素格式（如 AV_PIX_FMT_YUV420P）
                     m_pVideoCodecCtx->width, m_pVideoCodecCtx->height, 1); // 输入：图像宽高，对齐字节数

    m_videoWidth = m_pVideoCodecCtx->width;
    m_videoHeight = m_pVideoCodecCtx->height;

    if(m_videoStreamIdx == -1) {
        qDebug() << "can't find video stream...";
    } else {
        AVStream* videoStream = m_pFormatCtx->streams[m_videoStreamIdx];
        qDebug() << "video width: " << m_videoWidth << ", video height: " << m_videoHeight;
        m_videoRational = videoStream->time_base;
        qDebug() << "AVStream Video Rational: " << m_videoRational.num << "/" << m_videoRational.den;
    }

    if(m_audioStreamIdx == -1) {
        qDebug() << "can't find audio stream...";
    } else {
        AVStream* audioStream = m_pFormatCtx->streams[m_audioStreamIdx];
        m_audioRational = audioStream->time_base;
        // 1 / 48000
        qDebug() << "AVStream Audio Rational: " << m_audioRational.num << "/" << m_audioRational.den;
        qDebug() << "audio time base: " << av_q2d(m_audioRational);
    }
    
    return 0;
}

void JCAVCodecHandler::unInitVideoStream() {

}

void JCAVCodecHandler::SetVideoFilePath(const QString& path) {

    m_videoPathString = path;
}
QString JCAVCodecHandler::GetVideoFilePath() {

    return m_videoPathString;
}

QSize JCAVCodecHandler::GetMediaWidthHeight() {

    return QSize(m_videoWidth, m_videoHeight);
}

void JCAVCodecHandler::StartPlayVideo() {

    JCAudioPlayer::GetInstance()->StartAudioPlayer();
    StartMediaProcessThread();
}

void JCAVCodecHandler::PausePlayVideo() {
    
     if(PlayStatus == MEDIAPLAY_STATUS_PLAYING) {
        PlayStatus = MEDIAPLAY_STATUS_PAUSE;
    } else if(PlayStatus == MEDIAPLAY_STATUS_PAUSE) {
        PlayStatus = MEDIAPLAY_STATUS_PLAYING;
    } else {
        PlayStatus = MEDIAPLAY_STATUS_PLAYING;
    }
}

void JCAVCodecHandler::StopPlayVideo() {

    JCAudioPlayer::GetInstance()->StopAudioPlayer();
    StopMediaProcessThread();
}

void JCAVCodecHandler::StartMediaProcessThread() {

    StopMediaProcessThread();

    m_bReadFileEOF = false;
    m_hasLoggedEOF = false;

    allThreadRunning = true;

    m_readThread  = std::thread(&JCAVCodecHandler::doReadMediaFrameThread, this);
    m_videoThread = std::thread(&JCAVCodecHandler::doVideoDecodeThread, this);
    m_audioThread = std::thread(&JCAVCodecHandler::doAudioDecodeThread, this);

    // PlayStatus = MEDIAPLAY_STATUS_PLAYING;
}

void JCAVCodecHandler::StopMediaProcessThread() {

    allThreadRunning = false;

    // 等待线程真正退出！！！（关键）
    if (m_readThread.joinable()) {
        m_readThread.join();
    }
    if (m_videoThread.joinable()) {
        m_videoThread.join();
    }
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }
}










// 读取媒体数据线程
void JCAVCodecHandler::doReadMediaFrameThread() {

    while(allThreadRunning) {

        ReadThreadRunning = true;
        if(PlayStatus == MEDIAPLAY_STATUS_PAUSE) {
            qDebug() << "media play status is pause...";
            stdThreadSleep(10);
            continue;
        }

        if(m_videoPacketQueue.size() > 600 && m_audioPacketQueue.size() > 1200) {
            stdThreadSleep(10);
            continue;
        }

        if(m_bReadFileEOF == false) {
            readMediaPacket();
        } else {
            if(!m_hasLoggedEOF) {
                qDebug() << "read media file reach EOF...";
                m_hasLoggedEOF = true;
            }
            stdThreadSleep(10);
        }
    }
}
// 读取媒体数据包，并根据数据包类型（视频或音频）将其放入相应的队列中
void JCAVCodecHandler::readMediaPacket() {
    
    AVPacket* packet = (AVPacket*)malloc(sizeof(AVPacket));
    if(packet == NULL) {
        qDebug() << "malloc AVPacket failed...";
        return;
    }

    av_init_packet(packet);

    // PlayStatus = MEDIAPLAY_STATUS_PLAYING;
    int ret = av_read_frame(m_pFormatCtx, packet);
    if(ret == 0) {
        if(packet->stream_index == m_videoStreamIdx) {
            if(!av_dup_packet(packet)) {
               m_videoPacketQueue.enqueue(packet);
            } else {
                freePacket(packet);
            }
        } 

        if(packet->stream_index == m_audioStreamIdx) {
            if(!av_dup_packet(packet)) {
                m_audioPacketQueue.enqueue(packet);
            } else {
                freePacket(packet);
            }
        }

        if(packet->stream_index != m_videoStreamIdx && packet->stream_index != m_audioStreamIdx) {
            freePacket(packet);
        }
    } else if(ret < 0) {
        if(ret == AVERROR_EOF) {
            m_bReadFileEOF = true;
        }
        freePacket(packet);
    }
}


void JCAVCodecHandler::freePacket(AVPacket* packet) {

    if(packet == NULL) {
        return;
    }

    av_free_packet(packet);
    free(packet);
}























// 视频解码线程
void JCAVCodecHandler::doVideoDecodeThread() {
    if(m_pFormatCtx == NULL) {
        return;
    }
    if(m_pVideoFrame == NULL) {
        m_pVideoFrame = av_frame_alloc();
    }

    while(allThreadRunning) {

        VideoThreadRunning = true;

        if(PlayStatus == MEDIAPLAY_STATUS_PAUSE) {
            stdThreadSleep(10);
            continue;
        }
        if(m_videoPacketQueue.isEmpty()) {
            stdThreadSleep(10);
            continue;
        }

        // 从视频数据包队列中取出一个数据包进行解码
        AVPacket* packet = m_videoPacketQueue.dequeue();
        if(packet == NULL) {
            continue;
        }

        // 送入压缩数据包
        int ret = avcodec_send_packet(m_pVideoCodecCtx, packet);
        if(ret < 0) {
            freePacket(packet);
            continue;
        }

        // 取出解码后的帧
        // m_pVideoFrame 中存储的是解码后的原始帧数据（如 H.264），需要转换为 YUV420P 格式才能渲染
        int decodeRet = avcodec_receive_frame(m_pVideoCodecCtx, m_pVideoFrame);
        if(decodeRet == 0) {
            int delay = syncVideoByAudioClock(m_pVideoFrame->pts);
            if(delay > 0) {
                stdThreadSleep(delay);
            }
            if(delay >= 0) {
                convertAndRenderVideo(m_pVideoFrame, m_pVideoFrame->pts);
            }
        }

        freePacket(packet);
    }
    qDebug() << "video decode thread exit...";
}

void JCAVCodecHandler::convertAndRenderVideo(AVFrame* decodeFrame, long long videoPts) {

    if(decodeFrame == NULL) {
        return;
    }

    if(m_pVideoSwsCtx == NULL) {
        m_pVideoSwsCtx = sws_getContext(m_pVideoCodecCtx->width, m_pVideoCodecCtx->height, 
            m_pVideoCodecCtx->pix_fmt, m_pVideoCodecCtx->width, m_pVideoCodecCtx->height, 
            AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    }

    sws_scale(m_pVideoSwsCtx,           // 转换上下文 
          decodeFrame->data,            // 输入：原始帧数据（H.264等）
          decodeFrame->linesize,        // 输入：原始帧linesize
          0, m_pVideoCodecCtx->height,
          m_pYUVFrame->data,            // 输出：YUV420P平面指针
          m_pYUVFrame->linesize);       // 输出：YUV420P linesize

    // 正确计算 YUV420P 的缓冲区大小
    unsigned int width = m_pVideoCodecCtx->width;               // 1280
    unsigned int height = m_pVideoCodecCtx->height;             // 720
    unsigned int lumaLength = width * height;                   // 1280x720
    unsigned int chromBLength = (width / 2) * (height / 2);     // 1280/2 x 720/2
    unsigned int chromRLength = (width / 2) * (height / 2);     // 1280/2 x 720/2


    H264YUV_Frame* updateYUVFrame = new H264YUV_Frame();

    updateYUVFrame->width = width;
    updateYUVFrame->height = height;
    updateYUVFrame->pts = videoPts;

    updateYUVFrame->luma.length = lumaLength;
    updateYUVFrame->chromB.length = chromBLength;
    updateYUVFrame->chromR.length = chromRLength;

    updateYUVFrame->luma.dataBuffer = (unsigned char*)malloc(lumaLength);
    updateYUVFrame->chromB.dataBuffer = (unsigned char*)malloc(chromBLength);
    updateYUVFrame->chromR.dataBuffer = (unsigned char*)malloc(chromRLength);

    // 复制时使用实际图像宽度，不是 linesize（linesize 可能包含对齐填充）
    // qDebug() << "linesize: " << m_pYUVFrame->linesize[0] << m_pYUVFrame->linesize[1] << m_pYUVFrame->linesize[2];
    copyDecodeFrame420(m_pYUVFrame->data[0], updateYUVFrame->luma.dataBuffer, m_pYUVFrame->linesize[0], width, height);
    copyDecodeFrame420(m_pYUVFrame->data[1], updateYUVFrame->chromB.dataBuffer, m_pYUVFrame->linesize[1], width / 2, height / 2);
    copyDecodeFrame420(m_pYUVFrame->data[2], updateYUVFrame->chromR.dataBuffer, m_pYUVFrame->linesize[2], width / 2, height / 2);

    // m_updateVideoCallback 是函数指针，数据类型是： void (*UpdateVideo2GUI_Callback)(H264YUV_Frame* yuv, unsigned long userData);
    if(m_updateVideoCallback) {
        // 将内存管理责任转移给回调接收方（GUI线程）
        // 接收方负责释放此内存
        m_updateVideoCallback(updateYUVFrame, m_userdataVideo);
    } else {
        // 如果没有回调，立即释放内存
        if(updateYUVFrame->luma.dataBuffer) {
            free(updateYUVFrame->luma.dataBuffer);
            updateYUVFrame->luma.dataBuffer = NULL;
        }
        if(updateYUVFrame->chromB.dataBuffer) {
            free(updateYUVFrame->chromB.dataBuffer);
            updateYUVFrame->chromB.dataBuffer = NULL;
        }
        if(updateYUVFrame->chromR.dataBuffer) {
            free(updateYUVFrame->chromR.dataBuffer);
            updateYUVFrame->chromR.dataBuffer = NULL;
        }
        if(updateYUVFrame) {
            delete updateYUVFrame;
            updateYUVFrame = NULL;
        }
    }

}

void JCAVCodecHandler::copyDecodeFrame420(uint8_t* src, uint8_t* dst, int linesize, int width, int height) {

    // 注意：linesize 可能大于 width（由于对齐填充）
    // 我们只复制实际的数据宽度，源指针按 linesize 跳过
    for(int i=0; i<height; i++) {
        memcpy(dst, src, width);         // 只复制实际的数据宽度
        src += linesize;                 // 源指针按 linesize 跳过（可能包含填充）
        dst += width;                    // 目标指针紧跟数据宽度
    }
}

























// 音频解码线程
void JCAVCodecHandler::doAudioDecodeThread() {

    if(m_pFormatCtx == NULL) {
        return;
    }

    if(m_pAudioFrame == NULL) {
        m_pAudioFrame = av_frame_alloc();
    }

    while(allThreadRunning) {
        AudioThreadRunning = true;

        if(PlayStatus == MEDIAPLAY_STATUS_PAUSE) {
            stdThreadSleep(10);
            continue;
        }
        if(m_audioPacketQueue.isEmpty()) {
            stdThreadSleep(1);
            continue;
        }

        AVPacket* packet = m_audioPacketQueue.dequeue();
        if(packet == NULL) {
            continue;
        }

        updateAudioClock(packet->pts);
        qDebug() << "audio pts: " << packet->pts << ", audio timestamp: " << m_nCurrAudioTimeStamp;

        int ret = avcodec_send_packet(m_pAudioCodecCtx, packet);
        if(ret < 0) {
            qDebug() << "avcodec_send_packet failed...";
            freePacket(packet);
            continue;
        }

        int decodeRet = avcodec_receive_frame(m_pAudioCodecCtx, m_pAudioFrame);
        if(decodeRet == 0) {
            convertAndPlayAudio(m_pAudioFrame);
        }

        freePacket(packet);
        // qDebug() << "running...";
        // stdThreadSleep(50);
    }
    qDebug() << "audio decode thread exit...";
}

void JCAVCodecHandler::convertAndPlayAudio(AVFrame* decodeFrame) {

    if(!m_pFormatCtx || !m_pAudioCodecCtx || !decodeFrame) {
        return;
    }

    if(!m_pAudioSwrCtx) {

        m_pAudioSwrCtx = swr_alloc();
        swr_alloc_set_opts(m_pAudioSwrCtx, av_get_default_channel_layout(m_channel), 
                AV_SAMPLE_FMT_S16, 
                m_sampleRate, 
                av_get_default_channel_layout(m_pAudioCodecCtx->channels), 
                m_pAudioCodecCtx->sample_fmt, 
                m_pAudioCodecCtx->sample_rate,
                0, NULL);
        if(m_pAudioSwrCtx) {
            int ret = swr_init(m_pAudioSwrCtx);
        }
    }

    int buffSize = av_samples_get_buffer_size(NULL, m_channel, decodeFrame->nb_samples, AV_SAMPLE_FMT_S16, 0);
    if(!m_pSwrBuffer || m_swrBufferSize < buffSize) {

        m_swrBufferSize = buffSize;
        m_pSwrBuffer = (uint8_t*)realloc(m_pSwrBuffer, m_swrBufferSize);
    }

    uint8_t* outBuffer[2] = {m_pSwrBuffer, 0};
    int len = swr_convert(m_pAudioSwrCtx, outBuffer, decodeFrame->nb_samples, (const uint8_t**)decodeFrame->data, decodeFrame->nb_samples);
    if(len < 0) {
        qDebug() << "swr_convert failed...";
        return;
    }

    int freeSpace = 0;
    
    while(allThreadRunning) {

        freeSpace = JCAudioPlayer::GetInstance()->GetFreeSpace();
        if(freeSpace > buffSize) {
            break;
        } else {
            stdThreadSleep(1);
        }
    }

    if(!allThreadRunning) {
        return;
    }

    JCAudioPlayer::GetInstance()->SetVolume(100);
    JCAudioPlayer::GetInstance()->WriteAudioData((const char*)m_pSwrBuffer, buffSize);
}



























float JCAVCodecHandler::getAudioTimeStampFromPTS(qint64 pts) {

    return pts * av_q2d(m_audioRational);
}

float JCAVCodecHandler::getVideoTimeStampFromPTS(qint64 pts) {

    return pts * av_q2d(m_videoRational);
}

// 音频帧到达时调用：只更新当前音频时间，不做任何等待
void JCAVCodecHandler::updateAudioClock(int64_t pts) {
    m_nCurrAudioTimeStamp = getAudioTimeStampFromPTS(pts);
}

// 视频帧显示前调用
int JCAVCodecHandler::syncVideoByAudioClock(int64_t videoPts) {
    float videoTime = getVideoTimeStampFromPTS(videoPts);
    float diff = videoTime - m_nCurrAudioTimeStamp; // 单位：秒

    int delayMs = diff * 1000;

    // 同步阈值：±50ms
    if (delayMs > 50) {
        // 视频太快，等待
        return delayMs;
    } else if (delayMs < -50) {
        // 视频太慢，丢帧
        return -1;
    } else {
        // 正常播放
        return 0;
    }
}







void JCAVCodecHandler::SeekMedia(float seekSeconds) {

    // if (seekSeconds < 0 || !m_pFormatCtx) {
    //     return;
    // }

    // StopMediaProcessThread();

    // m_bReadFileEOF = false;

    // qint64 seekPos = seekSeconds / av_q2d(m_pFormatCtx->streams[m_videoStreamIdx]->time_base);

    // av_seek_frame(m_pFormatCtx, m_videoStreamIdx, seekPos, AVSEEK_FLAG_BACKWARD);

    // // QMutexLocker locker(&m_mutex);
    // while(!m_videoPacketQueue.isEmpty())
    //     freePacket(m_videoPacketQueue.takeFirst());
    // while(!m_audioPacketQueue.isEmpty())
    //     freePacket(m_audioPacketQueue.takeFirst());

    // avformat_flush(m_pFormatCtx);

    // m_nCurrAudioTimeStamp = 0;
    // m_nLastAudioTimeStamp = 0;


    // StartMediaProcessThread();
}

void JCAVCodecHandler::SetupUpdateVideoCallback(UpdateVideo2GUI_Callback callback, unsigned long userData) {
    m_updateVideoCallback = callback;
    m_userdataVideo = userData;
}

void JCAVCodecHandler::stdThreadSleep(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
