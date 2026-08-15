#include "FormatWorker.h"
#include "core/BadBlocks.h"
#include "core/QProc.h"
#include "backend/Mounter.h"
#include <QElapsedTimer>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <QThread>
#include <QTemporaryDir>
#include <QProcess>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QRegularExpression>

// Downloadable boot files (grldr, grldr.mbr, uefi-ntfs.img) must never be
// written next to the binary (e.g. /usr/local/bin is not guaranteed writable
// and gets wiped on package upgrades) - use the per-user data directory.
static QString dataDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir;
}

// Resolves a boot file that may be bundled with the app or previously
// downloaded into the data directory. Returns an empty string if absent.
static QString resolveDataFile(const QString &fileName) {
    const QString bundled =
        QCoreApplication::applicationDirPath() + QStringLiteral("/") + fileName;
    if (QFileInfo::exists(bundled))
        return bundled;
    const QString inData = dataDir() + QStringLiteral("/") + fileName;
    if (QFileInfo::exists(inData))
        return inData;
    return QString();
}

FormatWorker::FormatWorker(QObject *parent) : QObject(parent) {}

void FormatWorker::setConfig(const Config &config) { m_config = config; }
void FormatWorker::cancel() { m_cancelled.storeRelaxed(1); }
bool FormatWorker::isCancelled() const { return m_cancelled.loadRelaxed() != 0; }

QString FormatWorker::mainPartitionPath() const {
    return partitionNthPath(1);
}

QString FormatWorker::partitionNthPath(int n) const {
    // Loop devices use the same p-suffix convention as NVMe/MMC
    // (/dev/loop0p1), while SATA/SCSI use /dev/sda1.
    if (m_config.targetDevice.path.contains("nvme") ||
        m_config.targetDevice.path.contains("mmcblk") ||
        m_config.targetDevice.path.contains("loop"))
        return m_config.targetDevice.path + "p" + QString::number(n);
    return m_config.targetDevice.path + QString::number(n);
}

QString FormatWorker::persistencePartitionPath() const {
    // Persistence is the first extra in the append order, right after the
    // main partition (original Rufus: [Main][Persistence][ESP]).
    return partitionNthPath(2);
}

bool FormatWorker::waitForPartition(const QString &path, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (isCancelled())
            return false;
        if (QFileInfo::exists(path))
            return true;
        QThread::msleep(100);
    }
    return !isCancelled() && QFileInfo::exists(path);
}

ImageInfo FormatWorker::detectImage() const {
    if (!m_imageInfoCached && !m_config.imagePath.isEmpty()) {
        m_cachedImageInfo = ImageHandler::detect(m_config.imagePath);
        m_imageInfoCached = true;
    }
    return m_cachedImageInfo;
}

void FormatWorker::setProgress(int percent) {
    // The progress bar must never go backwards: some steps are mapped onto
    // fixed percentages (bad blocks 0-50, fake flash 0-10, ISO steps 10-100)
    // and their ranges overlap depending on the selected options.
    m_lastProgress = qMax(m_lastProgress, percent);
    emit progressChanged(m_lastProgress);
}

static bool loopMountIso(const QString &isoPath, const QString &mountPoint) {
    // One QProcess per attempt, reaped before the next one starts.
    QProcess fuse;
    fuse.start("fuseiso", {isoPath, mountPoint});
    finishProcess(fuse, 10000);
    if (fuse.exitStatus() == QProcess::NormalExit && fuse.exitCode() == 0)
        return true;
    QProcess mount;
    mount.start("mount", {"-o", "loop,ro", isoPath, mountPoint});
    finishProcess(mount, 10000);
    return mount.exitStatus() == QProcess::NormalExit && mount.exitCode() == 0;
}

static qint64 totalDirSize(const QString &dir) {
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// Same "X KB / X MB / X GB" human-readable size used by the UI, so log
// lines like "Extracting: /arch/boot/vmlinuz-linux (14 MB)" read
// naturally.
static QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    if (bytes < 1024LL * 1024 * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / (1024LL * 1024 * 1024));
    return QStringLiteral("%1 TB").arg(bytes / (1024LL * 1024 * 1024 * 1024));
}

static void loopUnmount(const QString &mountPoint) {
    // Each command gets its own QProcess and runs to completion: a
    // QProcess must never be destroyed (or reused with start()) while
    // its child is still running, or the unmount flush can be cut short.
    QProcess fusermount;
    fusermount.start("fusermount", {"-u", mountPoint});
    finishProcess(fusermount, 10000);
    if (fusermount.exitStatus() == QProcess::NormalExit && fusermount.exitCode() == 0)
        return;
    QProcess umount;
    umount.start("umount", {mountPoint});
    finishProcess(umount, 10000);
}

static bool copyFiles(const QString &srcDir, const QString &destDir,
                      std::function<void(qint64)> progress,
                      std::function<bool()> isCancelled = nullptr,
                      std::function<void(const QString &)> fileCopied = nullptr) {
    // rsync reports progress to stderr as a single rolling line (--info=progress2):
    // "<bytes> <pct> <speed> <eta> (xfr#, to-chk=...)" separated by \r.
    // -v lists each copied file on stdout (relative names), which the
    // status bar uses to show the file currently being extracted.
    QProcess rsync;
    rsync.setProcessChannelMode(QProcess::SeparateChannels);
    rsync.start("rsync", {"-a", "-v", "--info=progress2",
                          srcDir + "/", destDir + "/"});
    if (rsync.waitForStarted(5000)) {
        QByteArray buf;
        QByteArray bufOut;
        while (rsync.state() != QProcess::NotRunning) {
            if (isCancelled && isCancelled()) {
                rsync.kill();
                rsync.waitForFinished(3000);
                return false;
            }
            if (!rsync.waitForReadyRead(200))
                continue;
            buf += rsync.readAllStandardError();
            int sep = buf.lastIndexOf('\r');
            if (sep < 0) continue;
            QByteArray line = buf.mid(sep + 1).trimmed();
            buf.clear();
            if (progress && !line.isEmpty()) {
                const QList<QByteArray> fields = line.split(' ');
                if (fields.size() >= 2) {
                    bool ok = false;
                    QByteArray bytesField = fields[0];
                    bytesField.replace(",", "");
                    qint64 bytes = bytesField.toLongLong(&ok);
                    if (ok && bytes > 0)
                        progress(bytes);
                }
            }
            if (fileCopied) {
                bufOut += rsync.readAllStandardOutput();
                int nl;
                while ((nl = bufOut.indexOf('\n')) >= 0) {
                    QString name = QString::fromUtf8(bufOut.left(nl)).trimmed();
                    bufOut.remove(0, nl + 1);
                    if (name.isEmpty())
                        continue;
                    // Skip rsync's own chatter, keep only file names.
                    if (name == QStringLiteral("sending incremental file list") ||
                        name.startsWith(QStringLiteral("sent ")) ||
                        name.startsWith(QStringLiteral("total size is ")) ||
                        name.startsWith(QStringLiteral("created directory ")))
                        continue;
                    fileCopied(name);
                }
            }
        }
        rsync.waitForFinished(5000);
        if (rsync.exitStatus() == QProcess::NormalExit && rsync.exitCode() == 0)
            return true;
    }

    // Fallback: manual recursive copy with real byte-counted progress,
    // still cancellable and without a hard timeout (large ISOs on slow
    // flash drives must not time out). Chunked QFile I/O so the progress
    // callback fires steadily instead of once per file.
    QFileInfoList files;
    qint64 total = 0;
    {
        QDirIterator it(srcDir, QDir::AllEntries | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo &fi = it.fileInfo();
            if (fi.isDir()) {
                QDir().mkpath(destDir + "/" +
                    fi.absoluteFilePath().mid(srcDir.length()).mid(1));
            } else if (fi.isFile()) {
                files.append(fi);
                total += fi.size();
            }
        }
    }
    qint64 done = 0;
    for (const QFileInfo &fi : files) {
        if (isCancelled && isCancelled())
            return false;
        QString rel = fi.absoluteFilePath().mid(srcDir.length());
        if (rel.startsWith('/')) rel = rel.mid(1);
        if (fileCopied)
            fileCopied(rel);
        QString target = destDir + "/" + rel;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile in(fi.absoluteFilePath());
        QFile out(target);
        if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly))
            return false;
        out.setPermissions(in.permissions());
        QByteArray buf;
        buf.resize(64 * 1024);
        while (!in.atEnd()) {
            if (isCancelled && isCancelled()) {
                in.close();
                out.close();
                return false;
            }
            qint64 n = in.read(buf.data(), buf.size());
            if (n <= 0) break;
            if (out.write(buf.constData(), n) != n) {
                in.close();
                out.close();
                return false;
            }
            done += n;
            if (progress)
                progress(done);
        }
        in.close();
        out.close();
    }
    return true;
}

