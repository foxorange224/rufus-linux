#include "PartitionManager.h"
#include "core/ImageHandler.h"
#include "core/QProc.h"
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDebug>
#include <QObject>

static constexpr qint64 kSectorSize = 512;
static constexpr qint64 kMib = 1024LL * 1024;
static constexpr qint64 kGib = 1024LL * kMib;
static constexpr qint64 kKB = 1024LL;
static constexpr qint64 kMB = 1024LL * 1024;
static constexpr qint64 kGB = 1024LL * kMB;
static constexpr qint64 kTB = 1024LL * kGB;
static constexpr qint64 kLARGE_FAT32_SIZE = 32LL * kGB;
static constexpr qint64 kMAX_FAT32_SIZE = 2LL * kTB;
static constexpr qint64 kFAT32_CLUSTER_THRESHOLD = 1011; // 1.011 * 1000
static constexpr qint64 kMIN_EXT_SIZE = 256LL * kMB;
static constexpr qint64 kSINGLE_CLUSTERSIZE_DEFAULT = 0x00000100;

static const char* kClusterSizeLabels[] = {
    "512 bytes", "1024 bytes", "2048 bytes", "4096 bytes",
    "8192 bytes", "16 KB", "32 KB", "64 KB",
    "128 KB", "256 KB", "512 KB", "1024 KB",
    "2048 KB", "4096 KB", "8192 KB", "16384 KB",
    "32768 KB", "65536 KB"
};

ClusterSizeInfo PartitionManager::getClusterSizes(FileSystem fs, qint64 diskSize) {
    ClusterSizeInfo info;
    info.allowedMask = 0;
    info.defaultSize = 0;

    switch (fs) {
    case FileSystem::FAT16:
        if (diskSize < 4 * kGB) {
            info.allowedMask = 0x00001E00;
            for (qint64 i = 32; i <= 4096; i <<= 1) {
                if (diskSize < i * kMB) {
                    info.defaultSize = 16 * i * kKB;
                    break;
                }
                info.allowedMask <<= 1;
            }
            info.allowedMask &= 0x0001FE00;
        }
        break;

    case FileSystem::FAT32:
        if (diskSize >= 32 * kMB && diskSize < kMAX_FAT32_SIZE) {
            info.allowedMask = 0x000001F8;
            for (qint64 i = 32; i <= (32 * 1024); i <<= 1) {
                if (diskSize * 1000.0f < i * kMB * kFAT32_CLUSTER_THRESHOLD) {
                    info.defaultSize = 8 * i * kKB;
                    break;
                }
                info.allowedMask <<= 1;
            }
            info.allowedMask &= 0x0001FE00;

            if (diskSize >= 256 * kMB && diskSize < 32 * kGB) {
                for (qint64 i = 8; i <= 32; i <<= 1) {
                    if (diskSize * 1000.0f < i * kGB * kFAT32_CLUSTER_THRESHOLD) {
                        info.defaultSize = (i / 2) * kKB;
                        break;
                    }
                }
            }
            if (diskSize >= 32 * kGB) {
                info.allowedMask &= 0x0001C000;
                info.defaultSize = 0x00008000;
            }
        }
        break;

    case FileSystem::NTFS:
        if (diskSize < 256 * kTB) {
            info.allowedMask = 0x0001F000;
            for (qint64 i = 16; i <= 256; i <<= 1) {
                if (diskSize < i * kTB) {
                    info.defaultSize = (i / 4) * kKB;
                    break;
                }
            }
        }
        break;

    case FileSystem::exFAT:
        if (diskSize < 256 * kTB) {
            info.allowedMask = 0x03FFFE00;
            if (diskSize < 256 * kMB)
                info.defaultSize = 4 * kKB;
            else if (diskSize < 32 * kGB)
                info.defaultSize = 32 * kKB;
            else
                info.defaultSize = 128 * kKB;
        }
        break;

    case FileSystem::UDF:
        info.allowedMask = kSINGLE_CLUSTERSIZE_DEFAULT;
        info.defaultSize = 1;
        break;

    case FileSystem::ext2:
    case FileSystem::ext3:
    case FileSystem::ext4:
        if (diskSize >= kMIN_EXT_SIZE) {
            info.allowedMask = kSINGLE_CLUSTERSIZE_DEFAULT;
            info.defaultSize = 1;
        }
        break;

    default:
        info.allowedMask = 0x00000100;
        info.defaultSize = 0;
        break;
    }

    return info;
}

