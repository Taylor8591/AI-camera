#ifndef CCOPENGLWIDGET_H
#define CCOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions>

#include "CCYUVDataDefine.h"

#define ATTRIB_VERTEX 3
#define ATTRIB_TEXTURE 4

enum {
    ATTRIBUTE_VERTEX,
    ATTRIBUTE_TEXCOORD
};

class CCOpenGLWidget: public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    CCOpenGLWidget(QWidget *parent = 0);
    ~CCOpenGLWidget();

    void RendVideo(H264YUV_Frame * frame);

protected:
    void initializeGL();
    void resizeGL(int w, int h);
    void paintGL();

private:
    void initializeGLSLShaders();
    GLuint createImageTextures(QString &pathString);

private:
    bool    m_bUpdateData = false;

    GLuint  m_textures[3];

    QOpenGLShaderProgram *m_pShaderProgram = NULL;

    int m_nVideoW        =0; //视频分辨率宽
    int m_nVideoH        =0; //视频分辨率高
    int m_yFrameLength   =0;
    int m_uFrameLength   =0;
    int m_vFrameLength   =0;

    unsigned char* m_pBufYuv420p = NULL;

    struct CCVertex {
        float x,y,z;
        float u,v;
    };
};

#endif // CCOPENGLWIDGET_H