// ─── Main entry point ───────────────────────────────────────────────
void FormatWorker::run() {
    QElapsedTimer totalTimer;
    totalTimer.start();
    QString message;
    bool cancelled = false;

    m_lastProgress = 0;

    // An ISO image to extract makes the file-copy phase the long pole of
    // the operation, so the format steps (which are comparatively quick)
    // must not eat half of the progress bar. All progress anchors below
    // are compressed when an image will follow; a plain format keeps the
    // original Rufus-like spacing.
    m_hasImage = (m_config.mode == Mode::CreateBootable ||
                  m_config.mode == Mode::WriteImageIso) &&
                 !m_config.imagePath.isEmpty();

    auto fail = [&](const QString &msg) {
        message = msg;
        emit logMessage(msg, 1);
        setProgress(100);
        emit finished(false, msg, m_fakeFlashDetected);
    };

    auto done = [&]() {
        message = QStringLiteral("Completed successfully in %1 seconds.")
            .arg(totalTimer.elapsed() / 1000.0, 0, 'f', 1);
        emit logMessage(message, 0);
        setProgress(100);
        emit finished(true, message, m_fakeFlashDetected);
    };

    emit logMessage(QStringLiteral("Rufus operation started"), 0);
    emit logMessage(QStringLiteral("Target device: %1 (%2)")
        .arg(m_config.targetDevice.path)
        .arg(m_config.targetDevice.name), 0);
    emit logMessage(QStringLiteral("Mode: %1")
        .arg(m_config.mode == Mode::WriteImage ? "Write Image (DD)" :
             m_config.mode == Mode::WriteImageIso ? "Write Image (ISO)" :
             m_config.mode == Mode::FormatOnly ? "Format Only" :
             "Create Bootable"), 0);

    // Status bar detail: with an image loaded, say which image is being
    // used; a plain format shows a neutral "working with the drive" hint.
    if (m_config.imagePath.isEmpty()) {
        emit statusBarMessage(tr("Working with the drive..."));
    } else {
        emit statusBarMessage(tr("Using image: %1")
            .arg(QFileInfo(m_config.imagePath).fileName()));
    }

    // Unmount all partitions on the device
    emit statusChanged(tr("Unmounting partitions..."));
    if (!DeviceManager::unmountPartitions(m_config.targetDevice.path)) {
        emit logMessage(QStringLiteral(
            "WARNING: could not unmount one or more partitions of %1; "
            "the device may be in use by another application")
            .arg(m_config.targetDevice.path), 2);
    }

    if (isCancelled()) { fail(QStringLiteral("Operation cancelled by user.")); return; }

    // ── Step 1: Bad blocks check ──
    if (m_config.checkBadBlocks) {
        emit statusChanged(tr("Checking for bad blocks..."));
        emit logMessage(QStringLiteral("Bad block check started (%1 passes)")
            .arg(m_config.badBlocksPasses), 0);

        if (!checkBadBlocksPass()) {
            if (isCancelled()) { fail(QStringLiteral("Operation cancelled by user.")); return; }
            fail(QStringLiteral("Bad block check failed"));
            return;
        }
    }

    // ── Step 2: Fake flash detection ──
    {
        emit statusChanged(tr("Checking for fake flash..."));
        bool fake = BadBlocks::detectFakeFlash(
            m_config.targetDevice.path, m_config.targetDevice.size,
            [this](int p) { setProgress(m_hasImage ? p / 20 : p / 10); },
            [this]() { return isCancelled(); });
        m_fakeFlashDetected = fake;
        if (fake)
            emit logMessage(QStringLiteral("WARNING: Device appears to be fake/flash!"), 2);
    }

    if (isCancelled()) { fail(QStringLiteral("Operation cancelled by user.")); return; }

    // ── Step 3: Determine if DD or ISO mode ──
    // An explicit WriteImage always means DD. For bootable-image mode, DD
    // is used exactly when the image is a raw/DD-only bootable image
    // (original rufus.c: IS_DD_BOOTABLE && (!is_iso || disable_iso)).
    bool isDdMode = (m_config.mode == Mode::WriteImage);
    if (m_config.mode == Mode::CreateBootable && !m_config.imagePath.isEmpty()) {
        ImageInfo imgInfo = detectImage();
        isDdMode = imgInfo.isDDOnly();
    }

    if (isDdMode) {
        if (!writeImageDD()) {
            fail(QStringLiteral("Image write failed."));
            return;
        }
        if (m_config.verifyAfterWrite) {
            emit statusChanged(tr("Verifying written data..."));
            if (!verifyWrite()) {
                fail(QStringLiteral("Verification failed."));
                return;
            }
        }
        done();
        return;
    }

    // ── ISO Mode: partition, format, extract ──
    emit logMessage(QStringLiteral("Starting ISO mode on %1 (%2)")
        .arg(m_config.targetDevice.path)
        .arg(m_config.imagePath.isEmpty() ? QStringLiteral("no image")
                                           : m_config.imagePath), 0);

    // Progress anchors. With an image to extract afterwards, the format
    // steps are compressed so the ISO copy dominates the bar; without an
    // image the format itself is the whole operation and keeps the wider
    // spacing (like original Rufus).
    const int kZero = m_hasImage ? 5 : 10;
    const int kInit = m_hasImage ? 8 : 15;
    const int kParts = m_hasImage ? 12 : 25;
    const int kPersist = m_hasImage ? 17 : 35;
    const int kFormat = m_hasImage ? 22 : 50;
    const int kMbr = m_hasImage ? 25 : 55;
    m_copyStart = kMbr;
    m_copyEnd = m_hasImage ? 88 : 85;

    // Original Rufus log sequence: clearing structures, zeroing 8 MB at
    // the top and 1 MB at the end of the drive, then initializing disk.
    emit logMessage(QStringLiteral("Clearing MBR/PBR/GPT structures..."), 0);
    if (!zeroMbr()) { fail(QStringLiteral("Failed to clear device")); return; }
    emit logMessage(QStringLiteral("Zeroed 8 MB at the top of the drive"), 0);
    emit logMessage(QStringLiteral("Zeroed 1 MB at the end of the drive"), 0);
    setProgress(kZero);

    emit logMessage(QStringLiteral("Initializing disk..."), 0);
    emit logMessage(QStringLiteral("Partitioning (%1)...")
        .arg(m_config.scheme == PartitionScheme::GPT ? QStringLiteral("GPT") : QStringLiteral("MBR")), 0);
    if (!createPartitionTable()) { fail(QStringLiteral("Failed to create partition table")); return; }
    setProgress(kInit);

    emit logMessage(QStringLiteral("Creating partitions..."), 0);
    if (!createRufusPartitions()) { fail(QStringLiteral("Failed to create partitions")); return; }

    // Log the main data partition creation with the same offset Rufus
    // uses (1048576 = 2048 sectors x 512B = first MiB).
    {
        const qint64 kMib = 1024LL * 1024;
        qint64 mainSize = m_config.targetDevice.size - kMib;
        if (mainSize < 0) mainSize = 0;
        emit logMessage(QStringLiteral("Creating Main Data Partition (offset: %1, size: %2 GB)")
            .arg(kMib)
            .arg(qMax<qint64>(mainSize / kMib, 1) / 1024.0, 0, 'f', 0), 0);
    }
    setProgress(kParts);

    if (m_config.extraParts.persistence) {
        if (!formatPersistencePartition()) { fail(QStringLiteral("Failed to format persistence partition")); return; }
    }
    setProgress(kPersist);

    if (!formatMainPartition()) { fail(QStringLiteral("Failed to format main partition")); return; }
    setProgress(kFormat);

    // Write MBR (boot-type-aware selection, like original Rufus)
    {
        emit statusChanged(tr("Writing boot records..."));
        emit logMessage(QStringLiteral("Writing Master Boot Record..."), 0);

        // MBR + BIOS: mark the USB partition as bootable (0x80), exactly
        // like original Rufus ('Set bootable USB partition as 0x80').
        // GPT and MBR-for-UEFI leave the partition unmarked.
        if (m_config.scheme == PartitionScheme::MBR &&
            m_config.targetType != TargetSystemType::UEFI)
            emit logMessage(QStringLiteral("Set bootable USB partition as 0x80"), 0);

        BootloaderResult mbrResult = BootloaderInstaller::writeMbrForBootType(
            m_config.targetDevice.path, m_config.bootloaderType,
            m_config.scheme, nullptr);
        if (mbrResult.success) {
            if (m_config.scheme == PartitionScheme::GPT)
                emit logMessage(QStringLiteral("Using Rufus protective MBR"), 0);
            else
                emit logMessage(QStringLiteral("Using Rufus MBR"), 0);
        } else {
            emit logMessage(QStringLiteral("MBR note: %1").arg(mbrResult.errorMessage), 2);
        }
    }
    // Write SBR (partition boot record) + remount for bootloader
    if (!writeSbr()) {
        emit logMessage(QStringLiteral("SBR write failed"), 2);
    }
    setProgress(kMbr);

    // Mount, install bootloader (before file copy), copy files, remount after
    {
        QString failureReason;
        if (!mountAndCopyFiles(&failureReason)) {
            if (isCancelled()) { fail(QStringLiteral("Operation cancelled by user.")); return; }
            fail(failureReason.isEmpty() ? QStringLiteral("Failed to copy files")
                                         : failureReason);
            return;
        }
    }
    setProgress(m_copyEnd);

    // NTFS checkdisk at end (like Rufus does)
    if (m_config.filesystem == FileSystem::NTFS) {
        emit statusChanged(tr("Running NTFS checkdisk..."));
        ntfsCheckDisk();
    }

    done();
}