QStringList PartitionManager::getClusterSizeLabels(FileSystem fs, qint64 diskSize) {
    QStringList labels;
    ClusterSizeInfo csi = getClusterSizes(fs, diskSize);
    if (csi.allowedMask == 0) {
        labels << QObject::tr("Default");
        return labels;
    }

    labels << QObject::tr("Default");
    for (int i = 0; i < 18; i++) {
        qint64 clusterSize = (i < 13) ? (512LL << i) : (8192LL << (i - 13));
        if (clusterSize == 0) continue;
        if (csi.allowedMask & clusterSize) {
            QString label = kClusterSizeLabels[i];
            if (clusterSize == csi.defaultSize)
                label = QObject::tr("%1 (Default)").arg(label);
            labels << label;
        }
    }
    return labels;
}

int PartitionManager::getDefaultClusterIndex(FileSystem fs, qint64 diskSize) {
    ClusterSizeInfo csi = getClusterSizes(fs, diskSize);
    if (csi.defaultSize == 0 || csi.allowedMask == 0)
        return 0;

    int index = 0;
    for (int i = 0; i < 18; i++) {
        qint64 clusterSize = (i < 13) ? (512LL << i) : (8192LL << (i - 13));
        if (clusterSize == 0) continue;
        if ((csi.allowedMask & clusterSize) == 0) continue;
        index++;
        if (clusterSize == csi.defaultSize)
            return index;
    }
    return 0;
}

// Returns the cluster size in KiB selected by the combo index, or 0 for
// "Default" (let mkfs decide).
int PartitionManager::getClusterSizeFromIndex(FileSystem fs, qint64 diskSize, int index) {
    ClusterSizeInfo csi = getClusterSizes(fs, diskSize);
    if (csi.allowedMask == 0 || index <= 0)
        return 0;

    int idx = 0;
    for (int i = 0; i < 18; i++) {
        qint64 clusterSize = (i < 13) ? (512LL << i) : (8192LL << (i - 13));
        if (clusterSize == 0) continue;
        if ((csi.allowedMask & clusterSize) == 0) continue;
        idx++;
        if (idx == index)
            return static_cast<int>(clusterSize / kKB);
    }
    return 0;
}

QList<FileSystem> PartitionManager::getAllowedFileSystems(BootType bootType,
    const QString &imagePath, bool advancedFormat)
{
    QList<FileSystem> allowed;
    switch (bootType) {
    case BootType::NonBootable:
        allowed = { FileSystem::FAT16, FileSystem::FAT32, FileSystem::exFAT,
                    FileSystem::NTFS, FileSystem::UDF };
        if (advancedFormat)
            allowed << FileSystem::ext2 << FileSystem::ext3 << FileSystem::ext4;
        break;
    case BootType::MSDOS:
    case BootType::FreeDOS:
        allowed = { FileSystem::FAT16, FileSystem::FAT32 };
        break;
    case BootType::Image:
        allowed = { FileSystem::FAT16, FileSystem::FAT32, FileSystem::NTFS, FileSystem::exFAT };
        if (advancedFormat) {
            allowed << FileSystem::ext2 << FileSystem::ext3 << FileSystem::ext4;
        }
        break;
    case BootType::SyslinuxV4:
    case BootType::SyslinuxV6:
    case BootType::Grub4Dos:
        allowed = { FileSystem::FAT16, FileSystem::FAT32, FileSystem::NTFS, FileSystem::ext2,
                    FileSystem::ext3, FileSystem::ext4 };
        break;
    case BootType::Grub2:
        allowed = { FileSystem::ext2, FileSystem::ext3, FileSystem::ext4, FileSystem::NTFS,
                    FileSystem::FAT16, FileSystem::FAT32 };
        break;
    case BootType::UefiNtfs:
        allowed = { FileSystem::NTFS, FileSystem::exFAT };
        break;
    case BootType::ReactOS:
        allowed = { FileSystem::FAT16, FileSystem::FAT32, FileSystem::NTFS };
        break;
    default:
        allowed = { FileSystem::FAT32, FileSystem::NTFS, FileSystem::exFAT,
                    FileSystem::ext4, FileSystem::ext2, FileSystem::ext3,
                    FileSystem::FAT16, FileSystem::UDF };
    }
    return allowed;
}

