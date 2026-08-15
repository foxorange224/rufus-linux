#include "Filesystem.h"
#include "core/QProc.h"
#include <QProcess>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>

// QFileInfo::size() returns 0 for block devices, so query the kernel directly
static qint64 blockDeviceSize(const QString &path) {
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    qint64 bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &bytes) != 0)
        bytes = 0;
    ::close(fd);
    return bytes;
}

FormatResult Filesystem::format(const QString &partitionPath, FileSystem fs,
                                const QString &label, bool quick, int clusterSizeKB,
                                std::function<void(int)> progressCallback,
                                std::function<bool()> isCancelled) {
    switch (fs) {
    case FileSystem::FAT16:
    case FileSystem::FAT32:
        return formatVfat(partitionPath, fs, label, quick, clusterSizeKB, isCancelled);
    case FileSystem::ext2:
    case FileSystem::ext3:
    case FileSystem::ext4:
        return formatExt(partitionPath, fs, label, quick, clusterSizeKB, isCancelled);
    case FileSystem::NTFS:
        return formatNtfs(partitionPath, label, quick, clusterSizeKB, isCancelled);
    case FileSystem::exFAT:
        return formatExfat(partitionPath, label, quick, clusterSizeKB, isCancelled);
    default:
        return formatVfat(partitionPath, FileSystem::FAT32, label, quick, clusterSizeKB, isCancelled);
    }
}

bool Filesystem::checkFilesystem(const QString &partitionPath, FileSystem fs) {
    QString tool;
    switch (fs) {
    case FileSystem::FAT16:
    case FileSystem::FAT32:
        tool = "fsck.fat"; break;
    case FileSystem::ext2:
    case FileSystem::ext3:
    case FileSystem::ext4:
        tool = "fsck.ext4"; break;
    case FileSystem::NTFS:
        tool = "ntfsfix"; break;
    case FileSystem::exFAT:
        tool = "fsck.exfat"; break;
    default:
        return false;
    }

    QProcess proc;
    proc.start(tool, {"-y", partitionPath});
    return proc.waitForFinished(30000) && proc.exitCode() < 4;
}

QString Filesystem::mkfsTool(FileSystem fs) {
    switch (fs) {
    case FileSystem::FAT16: return QStringLiteral("mkfs.fat");
    case FileSystem::FAT32: return QStringLiteral("mkfs.fat");
    case FileSystem::ext2:  return QStringLiteral("mkfs.ext2");
    case FileSystem::ext3:  return QStringLiteral("mkfs.ext3");
    case FileSystem::ext4:  return QStringLiteral("mkfs.ext4");
    case FileSystem::NTFS:  return QStringLiteral("mkfs.ntfs");
    case FileSystem::exFAT: return QStringLiteral("mkfs.exfat");
    case FileSystem::btrfs: return QStringLiteral("mkfs.btrfs");
    case FileSystem::XFS:   return QStringLiteral("mkfs.xfs");
    case FileSystem::F2FS:  return QStringLiteral("mkfs.f2fs");
    default: return {};
    }
}

FileSystem Filesystem::detectExisting(const QString &partitionPath) {
    QProcess proc;
    proc.start("blkid", {"-o", "value", "-s", "TYPE", partitionPath});
    if (!proc.waitForFinished(5000))
        return FileSystem::Unknown;

    QString type = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return PartitionManager::fsFromString(type);
}

static QString sanitizeFatLabel(const QString &label) {
    QString clean = label.toUpper();
    clean.truncate(11);
    for (int i = 0; i < clean.size(); i++) {
        QChar c = clean[i];
        if (!c.isLetterOrNumber() && c != '_' && c != '-' && c != '~')
            clean[i] = '_';
    }
    if (clean.isEmpty())
        clean = QStringLiteral("NO_NAME");
    return clean;
}

FormatResult Filesystem::formatVfat(const QString &partitionPath, FileSystem fs,
                                    const QString &label, bool quick, int clusterSizeKB,
                                    std::function<bool()> isCancelled) {
    FormatResult result;
    result.fsType = (fs == FileSystem::FAT16) ? "FAT16" : "FAT32";

    // FAT16 only supports volumes up to ~2GB; silently upgrading to FAT32
    // avoids mkfs.fat failing with "Requested FAT type is 16, but volume
    // size is too big" (matches original Rufus behavior).
    if (fs == FileSystem::FAT16) {
        qint64 sizeBytes = blockDeviceSize(partitionPath);
        if (sizeBytes < 0)
            sizeBytes = QFileInfo(partitionPath).size(); // regular file fallback
        if (sizeBytes > 2047LL * 1024 * 1024) {
            fs = FileSystem::FAT32;
            result.fsType = "FAT32";
            result.note = "Volume is larger than 2GB: FAT16 upgraded to FAT32";
        }
    }

    QProcess proc;
    QStringList args;

    args << "-F";
    args << (fs == FileSystem::FAT16 ? "16" : "32");

    if (clusterSizeKB > 0)
        args << "-s" << QString::number(clusterSizeKB * 2);  // sectors per cluster (512B)

    // Allow writing to whole device (bypass partition table safety check)
    args << "-I";

    if (!label.isEmpty())
        args << "-n" << sanitizeFatLabel(label);

    args << partitionPath;

    QElapsedTimer timer;
    timer.start();
    proc.start("mkfs.fat", args);

    if (finishProcess(proc, 30000, isCancelled)) {
        result.cancelled = true;
        return result;
    }
    if (!missingToolMessage(proc, "mkfs.fat").isEmpty()) {
        result.errorMessage = missingToolMessage(proc, "mkfs.fat");
        return result;
    }
    if (proc.exitStatus() != QProcess::NormalExit) {
        result.errorMessage = "mkfs.fat timed out";
        return result;
    }

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.success = (proc.exitCode() == 0);
    result.label = label;

    if (!result.success)
        result.errorMessage = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    return result;
}