// ─── DD write image ────────────────────────────────────────────────
bool FormatWorker::writeImageDD() {
    emit logMessage(QStringLiteral("Writing image in DD mode..."), 0);
    // Show the operation name inside the progress bar, next to the
    // percentage ("Writing image in DD mode: 42%").
    emit statusChanged(tr("Writing image in DD mode..."));

    // For DD mode, first unmount everything
    DeviceManager::unmountPartitions(m_config.targetDevice.path);

    qint64 totalBytes = QFileInfo(m_config.imagePath).size();
    bool isCompressed = (m_config.imagePath.endsWith(".gz") ||
                         m_config.imagePath.endsWith(".xz") ||
                         m_config.imagePath.endsWith(".bz2") ||
                         m_config.imagePath.endsWith(".zst"));

    // Progress total: the projected (uncompressed) size — otherwise a
    // compressed image's progress would exceed 100% before the write ends.
    if (isCompressed && m_config.projectedSize > totalBytes)
        totalBytes = m_config.projectedSize;

    WriteResult writeResult = DriveWriter::writeImage(
        m_config.imagePath, m_config.targetDevice.path, isCompressed,
        [this, totalBytes](qint64 n) { emit deviceProgress(n, totalBytes); },
        [this]() { return isCancelled(); });

    if (writeResult.cancelled) {
        emit logMessage(QStringLiteral("Write cancelled - device may be "
                                       "partially written"), 2);
        return false;
    }
    if (!writeResult.success) {
        emit logMessage(QStringLiteral("Write failed: %1").arg(writeResult.errorMessage), 1);
        return false;
    }

    emit logMessage(QStringLiteral("Wrote %1 bytes to %2")
        .arg(writeResult.bytesWritten).arg(m_config.targetDevice.path), 0);

    DriveWriter::syncDevice(m_config.targetDevice.path);
    setProgress(100);
    return true;
}

// ─── Bad blocks check ──────────────────────────────────────────────
bool FormatWorker::checkBadBlocksPass() {
    int totalPasses = m_config.badBlocksPasses;

    // 1 pass: non-destructive read test. 2+ passes: destructive
    // write+verify pass per pattern (SLC, MLC, TLC like original Rufus).
    BadBlocks::Mode mode = (totalPasses >= 2)
        ? BadBlocks::Mode::ReadWrite
        : BadBlocks::Mode::Read;

    BadBlockResult result = BadBlocks::check(
        m_config.targetDevice.path, 0, mode,
        [this](int p) { setProgress(p / 2); },
        totalPasses,
        [this]() { return isCancelled(); });

    if (result.cancelled)
        return false;

    emit logMessage(result.summary, result.badSectors > 0 ? 2 : 0);

    if (result.badSectors > 0) {
        emit logMessage(QStringLiteral("WARNING: %1 bad sectors found")
            .arg(result.badSectors), 2);
        return false;
    }
    return true;
}

// ─── Zero MBR/GPT ──────────────────────────────────────────────────
bool FormatWorker::zeroMbr() {
    emit statusChanged(tr("Clearing device..."));
    return PartitionManager::clearDevice(m_config.targetDevice.path);
}

// ─── Create partition table ────────────────────────────────────────
bool FormatWorker::createPartitionTable() {
    emit statusChanged(tr("Creating partition table..."));

    PartitionScheme scheme = m_config.scheme;
    if (m_config.targetType == TargetSystemType::UEFI && scheme == PartitionScheme::MBR)
        scheme = PartitionScheme::MBRForUEFI;

    return PartitionManager::createPartitionTable(m_config.targetDevice.path, scheme);
}

// ─── Create Rufus-style partitions ─────────────────────────────────
bool FormatWorker::createRufusPartitions() {
    emit statusChanged(tr("Creating partitions..."));

    PartitionLayout layout;
    layout.scheme = m_config.scheme;
    layout.fs = m_config.filesystem;
    layout.label = m_config.volumeLabel;
    layout.extraPartitions = XP_NONE;

    if (m_config.extraParts.persistence)
        layout.extraPartitions |= XP_PERSISTENCE;
    if (m_config.extraParts.esp)
        layout.extraPartitions |= XP_ESP;
    if (m_config.extraParts.uefiNtfs)
        layout.extraPartitions |= XP_UEFI_NTFS;
    if (m_config.extraParts.msr)
        layout.extraPartitions |= XP_MSR;
    if (m_config.extraParts.compatibility)
        layout.extraPartitions |= XP_COMPAT;
    if (m_config.targetType == TargetSystemType::UEFI && m_config.scheme == PartitionScheme::MBR)
        layout.mbrUefiMarker = true;

    layout.persistenceSizeMB = m_config.persistentSizeMB;
    layout.espSizeMB = m_config.extraParts.espSizeMB;
    // Align the main partition to the cluster size like the original
    // (drive.c:2418-2419 FLOOR_ALIGN), which matters for NTFS/FFU.
    if (m_config.clusterSizeKB > 0)
        layout.clusterSizeBytes = (qint64)m_config.clusterSizeKB * 1024;

    return PartitionManager::createRufusPartitions(m_config.targetDevice.path, layout);
}

// ─── Format main partition ─────────────────────────────────────────
bool FormatWorker::formatMainPartition() {
    emit statusChanged(tr("Formatting main partition..."));

    // Wait for partition to appear
    QString partPath = mainPartitionPath();
    if (!waitForPartition(partPath)) {
        emit logMessage(QStringLiteral("Main partition did not appear: %1").arg(partPath), 1);
        return false;
    }

    emit logMessage(QStringLiteral("Formatting %1 as %2 (label: %3)")
        .arg(partPath)
        .arg(PartitionManager::fsToString(m_config.filesystem))
        .arg(m_config.volumeLabel.isEmpty() ? "(none)" : m_config.volumeLabel), 0);

    FormatResult result = Filesystem::format(partPath, m_config.filesystem,
                                              m_config.volumeLabel, m_config.quickFormat,
                                              m_config.clusterSizeKB,
                                              nullptr,
                                              [this]() { return isCancelled(); });
    if (result.cancelled) {
        emit logMessage(QStringLiteral("Format cancelled"), 2);
        return false;
    }
    if (!result.success) {
        emit logMessage(QStringLiteral("Format failed: %1").arg(result.errorMessage), 1);
        return false;
    }

    if (!result.note.isEmpty())
        emit logMessage(result.note, 1);

    emit logMessage(QStringLiteral("Format took %1s").arg(result.elapsedSeconds, 0, 'f', 1), 0);
    return true;
}

// ─── Format persistence partition ──────────────────────────────────
bool FormatWorker::formatPersistencePartition() {
    emit statusChanged(tr("Formatting persistence partition..."));

    QString partPath = persistencePartitionPath();
    if (!waitForPartition(partPath)) {
        emit logMessage(QStringLiteral("Persistence partition did not appear"), 1);
        return false;
    }

    // Determine label based on whether it's Ubuntu-style (casper-rw) or Debian-style
    ImageInfo imgInfo = detectImage();
    QString persistenceLabel = imgInfo.usesCasper
        ? QStringLiteral("casper-rw") : QStringLiteral("persistence");

    emit logMessage(QStringLiteral("Formatting persistence partition as ext4 (label: %1)")
        .arg(persistenceLabel), 0);

    FormatResult result = Filesystem::format(partPath, FileSystem::ext4,
                                              persistenceLabel, m_config.quickFormat,
                                              0, nullptr,
                                              [this]() { return isCancelled(); });
    if (result.cancelled) {
        emit logMessage(QStringLiteral("Persistence format cancelled"), 2);
        return false;
    }
    if (!result.success) {
        emit logMessage(QStringLiteral("Persistence format failed: %1").arg(result.errorMessage), 1);
        return false;
    }

    // For Debian-style persistence, create persistence.conf
    if (!imgInfo.usesCasper) {
        QString mountPoint = Mounter::createTempMountPoint();
        if (!mountPoint.isEmpty() && Mounter::mount(partPath, mountPoint, "ext4")) {
            QFile conf(mountPoint + "/persistence.conf");
            if (conf.open(QIODevice::WriteOnly)) {
                conf.write("/ union\n");
                conf.close();
                emit logMessage(QStringLiteral("Created persistence.conf"), 0);
            }
            Mounter::unmount(mountPoint);
            Mounter::removeMountPoint(mountPoint);
        }
    }

    return true;
}

