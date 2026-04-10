#ifndef JCAUDIOPLAYER_H
#define JCAUDIOPLAYER_H

#include <QtMultimedia/QAudioOutput>
#include <QMutex>

class QIODevice;
class QAudioOutput;

class JCAudioPlayer
{
public:
    static JCAudioPlayer* GetInstance();

    JCAudioPlayer();
    void StartAudioPlayer();
    void StopAudioPlayer();

    int GetFreeSpace();
    void SetVolume(int value);
    bool WriteAudioData(const char* dataBuff,int size);

    void playAudio(bool bPlay);

    void SetSampleRate(int value){m_sampleRate = value;}
    void SetSampleSize(int value){m_sampleSize = value;}
    void Setchannel(int value){m_channel = value;}

private:
    int m_sampleRate = 48000;
    int m_sampleSize = 16;
    int m_channel = 2;

    QIODevice*      m_pIODevice = NULL;
    QAudioOutput*   m_pAudioOutput= NULL;

    QMutex          m_Mutex;
};

#endif // JCAUDIOPLAYER_H