FormatResult Filesystem::formatExt(const QString &partitionPath, FileSystem fs,
                                   const QString &label, bool quick, int clusterSizeKB,
                                   std::function<bool()> isCancelled) {
    FormatResult result;
    result.fsType = PartitionManager::fsToString(fs);

    QString tool = mkfsTool(fs);
    QProcess proc;
    QStringList args;

    // -F: force overwrite even if not cleanly unmounted
    args << "-F";

    if (clusterSizeKB > 0)
        args << "-b" << QString::number(clusterSizeKB * 1024);

    if (!label.isEmpty())
        args << "-L" << label;

    args << partitionPath;

    QElapsedTimer timer;
    timer.start();
    proc.start(tool, args);

    if (finishProcess(proc, 60000, isCancelled)) {
        result.cancelled = true;
        return result;
    }
    if (!missingToolMessage(proc, tool).isEmpty()) {
        result.errorMessage = missingToolMessage(proc, tool);
        return result;
    }
    if (proc.exitStatus() != QProcess::NormalExit) {
        result.errorMessage = tool + " timed out";
        return result;
    }

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.success = (proc.exitCode() == 0);
    result.label = label;

    if (!result.success)
        result.errorMessage = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    return result;
}

FormatResult Filesystem::formatNtfs(const QString &partitionPath, const QString &label,
                                    bool quick, int clusterSizeKB,
                                    std::function<bool()> isCancelled) {
    FormatResult result;
    result.fsType = "NTFS";

    QProcess proc;
    QStringList args;

    if (quick)
        args << "-Q";  // Quick format
    // Full format: omit -Q to do full format (slower)

    QString clean = label.toUpper();
    clean.truncate(32);
    clean.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    clean = clean.trimmed();
    if (!clean.isEmpty())
        args << "-L" << clean;

    if (clusterSizeKB > 0)
        args << "-c" << QString::number(clusterSizeKB * 1024);

    args << "-f" << partitionPath;

    QElapsedTimer timer;
    timer.start();
    proc.start("mkfs.ntfs", args);

    if (finishProcess(proc, 60000, isCancelled)) {
        result.cancelled = true;
        return result;
    }
    if (!missingToolMessage(proc, "mkfs.ntfs").isEmpty()) {
        result.errorMessage = missingToolMessage(proc, "mkfs.ntfs");
        return result;
    }
    if (proc.exitStatus() != QProcess::NormalExit) {
        result.errorMessage = "mkfs.ntfs timed out";
        return result;
    }

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.success = (proc.exitCode() == 0);
    result.label = label;

    if (!result.success)
        result.errorMessage = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    return result;
}

FormatResult Filesystem::formatExfat(const QString &partitionPath, const QString &label,
                                     bool quick, int clusterSizeKB,
                                     std::function<bool()> isCancelled) {
    FormatResult result;
    result.fsType = "exFAT";

    QProcess proc;
    QStringList args;

    // exFAT labels are limited to 15 characters and must not contain
    // reserved characters, otherwise mkfs.exfat refuses to run.
    QString clean = label.toUpper();
    clean.truncate(15);
    clean.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|.]")), QStringLiteral("_"));
    clean = clean.trimmed();
    if (!clean.isEmpty())
        args << "-n" << clean;

    if (clusterSizeKB > 0)
        args << "-c" << QString::number(clusterSizeKB * 1024);

    args << partitionPath;

    QElapsedTimer timer;
    timer.start();
    proc.start("mkfs.exfat", args);

    if (finishProcess(proc, 30000, isCancelled)) {
        result.cancelled = true;
        return result;
    }
    if (!missingToolMessage(proc, "mkfs.exfat").isEmpty()) {
        result.errorMessage = missingToolMessage(proc, "mkfs.exfat");
        return result;
    }
    if (proc.exitStatus() != QProcess::NormalExit) {
        result.errorMessage = "mkfs.exfat timed out";
        return result;
    }

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.success = (proc.exitCode() == 0);
    result.label = label;

    if (!result.success)
        result.errorMessage = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    return result;
}