// ─── Mount and copy files ──────────────────────────────────────────
bool FormatWorker::mountAndCopyFiles(QString *failureReason) {
    emit statusChanged(tr("Mounting and copying files..."));

    QString partPath = mainPartitionPath();
    if (!waitForPartition(partPath, 10000)) {
        emit logMessage(QStringLiteral("Partition not available"), 1);
        if (failureReason)
            *failureReason = tr("The partition could not be created or detected.");
        return false;
    }

    // Create mount point
    QString mountPoint = Mounter::createTempMountPoint();
    if (mountPoint.isEmpty()) {
        emit logMessage(QStringLiteral("Failed to create mount point"), 1);
        if (failureReason)
            *failureReason = tr("Could not create a temporary mount point.");
        return false;
    }

    // Determine FS type for mount
    QString fsType;
    switch (m_config.filesystem) {
    case FileSystem::FAT16:
    case FileSystem::FAT32: fsType = "vfat"; break;
    case FileSystem::NTFS:  fsType = "ntfs-3g"; break;
    case FileSystem::exFAT: fsType = "exfat"; break;
    case FileSystem::ext4:  fsType = "ext4"; break;
    case FileSystem::ext3:  fsType = "ext3"; break;
    case FileSystem::ext2:  fsType = "ext2"; break;
    default: fsType = "vfat"; break;
    }

    if (!Mounter::mount(partPath, mountPoint, fsType)) {
        emit logMessage(QStringLiteral("Failed to mount partition"), 1);
        Mounter::removeMountPoint(mountPoint);
        if (failureReason)
            *failureReason = tr("Could not mount the formatted partition.");
        return false;
    }

    // Detect the image once so the whole flow can branch on it (ReactOS
    // needs its own FreeLoader boot files from the ISO, like original Rufus).
    ImageInfo imgInfo;
    if (!m_config.imagePath.isEmpty())
        imgInfo = detectImage();
    bool reactosBoot = (imgInfo.osType == OsType::ReactOS);

    // Make sure we unmount on exit
    auto cleanup = [&]() {
        Mounter::unmount(mountPoint);
        Mounter::removeMountPoint(mountPoint);
    };

    // 1. Install bootloader (before file copy - original Rufus order)
    {
        bool hasBootloader = (m_config.bootloaderType != "none" && !m_config.bootloaderType.isEmpty());
        if (hasBootloader && m_config.bootloaderType != "msdos" && m_config.bootloaderType != "freedos") {
            emit statusChanged(tr("Installing bootloader..."));
            if (!installBootloader(mountPoint)) {
                emit logMessage(QStringLiteral("Bootloader installation failed"), 1);
            }
        }
    }

    // Remount after bootloader install to flush VBR changes (original Rufus does this)
    // "if you don't remount, you don't boot!" - Rufus comment for NTFS
    {
        emit statusChanged(tr("Remounting volume..."));
        Mounter::unmount(mountPoint);
        if (!Mounter::mount(partPath, mountPoint, fsType)) {
            emit logMessage(QStringLiteral("Remount failed"), 2);
            // Try once more
            if (!Mounter::mount(partPath, mountPoint, fsType)) {
                cleanup();
                emit logMessage(QStringLiteral("Failed to remount partition"), 1);
                if (failureReason)
                    *failureReason = tr("Could not remount the partition after installing the bootloader.");
                return false;
            }
        }
        emit logMessage(QStringLiteral("Volume remounted after bootloader install"), 0);
    }

    // 2. Handle MS-DOS / FreeDOS file copies (these handle their own bootloader setup)
    if (m_config.bootloaderType == "msdos") {
        emit statusChanged(tr("Installing MS-DOS..."));
        ImageHandler::extractMsDos(mountPoint);
    }
    // ReactOS does NOT use the bundled FreeDOS files: it ships its own
    // FreeLoader boot files inside the ISO (/loader/), which are installed
    // right after the ISO extraction below.
    if (m_config.bootloaderType == "freedos" && !reactosBoot) {
        emit statusChanged(tr("Installing FreeDOS..."));
        QString partPath = mainPartitionPath();
        BootloaderInstaller::installFreeDos(partPath, mountPoint, nullptr);
    }

    // 3. Copy/extract ISO files
    if (!m_config.imagePath.isEmpty()) {
        if (imgInfo.type == ImageType::ISO) {
            emit statusChanged(tr("Extracting ISO files to USB..."));
            emit logMessage(QStringLiteral("Extracting ISO files to USB..."), 0);

            // Per-file reporting: the copy callbacks fire when a file
            // STARTS being written, so its size on the target is still
            // 0 (or partial). Stat the source (the mounted ISO) instead:
            // it is complete, so the log line appears right when the
            // file starts extracting, matching the status bar.
            QTemporaryDir isoMountDir;
            isoMountDir.setAutoRemove(true);
            const QString isoMp = isoMountDir.path();

            auto onFile = [this, isoMp](const QString &name) {
                const QString rel = name.startsWith('/') ? name
                                                         : QStringLiteral("/") + name;
                const QString msg = tr("Extracting: %1 (%2)")
                    .arg(rel)
                    .arg(formatBytes(QFileInfo(isoMp + rel).size()));
                emit logMessage(msg, 0);
                emit statusBarMessage(tr("Extracting: %1").arg(rel));
            };

            // The extraction phase occupies the m_copyStart..m_copyEnd
            // window of the overall progress; map the *real* extraction
            // percentage (bytes copied vs total) into that window so the
            // bar moves steadily instead of jumping.
            auto extractPercent = [this](int pct) {
                setProgress(qBound(m_copyStart,
                    m_copyStart + (m_copyEnd - m_copyStart) *
                                 qBound(0, pct, 100) / 100, m_copyEnd));
            };

            if (loopMountIso(m_config.imagePath, isoMp)) {
                qint64 total = totalDirSize(isoMp);
                if (!copyFiles(isoMp, mountPoint,
                    [this, total, &extractPercent](qint64 n) {
                        extractPercent(static_cast<int>(
                            n * 100 / qMax<qint64>(1, total)));
                    },
                    [this]() { return isCancelled(); },
                    onFile)) {
                    loopUnmount(isoMp);
                    cleanup();
                    emit logMessage(QStringLiteral("Failed to copy files from ISO"), 1);
                    return false;
                }
                loopUnmount(isoMp);
            } else {
                QStringList missingTools;
                if (!ImageHandler::extractIso(m_config.imagePath, mountPoint,
                    [&extractPercent](int pct) { extractPercent(pct); },
                    [this]() { return isCancelled(); },
                    onFile, &missingTools)) {
                    cleanup();
                    emit logMessage(QStringLiteral("ISO extraction failed"), 1);
                    if (failureReason) {
                        if (!missingTools.isEmpty())
                            *failureReason = tr("Could not extract the ISO: %1 is not installed.")
                                .arg(missingTools.join(QStringLiteral(", ")));
                        else
                            *failureReason = tr("Could not extract the ISO.");
                    }
                    return false;
                }
            }
            // Apply Windows 7 EFI fix if needed
            if (imgInfo.hasBootmgrEfi && !imgInfo.isUefiBootable &&
                m_config.targetType == TargetSystemType::UEFI) {
                emit statusChanged(tr("Applying Win7 EFI boot fix..."));
                QString efiBootDir = mountPoint + "/EFI/BOOT";
                QDir().mkpath(efiBootDir);
                if (!imgInfo.wininstPaths.isEmpty()) {
                    QFile::copy(mountPoint + "/windows/boot/efi/bootmgfw.efi",
                                efiBootDir + "/bootx64.efi");
                }
            }

            // Apply Windows User Experience (WUE) customization
            if (m_config.wue.enabled) {
                applyUnattendCustomization(mountPoint);
            }

            // ReactOS: install Syslinux + mboot.c32, exactly like original
            // Rufus does (format.c InstallSyslinux + syslinux.c "Setting up
            // ReactOS..."). Syslinux loads mboot.c32 (a Multiboot loader)
            // which boots freeldr.sys from the ISO path, so FreeLoader can
            // start ReactOS on legacy BIOS machines.
            if (reactosBoot) {
                emit statusChanged(tr("Installing ReactOS bootloader..."));
                emit logMessage(QStringLiteral("ReactOS detected: installing Syslinux + mboot.c32"), 0);

                // syslinux(1) installs on the partition block device (the
                // FAT volume must not be mounted while it runs). If the
                // generic bootloader step already installed Syslinux
                // before the file copy, skip the duplicate install.
                bool syslinuxOk = true;
                if (m_config.bootloaderType != "syslinux") {
                    Mounter::unmount(mountPoint);

                    BootloaderResult r = BootloaderInstaller::installSyslinux(
                        m_config.targetDevice.path, partPath, nullptr);
                    syslinuxOk = r.success;
                    if (!syslinuxOk)
                        emit logMessage(QStringLiteral("ReactOS: syslinux install failed: %1").arg(r.errorMessage), 2);
                    else
                        emit logMessage(QStringLiteral("ReactOS: syslinux MBR + VBR installed"), 0);

                    if (!Mounter::mount(partPath, mountPoint, fsType))
                        emit logMessage(QStringLiteral("ReactOS: remount failed after syslinux install"), 2);
                }

                // mboot.c32: Multiboot loader that starts FreeLoader. On
                // syslinux >= 6, every COM32 module depends on libcom32.c32
                // running beside it, so copy the whole .c32 module set of the
                // syslinux that was installed to the volume root.
                QString mbootSrc;
                const QStringList mbootCandidates = {
                    QCoreApplication::applicationDirPath() + "/../share/rufus/syslinux/mboot.c32",
                    QStringLiteral("/usr/local/share/rufus/syslinux/mboot.c32"),
                    QStringLiteral("/usr/share/rufus/syslinux/mboot.c32"),
                    QStringLiteral("/usr/lib/syslinux/bios/mboot.c32")
                };
                for (const QString &c : mbootCandidates) {
                    if (QFileInfo::exists(c)) {
                        mbootSrc = c;
                        break;
                    }
                }
                if (!mbootSrc.isEmpty() && syslinuxOk) {
                    if (QFile::copy(mbootSrc, mountPoint + "/mboot.c32"))
                        emit logMessage(QStringLiteral("ReactOS: mboot.c32 copied"), 0);
                    else
                        emit logMessage(QStringLiteral("ReactOS: failed to copy mboot.c32"), 2);

                    // Copy any library module needed by the COM32 loader
                    // (libcom32.c32, ...) from the same directory.
                    QDir modDir(QFileInfo(mbootSrc).absolutePath());
                    const QStringList c32Files =
                        modDir.entryList(QStringList() << "*.c32", QDir::Files);
                    for (const QString &m : c32Files) {
                        QString dst = mountPoint + "/" + m;
                        if (QFileInfo::exists(dst))
                            continue;
                        if (QFile::copy(modDir.absoluteFilePath(m), dst))
                            emit logMessage(QStringLiteral("ReactOS: copied syslinux module %1").arg(m), 0);
                    }
                } else {
                    emit logMessage(QStringLiteral("ReactOS: mboot.c32 not found"), 2);
                }

                // syslinux.cfg boots FreeLoader from its ISO path via mboot
                QString reactosPath = imgInfo.reactosPath;
                if (reactosPath.isEmpty())
                    reactosPath = QStringLiteral("/loader/freeldr.sys");
                QFile cfg(mountPoint + "/syslinux.cfg");
                if (cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    cfg.write("DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL mboot.c32\n  APPEND ");
                    cfg.write(reactosPath.toUtf8());
                    cfg.write("\n");
                    cfg.close();
                    emit logMessage(QStringLiteral("ReactOS: syslinux.cfg created (APPEND %1)").arg(reactosPath), 0);
                } else {
                    emit logMessage(QStringLiteral("ReactOS: failed to write syslinux.cfg"), 2);
                }

                // freeldr.ini: boot in "Screen debug" mode by default, so a
                // kernel/driver crash shows on the console instead of a
                // silent black screen, but keep the regular Setup entry
                // selectable via the menu (TimeOut=10).
                QFile flIni(mountPoint + "/freeldr.ini");
                if (flIni.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    flIni.write(
                        "[FREELOADER]\r\n"
                        "DefaultOS=Screen\r\n"
                        "TimeOut=10\r\n"
                        "\r\n"
                        "[Display]\r\n"
                        "TitleText=ReactOS USB\r\n"
                        "MinimalUI=Yes\r\n"
                        "\r\n"
                        "[Operating Systems]\r\n"
                        "Screen=\"ReactOS Setup (Screen debug)\"\r\n"
                        "Setup=\"ReactOS Setup\"\r\n"
                        "\r\n"
                        "[Screen]\r\n"
                        "BootType=ReactOSSetup\r\n"
                        "Options=/DEBUG /DEBUGPORT=SCREEN /NOGUIBOOT /SIFOPTIONSOVERRIDE\r\n"
                        "\r\n"
                        "[Setup]\r\n"
                        "BootType=ReactOSSetup\r\n");
                    flIni.close();
                    emit logMessage(QStringLiteral("ReactOS: freeldr.ini written (default = Screen debug)"), 0);
                } else {
                    emit logMessage(QStringLiteral("ReactOS: failed to write freeldr.ini"), 2);
                }
            }
        }
    }

    // Finalize (original Rufus 'Finalizing, please wait...'): create the
    // extended label files, in the same relative order as the original
    // logs.
    emit logMessage(QStringLiteral("Finalizing, please wait..."), 0);
    createExtendedLabelFiles(mountPoint);

    // Match bootloader version from image and download if needed
    if (m_config.bootloaderType != "none" && m_config.mode == Mode::CreateBootable) {
        ImageInfo imgInfo = detectImage();
        matchBootloaderVersion(mountPoint, imgInfo);
        downloadBootloaderIfNeeded(mountPoint);
    }

    // 4. Extract additional archive
    if (!m_config.archivePath.isEmpty()) {
        emit statusChanged(tr("Extracting additional files..."));
        // Per-file reporting: 7z prints each file when its extraction
        // starts, and the target file is only partially written, so the
        // size is not known yet. Log it immediately, like the status bar.
        auto onFile = [this](const QString &name) {
            const QString rel = name.startsWith('/') ? name
                                                     : QStringLiteral("/") + name;
            emit logMessage(tr("Extracting: %1").arg(rel), 0);
            emit statusBarMessage(tr("Extracting: %1").arg(rel));
        };
if (!ImageHandler::extractCompressed(m_config.archivePath, mountPoint,
            [this](int pct) {
                setProgress(qBound(85, 85 + 10 * qBound(0, pct, 100) / 100, 95));
            },
            onFile)) {
            cleanup();
            emit logMessage(QStringLiteral("Additional file extraction failed"), 1);
            if (failureReason)
                *failureReason = tr("Could not extract the additional file.\n"
                                    "Make sure 7z is installed and the archive is valid.");
            return false;
        }
    }

    cleanup();
    return true;
}