QString PartitionManager::partitionDevice(const QString &devicePath, int num) {
    // Loop devices use the same p-suffix convention as NVMe/MMC
    // (/dev/loop0p1), while SATA/SCSI use /dev/sda1.
    if (devicePath.contains("nvme") || devicePath.contains("mmcblk") ||
        devicePath.contains("loop"))
        return QStringLiteral("%1p%2").arg(devicePath).arg(num);
    return QStringLiteral("%1%2").arg(devicePath).arg(num);
}

bool PartitionManager::clearDevice(const QString &devicePath) {
    // Same as original Rufus: "Zeroed 8 MB at the top of the drive"
    // (clears MBR/PBR/GPT headers, partition tables and boot code).
    QProcess dd;
    dd.start("dd", {"if=/dev/zero", "of=" + devicePath, "bs=1M", "count=8", "status=none"});
    if (!dd.waitForFinished(30000))
        return false;
    bool ok = (dd.exitCode() == 0);

    // Same as original Rufus: "Zeroed 1 MB at the end of the drive"
    // (clears the GPT backup header + partition entries and any
    // trailing PBR/boot structures).
    qint64 devSize = getDeviceSize(devicePath);
    if (devSize > 0 && devSize >= 2LL * kMib) {
        qint64 lastSector = devSize / 512;
        QProcess dd2;
        dd2.start("dd", {"if=/dev/zero", "of=" + devicePath, "bs=512",
                          "seek=" + QString::number(lastSector - 2048),
                          "count=2048", "status=none"});
        dd2.waitForFinished(30000);
    }

    // Wipe filesystem signatures
    QProcess wipefs;
    wipefs.start("wipefs", {"-a", devicePath});
    if (!wipefs.waitForFinished(10000))
        return false;

    return ok;
}

bool PartitionManager::createPartitionTable(const QString &devicePath, PartitionScheme scheme) {
    QString typeStr;
    switch (scheme) {
    case PartitionScheme::GPT: typeStr = "gpt"; break;
    case PartitionScheme::MBRForUEFI:
    case PartitionScheme::MBR: typeStr = "dos"; break;
    }

    // Use parted for GPT (more reliable), sfdisk for MBR
    if (scheme == PartitionScheme::GPT) {
        if (execParted({"-s", devicePath, "mklabel", "gpt"}))
            return true;
        // Fallback
    }

    // Feed the label script directly to the first sfdisk run: without
    // input, sfdisk waits on stdin forever and a leftover hung process
    // holds the device open, making every retry fail with "device busy".
    QProcess sfdisk;
    sfdisk.start("sfdisk", {"--wipe", "always", "--wipe-partitions", "always", devicePath});
    sfdisk.write(QByteArray("label: ") + typeStr.toUtf8() + "\n");
    sfdisk.closeWriteChannel();
    if (sfdisk.waitForFinished(10000) && sfdisk.exitCode() == 0)
        goto done;
    if (sfdisk.state() != QProcess::NotRunning) {
        sfdisk.kill();
        sfdisk.waitForFinished(2000);
    }

    // Fallback to parted
    if (!execParted({"-s", devicePath, "mklabel", typeStr}))
        return false;

done:
    QThread::msleep(500);

    // For MBR+UEFI, write the UEFI marker
    if (scheme == PartitionScheme::MBRForUEFI)
        writeMbrUefiMarker(devicePath);

    return true;
}

bool PartitionManager::writeMbrUefiMarker(const QString &devicePath) {
    // Write "UEFI" marker at offset 0x1B8 in the MBR (bytes 440-443)
    // This is how Rufus marks MBR for UEFI
    QFile f(devicePath);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    if (!f.seek(0x1B8))
        return false;
    // Write 'U', 'E', 'F', 'I' as 32-bit little-endian: 0x49464555
    QByteArray marker;
    marker.append((char)0x55); // 'U'
    marker.append((char)0x45); // 'E'
    marker.append((char)0x46); // 'F'
    marker.append((char)0x49); // 'I'
    if (f.write(marker) != 4)
        return false;
    f.close();
    return true;
}

