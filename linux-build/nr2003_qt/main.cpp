#include <QApplication>
#include <QLockFile>
#include <QMessageBox>
#include "mainwindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // singleton like blowover FUN_00401150 Local\NR2003Companion.UI.v2
    QLockFile lock("/tmp/nr2003_qt.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) {
        QMessageBox::warning(nullptr, "NR2003 Qt", "Already running.");
        return 0;
    }
    MainWindow w;
    w.show();
    return app.exec();
}