// ─── Extended label and icon files (autorun.inf / autorun.ico) ─────
// Original Rufus: with "Create extended label and icon files" checked,
// an autorun.inf (and an autorun.ico icon) is created on the volume so
// the drive gets a friendly label in Explorer. If the ISO already ships
// an autorun.inf (e.g. ReactOS), it is kept untouched:
//   'E:autorun.inf already exists - keeping it'
bool FormatWorker::createExtendedLabelFiles(const QString &mountPoint) {
    if (!m_config.extendedLabel)
        return true;

    // Only applies to Windows-style file systems (like original Rufus).
    switch (m_config.filesystem) {
    case FileSystem::FAT16:
    case FileSystem::FAT32:
    case FileSystem::NTFS:
    case FileSystem::exFAT:
        break;
    default:
        return true;
    }

    // autorun.inf: keep any existing one from the ISO.
    QString autorunInf = mountPoint + "/autorun.inf";
    if (QFileInfo::exists(autorunInf)) {
        emit logMessage(QStringLiteral("autorun.inf already exists - keeping it"), 0);
    } else {
        QFile f(autorunInf);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit logMessage(QStringLiteral("Failed to create autorun.inf"), 2);
            return false;
        }
        f.write("[autorun]\r\n");
        if (!m_config.volumeLabel.isEmpty()) {
            f.write("label=");
            f.write(m_config.volumeLabel.toUtf8());
            f.write("\r\n");
        }
        f.write("icon=autorun.ico\r\n");
        f.close();
        emit logMessage(QStringLiteral("Created: autorun.inf"), 0);
    }

    // autorun.ico: only if the ISO did not already provide one.
    QString autorunIco = mountPoint + "/autorun.ico";
    if (QFileInfo::exists(autorunIco)) {
        emit logMessage(QStringLiteral("autorun.ico already exists - keeping it"), 0);
        return true;
    }

    // Prefer the bundled Rufus icon (res/icons/rufus.ico, embedded in the
    // binary through the Qt resource system), like the Windows original
    // ships its own icon on the volume.
    QByteArray ico;
    QFile resIco(QStringLiteral(":/icons/icons/rufus.ico"));
    if (resIco.open(QIODevice::ReadOnly)) {
        ico = resIco.readAll();
        resIco.close();
    } else {
        // Fallback (e.g. headless tests that do not embed the resource
        // file): generate a 16x16 32-bit RGBA ICO with a simple USB-stick
        // design (body + plug), so it stays a valid icon.
        const int size = 16;
        QByteArray pixels;
        pixels.fill(0, size * size * 4); // BGRA, fully transparent

        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                bool plug = (x >= 6 && x <= 9 && y >= 1 && y <= 4);
                bool body = (x >= 4 && x <= 11 && y >= 5 && y <= 13);
                bool slotHoles = ((x == 5 || x == 10) && y >= 7 && y <= 9);
                if (!plug && !body)
                    continue;
                int r, g, b;
                if (plug) { r = 110; g = 116; b = 124; }
                else if (slotHoles) { r = 130; g = 136; b = 144; }
                else {
                    bool edge = (x == 4 || x == 11 || y == 5 || y == 13);
                    if (edge) { r = 96; g = 102; b = 110; }
                    else { r = 224; g = 226; b = 230; }
                }
                int i = (y * size + x) * 4;
                pixels[i] = b; pixels[i + 1] = g; pixels[i + 2] = r; pixels[i + 3] = 255;
            }
        }

        // ICONDIR: reserved(2) + type(2) + count(2)
        ico.append((char)0).append((char)0);
        ico.append((char)1).append((char)0);
        ico.append((char)1).append((char)0);
        // ICONDIRENTRY: 16x16, 32 bpp, size, offset 22
        ico.append((char)size).append((char)size);
        ico.append((char)0).append((char)0);            // palette
        ico.append((char)1).append((char)0);            // planes
        ico.append((char)32).append((char)0);           // bpp
        const quint32 icoImageSize = 40 + size * size * 4 + size * size / 8;
        const quint32 icoOffset = 22;
        for (int i = 0; i < 4; i++) ico.append((char)((icoImageSize >> (8 * i)) & 0xFF));
        for (int i = 0; i < 4; i++) ico.append((char)((icoOffset >> (8 * i)) & 0xFF));
        // BITMAPINFOHEADER (bottom-up rows: XOR first, then AND mask)
        auto le32 = [&ico](quint32 v) {
            for (int i = 0; i < 4; i++) ico.append((char)((v >> (8 * i)) & 0xFF));
        };
        auto le16 = [&ico](quint16 v) {
            for (int i = 0; i < 2; i++) ico.append((char)((v >> (8 * i)) & 0xFF));
        };
        le32(40); le32(size); le32(size * 2); le16(1); le16(32);
        le32(0); le32(size * size * 4); le32(0); le32(0); le32(0); le32(0);
        for (int y = size - 1; y >= 0; y--)
            ico.append(pixels.mid(y * size * 4, size * 4));
        ico.append(QByteArray(size * size / 8, 0));     // AND mask: fully opaque
    }

    QFile ic(autorunIco);
    if (!ic.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit logMessage(QStringLiteral("Failed to create autorun.ico"), 2);
        return false;
    }
    ic.write(ico);
    ic.close();
    emit logMessage(QStringLiteral("Created: autorun.ico (%1 bytes)").arg(ico.size()), 0);
    return true;
}