bool PartitionManager::createPartition(const QString &devicePath, const PartitionLayout &layout) {
    qint64 startSector = layout.offsetSectors;
    qint64 sizeSectors;
    qint64 devSize = getDeviceSize(devicePath);
    qint64 totalSectors = devSize / 512;

    if (layout.partitionSize > 0)
        sizeSectors = layout.partitionSize / 512;
    else
        sizeSectors = totalSectors - startSector;

    // Determine partition type
    QString typeId = "83"; // Linux filesystem default
    switch (layout.fs) {
    case FileSystem::FAT16: typeId = "0E"; break; // FAT16 LBA
    case FileSystem::FAT32: typeId = "0C"; break; // FAT32 LBA
    case FileSystem::NTFS:  typeId = "07"; break; // NTFS/exFAT
    case FileSystem::exFAT: typeId = "07"; break;
    case FileSystem::ext2:
    case FileSystem::ext3:
    case FileSystem::ext4:  typeId = "83"; break;
    default: break;
    }

    // Use sfdisk for precise control
    QString script;
    if (sizeSectors > 0) {
        script = QStringLiteral("start=%1, size=%2, type=%3\n")
            .arg(startSector).arg(sizeSectors).arg(typeId);
    } else {
        script = QStringLiteral("start=%1, type=%2\n")
            .arg(startSector).arg(typeId);
    }

    QProcess sfdisk;
    sfdisk.start("sfdisk", {"-a", devicePath});
    sfdisk.write(script.toUtf8());
    sfdisk.closeWriteChannel();
    finishProcess(sfdisk, 10000);
    if (sfdisk.exitStatus() == QProcess::NormalExit && sfdisk.exitCode() == 0)
        goto done;

    // Fallback to parted
    {
        QString fsStr;
        switch (layout.fs) {
        case FileSystem::FAT16: fsStr = "fat16"; break;
        case FileSystem::FAT32: fsStr = "fat32"; break;
        case FileSystem::NTFS:  fsStr = "ntfs"; break;
        case FileSystem::ext4:
        case FileSystem::ext3:
        case FileSystem::ext2: fsStr = "ext4"; break;
        default: fsStr = "fat32"; break;
        }

        qint64 sizeMB = (layout.partitionSize > 0) ? layout.partitionSize / kMib : -1;
        QString sizeStr = (sizeMB > 0) ? QString::number(sizeMB) + "MiB" : "100%";
        QString startStr = QString::number(startSector * 512 / 1024) + "KiB";

        if (!execParted({"-s", devicePath, "mkpart", "primary", fsStr, startStr, sizeStr}))
            return false;
    }

done:
    refreshPartitionTable(devicePath);
    QThread::msleep(300);
    return true;
}

