#include "Mounter.h"
#include "core/QProc.h"
#include <QProcess>
#include <QDir>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDebug>

bool Mounter::mount(const QString &devicePath, const QString &mountPoint,
                    const QString &fsType, const QStringList &options) {
    QDir().mkpath(mountPoint);

    QStringList args;
    if (!fsType.isEmpty())
        args << "-t" << fsType;
    for (const QString &opt : options)
        args << "-o" << opt;
    args << devicePath << mountPoint;

    return execMount(args);
}

bool Mounter::unmount(const QString &path, bool force) {
    // If path is a mount point, unmount it
    if (QFileInfo::exists(path) && path.startsWith('/')) {
        QStringList args;
        if (force)
            args << "-l";
        args << path;

        // Unmount must always run to completion (finishProcess never
        // destroys a running child), otherwise a cut-off umount leaves
        // the USB drive corrupt.
        QProcess proc;
        proc.start("umount", args);
        finishProcess(proc, 60000);
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0)
            return true;

        // If lazy unmount was requested and regular failed, try lazy
        if (!force) {
            QProcess proc2;
            proc2.start("umount", {"-l", path});
            finishProcess(proc2, 60000);
            return proc2.exitStatus() == QProcess::NormalExit && proc2.exitCode() == 0;
        }
        return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    }

    // If path is a device, find and unmount all mount points on this device
    QStringList args;
    if (force)
        args << "-l";
    args << path;

    QProcess proc;
    proc.start("umount", args);
    finishProcess(proc, 60000);
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

bool Mounter::isMounted(const QString &path) {
    QProcess proc;
    proc.start("findmnt", {"-n", path});
    if (!proc.waitForFinished(3000))
        return false;
    return proc.exitCode() == 0;
}

QString Mounter::findMountPoint(const QString &devicePath) {
    QProcess proc;
    proc.start("findmnt", {"-n", "-o", "TARGET", "--source", devicePath});
    if (!proc.waitForFinished(3000))
        return {};
    QString mp = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return mp.isEmpty() ? QString() : mp;
}

QList<MountInfo> Mounter::getAllMounts() {
    QList<MountInfo> mounts;
    QProcess proc;
    proc.start("findmnt", {"-n", "-o", "SOURCE,TARGET,FSTYPE,OPTIONS"});
    if (!proc.waitForFinished(3000))
        return mounts;

    for (const QByteArray &line : proc.readAllStandardOutput().split('\n')) {
        MountInfo info;
        if (parseMountInfo(QString::fromUtf8(line), info))
            mounts.append(info);
    }
    return mounts;
}

QString Mounter::createTempMountPoint() {
    QTemporaryDir dir;
    if (dir.isValid()) {
        dir.setAutoRemove(false);
        return dir.path();
    }
    return {};
}

bool Mounter::removeMountPoint(const QString &mountPoint) {
    QDir dir(mountPoint);
    return dir.rmdir(".") || !dir.exists();
}

bool Mounter::execMount(const QStringList &args) {
    QProcess proc;
    proc.start("mount", args);
    if (!proc.waitForFinished(10000))
        return false;
    return proc.exitCode() == 0;
}

bool Mounter::parseMountInfo(const QString &line, MountInfo &info) {
    QStringList parts = line.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return false;

    info.device = parts[0];
    info.mountPoint = parts[1];
    info.fsType = parts[2];
    if (parts.size() >= 4) {
        info.options = parts[3].split(',');
        info.isReadOnly = parts[3].contains("ro");
    }
    return true;
}