// ─── Syslinux config redirect note ──────────────────────────────────
// No root-level syslinux.cfg redirect is created: syslinux already
// searches /boot/syslinux/syslinux.cfg natively, and an 'INCLUDE'
// redirect breaks module resolution (after INCLUDE, relative paths like
// 'COM32 whichsys.c32' resolve from the partition root), so Arch Linux
// BIOS boot would fail with 'Failed to load COM32 file whichsys.c32'.
bool FormatWorker::verifyWrite() {
    emit logMessage(QStringLiteral("Verifying written data..."), 0);

    // Compressed images are streamed through a decompressor, so the device
    // holds the *decompressed* content while this function would hash the
    // compressed source. The hashes can never match, so verification is
    // skipped for compressed images (the decompressor's exit code is already
    // checked during the write).
    bool isCompressed = (m_config.imagePath.endsWith(".gz") ||
                         m_config.imagePath.endsWith(".xz") ||
                         m_config.imagePath.endsWith(".bz2") ||
                         m_config.imagePath.endsWith(".zst") ||
                         m_config.imagePath.endsWith(".zip"));
    if (isCompressed) {
        emit logMessage(QStringLiteral("Skipping verification for compressed image "
                                       "(device holds decompressed data)"), 2);
        return true;
    }

    QByteArray sourceHash;
    {
        QFile f(m_config.imagePath);
        if (!f.open(QIODevice::ReadOnly)) {
            emit logMessage(QStringLiteral("Cannot open source image"), 1);
            return false;
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 totalSize = f.size();
        qint64 readSize = 0;
        QByteArray buf;
        buf.resize(64 * 1024);

        while (!f.atEnd() && !isCancelled()) {
            qint64 n = f.read(buf.data(), buf.size());
            if (n <= 0) break;
            hash.addData(QByteArrayView(buf.constData(), n));
            readSize += n;
            emit deviceProgress(readSize, totalSize * 2);
        }
        f.close();
        if (isCancelled()) return false;
        sourceHash = hash.result();
    }

    QByteArray deviceHash;
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 totalSize = QFileInfo(m_config.imagePath).size();
        qint64 readSize = 0;

        // Use O_DIRECT to bypass kernel cache for accurate device verification
        bool directIo = false;
        int fd = open(m_config.targetDevice.path.toUtf8().constData(), O_RDONLY | O_DIRECT);
        if (fd < 0) {
            // Fall back to buffered I/O if O_DIRECT not supported
            fd = open(m_config.targetDevice.path.toUtf8().constData(), O_RDONLY);
            if (fd < 0) {
                emit logMessage(QStringLiteral("Cannot open device for verification"), 1);
                return false;
            }
        } else {
            directIo = true;
        }

        // O_DIRECT requires aligned buffer (512-byte alignment)
        void *alignedBuf = nullptr;
        if (posix_memalign(&alignedBuf, 512, 64 * 1024) != 0) {
            close(fd);
            emit logMessage(QStringLiteral("Memory allocation failed for verification"), 1);
            return false;
        }

        while (readSize < totalSize && !isCancelled()) {
            // O_DIRECT also requires the read length to be 512-aligned, so
            // round up the request; the hash input is capped at the real
            // remaining size so extra bytes read are not counted.
            qint64 remaining = totalSize - readSize;
            qint64 toRead = qMin(static_cast<qint64>(64 * 1024),
                                 ((remaining + 511) / 512) * 512);
            qint64 n = read(fd, alignedBuf, toRead);
            if (n <= 0) break;
            qint64 hashLen = qMin(n, remaining);
            hash.addData(QByteArrayView(static_cast<const char*>(alignedBuf), hashLen));
            readSize += hashLen;
            emit deviceProgress(totalSize + readSize, totalSize * 2);
        }
        std::free(alignedBuf);
        close(fd);
        if (isCancelled()) return false;
        deviceHash = hash.result();
    }

    emit logMessage(QStringLiteral("Source hash: %1").arg(Hash::toString(sourceHash)), 0);
    emit logMessage(QStringLiteral("Device hash: %1").arg(Hash::toString(deviceHash)), 0);

    return sourceHash == deviceHash;
}

// ─── Install bootloader ────────────────────────────────────────────
bool FormatWorker::installBootloader(const QString &existingMountPoint) {
    QString mountPoint = existingMountPoint;
    bool needsUnmount = false;

    QString fsType;
    switch (m_config.filesystem) {
    case FileSystem::FAT16:
    case FileSystem::FAT32: fsType = "vfat"; break;
    case FileSystem::NTFS:  fsType = "ntfs-3g"; break;
    case FileSystem::exFAT: fsType = "exfat"; break;
    case FileSystem::ext4:  fsType = "ext4"; break;
    default: fsType = "vfat"; break;
    }

    if (mountPoint.isEmpty()) {
        QString partPath = mainPartitionPath();
        if (!waitForPartition(partPath, 10000)) {
            emit logMessage(QStringLiteral("Partition not available for bootloader"), 1);
            return false;
        }
        mountPoint = Mounter::createTempMountPoint();
        if (mountPoint.isEmpty()) {
            emit logMessage(QStringLiteral("Failed to create mount point for bootloader"), 1);
            return false;
        }

        if (!Mounter::mount(partPath, mountPoint, fsType)) {
            emit logMessage(QStringLiteral("Failed to mount for bootloader"), 1);
            Mounter::removeMountPoint(mountPoint);
            return false;
        }
        needsUnmount = true;
    }

    bool result = false;
    QString bootMsg;

    if (m_config.bootloaderType == "syslinux") {
        // Original Rufus order: the partition is formatted, then Syslinux
        // is installed on the FAT volume ('Installing Syslinux ...',
        // 'Successfully wrote ldlinux.sys', 'Successfully wrote Syslinux
        // boot record'). Linux builds of syslinux(1) only accept a block
        // device (or file), not a mounted directory, so the volume is
        // unmounted, installed and remounted — the same trick the original
        // app uses for its NTFS "remount or you don't boot" step.
        Mounter::unmount(mountPoint);
        BootloaderResult r = BootloaderInstaller::installSyslinux(
            m_config.targetDevice.path, mainPartitionPath(), nullptr,
            [this]() { return isCancelled(); });
        result = r.success;
        bootMsg = r.errorMessage;
        if (!Mounter::mount(mainPartitionPath(), mountPoint, fsType)) {
            emit logMessage(QStringLiteral("Failed to remount after syslinux install"), 1);
            result = false;
        } else {
            emit logMessage(QStringLiteral("Volume remounted after syslinux install"), 0);
        }
    } else if (m_config.bootloaderType == "grub2") {
        BootloaderResult r = BootloaderInstaller::installGrub2(
            m_config.targetDevice.path, mountPoint, nullptr,
            [this]() { return isCancelled(); });
        result = r.success;
        bootMsg = r.errorMessage;
    } else if (m_config.bootloaderType == "mbr") {
        BootloaderResult r = BootloaderInstaller::installMbr(
            m_config.targetDevice.path, nullptr);
        result = r.success;
        bootMsg = r.errorMessage;
    } else if (m_config.bootloaderType == "grub4dos") {
        BootloaderResult r = BootloaderInstaller::installMbr(
            m_config.targetDevice.path, nullptr);
        if (r.success) {
            const QString grldrSrc = resolveDataFile(QStringLiteral("grldr"));
            if (!grldrSrc.isEmpty())
                QFile::copy(grldrSrc, mountPoint + "/grldr");
            result = true;
        } else {
            bootMsg = r.errorMessage;
        }
    } else if (m_config.bootloaderType == "uefintfs") {
        BootloaderResult r = BootloaderInstaller::installUefiNtfs(
            m_config.targetDevice.path, mountPoint, nullptr);
        result = r.success;
        bootMsg = r.errorMessage;
    }

    emit logMessage(QStringLiteral("Bootloader (%1): %2")
        .arg(m_config.bootloaderType)
        .arg(result ? QStringLiteral("installed") : bootMsg),
        result ? 0 : 1);

    if (needsUnmount) {
        Mounter::unmount(mountPoint);
        Mounter::removeMountPoint(mountPoint);
    }

    return result;
}

