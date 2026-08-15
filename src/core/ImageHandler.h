#pragma once

#include <QString>
#include <QStringList>
#include <functional>

enum class ImageType {
    Unknown,
    ISO,
    IMG,
    VHD,
    VHDX,
    FFU,
    CompressedZip,
    CompressedGz,
    CompressedBz2,
    CompressedXz,
    CompressedZst,
    WIM,
    ESD
};

enum class OsType {
    Unknown,
    Windows,
    Linux,
    Mac,
    FreeDOS,
    ReactOS,
    KolibriOS
};

enum class BootType {
    NonBootable = 0,
    MSDOS,
    FreeDOS,
    Image,
    SyslinuxV4,
    SyslinuxV6,
    ReactOS,
    Grub4Dos,
    Grub2,
    UefiNtfs,
    Max
};

enum class PartitionSchemeType {
    MBR,
    GPT,
    MBRForUEFI
};

enum class TargetSystemType {
    BIOS,
    UEFI,
    Both
};

struct ImageInfo {
    // Basic info
    ImageType type = ImageType::Unknown;
    QString path;
    qint64 size = 0;
    qint64 uncompressedSize = 0;
    bool isCompressed = false;

    // Bootability
    bool isBootable = false;
    // Raw bootable image (DD-style), same 3 states as the original's
    // is_bootable_img: 0 = not bootable, 1 = bootable, 2 = forced bootable
    // (the original's Alt-M cheat mode accepts images without a boot marker).
    int8_t isBootableImg = 0;
    bool isIso = false;
    bool disableIso = false;
    bool isVhd = false;
    bool isWindowsImg = false;

    // ISO scan results (mirrors RUFUS_IMG_REPORT)
    QString label;
    QString usbLabel;
    QString cfgPath;
    QString reactosPath;
    QStringList wininstPaths;
    QString efiImgPath;
    uint64_t projectedSize = 0;
    int64_t mismatchSize = 0;
    uint32_t wininstVersion = 0;

    // EFI detection
    uint16_t hasEfi = 0;           // bitmask of EFI boot entries
    bool hasSecureBootBootloader = false;
    bool hasBootmgr = false;
    bool hasBootmgrEfi = false;
    bool hasAutorun = false;

    // Syslinux
    uint16_t slVersion = 0;         // Syslinux/Isolinux version
    QString slVersionStr;
    QString slVersionExt;
    bool hasOldC32 = false;
    bool hasEfiSyslinux = false;

    // GRUB
    bool hasGrub4Dos = false;
    uint8_t hasGrub2 = 0;
    uint8_t hasGrub2Fs = 0;
    QString grub2Version;

    // Windows
    uint16_t winpe = 0;
    uint8_t wininstIndex = 0;
    struct { uint16_t major = 0; uint16_t minor = 0; uint16_t build = 0; } winVersion;

    // Linux live
    bool usesCasper = false;
    bool usesMinint = false;
    bool rh8Derivative = false;
    bool suseDerivative = false;
    bool slackwareDerivative = false;
    bool archDerivative = false;
    bool gentooDerivative = false;

    // Secure Boot / revocation data
    bool hasSecureBootDbx = false;  // Has DBX (Secure Boot revocation) update
    bool hasSbatPolicy = false;     // Has SBAT policy
    uint8_t biosBootSvn = 0;        // BIOS boot protection SVN (0 = unknown)

    // PE architecture
    uint8_t peArch = 0;  // 0=unknown, 1=x86, 2=x64, 3=IA64, 4=ARM, 5=ARM64

    // Bootloader version (detected from image)
    uint32_t bootloaderVersion = 0;
    QString bootloaderVersionStr;

    // Misc
    bool hasKolibrios = false;
    bool has4GBFile = false;
    bool hasLongFilename = false;
    bool hasDeepDirectories = false;
    bool hasSymlinks = false;
    bool hasPantherUnattend = false;
    bool needsSyslinuxOverwrite = false;
    bool needsNtfs = false;
    uint8_t compressionType = 0;

    // Derived
    bool isUefiBootable = false;
    bool isBiosBootable = false;
    OsType osType = OsType::Unknown;
    QString partitionScheme;
    QString recommendedFs;
    QString recommendedBootloader;
    BootType autoBootType = BootType::Image;

    // Convenience helpers
    bool isRawDiskImage() const {
        return (type == ImageType::IMG || type == ImageType::VHD ||
                type == ImageType::VHDX || type == ImageType::FFU);
    }
    bool hasWindows() const {
        return hasBootmgr || hasBootmgrEfi || usesMinint || (winpe != 0);
    }
    bool hasPersistence() const {
        return (slVersion != 0 || hasGrub2 || hasGrub4Dos) &&
               !hasWindows() && !hasKolibrios && (osType != OsType::ReactOS);
    }
    bool isDDOnly() const {
        return isBootableImg > 0 && (!isIso || disableIso);
    }
};

class ImageHandler {
public:
    static ImageInfo detect(const QString &path);
    static bool extractIso(const QString &isoPath, const QString &destPath,
                           std::function<void(int)> percentCallback,
                           std::function<bool()> isCancelled = nullptr,
                           std::function<void(const QString &)> fileCopied = nullptr);
    static bool extractCompressed(const QString &archivePath, const QString &destPath,
                                  std::function<void(int)> percentCallback,
                                  std::function<void(const QString &)> fileCopied = nullptr);
    static bool extractMsDos(const QString &destPath);
    static bool isBootableIso(const QString &path);
    static bool hasPartitionTable(const QString &path);
    static QString detectLabel(const QString &path);

private:
    static ImageType detectByExtension(const QString &path);
    static bool detectIso9660(const QString &path, ImageInfo &info);
    static void scanIsoContents(const QString &path, ImageInfo &info);
    static uint16_t detectSyslinuxVersion(const QString &mountPoint);
    static bool hasEfiDirectory(const QString &mountPoint);
    static void detectEfiBootloaders(const QString &mountPoint, ImageInfo &info);
    static void detectWindowsFiles(const QString &mountPoint, ImageInfo &info);
    static void detectLinuxLive(const QString &mountPoint, ImageInfo &info);
    static void detectGrub2(const QString &mountPoint, ImageInfo &info);
    static uint8_t detectGrub2Fs(const QString &mountPoint);
    static void detectSecureBoot(const QString &mountPoint, ImageInfo &info);
    static void detectPeArchitecture(const QString &mountPoint, ImageInfo &info);
    static void detectSuseDerivative(const QString &mountPoint, ImageInfo &info);
    static QStringList findFiles(const QString &dir, const QString &pattern);
    static bool isRawBootableImage(const QString &path);
};
