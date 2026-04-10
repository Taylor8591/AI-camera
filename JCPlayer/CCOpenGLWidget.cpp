#include "CCOpenGLWidget.h"

#include <QDebug>

#include <QMouseEvent>

CCOpenGLWidget::CCOpenGLWidget(QWidget *parent):QOpenGLWidget(parent)
{
    m_pBufYuv420p = NULL;
    m_pShaderProgram = NULL;

    m_nVideoH = 0;
    m_nVideoW = 0;
    m_yFrameLength =0;
    m_uFrameLength =0;
    m_vFrameLength =0;
}

CCOpenGLWidget::~CCOpenGLWidget()
{
    if(m_pShaderProgram != NULL){
        delete m_pShaderProgram;
        m_pShaderProgram= NULL;
    }

    if(NULL != m_pBufYuv420p)
    {
        free(m_pBufYuv420p);
        m_pBufYuv420p=NULL;
    }

    glDeleteTextures(3, m_textures);
}

void CCOpenGLWidget::RendVideo(H264YUV_Frame* yuvFrame)
{
    if(yuvFrame == NULL ){
        return;
    }

    if(m_nVideoH != yuvFrame->height || m_nVideoW != yuvFrame->width){
        if(NULL != m_pBufYuv420p)
        {
            free(m_pBufYuv420p);
            m_pBufYuv420p=NULL;
        }
    }

    m_nVideoW = yuvFrame->width;
    m_nVideoH = yuvFrame->height;

    m_yFrameLength = yuvFrame->luma.length;
    m_uFrameLength = yuvFrame->chromB.length;
    m_vFrameLength = yuvFrame->chromR.length;

    //申请内存一帧yuv图像数据,其大小为分辨率的1.5倍
    int nLen = m_yFrameLength + m_uFrameLength +m_vFrameLength;

    if(NULL == m_pBufYuv420p)
    {
        m_pBufYuv420p = (unsigned char*) malloc(nLen);
    }

    memcpy(m_pBufYuv420p,yuvFrame->luma.dataBuffer,m_yFrameLength);
    memcpy(m_pBufYuv420p+m_yFrameLength,yuvFrame->chromB.dataBuffer,m_uFrameLength);
    memcpy(m_pBufYuv420p+m_yFrameLength +m_uFrameLength,yuvFrame->chromR.dataBuffer,m_vFrameLength);

    m_bUpdateData =true;
    update();
}

void CCOpenGLWidget::initializeGL()
{
    m_bUpdateData = false;

    initializeOpenGLFunctions();

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.0,0.0,0.0,1.0);//设置背景色

    glGenTextures(3, m_textures);

    initializeGLSLShaders();

    return;
}

void CCOpenGLWidget::initializeGLSLShaders()
{
    QOpenGLShader* vertexShader = new QOpenGLShader(QOpenGLShader::Vertex,this);
    bool bCompileVS = vertexShader->compileSourceFile(":/Shaders/vertex.vert");
    if(bCompileVS == false){
        qDebug()<<"VS Compile ERROR:"<<vertexShader->log();
    }

    QOpenGLShader* fragmentShader = new QOpenGLShader(QOpenGLShader::Fragment,this);
    bool bCompileFS = fragmentShader->compileSourceFile(":/Shaders/fragment.frag");
    if(bCompileFS == false){
        qDebug()<<"FS Compile ERROR:"<<fragmentShader->log();
    }

    m_pShaderProgram = new QOpenGLShaderProgram();
    m_pShaderProgram->addShader(vertexShader);
    m_pShaderProgram->addShader(fragmentShader);

    bool linkStatus = m_pShaderProgram->link();
    if(linkStatus == false){
        qDebug()<<"LINK ERROR:"<<m_pShaderProgram->log();
    }

    if(vertexShader != NULL){
        delete vertexShader;
        vertexShader = NULL;
    }

    if(fragmentShader != NULL){
        delete fragmentShader;
        fragmentShader = NULL;
    }
}

void CCOpenGLWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void CCOpenGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();

    if(m_bUpdateData == false){
        return;
    }

    static CCVertex triangleVert[] = {
        {-1, 1,  1,    0,0},
        {-1, -1, 1,    0,1},
        {1,  1,  1,    1,0},
        {1,  -1, 1,    1,1},
    };

    QMatrix4x4 matrix;
    matrix.ortho(-1,1,-1,1,0.1,1000);
    matrix.translate(0,0,-3);

    m_pShaderProgram->bind();
    m_pShaderProgram->setUniformValue("uni_mat",matrix);

    m_pShaderProgram->enableAttributeArray("attr_position");
    m_pShaderProgram->enableAttributeArray("attr_uv");

    m_pShaderProgram->setAttributeArray("attr_position", GL_FLOAT, triangleVert, 3, sizeof(CCVertex));
    m_pShaderProgram->setAttributeArray("attr_uv", GL_FLOAT, &triangleVert[0].u, 2, sizeof(CCVertex));


    // Y Texture
    m_pShaderProgram->setUniformValue("uni_textureY", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_nVideoW, m_nVideoH, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_pBufYuv420p);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // U Texture
    m_pShaderProgram->setUniformValue("uni_textureU", 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_nVideoW/2, m_nVideoH/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, (char*)(m_pBufYuv420p+m_yFrameLength));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // V Texture
    m_pShaderProgram->setUniformValue("uni_textureV", 2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_nVideoW/2, m_nVideoH/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, (char*)(m_pBufYuv420p+m_yFrameLength+m_uFrameLength));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_pShaderProgram->disableAttributeArray("attr_position");
    m_pShaderProgram->disableAttributeArray("attr_uv");

    m_pShaderProgram->release();

    return;
    }

GLuint CCOpenGLWidget::createImageTextures(QString &pathString)
{
    unsigned int    textureId;
    glGenTextures(1,&textureId);//产生纹理索引
    glBindTexture(GL_TEXTURE_2D,textureId); //绑定纹理索引, 之后的操作都针对当前纹理索引

    QImage texImage=QImage(pathString.toLocal8Bit().data());

    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);//指当纹理图象被使用到一个大于它的形状上时
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);//指当纹理图象被使用到一个小于或等于它的形状上时
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,texImage.width(),texImage.height(),0,GL_RGBA,GL_UNSIGNED_BYTE,texImage.rgbSwapped().bits());//指定参数, 生成纹理

    return textureId;
}
