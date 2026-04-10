QT       += core gui multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

TARGET = JCMPlayer

INCLUDEPATH +=  $$PWD/3rdParty/libFFmpeg/include

LIBS += -L$$PWD/3rdParty/libFFmpeg/lib -L/usr/local/lib -lavformat -lavcodec -lavutil -lswresample -lswscale -lpostproc -lavfilter -lx264 /usr/lib/x86_64-linux-gnu/libbz2.so.1 /usr/lib/x86_64-linux-gnu/liblzma.so.5 -ldl -lX11 -lfdk-aac -lpthread -lm


# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    CCOpenGLWidget.cpp \
    JCAVCodecHandler.cpp \
    JCAudioPlayer.cpp \
    main.cpp \
    jcmainwindow.cpp

HEADERS += \
    CCOpenGLWidget.h \
    CCYUVDataDefine.h \
    JCAVCodecHandler.h \
    JCAudioPlayer.h \
    JCDataDefine.h \
    jcmainwindow.h

FORMS += \
    jcmainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    jcmplayer.qrc
