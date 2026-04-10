#include "jcmainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    JCMainWindow w;
    w.show();
    return a.exec();
}
