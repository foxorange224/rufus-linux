#pragma once

#include <QProcess>
#include <QElapsedTimer>
#include <functional>

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

// Same as above, but additionally kills the child as soon as the
// isCancelled predicate turns true, so long-running steps (mkfs,
// bootloader installs, downloads, ...) stop promptly when the user
// cancels the operation. Returns true if the process was interrupted
// by the cancel request (the child was killed and reaped).
inline bool finishProcess(QProcess &p, int timeoutMs,
                          const std::function<bool()> &isCancelled) {
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning) {
        if (isCancelled && isCancelled()) {
            p.kill();
            p.waitForFinished(3000);
            return true;
        }
        if (p.waitForFinished(100))
            return false;
        if (t.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(3000);
            return false;
        }
    }
    return false;
}
