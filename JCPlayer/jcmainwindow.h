#ifndef JCMAINWINDOW_H
#define JCMAINWINDOW_H

#include <QMainWindow>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPushButton>
#include <QToolBar>
#include "JCAVCodecHandler.h"
#include "CCOpenGLWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui { class JCMainWindow; }
QT_END_NAMESPACE

class JCMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    JCMainWindow(QWidget *parent = nullptr);
    ~JCMainWindow();
    void UpdateYUVFrameData(H264YUV_Frame* yuv);
private:
    void setupPlayerCoreWidget();
    void releasePlayerCoreWidget();

    static void updateVideoData(H264YUV_Frame* yuv, unsigned long userData);
protected:
    void dropEvent(QDropEvent* event);
    void dragEnterEvent(QDragEnterEvent* event);

private slots:
    void openVideoFile(const QString& filepath);
    void onPlayButtonClicked();
    void onPauseButtonClicked();

private:
    Ui::JCMainWindow *ui;

    JCAVCodecHandler* AVPlayerHandler = NULL;
    CCOpenGLWidget* m_pOpenGLWidget = NULL;
    QToolBar* m_pPlayerToolBar = NULL;
    QPushButton* m_pPlayButton = NULL;
    QPushButton* m_pPauseButton = NULL;
    bool hasStartedPlay = false;
};

#endif // JCMAINWINDOW_H