bool FormatWorker::applyUnattendCustomization(const QString &mountPoint) {
    if (m_config.imagePath.isEmpty()) {
        emit logMessage(QStringLiteral("No image path for unattend customization"), 2);
        return true;
    }

    ImageInfo imgInfo = detectImage();
    if (!imgInfo.hasWindows()) {
        emit logMessage(QStringLiteral("Not a Windows image, skipping unattend"), 2);
        return true;
    }

    // Check if WUE is enabled in config
    if (!m_config.wue.enabled) {
        emit logMessage(QStringLiteral("Windows User Experience customization disabled"), 2);
        return true;
    }

    emit statusChanged(tr("Applying Windows unattended installation..."));
    emit logMessage(QStringLiteral("Generating unattend.xml with WUE settings"), 0);

    auto genXml = [](const WueConfig &wue) -> QString {
        QString xml;
        xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        xml += "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n";
        xml += "    <settings pass=\"windowsPE\">\n";
        xml += "        <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"amd64\"";
        xml += " publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n";
        if (wue.bypassTpm && wue.bypassSecureBoot && wue.bypassRam) {
            xml += "            <RunSynchronous>\n";
            xml += "                <RunSynchronousCommand wcm:action=\"add\">\n";
            xml += "                    <Path>reg add \"HKLM\\System\\Setup\\LabConfig\"";
            xml += " /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>\n";
            xml += "                    <Order>1</Order>\n";
            xml += "                </RunSynchronousCommand>\n";
            xml += "                <RunSynchronousCommand wcm:action=\"add\">\n";
            xml += "                    <Path>reg add \"HKLM\\System\\Setup\\LabConfig\"";
            xml += " /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>\n";
            xml += "                    <Order>2</Order>\n";
            xml += "                </RunSynchronousCommand>\n";
            xml += "                <RunSynchronousCommand wcm:action=\"add\">\n";
            xml += "                    <Path>reg add \"HKLM\\System\\Setup\\LabConfig\"";
            xml += " /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path>\n";
            xml += "                    <Order>3</Order>\n";
            xml += "                </RunSynchronousCommand>\n";
            xml += "            </RunSynchronous>\n";
        } else {
            if (wue.bypassTpm)
                xml += "            <RunSynchronous><RunSynchronousCommand wcm:action=\"add\">"
                       "<Path>reg add \"HKLM\\System\\Setup\\LabConfig\" /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>"
                       "<Order>1</Order></RunSynchronousCommand></RunSynchronous>\n";
            if (wue.bypassSecureBoot)
                xml += "            <RunSynchronous><RunSynchronousCommand wcm:action=\"add\">"
                       "<Path>reg add \"HKLM\\System\\Setup\\LabConfig\" /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>"
                       "<Order>1</Order></RunSynchronousCommand></RunSynchronous>\n";
            if (wue.bypassRam)
                xml += "            <RunSynchronous><RunSynchronousCommand wcm:action=\"add\">"
                       "<Path>reg add \"HKLM\\System\\Setup\\LabConfig\" /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path>"
                       "<Order>1</Order></RunSynchronousCommand></RunSynchronous>\n";
        }
        if (wue.bypassNro)
            xml += "            <RunSynchronous><RunSynchronousCommand wcm:action=\"add\">"
                   "<Path>reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f</Path>"
                   "<Order>1</Order></RunSynchronousCommand></RunSynchronous>\n";
        xml += "        </component>\n";
        xml += "    </settings>\n";
        xml += "    <settings pass=\"oobeSystem\">\n";
        xml += "        <component name=\"Microsoft-Windows-Shell-Setup\"";
        xml += " processorArchitecture=\"amd64\"";
        xml += " publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n";
        if (wue.skipMicrosoftAccount) {
            xml += "            <OOBE>\n";
            xml += "                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n";
            xml += "                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n";
            xml += "                <ProtectYourPC>3</ProtectYourPC>\n";
            xml += "                <SkipMachineOOBE>true</SkipMachineOOBE>\n";
            xml += "                <SkipUserOOBE>true</SkipUserOOBE>\n";
            xml += "            </OOBE>\n";
        }
        if (wue.disableBitLocker) {
            xml += "            <BitLocker>\n";
            xml += "                <BitLockerAutoEnable>false</BitLockerAutoEnable>\n";
            xml += "            </BitLocker>\n";
        }
        if (!wue.localAccountName.isEmpty()) {
            xml += "            <AutoLogon><Enabled>true</Enabled>";
            xml += QString("<Username>%1</Username>").arg(wue.localAccountName);
            xml += "<LogonCount>1</LogonCount></AutoLogon>\n";
            xml += "            <UserAccounts><LocalAccounts>";
            xml += "<LocalAccount wcm:action=\"add\">";
            xml += QString("<Name>%1</Name>").arg(wue.localAccountName);
            if (!wue.localAccountPassword.isEmpty())
                xml += QString("<Password><Value>%1</Value><PlainText>true</PlainText></Password>").arg(wue.localAccountPassword);
            xml += "<Group>Administrators</Group></LocalAccount></LocalAccounts></UserAccounts>\n";
        }
        if (wue.disablePrivacySettings) {
            xml += "            <OOBE>";
            xml += "<HideEULAPage>true</HideEULAPage>";
            xml += "<SkipMachineOOBE>true</SkipMachineOOBE>";
            xml += "<SkipUserOOBE>true</SkipUserOOBE></OOBE>\n";
        }
        xml += "        </component>\n";
        xml += "    </settings>\n";
        xml += "</unattend>\n";
        return xml;
    };

    QString unattendContent = genXml(m_config.wue);

    // Determine where to write - Windows setup searches for autounattend.xml
    // Priority: root of USB -> /sources/winsetupinstall.inf -> /sources/unattend.xml
    QStringList candidatePaths = {
        mountPoint + "/autounattend.xml",
        mountPoint + "/AUTOUNATTEND.XML",
        mountPoint + "/unattend.xml",
        mountPoint + "/UNATTEND.XML",
        mountPoint + "/sources/unattend.xml",
        mountPoint + "/sources/WinPreinstall.xml"
    };

    QString targetPath;
    for (const QString &p : candidatePaths) {
        if (QFileInfo::exists(p)) {
            targetPath = p;
            break;
        }
    }

    if (targetPath.isEmpty()) {
        // Write to autounattend.xml at root (highest priority)
        targetPath = mountPoint + "/autounattend.xml";
    }

    QFile f(targetPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit logMessage(QStringLiteral("Failed to write unattend.xml: %1").arg(targetPath), 1);
        return false;
    }
    f.write(unattendContent.toUtf8());
    f.close();

    emit logMessage(QStringLiteral("Wrote unattend.xml to %1 (%2 bytes)")
        .arg(targetPath).arg(unattendContent.size()), 0);

    // Also handle BypassNRO via cfg file (Windows 11 22H2+)
    if (m_config.wue.bypassNro) {
        QString bypassDir = mountPoint + "/sources";
        QDir().mkpath(bypassDir);
        QFile bypass(bypassDir + "/bypassnro");
        if (bypass.open(QIODevice::WriteOnly)) {
            bypass.write("_");
            bypass.close();
            emit logMessage(QStringLiteral("Created bypassnro flag file"), 0);
        }
    }

    return true;
}

