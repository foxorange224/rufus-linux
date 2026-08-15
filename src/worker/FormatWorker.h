#pragma once

#include <QObject>
#include <QString>
#include <QAtomicInt>

#include "backend/DeviceManager.h"
#include "backend/PartitionManager.h"
#include "backend/DriveWriter.h"
#include "backend/Filesystem.h"
#include "backend/BootloaderInstaller.h"
#include "core/ImageHandler.h"
#include "core/Hash.h"

struct WueConfig {
    bool enabled = false;
    bool bypassTpm = false;
    bool bypassSecureBoot = false;
    bool bypassRam = false;
    bool bypassNro = false;
    bool disableBitLocker = false;
    bool skipMicrosoftAccount = false;
    bool disablePrivacySettings = false;
    bool enableLocalAccount = false;
    QString localAccountName;
    QString localAccountPassword;
};

struct ExtraPartitionConfig {
    bool persistence = false;
    qint64 persistenceSizeMB = 4096;
    bool esp = false;
    qint64 espSizeMB = 100;
    bool uefiNtfs = false;
    bool msr = false;
    bool compatibility = false;
};

class FormatWorker : public QObject {
    Q_OBJECT
public:
    explicit FormatWorker(QObject *parent = nullptr);

    enum class Mode {
        WriteImage,       // DD mode: write raw image to device
        FormatOnly,       // Just format, no image
        CreateBootable,   // Partition + format + extract ISO + install bootloader
        WriteImageIso     // Partition as FAT32/NTFS + extract ISO files
    };

    struct Config {
        DeviceInfo targetDevice;
        QString imagePath;
        Mode mode = Mode::WriteImage;
        PartitionScheme scheme = PartitionScheme::MBR;
        FileSystem filesystem = FileSystem::FAT32;
        QString volumeLabel;
        int clusterSizeKB = 0;      // 0 = auto
        bool quickFormat = true;
        bool checkBadBlocks = false;
        int badBlocksPasses = 1;    // Number of bad block passes
        qint64 persistentSizeMB = 4096;
        qint64 projectedSize = 0;   // uncompressed image size (DD progress total)
        QString bootloaderType;     // "none", "syslinux", "grub2", "freedos", "msdos", "grub4dos", "mbr"
        bool verifyAfterWrite = true;
        BootType autoBootType = BootType::Image;
        TargetSystemType targetType = TargetSystemType::BIOS;
        ExtraPartitionConfig extraParts;
        WueConfig wue;              // Windows User Experience customization
        QString archivePath;        // Additional zip to extract
        uint32_t bootloaderVersion = 0; // For version-matching bootloader download
        QString bootloaderVersionStr;
        bool extendedLabel = true;  // Create autorun.inf + autorun.ico (Rufus "extended label and icon files")
    };

    void setConfig(const Config &config);
    void cancel();
    bool isCancelled() const;

public slots:
    void run();

signals:
    void progressChanged(int percent);
    void progressText(const QString &text);
    void logMessage(const QString &message, int type = 0);
    void deviceProgress(qint64 current, qint64 total);
    void finished(bool success, const QString &message);
    void statusChanged(const QString &status);
    // Detail line rendered in the window status bar while the operation
    // runs ("Usando la imagen: arch.iso", "Extrayendo: <ruta>/<archivo>",
    // "Trabajando con la unidad..."), kept separate from statusChanged
    // which drives the text inside the progress bar.
    void statusBarMessage(const QString &text);

private:
    // Main operations
    bool writeImageDD();
    bool partitionAndFormat();
    bool extractIsoToPartition();
    bool installBootloader(const QString &existingMountPoint = {});
    bool checkBadBlocksPass();

    // Sub-operations
    bool zeroMbr();
    bool createPartitionTable();
    bool createRufusPartitions();
    bool formatMainPartition();
    bool formatPersistencePartition();
    bool mountAndCopyFiles();
    bool verifyWrite();
    bool applyUnattendCustomization(const QString &mountPoint);
    bool writeSbr();
    bool ntfsCheckDisk();
    bool createExtendedLabelFiles(const QString &mountPoint);

    // Bootloader version matching
    bool matchBootloaderVersion(const QString &mountPoint, const ImageInfo &imgInfo);
    bool downloadBootloaderIfNeeded(const QString &mountPoint);

    // Utility
    QString mainPartitionPath() const;
    QString partitionNthPath(int n) const;
    QString persistencePartitionPath() const;
    bool waitForPartition(const QString &path, int timeoutMs = 5000);
    ImageInfo detectImage() const;
    void setProgress(int percent);

    Config m_config;
    QAtomicInt m_cancelled{0};
    int m_totalSteps = 10;
    int m_lastProgress = 0;
    bool m_hasImage = false;
    // Progress window occupied by the ISO extraction phase (computed in
    // run(), used by mountAndCopyFiles()).
    int m_copyStart = 55;
    int m_copyEnd = 85;
    mutable ImageInfo m_cachedImageInfo;
    mutable bool m_imageInfoCached = false;
};
