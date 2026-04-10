#ifndef CCYUVDATADEFINE_H
#define CCYUVDATADEFINE_H

#include <stdint.h>
#include <stdio.h>

#define MAX_AUDIO_FRAME_IN_QUEUE 1200
#define MAX_VIDEO_FRAME_IN_QUEUE 600

typedef struct YUVFrameDef
{
    unsigned int length;
    unsigned char* dataBuffer;
} CCYUVFrame;

typedef struct H264YUVDef
{
    unsigned int width;
    unsigned int height;
    CCYUVFrame luma;
    CCYUVFrame chromB;
    CCYUVFrame chromR;
    long long pts;
} H264YUV_Frame;

typedef struct DecodeAudiodataDef
{
    unsigned char* dataBuffer;
    unsigned int datalength;
} JCDecodeAudioData;

#pragma pack(pop)

#endif // CCYUVDATADEFINE_H
