#include "MainWindow.h"

#include <QApplication>
#include <QVector>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pico6487_gui"));

    // Needed for queued signal delivery from the acquisition thread (Qt5).
    qRegisterMetaType<QVector<double>>("QVector<double>");

    MainWindow w;
    w.show();
    return app.exec();
}
