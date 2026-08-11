#pragma once

#include <QString>
#include <QList>
#include "core/ImageHandler.h"

enum class PartitionScheme {
    MBR,
    GPT,
    MBRForUEFI   // MBR with protective UEFI marker (0x49464555)
};

enum class FileSystem {
    Unknown,
    FAT16,
    FAT32,
    exFAT,
    NTFS,
    UDF,
    ReFS,
    ext2,
    ext3,
    ext4,
    btrfs,
    XFS,
    F2FS
};

// Extra partition flags matching Rufus original
enum ExtraPartitionFlag : uint8_t {
    XP_NONE       = 0x00,
    XP_PERSISTENCE = 0x01,
    XP_ESP        = 0x02,
    XP_MSR        = 0x04,
    XP_UEFI_NTFS  = 0x08,
    XP_COMPAT     = 0x10,
    XP_DATA       = 0x20
};

struct PartitionLayout {
    PartitionScheme scheme = PartitionScheme::MBR;
    FileSystem fs = FileSystem::FAT32;
    qint64 partitionSize = 0;       // 0 = use all remaining space
    QString label;
    QString espLabel = QStringLiteral("ESP");
    // 260 MB ESP by default, like the original (drive.c:2291): keeps 4K
    // sector users and macOS happy.
    qint64 espSizeMB = 260;         // Default ESP size in MB
    qint64 persistenceSizeMB = 4096; // Default persistence size in MB
    qint64 offsetSectors = 2048;
    qint64 clusterSizeBytes = 0;    // Align main partition size to this (0 = no alignment)
    uint8_t extraPartitions = XP_NONE;  // Bitmask of ExtraPartitionFlag
    bool mbrUefiMarker = false;     // Write MBR UEFI marker for MBR+UEFI
};

struct PartitionInfo {
    QString path;
    QString filesystem;
    qint64 size = 0;
    qint64 offset = 0;
    int number = 0;
    QString label;
    bool isEsp = false;
};

struct ClusterSizeInfo {
    qint64 allowedMask = 0;
    qint64 defaultSize = 0;
};

class PartitionManager {
public:
    static bool clearDevice(const QString &devicePath);
    static bool createPartitionTable(const QString &devicePath, PartitionScheme scheme);
    static bool createPartition(const QString &devicePath, const PartitionLayout &layout);
    static bool deleteAllPartitions(const QString &devicePath);
    static bool refreshPartitionTable(const QString &devicePath);
    static QList<PartitionInfo> listPartitions(const QString &devicePath);
    static qint64 getDeviceSize(const QString &devicePath);

    // Rufus-style full partition creation
    static bool createRufusPartitions(const QString &devicePath, const PartitionLayout &layout);
    static bool writeMbrUefiMarker(const QString &devicePath);

    // Dynamic cluster size calculation (matching original Rufus logic)
    static ClusterSizeInfo getClusterSizes(FileSystem fs, qint64 diskSize);
    static QStringList getClusterSizeLabels(FileSystem fs, qint64 diskSize);
    static int getDefaultClusterIndex(FileSystem fs, qint64 diskSize);
    static int getClusterSizeFromIndex(FileSystem fs, qint64 diskSize, int index);

    // Dynamic filesystem filtering (matching original Rufus)
    static QList<FileSystem> getAllowedFileSystems(BootType bootType,
        const QString &imagePath, bool advancedFormat);

    // Utility
    static QString schemeToString(PartitionScheme s);
    static PartitionScheme schemeFromString(const QString &s);
    static QString fsToString(FileSystem fs);
    static FileSystem fsFromString(const QString &s);
    static bool isSupportedOnLinux(FileSystem fs);

    static QString partitionDevice(const QString &devicePath, int num);

private:
    static bool execParted(const QStringList &args);
    static bool execSfdisk(const QStringList &args);
    static bool execBlockdev(const QStringList &args);
    static bool writeMbr(const QString &devicePath, const QByteArray &mbrData);
};