// ─── Write SBR (partition boot record) ─────────────────────────────
bool FormatWorker::writeSbr() {
    emit statusChanged(tr("Writing partition boot record..."));
    QString partPath = mainPartitionPath();

    // SBR/Volume Boot Record is handled by the bootloader installer:
    // - syslinux --install writes VBR to the partition
    // - grub-install writes boot code to both MBR and partition
    if (m_config.bootloaderType == "syslinux" || m_config.bootloaderType == "grub2" ||
        m_config.bootloaderType == "freedos") {
        emit logMessage(QStringLiteral("SBR will be written by bootloader installer"), 0);
        return true;
    }

    // For grub4dos: write grldr.mbr to partition boot sector
    if (m_config.bootloaderType == "grub4dos") {
        const QString grldrMbr = resolveDataFile(QStringLiteral("grldr.mbr"));
        if (!grldrMbr.isEmpty()) {
            QProcess dd;
            dd.start("dd", {"if=" + grldrMbr, "of=" + partPath,
                             "bs=512", "count=1", "status=none", "conv=notrunc"});
            if (dd.waitForFinished(10000) && dd.exitCode() == 0) {
                emit logMessage(QStringLiteral("GRUB4DOS SBR written"), 0);
                return true;
            }
        }
        emit logMessage(QStringLiteral("grldr.mbr not found, SBR may be missing"), 2);
        return true;
    }

    // For MS-DOS / plain MBR / FreeDOS: write Windows-compatible PBR via ms-sys to partition
    if (m_config.bootloaderType == "mbr" || m_config.bootloaderType == "msdos") {
        QProcess msys;
        // Try ms-sys with partition option
        msys.start("ms-sys", {"--partition", partPath});
        if (msys.waitForFinished(10000) && msys.exitCode() == 0) {
            emit logMessage(QStringLiteral("SBR (PBR) written via ms-sys to partition"), 0);
            return true;
        }
        // Fallback: write generic FAT/NTFS boot sector using dd from template
        emit logMessage(QStringLiteral("ms-sys not available, using mkfs VBR"), 2);
    }

    // For all other cases, the VBR written by mkfs during format is used
    emit logMessage(QStringLiteral("SBR: using VBR from filesystem format"), 0);
    return true;
}

// ─── Bootloader version matching ───────────────────────────────────
bool FormatWorker::matchBootloaderVersion(const QString &mountPoint, const ImageInfo &imgInfo) {
    if (imgInfo.bootloaderVersion == 0 && imgInfo.slVersion == 0) {
        emit logMessage(QStringLiteral("No bootloader version found in image"), 0);
        return true;
    }

    uint32_t imgVersion = imgInfo.bootloaderVersion;
    if (imgVersion == 0)
        imgVersion = imgInfo.slVersion; // Fall back to syslinux version

    m_config.bootloaderVersion = imgVersion;
    m_config.bootloaderVersionStr = QStringLiteral("%1.%2")
        .arg(imgVersion >> 8).arg(imgVersion & 0xFF);

    emit logMessage(QStringLiteral("Image bootloader version: %1")
        .arg(m_config.bootloaderVersionStr), 0);

    // Check if syslinux version on this system is compatible
    if (m_config.bootloaderType == "syslinux") {
        QProcess proc;
        proc.start("syslinux", {"--version"});
        if (proc.waitForFinished(5000) && proc.exitCode() == 0) {
            QString verStr = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            emit logMessage(QStringLiteral("System syslinux version: %1").arg(verStr), 0);

            // Warn when the system syslinux is older than the image's:
            // the image's modules may not be compatible with it.
            QRegularExpression re(QStringLiteral("(\\d+)\\.(\\d+)"));
            QRegularExpressionMatch m = re.match(verStr);
            if (m.hasMatch()) {
                uint32_t sysVer = (m.captured(1).toUInt() << 8) |
                                  (m.captured(2).toUInt() & 0xFF);
                if (sysVer < imgVersion) {
                    emit logMessage(QStringLiteral(
                        "WARNING: system syslinux (%1) is older than the image's (%2); "
                        "the bootloader may not work correctly")
                        .arg(verStr, m_config.bootloaderVersionStr), 2);
                }
            }
        }
    }

    return true;
}

bool FormatWorker::downloadBootloaderIfNeeded(const QString &mountPoint) {
    // Check if bootloader type needs external files not present on system
    if (m_config.bootloaderType == "grub4dos") {
        const QString grldrPath = resolveDataFile(QStringLiteral("grldr"));
        const QString grldrMbrPath = resolveDataFile(QStringLiteral("grldr.mbr"));

        if (grldrPath.isEmpty() || grldrMbrPath.isEmpty()) {
            emit logMessage(QStringLiteral("GRUB4DOS files not found, attempting download..."), 0);

            // Try to download from GRUB4DOS repo
            QProcess wget;
            const QString dlDir = dataDir();
            wget.start("wget", {"-q", "-O", dlDir + "/grldr.zip",
                                 "https://github.com/chenall/grub4dos/releases/download/v0.4.6a/grub4dos-0.4.6a.zip"});
            if (!missingToolMessage(wget, "wget").isEmpty()) {
                emit logMessage(missingToolMessage(wget, "wget"), 1);
                return false;
            }
            if (!finishProcess(wget, 30000, [this]() { return isCancelled(); }) &&
                wget.exitStatus() == QProcess::NormalExit && wget.exitCode() == 0) {
                QProcess unzip;
                unzip.start("unzip", {"-o", dlDir + "/grldr.zip",
                                       "grldr", "grldr.mbr", "-d", dlDir});
                if (!missingToolMessage(unzip, "unzip").isEmpty()) {
                    emit logMessage(missingToolMessage(unzip, "unzip"), 1);
                    return false;
                }
                if (!finishProcess(unzip, 10000, [this]() { return isCancelled(); }) &&
                    unzip.exitStatus() == QProcess::NormalExit && unzip.exitCode() == 0) {
                    QFile::remove(dlDir + "/grldr.zip");
                    emit logMessage(QStringLiteral("GRUB4DOS downloaded successfully"), 0);
                    return true;
                }
            }
            if (isCancelled())
                return false;
            emit logMessage(QStringLiteral("Failed to download GRUB4DOS. Will use bundled files."), 2);
            return false;
        }
    }

    if (m_config.bootloaderType == "uefintfs") {
        const QString uefiNtfsPath = resolveDataFile(QStringLiteral("uefi-ntfs.img"));
        if (uefiNtfsPath.isEmpty()) {
            emit logMessage(QStringLiteral("UEFI:NTFS image not found, attempting download..."), 0);

            QProcess wget;
            const QString dlDir = dataDir();
            wget.start("wget", {"-q", "-O", dlDir + "/uefi-ntfs.zip",
                                 "https://github.com/pbatard/UEFI-NTFS/releases/latest/download/UEFI-NTFS.zip"});
            if (!missingToolMessage(wget, "wget").isEmpty()) {
                emit logMessage(missingToolMessage(wget, "wget"), 1);
                return false;
            }
            if (!finishProcess(wget, 30000, [this]() { return isCancelled(); }) &&
                wget.exitStatus() == QProcess::NormalExit && wget.exitCode() == 0) {
                QProcess unzip;
                unzip.start("unzip", {"-o", dlDir + "/uefi-ntfs.zip",
                                       "uefi-ntfs.img", "-d", dlDir});
                if (!missingToolMessage(unzip, "unzip").isEmpty()) {
                    emit logMessage(missingToolMessage(unzip, "unzip"), 1);
                    return false;
                }
                if (!finishProcess(unzip, 10000, [this]() { return isCancelled(); }) &&
                    unzip.exitStatus() == QProcess::NormalExit && unzip.exitCode() == 0) {
                    QFile::remove(dlDir + "/uefi-ntfs.zip");
                    emit logMessage(QStringLiteral("UEFI:NTFS downloaded successfully"), 0);
                    return true;
                }
            }
            if (isCancelled())
                return false;
            emit logMessage(QStringLiteral("Failed to download UEFI:NTFS"), 2);
            return false;
        }
    }

    return true;
}

// ─── NTFS checkdisk ────────────────────────────────────────────────
bool FormatWorker::ntfsCheckDisk() {
    QString partPath = mainPartitionPath();
    emit logMessage(QStringLiteral("Running ntfsfix on %1").arg(partPath), 0);
    QProcess ntfsfix;
    ntfsfix.start("ntfsfix", {partPath});
    if (finishProcess(ntfsfix, 30000, [this]() { return isCancelled(); })) {
        emit logMessage(QStringLiteral("ntfsfix cancelled"), 2);
        return false;
    }
    if (!missingToolMessage(ntfsfix, "ntfsfix").isEmpty()) {
        emit logMessage(missingToolMessage(ntfsfix, "ntfsfix"), 1);
        return false;
    }
    if (ntfsfix.exitStatus() != QProcess::NormalExit) {
        emit logMessage(QStringLiteral("ntfsfix timed out"), 1);
        return false;
    }
    QString output = QString::fromUtf8(ntfsfix.readAllStandardOutput()).trimmed();
    emit logMessage(QStringLiteral("ntfsfix output: %1").arg(output), 0);
    return ntfsfix.exitCode() == 0;
}