bool PartitionManager::createRufusPartitions(const QString &devicePath, const PartitionLayout &layout) {
    qint64 devSize = getDeviceSize(devicePath);
    if (devSize <= 0) return false;

    qint64 sectorSize = 512;
    qint64 totalSectors = devSize / sectorSize;

    // If MBR for UEFI or UEFI marker flag set, use the UEFI marker
    if (layout.scheme == PartitionScheme::MBRForUEFI || layout.mbrUefiMarker) {
        if (!writeMbrUefiMarker(devicePath))
            return false;
    }

    struct ExtraPart {
        QString label;
        qint64 sizeMB;
        FileSystem fs;
        bool isEsp;
        bool isMsr = false;
    };
    QList<ExtraPart> extras;

    // Determine extra partitions
    if (layout.extraPartitions & XP_PERSISTENCE) {
        extras.append({layout.label.isEmpty() ?
            (layout.fs == FileSystem::ext4 ? "casper-rw" : "persistence") : "persistence",
            layout.persistenceSizeMB, FileSystem::ext4, false});
    }
    if (layout.extraPartitions & XP_ESP) {
        extras.append({layout.espLabel, layout.espSizeMB, FileSystem::FAT32, true});
    }
    if (layout.extraPartitions & XP_UEFI_NTFS) {
        extras.append({QStringLiteral("UEFI_NTFS"), 32, FileSystem::FAT32, true});
    }
    if (layout.extraPartitions & XP_MSR) {
        // MSR partition - 128MB for GPT, must use MSR GUID and no filesystem
        extras.append({QStringLiteral("Microsoft Reserved"), 128, FileSystem::Unknown, false, true});
    }
    if (layout.extraPartitions & XP_COMPAT) {
        // Compatibility partition (BIOS boot partition for GPT)
        extras.append({QStringLiteral("BIOS_BOOT"), 8, FileSystem::FAT32, false});
    }

    // Extras are appended after the main partition, exactly like the original
    // Rufus drive.c CreatePartition for removable drives: the persistence
    // partition comes first, then the ESP (so the ESP ends up last on USB
    // sticks). Offset/size of each extra is a multiple of a track.

    // Main partition first so it ends up as partition #1
    // (FormatWorker expects the main filesystem on /dev/sdX1).
    qint64 nextStart = layout.offsetSectors;
    int partNum = 1;
    QStringList partCmds;

    // sfdisk needs the label header in the script itself: without it the
    // table silently defaults to DOS, so GPT requests would end up MBR.
    partCmds << QStringLiteral("label: %1\n").arg(
        (layout.scheme == PartitionScheme::GPT) ? "gpt" : "dos");

    qint64 extrasSectors = 0;
    for (const ExtraPart &ep : extras)
        extrasSectors += ep.sizeMB * kMib / 512;

    {
        qint64 mainSize = totalSectors - nextStart - extrasSectors - 34; // Leave GPT backup
        if (mainSize < 0) mainSize = totalSectors - nextStart - extrasSectors;
        if (mainSize <= 0) return false;
        // Make sure the main partition size is a multiple of the cluster
        // size (original drive.c:2418-2419 FLOOR_ALIGN with ClusterSize).
        if (layout.clusterSizeBytes > 0 && layout.clusterSizeBytes % 512 == 0) {
            qint64 clusterSectors = layout.clusterSizeBytes / 512;
            mainSize = (mainSize / clusterSectors) * clusterSectors;
        }

        QString typeId;
        QString gptType;
        switch (layout.fs) {
        case FileSystem::FAT16: typeId = "0E"; gptType = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"; break;
        case FileSystem::FAT32: typeId = "0C"; gptType = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"; break;
        case FileSystem::NTFS:  typeId = "07"; gptType = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"; break;
        case FileSystem::exFAT: typeId = "07"; gptType = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"; break;
        case FileSystem::ext2:
        case FileSystem::ext3:
        case FileSystem::ext4:  typeId = "83"; gptType = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"; break;
        default: typeId = "83"; gptType = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"; break;
        }

        if (layout.scheme == PartitionScheme::GPT) {
            // GPT partition name matches original Rufus ('Main Data
            // Partition'), regardless of the filesystem label.
            partCmds << QStringLiteral("start=%1, size=%2, type=%3, name=\"Main Data Partition\"\n")
                .arg(nextStart).arg(mainSize).arg(gptType);
        } else {
            // MBR: set bootable flag on main partition (0x80) for plain
            // BIOS; MBR-for-UEFI must not be marked bootable (the UEFI
            // firmware ignores 0x80 and the 0x49464555 marker is used).
            if (layout.scheme == PartitionScheme::MBR) {
                partCmds << QStringLiteral("start=%1, size=%2, type=%3, bootable\n")
                    .arg(nextStart).arg(mainSize).arg(typeId);
            } else {
                partCmds << QStringLiteral("start=%1, size=%2, type=%3\n")
                    .arg(nextStart).arg(mainSize).arg(typeId);
            }
        }
        nextStart += mainSize;
        partNum = 2;
    }

    // Create extra partitions after the main one
    for (const ExtraPart &ep : extras) {
        if (layout.scheme == PartitionScheme::GPT && ep.isEsp) {
            // EFI System Partition
            partCmds << QStringLiteral("start=%1, size=%2, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"%3\"\n")
                .arg(nextStart).arg(ep.sizeMB * kMib / 512).arg(ep.label);
        } else if (layout.scheme == PartitionScheme::GPT && ep.isMsr) {
            // Microsoft Reserved Partition
            partCmds << QStringLiteral("start=%1, size=%2, type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE, name=\"%3\"\n")
                .arg(nextStart).arg(ep.sizeMB * kMib / 512).arg(ep.label);
        } else if (layout.scheme == PartitionScheme::GPT) {
            partCmds << QStringLiteral("start=%1, size=%2, name=\"%3\"\n")
                .arg(nextStart).arg(ep.sizeMB * kMib / 512).arg(ep.label);
        } else {
            // MBR
            QString typeId = "83";
            if (ep.isEsp) typeId = "EF";
            else if (ep.fs == FileSystem::ext4) typeId = "83";
            partCmds << QStringLiteral("start=%1, size=%2, type=%3\n")
                .arg(nextStart).arg(ep.sizeMB * kMib / 512).arg(typeId);
        }
        nextStart += ep.sizeMB * kMib / 512;
        partNum++;
    }

    // Write the full partition table
    QTemporaryFile tmpScript;
    if (!tmpScript.open())
        return false;
    for (const QString &cmd : partCmds)
        tmpScript.write(cmd.toUtf8());
    tmpScript.flush();

    QProcess sfdisk;
    sfdisk.start("sfdisk", {devicePath});
    sfdisk.waitForStarted(5000);
    QFile input(tmpScript.fileName());
    if (input.open(QIODevice::ReadOnly))
        sfdisk.write(input.readAll());
    input.close();
    sfdisk.closeWriteChannel();
    finishProcess(sfdisk, 30000);
    if (sfdisk.exitStatus() != QProcess::NormalExit || sfdisk.exitCode() != 0) {
        // Fallback: create just the main partition
        PartitionLayout simple = layout;
        simple.partitionSize = 0;
        simple.offsetSectors = layout.offsetSectors;
        return createPartition(devicePath, simple);
    }

    refreshPartitionTable(devicePath);
    QThread::msleep(500);
    return true;
}

