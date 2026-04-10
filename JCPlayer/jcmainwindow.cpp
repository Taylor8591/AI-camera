#include "jcmainwindow.h"
#include "ui_jcmainwindow.h"
#include <QDebug>
#include <QDesktopWidget>
#include <QSize>
#include <QTimer>
#include <QMessageBox>

JCMainWindow::JCMainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::JCMainWindow)
{
    ui->setupUi(this);

    setAcceptDrops(true);

    this->setAutoFillBackground(true);
    // 获取调色板
    QPalette palette = this->palette();
    palette.setColor(QPalette::Background, Qt::black);
    this->setPalette(palette);

    // 初始化播放器组件
    // setupPlayerCoreWidget();

    m_pPlayerToolBar = addToolBar("PlayerControls");
    m_pPlayerToolBar->setMovable(false);

    m_pPlayButton = new QPushButton("Play", this);
    m_pPlayerToolBar->addWidget(m_pPlayButton);
    connect(m_pPlayButton, &QPushButton::clicked, this, &JCMainWindow::onPlayButtonClicked);

    m_pPauseButton = new QPushButton("Pause", this);
    m_pPlayerToolBar->addWidget(m_pPauseButton);
    connect(m_pPauseButton, &QPushButton::clicked, this, &JCMainWindow::onPauseButtonClicked);
}

JCMainWindow::~JCMainWindow()
{
    releasePlayerCoreWidget();
    delete ui;
}

void JCMainWindow::releasePlayerCoreWidget() {
    if(AVPlayerHandler != NULL) {
        delete AVPlayerHandler;
        AVPlayerHandler = NULL;
    }
    setCentralWidget(NULL);
}

void JCMainWindow::setupPlayerCoreWidget() {
    AVPlayerHandler = new JCAVCodecHandler();
    // updateVideoData 是函数指针，数据类型是： void (*UpdateVideo2GUI_Callback)(H264YUV_Frame* yuv, unsigned long userData);
    // 将 updateVideoData 函数赋值给 AVPlayerHandler 的成员 m_updateVideoCallback
    AVPlayerHandler->SetupUpdateVideoCallback(updateVideoData, (unsigned long)this);

    m_pOpenGLWidget = new CCOpenGLWidget(this);
    m_pOpenGLWidget->setAutoFillBackground(true);

    setCentralWidget(m_pOpenGLWidget);
}


void JCMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    qDebug()<<"you drag a file into the window...";
    if(event->mimeData()->hasFormat("text/uri-list")) {
        event->acceptProposedAction();
        qDebug()<<"accept the file..." << endl;
    }
}

void JCMainWindow::dropEvent(QDropEvent* event) {
    qDebug()<<"you drop a file into the window...";
    QUrl url = event->mimeData()->urls().first();
    if(url.isEmpty()) {
        qDebug()<<"URL is empty!";
        return;
    }

    QByteArray byte = QByteArray(url.toString().toUtf8());
    QUrl encodedUrl(byte);
    encodedUrl = encodedUrl.fromEncoded(byte);
    QString realPath = encodedUrl.toLocalFile();

    openVideoFile(realPath);
}

void JCMainWindow::openVideoFile(const QString& filepath) {
    if(filepath.isEmpty()) {
        qDebug() << "filepath is empty" << endl;
        return;
    }
    qDebug() << "filepath: " << filepath << endl;
    releasePlayerCoreWidget();

    setupPlayerCoreWidget();

    if(AVPlayerHandler != NULL) {
        AVPlayerHandler->SetVideoFilePath(filepath);
        AVPlayerHandler->InitVideoStream();
        hasStartedPlay = false;
    }
    // QSize videoSize = AVPlayerHandler->GetMediaWidthHeight();
    qDebug() << "filepath: " << AVPlayerHandler->GetVideoFilePath() << endl;
}

void JCMainWindow::onPlayButtonClicked() {

    if(AVPlayerHandler == NULL) {
        return;
    }
    if(AVPlayerHandler->GetVideoFilePath().isEmpty()) {
        QMessageBox::information(this, "Info", "Please drag a video file into the window first.");
        return;
    }
    if(hasStartedPlay) {
        return;
    }
    AVPlayerHandler->StartPlayVideo();
    hasStartedPlay = true;
}

void JCMainWindow::onPauseButtonClicked() {

    if(AVPlayerHandler == NULL) {
        return;
    }
     if(AVPlayerHandler->GetVideoFilePath().isEmpty()) {
        QMessageBox::information(this, "Info", "Please drag a video file into the window first.");
        return;
    }
    if(!hasStartedPlay) {
        return;
    }
    AVPlayerHandler->PausePlayVideo();
}


// 在 convertAndRenderVideo 中被调用
void JCMainWindow::updateVideoData(H264YUV_Frame* yuv, unsigned long userData) {

    if(userData == 0 || yuv == NULL) {
        return;
    }

    JCMainWindow* mainWindow = (JCMainWindow*)userData;
    if(mainWindow) {

        mainWindow->UpdateYUVFrameData(yuv);
    }
}

void JCMainWindow::UpdateYUVFrameData(H264YUV_Frame* yuv) {

    // opengl render
    if(yuv == NULL) {
        return;
    }
    m_pOpenGLWidget->RendVideo(yuv);
}