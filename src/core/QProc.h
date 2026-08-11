#pragma once

#include <QProcess>
#include <QElapsedTimer>

// Runs a QProcess to completion, polling in small increments so the
// object is never destroyed while the child process is still running.
// Qt warns with "QProcess: Destroyed while process (...) is still
// running" in that case, and for umount it is actually dangerous: the
// unmount child can be cut short before its data flush reaches the
// device, corrupting the USB drive. If the timeout is exceeded the
// child is killed and reaped before the function returns.
inline void finishProcess(QProcess &p, int timeoutMs = 30000) {
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning) {
        if (p.waitForFinished(100))
            return;
        if (t.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(3000);
            return;
        }
    }
}