QList<PartitionInfo> PartitionManager::listPartitions(const QString &devicePath) {
    QList<PartitionInfo> partitions;
    QProcess proc;
    // -b: raw bytes, otherwise SIZE is human-readable ("2G") and cannot
    // be parsed into a qint64 (all partitions would report size 0).
    proc.start("lsblk", {"-b", "-nlo", "NAME,SIZE,TYPE,FSTYPE,LABEL", devicePath});
    if (!proc.waitForFinished(5000))
        return partitions;

    for (const QByteArray &line : proc.readAllStandardOutput().split('\n')) {
        QString s = QString::fromUtf8(line).trimmed();
        if (s.isEmpty()) continue;
        QStringList parts = s.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 3) continue;
        if (parts[2] != "part") continue;

        PartitionInfo pi;
        pi.path = "/dev/" + parts[0];
        pi.size = parts[1].toLongLong();
        if (parts.size() >= 4) pi.filesystem = parts[3];
        if (parts.size() >= 5) pi.label = parts[4];

        // Extract partition number
        QRegularExpression re("(\\d+)$");
        auto m = re.match(parts[0]);
        if (m.hasMatch()) pi.number = m.captured(1).toInt();

        partitions.append(pi);
    }
    return partitions;
}

qint64 PartitionManager::getDeviceSize(const QString &devicePath) {
    QProcess proc;
    proc.start("blockdev", {"--getsize64", devicePath});
    if (!proc.waitForFinished(5000))
        return 0;
    return proc.readAllStandardOutput().trimmed().toLongLong();
}

bool PartitionManager::deleteAllPartitions(const QString &devicePath) {
    QProcess sfdisk;
    sfdisk.start("sfdisk", {"--delete", devicePath});
    if (sfdisk.waitForFinished(10000) && sfdisk.exitCode() == 0)
        return true;

    // Fallback: wipe the device
    return clearDevice(devicePath);
}

bool PartitionManager::refreshPartitionTable(const QString &devicePath) {
    // partprobe works on loop devices where blockdev --rereadpt fails with
    // EINVAL: loop drives are only rescanned by BLKRRPART if the loop
    // driver was loaded with partition scanning enabled, while partprobe
    // triggers the block-layer uevent that creates /dev/loopXpN.
    QProcess partprobe;
    partprobe.start("partprobe", {devicePath});
    if (partprobe.waitForFinished(10000) && partprobe.exitCode() == 0)
        return true;

    QProcess blockdev;
    blockdev.start("blockdev", {"--rereadpt", devicePath});
    if (!blockdev.waitForFinished(10000))
        return false;
    return blockdev.exitCode() == 0 || blockdev.exitCode() == 1;
}

