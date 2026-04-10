#include "JCAudioPlayer.h"
#include <QMutexLocker>
#include <QDebug>

static JCAudioPlayer* m_pInstance = NULL;
static QMutex m_audioPlayMutex;

JCAudioPlayer* JCAudioPlayer::GetInstance()
{
    QMutexLocker locker(&m_audioPlayMutex);
    if(m_pInstance == NULL){
        m_pInstance = new JCAudioPlayer();
    }
    return m_pInstance;
}

JCAudioPlayer::JCAudioPlayer()
{
}

void JCAudioPlayer::StartAudioPlayer()
{
    QMutexLocker locker(&m_audioPlayMutex);

    QAudioFormat fmt;

    fmt.setChannelCount(m_channel);
    fmt.setSampleSize(m_sampleSize);
    fmt.setSampleRate(m_sampleRate);
    fmt.setCodec("audio/pcm");
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);

    m_pAudioOutput = new QAudioOutput(fmt);

    QAudioDeviceInfo devnfo = QAudioDeviceInfo::defaultOutputDevice();
    if (devnfo.isFormatSupported(fmt))
    {
        fmt = devnfo.nearestFormat(fmt);
    }

    m_pIODevice = m_pAudioOutput->start();
}

void JCAudioPlayer::StopAudioPlayer()
{
    QMutexLocker locker(&m_audioPlayMutex);

    if (m_pAudioOutput != NULL)
    {
        m_pAudioOutput->stop();
        delete m_pAudioOutput;
        m_pAudioOutput = NULL;
    }
}

int JCAudioPlayer::GetFreeSpace()
{
    QMutexLocker locker(&m_audioPlayMutex);

    if (m_pAudioOutput == NULL){
        return 0;
    }
    
    int freeSpace = m_pAudioOutput->bytesFree();
    return freeSpace;
}


void JCAudioPlayer::SetVolume(int value)
{
    QMutexLocker locker(&m_audioPlayMutex);

    if(m_pAudioOutput == NULL){
        return;
    }
    m_pAudioOutput->setVolume(value);
}

bool JCAudioPlayer::WriteAudioData(const char* dataBuff,int size)
{
    QMutexLocker locker(&m_audioPlayMutex);
    if((dataBuff == NULL) || (size <= 0)){
        return false;
    }

    if(m_pIODevice == NULL){
        return false;
    }

    m_pIODevice->write(dataBuff,size);

    return true;
}

void JCAudioPlayer::playAudio(bool bPlay)
{
    QMutexLocker locker(&m_audioPlayMutex);
    if( m_pAudioOutput == NULL){
        return;
    }

    if(bPlay == true) {
        m_pAudioOutput->resume();
    }
    else {
        m_pAudioOutput->suspend();
    }

}