QString PartitionManager::schemeToString(PartitionScheme s) {
    switch (s) {
    case PartitionScheme::MBR: return QStringLiteral("MBR");
    case PartitionScheme::GPT: return QStringLiteral("GPT");
    case PartitionScheme::MBRForUEFI: return QStringLiteral("MBR (for UEFI)");
    }
    return {};
}

PartitionScheme PartitionManager::schemeFromString(const QString &s) {
    QString u = s.toUpper();
    if (u == "GPT") return PartitionScheme::GPT;
    if (u.contains("UEFI") || u.contains("MBR_FOR_UEFI")) return PartitionScheme::MBRForUEFI;
    return PartitionScheme::MBR;
}

QString PartitionManager::fsToString(FileSystem fs) {
    switch (fs) {
    case FileSystem::FAT16: return QStringLiteral("FAT16");
    case FileSystem::FAT32: return QStringLiteral("FAT32");
    case FileSystem::exFAT: return QStringLiteral("exFAT");
    case FileSystem::NTFS:  return QStringLiteral("NTFS");
    case FileSystem::UDF:   return QStringLiteral("UDF");
    case FileSystem::ReFS:  return QStringLiteral("ReFS");
    case FileSystem::ext2:  return QStringLiteral("ext2");
    case FileSystem::ext3:  return QStringLiteral("ext3");
    case FileSystem::ext4:  return QStringLiteral("ext4");
    case FileSystem::btrfs: return QStringLiteral("btrfs");
    case FileSystem::XFS:   return QStringLiteral("XFS");
    case FileSystem::F2FS:  return QStringLiteral("F2FS");
    default: return QStringLiteral("FAT32");
    }
}

FileSystem PartitionManager::fsFromString(const QString &s) {
    QString u = s.toUpper();
    if (u == "FAT16") return FileSystem::FAT16;
    if (u == "FAT32") return FileSystem::FAT32;
    if (u == "EXFAT") return FileSystem::exFAT;
    if (u == "NTFS")  return FileSystem::NTFS;
    if (u == "UDF")   return FileSystem::UDF;
    if (u == "REFS" || u == "REFS") return FileSystem::ReFS;
    if (u == "EXT2")  return FileSystem::ext2;
    if (u == "EXT3")  return FileSystem::ext3;
    if (u == "EXT4")  return FileSystem::ext4;
    if (u == "BTRFS") return FileSystem::btrfs;
    if (u == "XFS")   return FileSystem::XFS;
    if (u == "F2FS")  return FileSystem::F2FS;
    return FileSystem::FAT32;
}

bool PartitionManager::isSupportedOnLinux(FileSystem fs) {
    switch (fs) {
    case FileSystem::FAT16:
    case FileSystem::FAT32:
    case FileSystem::exFAT:
    case FileSystem::NTFS:
    case FileSystem::ext2:
    case FileSystem::ext3:
    case FileSystem::ext4:
        return true;
    case FileSystem::F2FS:
        return QProcess::execute("which", {"mkfs.f2fs"}) == 0;
    case FileSystem::UDF:
    case FileSystem::ReFS:
    case FileSystem::btrfs:
    case FileSystem::XFS:
        return false;
    default:
        return false;
    }
}

bool PartitionManager::execParted(const QStringList &args) {
    QProcess proc;
    proc.start("parted", args);
    if (!proc.waitForFinished(60000))
        return false;
    return proc.exitCode() == 0;
}

bool PartitionManager::execSfdisk(const QStringList &args) {
    QProcess proc;
    proc.start("sfdisk", args);
    if (!proc.waitForFinished(30000))
        return false;
    return proc.exitCode() == 0;
}

bool PartitionManager::execBlockdev(const QStringList &args) {
    QProcess proc;
    proc.start("blockdev", args);
    if (!proc.waitForFinished(10000))
        return false;
    return proc.exitCode() == 0;
}

bool PartitionManager::writeMbr(const QString &devicePath, const QByteArray &mbrData) {
    QFile f(devicePath);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    if (f.write(mbrData) != mbrData.size())
        return false;
    f.close();
    return true;
}
