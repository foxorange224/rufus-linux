#include "ImageHandler.h"
#include "QProc.h"
#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QDataStream>
#include <QCoreApplication>
#include <QDebug>

static constexpr qint64 kSectorSize = 2048;

ImageType ImageHandler::detectByExtension(const QString &path) {
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "iso")         return ImageType::ISO;
    if (ext == "img")         return ImageType::IMG;
    if (ext == "vhd")         return ImageType::VHD;
    if (ext == "vhdx")        return ImageType::VHDX;
    if (ext == "ffu")         return ImageType::FFU;
    if (ext == "zip")         return ImageType::CompressedZip;
    if (ext == "gz" || ext == "gzip") return ImageType::CompressedGz;
    if (ext == "bz2")         return ImageType::CompressedBz2;
    if (ext == "xz")          return ImageType::CompressedXz;
    if (ext == "zst" || ext == "zstd") return ImageType::CompressedZst;
    if (ext == "wim")         return ImageType::WIM;
    if (ext == "esd")         return ImageType::ESD;
    return ImageType::Unknown;
}

bool ImageHandler::detectIso9660(const QString &path, ImageInfo &info) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    // ISO9660 Primary Volume Descriptor at sector 16 (offset 32768)
    if (!file.seek(32768))
        return false;

    QByteArray sector = file.read(kSectorSize);
    file.close();

    if (sector.size() < 7)
        return false;

    // Check for CD001 magic at offset 1
    if ((sector[0] == 0x00 || sector[0] == 0x01 || sector[0] == 0xFF) &&
        sector.mid(1, 5) == "CD001") {
        info.type = ImageType::ISO;
        info.isIso = true;
        info.isBootable = (sector[0] == 0x00);
        return true;
    }

    return false;
}

bool ImageHandler::isRawBootableImage(const QString &path) {
    // Same as the original Rufus analyze_mbr / ms-sys is_br() check: an
    // image is considered to have a boot marker if the 0x55 0xAA signature
    // is present at offset 0x1FE. No partition table or JMP opcode check is
    // required — a raw MBR with just the signature is still reported as
    // bootable ("Image has an unknown Master Boot Record" in the original).
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    if (!file.seek(0x1FE)) { file.close(); return false; }
    QByteArray sig = file.read(2);
    file.close();

    return sig.size() == 2 && (uint8_t)sig[0] == 0x55 && (uint8_t)sig[1] == 0xAA;
}

bool ImageHandler::hasPartitionTable(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    // MBR boot signature at offset 0x1FE (also covers GPT via its
    // protective MBR).
    if (file.seek(0x1FE)) {
        QByteArray sig = file.read(2);
        if (sig.size() == 2 && (uint8_t)sig[0] == 0x55 && (uint8_t)sig[1] == 0xAA) {
            file.close();
            return true;
        }
    }

    // GPT header magic at offset 0x200
    if (file.seek(0x200)) {
        QByteArray magic = file.read(8);
        file.close();
        if (magic == "EFI PART")
            return true;
    }

    return false;
}

uint16_t ImageHandler::detectSyslinuxVersion(const QString &mountPoint) {
    // Check isolinux.bin or ldlinux.sys for version string
    QStringList candidates = {
        mountPoint + "/isolinux/isolinux.bin",
        mountPoint + "/isolinux/ldlinux.sys",
        mountPoint + "/syslinux/ldlinux.sys",
        mountPoint + "/boot/syslinux/ldlinux.sys",
        mountPoint + "/usr/lib/syslinux/ldlinux.sys"
    };

    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = f.read(4096);
        f.close();

        // Syslinux version string typically at offset 0x2A-0x40 or searchable
        // Look for pattern like "syslinux 6.04" or "isolinux 4.07" etc.
        int idx = data.indexOf("syslinux ");
        if (idx < 0) idx = data.indexOf("isolinux ");
        if (idx >= 0) {
            QByteArray ver = data.mid(idx + 9, 10);
            int dot = ver.indexOf('.');
            if (dot > 0 && dot < 5) {
                bool okMaj = false, okMin = false;
                int major = ver.left(dot).trimmed().toInt(&okMaj);
                int minor = ver.mid(dot + 1, 3).trimmed().toInt(&okMin);
                if (okMaj && okMin)
                    return (uint16_t)(major << 8 | minor);
            }
        }

        // Alternative: scan for version byte pattern
        // ldlinux.sys has version at offset 0x0A (major) and 0x0B (minor)
        if (path.contains("ldlinux.sys") && data.size() >= 0x0C) {
            uint8_t major = (uint8_t)data[0x0A];
            uint8_t minor = (uint8_t)data[0x0B];
            if (major > 0 && major < 10)
                return (uint16_t)(major << 8 | minor);
        }
    }

    // Try 7z l to find ldlinux.sys and extract version via hexdump
    QProcess proc;
    proc.start("7z", {"l", mountPoint + "/isolinux/isolinux.bin", "-y"});
    if (proc.waitForFinished(5000) && proc.exitCode() == 0) {
        // Could parse output but easier to just check file existence
    }

    return 0;
}

bool ImageHandler::hasEfiDirectory(const QString &mountPoint) {
    return QDir(mountPoint + "/EFI/BOOT").exists() ||
           QDir(mountPoint + "/efi/boot").exists() ||
           QDir(mountPoint + "/EFI/BOOTX64").exists();
}

void ImageHandler::detectEfiBootloaders(const QString &mountPoint, ImageInfo &info) {
    // Look for EFI boot files
    QStringList efiDirs = {
        mountPoint + "/EFI/BOOT",
        mountPoint + "/efi/boot",
        mountPoint + "/EFI/BOOTX64",
        mountPoint + "/EFI",
        mountPoint + "/efi"
    };

    info.hasEfi = 0;

    struct { const char *pattern; uint16_t bit; } efiChecks[] = {
        { "bootx64.efi",   0x0001 },
        { "bootia32.efi",  0x0002 },
        { "bootaa64.efi",  0x0004 },
        { "bootarm.efi",   0x0008 },
        { "bootriscv64.efi", 0x0010 },
        { "grubx64.efi",   0x0020 },
        { "grubia32.efi",  0x0040 },
        { "mmx64.efi",     0x0080 },  // MOK Manager
        { "bootmgr.efi",   0x0100 },  // Windows EFI boot manager
        { nullptr, 0 }
    };

    for (const QString &dir : efiDirs) {
        QDir d(dir);
        if (!d.exists())
            continue;
        for (const QFileInfo &fi : d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
            QString name = fi.fileName().toLower();
            for (int i = 0; efiChecks[i].pattern; i++) {
                if (name == efiChecks[i].pattern) {
                    info.hasEfi |= efiChecks[i].bit;
                    break;
                }
            }
        }
    }

    // Also search recursively in /EFI subdirectories
    QDirIterator it(mountPoint + "/EFI", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QString name = it.fileName().toLower();
        if (name.endsWith(".efi")) {
            for (int i = 0; efiChecks[i].pattern; i++) {
                if (name == efiChecks[i].pattern) {
                    info.hasEfi |= efiChecks[i].bit;
                    break;
                }
            }
        }
    }

    // Original Rufus: IS_EFI_BOOTABLE(r) == (r.has_efi != 0) — any
    // detected EFI boot file (bootx64.efi included) makes the image
    // UEFI-bootable. (The 0x7FFE mask in the original only applies to
    // its own bit layout where bit 0 is bootmgr.efi.)
    info.isUefiBootable = (info.hasEfi != 0);
}

void ImageHandler::detectWindowsFiles(const QString &mountPoint, ImageInfo &info) {
    // Check for bootmgr (BIOS)
    info.hasBootmgr = QFileInfo::exists(mountPoint + "/bootmgr") ||
                      QFileInfo::exists(mountPoint + "/BOOTMGR");

    // Check for bootmgr.efi (EFI)
    info.hasBootmgrEfi = QFileInfo::exists(mountPoint + "/EFI/BOOT/bootmgr.efi") ||
                         QFileInfo::exists(mountPoint + "/efi/boot/bootmgr.efi") ||
                         QFileInfo::exists(mountPoint + "/bootmgr.efi") ||
                         QFileInfo::exists(mountPoint + "/BOOTMGR.EFI");

    // Check for Windows install images (sources/install.wim or install.esd)
    QDir sourcesDir(mountPoint + "/sources");
    if (sourcesDir.exists()) {
        for (const QFileInfo &fi : sourcesDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
            QString name = fi.fileName().toLower();
            if (name.startsWith("install") && (name.endsWith(".wim") || name.endsWith(".esd"))) {
                if (info.wininstIndex < 4) {
                    info.wininstPaths << fi.absoluteFilePath();
                    info.wininstIndex++;
                }
                // Check if file > 4GB
                if (fi.size() > 4294967296LL)
                    info.has4GBFile = true;
            }
        }
    }

    // Check for Windows PE
    if (QDir(mountPoint + "/I386").exists()) info.winpe |= 0x0007;
    if (QDir(mountPoint + "/AMD64").exists()) info.winpe |= 0x0023;

    // Minint (Windows PE minimal)
    info.usesMinint = QFileInfo::exists(mountPoint + "/minint") ||
                      QFileInfo::exists(mountPoint + "/MININT");

    // Check for Panther unattend
    info.hasPantherUnattend = QFileInfo::exists(mountPoint + "/Windows/Panther/Unattend.xml") ||
                              QFileInfo::exists(mountPoint + "/windows/panther/unattend.xml");

    // Check for autorun.inf
    info.hasAutorun = QFileInfo::exists(mountPoint + "/autorun.inf") ||
                      QFileInfo::exists(mountPoint + "/AUTORUN.INF");

    // Detect Windows version from setup.exe or bootmgr
    QStringList bootFiles = {
        mountPoint + "/setup.exe",
        mountPoint + "/SETUP.EXE",
        mountPoint + "/bootmgr",
        mountPoint + "/BOOTMGR"
    };
    for (const QString &bf : bootFiles) {
        QFile f(bf);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = f.read(4096);
        f.close();
        // Look for PE version info
        // Not perfect but detects common patterns
        if (data.contains("Windows 11") || data.contains("10.0.2")) {
            info.winVersion.major = 10;
            info.winVersion.minor = 0;
            info.winVersion.build = 22000;
        } else if (data.contains("Windows 10") || data.contains("10.0.1")) {
            info.winVersion.major = 10;
            info.winVersion.minor = 0;
            info.winVersion.build = 10240;
        } else if (data.contains("Windows 8") || data.contains("6.2") || data.contains("6.3")) {
            info.winVersion.major = 6;
            info.winVersion.minor = 2;
            info.winVersion.build = 9200;
        } else if (data.contains("Windows 7") || data.contains("6.1")) {
            info.winVersion.major = 6;
            info.winVersion.minor = 1;
            info.winVersion.build = 7600;
        } else if (data.contains("Windows Vista") || data.contains("6.0")) {
            info.winVersion.major = 6;
            info.winVersion.minor = 0;
            info.winVersion.build = 6000;
        } else if (data.contains("Windows XP") || data.contains("5.1")) {
            info.winVersion.major = 5;
            info.winVersion.minor = 1;
            info.winVersion.build = 2600;
        }

        // Search for a more specific build number from version strings
        // Look for patterns like "10.0.22000" or "10.0.19041"
        QRegularExpression buildRe(R"((\d+)\.(\d+)\.(\d+))");
        auto match = buildRe.match(QString::fromUtf8(data));
        if (match.hasMatch() && match.captured(1).toUInt() >= 6) {
            info.winVersion.build = match.captured(3).toUShort();
        }
        break;
    }
}

void ImageHandler::detectLinuxLive(const QString &mountPoint, ImageInfo &info) {
    // Casper (Ubuntu)
    info.usesCasper = QDir(mountPoint + "/casper").exists() ||
                      QFileInfo::exists(mountPoint + "/casper/vmlinuz") ||
                      (QFileInfo::exists(mountPoint + "/casper/filesystem.squashfs") &&
                       QFileInfo::exists(mountPoint + "/casper/filesystem.size"));

    // Generic live directory
    if (!info.usesCasper) {
        info.usesCasper = QDir(mountPoint + "/live").exists() ||
                          QFileInfo::exists(mountPoint + "/live/vmlinuz") ||
                          QFileInfo::exists(mountPoint + "/live/vmlinuz64");
    }

    // Check for KolibriOS
    info.hasKolibrios = QFileInfo::exists(mountPoint + "/HD_Load/USB_Boot/MTLD_F32") ||
                        QFileInfo::exists(mountPoint + "/hd_load/usb_boot/mtld_f32");

    // Check for ReactOS (FreeLoader lives at the root or under /loader/)
    info.reactosPath.clear();
    QStringList reactosCandidates = {
        mountPoint + "/freeldr.sys",   mountPoint + "/FREELDR.SYS",
        mountPoint + "/loader/freeldr.sys", mountPoint + "/loader/FREELDR.SYS",
        mountPoint + "/setupldr.sys",  mountPoint + "/SETUPLDR.SYS",
        mountPoint + "/loader/setupldr.sys", mountPoint + "/loader/SETUPLDR.SYS",
    };
    for (const QString &candidate : reactosCandidates) {
        if (QFileInfo::exists(candidate)) {
            info.reactosPath = candidate.mid(mountPoint.length());
            break;
        }
    }

    // RHEL 8 derivative (has squashfs in /LiveOS)
    info.rh8Derivative = QDir(mountPoint + "/LiveOS").exists() &&
                         QFileInfo::exists(mountPoint + "/LiveOS/squashfs.img");

    // Detect SUSE derivative
    detectSuseDerivative(mountPoint, info);

    // Detect Slackware derivative
    info.slackwareDerivative = QFileInfo::exists(mountPoint + "/slackware-version") ||
                                QDir(mountPoint + "/slackware64").exists() ||
                                QDir(mountPoint + "/slackware").exists() ||
                                QFileInfo::exists(mountPoint + "/boot/_slack.jpg");

    // Detect Arch derivative
    info.archDerivative = QFileInfo::exists(mountPoint + "/arch") ||
                          QDir(mountPoint + "/arch").exists() ||
                          QFileInfo::exists(mountPoint + "/archlinux-version") ||
                          (QFileInfo::exists(mountPoint + "/boot/vmlinuz-linux") &&
                           !info.rh8Derivative && !info.suseDerivative);

    // Detect Gentoo derivative
    info.gentooDerivative = QFileInfo::exists(mountPoint + "/gentoo") ||
                            QDir(mountPoint + "/gentoo").exists() ||
                            QFileInfo::exists(mountPoint + "/etc/gentoo-release") ||
                            QDir(mountPoint + "/usr/portage").exists();

    // Check for syslinux config
    QStringList cfgCandidates = {
        mountPoint + "/isolinux/isolinux.cfg",
        mountPoint + "/syslinux/syslinux.cfg",
        mountPoint + "/boot/syslinux/syslinux.cfg",
        mountPoint + "/isolinux.cfg",
        mountPoint + "/syslinux.cfg"
    };
    for (const QString &cfg : cfgCandidates) {
        if (QFileInfo::exists(cfg)) {
            info.cfgPath = cfg;
            break;
        }
    }
}

void ImageHandler::detectGrub2(const QString &mountPoint, ImageInfo &info) {
    info.hasGrub2 = 0;

    // Check for GRUB2 core images
    QStringList grubFiles = {
        mountPoint + "/boot/grub/grub.cfg",
        mountPoint + "/boot/grub2/grub.cfg",
        mountPoint + "/grub.cfg",
        mountPoint + "/boot/grub/core.img",
        mountPoint + "/boot/grub2/core.img",
        mountPoint + "/EFI/BOOT/grubx64.efi",
        mountPoint + "/efi/boot/grubx64.efi"
    };

    for (const QString &gf : grubFiles) {
        if (QFileInfo::exists(gf)) {
            info.hasGrub2 |= 0x01; // GRUB2 detected
            break;
        }
    }

    // Check for GRUB2 version in grub.cfg
    QStringList cfgPaths = {
        mountPoint + "/boot/grub/grub.cfg",
        mountPoint + "/boot/grub2/grub.cfg",
        mountPoint + "/grub.cfg"
    };

    for (const QString &cfg : cfgPaths) {
        QFile f(cfg);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = f.read(8192);
        f.close();

        // Look for GRUB version comment or string
        QByteArrayList lines = data.split('\n');
        for (const QByteArray &line : lines) {
            if (line.contains("grub_version") || line.contains("GRUB_VERSION") ||
                line.contains("GRUB 2.")) {
                info.grub2Version = QString::fromUtf8(line).trimmed();
                break;
            }
        }
        break;
    }

    // Detect GRUB filesystem support
    if (info.hasGrub2)
        info.hasGrub2Fs = detectGrub2Fs(mountPoint);

    // Check for GRUB4DOS
    info.hasGrub4Dos = QFileInfo::exists(mountPoint + "/grldr") ||
                       QFileInfo::exists(mountPoint + "/GRLDR") ||
                       QFileInfo::exists(mountPoint + "/menu.lst") ||
                       QFileInfo::exists(mountPoint + "/MENU.LST");
}

uint8_t ImageHandler::detectGrub2Fs(const QString &mountPoint) {
    uint8_t fs = 0;
    // Check if GRUB2 has specific filesystem modules
    QStringList grubDirs = {
        mountPoint + "/boot/grub",
        mountPoint + "/boot/grub2",
        mountPoint + "/usr/lib/grub"
    };

    for (const QString &dir : grubDirs) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QFileInfo &fi : d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
            QString name = fi.fileName();
            if (name.contains("exfat"))      fs |= 0x02;
            else if (name.contains("fat"))   fs |= 0x01;
            else if (name.contains("ntfs"))  fs |= 0x04;
        }
    }
    return fs;
}

void ImageHandler::detectSuseDerivative(const QString &mountPoint, ImageInfo &info) {
    info.suseDerivative = false;

    // Check for SUSE-specific files
    if (QFileInfo::exists(mountPoint + "/suse") ||
        QFileInfo::exists(mountPoint + "/SUSE") ||
        QDir(mountPoint + "/suse").exists() ||
        QDir(mountPoint + "/SUSE").exists()) {
        info.suseDerivative = true;
        return;
    }

    // Check for /etc/SuSE-release
    if (QFileInfo::exists(mountPoint + "/etc/SuSE-release") ||
        QFileInfo::exists(mountPoint + "/etc/SUSE-release") ||
        QFileInfo::exists(mountPoint + "/etc/SuSE-brand")) {
        info.suseDerivative = true;
        return;
    }

    // Check for YaST configuration
    if (QFileInfo::exists(mountPoint + "/etc/YaST2") ||
        QDir(mountPoint + "/etc/YaST2").exists() ||
        QDir(mountPoint + "/var/lib/YaST2").exists()) {
        info.suseDerivative = true;
        return;
    }

    // Check for SUSE-specific boot files
    if (QFileInfo::exists(mountPoint + "/boot/message") &&
        (QFileInfo::exists(mountPoint + "/boot/linux") ||
         QFileInfo::exists(mountPoint + "/boot/initrd"))) {
        info.suseDerivative = true;
        return;
    }

    // Check for SUSE GRUB2 theme
    if (QDir(mountPoint + "/boot/grub2/themes/SUSE").exists() ||
        QDir(mountPoint + "/boot/grub2/themes/openSUSE").exists()) {
        info.suseDerivative = true;
        return;
    }

    // Check for /LiveOS with SUSE-specific markers
    if (info.rh8Derivative) {
        // RHEL derivatives also have /LiveOS, but SUSE has specific config
        if (QFileInfo::exists(mountPoint + "/LiveOS/squashfs.img")) {
            // Try to detect SUSE from isolinux/syslinux config inside
            QStringList cfgPaths = {
                mountPoint + "/isolinux/isolinux.cfg",
                mountPoint + "/syslinux/syslinux.cfg",
                mountPoint + "/boot/syslinux/syslinux.cfg"
            };
            for (const QString &cfg : cfgPaths) {
                QFile f(cfg);
                if (!f.open(QIODevice::ReadOnly))
                    continue;
                QByteArray data = f.read(4096);
                f.close();
                QString cfgContent = QString::fromUtf8(data);
                if (cfgContent.contains("suse", Qt::CaseInsensitive) ||
                    cfgContent.contains("opensuse", Qt::CaseInsensitive) ||
                    cfgContent.contains("SUSE Linux", Qt::CaseInsensitive)) {
                    info.suseDerivative = true;
                    return;
                }
            }
        }
    }
}

void ImageHandler::detectSecureBoot(const QString &mountPoint, ImageInfo &info) {
    // Detect Secure Boot revocation data (DBX)
    info.hasSecureBootDbx = false;
    info.hasSbatPolicy = false;
    info.biosBootSvn = 0;

    // Check for DBX (Secure Boot Forbidden Signature Database) update files
    QStringList dbxPaths = {
        mountPoint + "/EFI/BOOT/dbxupdate.efi",
        mountPoint + "/efi/boot/dbxupdate.efi",
        mountPoint + "/EFI/BOOT/dbx.efi",
        mountPoint + "/efi/boot/dbx.efi",
        mountPoint + "/EFI/BOOT/dbxupdate_x64.efi",
        mountPoint + "/efi/boot/dbxupdate_x64.efi"
    };
    for (const QString &dbx : dbxPaths) {
        if (QFileInfo::exists(dbx)) {
            info.hasSecureBootDbx = true;
            break;
        }
    }

    // Check for DBX in system volume information (Windows-based)
    if (!info.hasSecureBootDbx) {
        QStringList sysVolPaths = {
            mountPoint + "/System Volume Information/EFI/DBX",
            mountPoint + "/system volume information/efi/dbx"
        };
        for (const QString &svp : sysVolPaths) {
            if (QDir(svp).exists()) {
                info.hasSecureBootDbx = true;
                break;
            }
        }
    }

    // Check for SBAT (Secure Boot Advanced Targeting) policy
    // Look for SBAT policy in grub configs
    QStringList sbatPaths = {
        mountPoint + "/boot/grub/sbat.efi",
        mountPoint + "/boot/grub2/sbat.efi",
        mountPoint + "/EFI/BOOT/sbat.efi",
        mountPoint + "/efi/boot/sbat.efi",
        mountPoint + "/boot/grub/sbat.csv",
        mountPoint + "/boot/grub2/sbat.csv"
    };
    for (const QString &sbat : sbatPaths) {
        if (QFileInfo::exists(sbat)) {
            info.hasSbatPolicy = true;
            break;
        }
    }

    // Check for SBAT policy in grub.cfg
    if (!info.hasSbatPolicy) {
        QStringList cfgPaths = {
            mountPoint + "/boot/grub/grub.cfg",
            mountPoint + "/boot/grub2/grub.cfg"
        };
        for (const QString &cfg : cfgPaths) {
            QFile f(cfg);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            QByteArray data = f.read(8192);
            f.close();
            if (QString::fromUtf8(data).contains("sbat", Qt::CaseInsensitive)) {
                info.hasSbatPolicy = true;
                break;
            }
        }
    }

    // Detect BIOS Boot Protection SVN
    // Look for SVN in bootloader files
    QStringList bootloaderPaths = {
        mountPoint + "/boot/boot.ini",
        mountPoint + "/boot/bootmgr.exe",
        mountPoint + "/EFI/BOOT/bootx64.efi",
        mountPoint + "/efi/boot/bootx64.efi"
    };
    for (const QString &bl : bootloaderPaths) {
        QFile f(bl);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = f.read(8192);
        f.close();
        // Look for SVN markers in PE binaries
        int idx = data.indexOf("SVN");
        if (idx >= 0 && idx + 6 < data.size()) {
            uint8_t svn = (uint8_t)data[idx + 3];
            if (svn > 0 && svn < 100) {
                info.biosBootSvn = svn;
                break;
            }
        }
    }
}

void ImageHandler::detectPeArchitecture(const QString &mountPoint, ImageInfo &info) {
    // Scan for PE executables to determine architecture
    info.peArch = 0; // Unknown

    // Priority: look at EFI bootloaders first
    QStringList efiCandidates = {
        mountPoint + "/EFI/BOOT/bootx64.efi",
        mountPoint + "/efi/boot/bootx64.efi",
        mountPoint + "/EFI/BOOT/bootia32.efi",
        mountPoint + "/efi/boot/bootia32.efi",
        mountPoint + "/EFI/BOOT/bootaa64.efi",
        mountPoint + "/efi/boot/bootaa64.efi",
        mountPoint + "/EFI/BOOT/bootarm.efi",
        mountPoint + "/efi/boot/bootarm.efi",
        mountPoint + "/EFI/BOOT/grubx64.efi",
        mountPoint + "/efi/boot/grubx64.efi"
    };

    for (const QString &efi : efiCandidates) {
        QFile f(efi);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray header = f.read(512);
        f.close();

        // PE header starts at offset stored at 0x3C
        if (header.size() < 0x3C + 4) continue;
        uint32_t peOffset = (uint8_t)header[0x3C] |
                           ((uint8_t)header[0x3D] << 8) |
                           ((uint8_t)header[0x3E] << 16) |
                           ((uint8_t)header[0x3F] << 24);
        if (peOffset + 24 > (uint32_t)header.size()) continue;

        // Check "PE\0\0" signature
        if (header.mid(peOffset, 2) != "PE") continue;

        // Machine field is at PE offset + 4 (2 bytes)
        uint16_t machine = (uint8_t)header[peOffset + 4] |
                          ((uint8_t)header[peOffset + 5] << 8);
        switch (machine) {
        case 0x014C: info.peArch = 1; break; // I386
        case 0x8664: info.peArch = 2; break; // x86-64
        case 0x0200: info.peArch = 3; break; // IA64
        case 0x01C4: info.peArch = 4; break; // ARM (Thumb)
        case 0xAA64: info.peArch = 5; break; // ARM64
        }
        if (info.peArch != 0) return;
    }

    // Fallback: look at setup.exe or other PE files
    QStringList peCandidates = {
        mountPoint + "/setup.exe",
        mountPoint + "/bootmgr",
        mountPoint + "/sources/setup.exe"
    };
    for (const QString &pe : peCandidates) {
        QFile f(pe);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray header = f.read(512);
        f.close();

        if (header.size() < 0x3C + 4) continue;
        uint32_t peOffset = (uint8_t)header[0x3C] |
                           ((uint8_t)header[0x3D] << 8) |
                           ((uint8_t)header[0x3E] << 16) |
                           ((uint8_t)header[0x3F] << 24);
        if (peOffset + 24 > (uint32_t)header.size()) continue;
        if (header.mid(peOffset, 2) != "PE") continue;

        uint16_t machine = (uint8_t)header[peOffset + 4] |
                          ((uint8_t)header[peOffset + 5] << 8);
        switch (machine) {
        case 0x014C: info.peArch = 1; break;
        case 0x8664: info.peArch = 2; break;
        case 0x0200: info.peArch = 3; break;
        case 0x01C4: info.peArch = 4; break;
        case 0xAA64: info.peArch = 5; break;
        }
        if (info.peArch != 0) return;
    }
}

QStringList ImageHandler::findFiles(const QString &dir, const QString &pattern) {
    QStringList result;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (QDir::match(pattern, it.fileName()))
            result << it.filePath();
    }
    return result;
}

void ImageHandler::scanIsoContents(const QString &path, ImageInfo &info) {
    QTemporaryDir mountDir;
    if (!mountDir.isValid()) return;
    mountDir.setAutoRemove(true);
    QString mp = mountDir.path();

    auto doUnmount = [&mp]() {
        // One QProcess per command, always reaped: destroying a QProcess
        // while its child still runs cuts umount's flush short.
        QProcess fusermount;
        fusermount.start("fusermount", {"-u", mp});
        finishProcess(fusermount, 10000);
        if (fusermount.exitStatus() == QProcess::NormalExit && fusermount.exitCode() == 0)
            return;
        QProcess umount;
        umount.start("umount", {mp});
        finishProcess(umount, 10000);
    };

    // Try mounting first
    bool mounted = false;
    QProcess fuse;
    fuse.start("fuseiso", {path, mp});
    finishProcess(fuse, 10000);
    if (fuse.exitStatus() == QProcess::NormalExit && fuse.exitCode() == 0) {
        mounted = true;
    }

    if (!mounted) {
        QProcess mount;
        mount.start("mount", {"-o", "loop,ro", path, mp});
        finishProcess(mount, 10000);
        if (mount.exitStatus() == QProcess::NormalExit && mount.exitCode() == 0)
            mounted = true;
    }

    if (!mounted) {
        // Use 7z to list contents
        QProcess sevenZip;
        sevenZip.start("7z", {"l", path});
        if (!sevenZip.waitForFinished(30000))
            return;
        QString out = QString::fromUtf8(sevenZip.readAllStandardOutput());

        info.isBootable = out.contains("boot.catalog", Qt::CaseInsensitive) ||
                          out.contains("isolinux.bin", Qt::CaseInsensitive) ||
                          out.contains("boot/etfsboot.com", Qt::CaseInsensitive);

        info.isUefiBootable = out.contains("EFI/BOOT/BOOTX64.EFI", Qt::CaseInsensitive) ||
                              out.contains("efi/boot/bootx64.efi", Qt::CaseInsensitive);

        info.hasBootmgr = out.contains("bootmgr", Qt::CaseInsensitive);
        info.hasBootmgrEfi = out.contains("bootmgr.efi", Qt::CaseInsensitive);

        QStringList paths;
        if (out.contains("sources/install.wim", Qt::CaseInsensitive) ||
            out.contains("sources\\install.wim", Qt::CaseInsensitive))
            info.wininstPaths << "sources/install.wim";
        if (out.contains("sources/install.esd", Qt::CaseInsensitive) ||
            out.contains("sources\\install.esd", Qt::CaseInsensitive))
            info.wininstPaths << "sources/install.esd";
        info.wininstIndex = info.wininstPaths.size();

        info.usesCasper = out.contains("casper/", Qt::CaseInsensitive) &&
                          out.contains("filesystem.squashfs", Qt::CaseInsensitive);

        info.hasGrub2 = out.contains("grub.cfg", Qt::CaseInsensitive) ? 1 : 0;
        info.hasGrub4Dos = out.contains("grldr", Qt::CaseInsensitive);

        info.hasKolibrios = out.contains("MTLD_F32", Qt::CaseInsensitive) ||
                            out.contains("kolibri", Qt::CaseInsensitive);

        info.hasEfi = out.contains(".efi", Qt::CaseInsensitive) ? 0x7FFE : 0;
        info.slVersion = out.contains("isolinux.bin", Qt::CaseInsensitive) ? 0x0406 : 0;

        info.rh8Derivative = out.contains("LiveOS/", Qt::CaseInsensitive) &&
                             out.contains("squashfs.img", Qt::CaseInsensitive);
        info.suseDerivative = out.contains("/suse", Qt::CaseInsensitive) ||
                               out.contains("/SUSE", Qt::CaseInsensitive) ||
                               out.contains("opensuse", Qt::CaseInsensitive);
        info.archDerivative = out.contains("/arch/", Qt::CaseInsensitive) ||
                               out.contains("archlinux", Qt::CaseInsensitive);
        info.gentooDerivative = out.contains("/gentoo", Qt::CaseInsensitive) ||
                                 out.contains("portage", Qt::CaseInsensitive);

        info.hasSecureBootDbx = out.contains("dbxupdate", Qt::CaseInsensitive) ||
                                out.contains("dbx.efi", Qt::CaseInsensitive);
        info.hasSbatPolicy = out.contains("sbat", Qt::CaseInsensitive);

        // DD-mode only images (disable_iso in the original, iso.c): Manjaro
        // (.miso file), Pop!_OS (casper/ + "pop-os" in the image name),
        // Proxmox (/proxmox directory)
        if (out.contains(".miso", Qt::CaseInsensitive) ||
            (out.contains("casper/", Qt::CaseInsensitive) &&
             path.contains("pop-os", Qt::CaseInsensitive)) ||
            out.contains("proxmox/", Qt::CaseInsensitive))
            info.disableIso = true;

        if (info.hasBootmgr || info.hasBootmgrEfi || out.contains("sources/", Qt::CaseInsensitive))
            info.osType = OsType::Windows;
        else if (out.contains("freedos", Qt::CaseInsensitive))
            info.osType = OsType::FreeDOS;
        else if (out.contains("freeldr.sys", Qt::CaseInsensitive) ||
                 out.contains("setupldr.sys", Qt::CaseInsensitive)) {
            info.osType = OsType::ReactOS;
            if (info.reactosPath.isEmpty())
                info.reactosPath = QStringLiteral("/freeldr.sys");
        }
        else if (out.contains("casper/", Qt::CaseInsensitive) ||
                 out.contains("/live/", Qt::CaseInsensitive) ||
                 out.contains("vmlinuz", Qt::CaseInsensitive) ||
                 out.contains("squashfs", Qt::CaseInsensitive) ||
                 out.contains("syslinux", Qt::CaseInsensitive) ||
                 out.contains("isolinux", Qt::CaseInsensitive) ||
                 out.contains("grub.cfg", Qt::CaseInsensitive))
            info.osType = OsType::Linux;

        // Check for symlinks
        if (out.contains(" Joliet ") || out.contains(" joliet "))
            info.hasSymlinks = true;
        if (out.contains(" Rock ") || out.contains(" rock "))
            info.hasSymlinks = true;

        // Check for long file names and deep directories
        for (const QString &line : out.split('\n')) {
            if (line.length() > 100) info.hasLongFilename = true;
            if (line.count('/') > 8) info.hasDeepDirectories = true;
        }

        return;
    }

    // --- ISO is mounted ---
    QDir dir(mp);

    // Detect Syslinux/Isolinux
    info.slVersion = detectSyslinuxVersion(mp);

    // Detect EFI bootloaders
    detectEfiBootloaders(mp, info);

    // Detect Windows files
    detectWindowsFiles(mp, info);

    // Detect Linux live systems
    detectLinuxLive(mp, info);

    // Detect GRUB2
    detectGrub2(mp, info);

    // Detect Secure Boot revocation data
    detectSecureBoot(mp, info);

    // Detect PE architecture
    detectPeArchitecture(mp, info);

    // Check for isolinux.bin (boot indicator)
    info.isBootable = info.slVersion > 0 ||
                      QFileInfo::exists(mp + "/isolinux/isolinux.bin") ||
                      QFileInfo::exists(mp + "/boot/etfsboot.com") ||
                      info.hasBootmgr ||
                      info.hasBootmgrEfi ||
                      info.hasEfi != 0 ||
                      info.hasGrub2 != 0;

    // Check for 4GB+ files
    QDirIterator it(mp, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().size() > 4294967296LL) {
            info.has4GBFile = true;
            break;
        }
    }

    // Symlinks
    QDirIterator sit(mp, QDir::Files | QDir::System, QDirIterator::Subdirectories);
    while (sit.hasNext()) {
        sit.next();
        if (sit.fileInfo().isSymLink()) {
            info.hasSymlinks = true;
            break;
        }
    }

    // Check for long filenames and deep directories
    QDirIterator dit(mp, QDir::Dirs | QDir::Files, QDirIterator::Subdirectories);
    while (dit.hasNext()) {
        dit.next();
        if (dit.fileName().length() > 128) info.hasLongFilename = true;
        QString rel = dit.filePath().mid(mp.length());
        if (rel.count('/') > 12) info.hasDeepDirectories = true;
    }

    // DD-mode only images (disable_iso in the original, iso.c): Manjaro
    // (.miso file), Pop!_OS (casper/ + "pop-os" in the image name),
    // Proxmox (/proxmox directory)
    if (QFileInfo::exists(mp + "/.miso") ||
        (info.usesCasper && path.contains("pop-os", Qt::CaseInsensitive)) ||
        QDir(mp + "/proxmox").exists())
        info.disableIso = true;

    // Determine OS type based on collected info
    if (info.hasBootmgr || info.hasBootmgrEfi || info.wininstIndex > 0)
        info.osType = OsType::Windows;
    else if (info.hasKolibrios)
        info.osType = OsType::KolibriOS;
    else if (!info.reactosPath.isEmpty())
        info.osType = OsType::ReactOS;
    else if (info.usesCasper || info.rh8Derivative || info.suseDerivative ||
             info.slackwareDerivative || info.archDerivative || info.gentooDerivative ||
             info.hasGrub2 || info.slVersion > 0 || !info.cfgPath.isEmpty())
        info.osType = OsType::Linux;

    doUnmount();
}

ImageInfo ImageHandler::detect(const QString &path) {
    ImageInfo info;
    info.path = path;
    info.type = detectByExtension(path);

    QFileInfo fi(path);
    info.size = fi.size();

    // Check magic bytes
    if (info.type == ImageType::Unknown || info.type >= ImageType::CompressedZip) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray magic = file.read(16);
            file.close();

            if (magic.size() >= 4) {
                if (magic.mid(0, 4) == "\x50\x4B\x03\x04")
                    info.type = ImageType::CompressedZip;
                else if (magic.mid(0, 2) == "\x1F\x8B")
                    info.type = ImageType::CompressedGz;
                else if (magic.mid(0, 3) == "\x42\x5A\x68")
                    info.type = ImageType::CompressedBz2;
                else if (magic.mid(0, 6) == "\xFD\x37\x7A\x58\x5A\x00")
                    info.type = ImageType::CompressedXz;
                else if (magic.mid(0, 4) == "\x28\xB5\x2F\xFD")
                    info.type = ImageType::CompressedZst;
                else if (magic.mid(0, 2) == "\x4D\x5A")
                    info.type = ImageType::IMG;
            }
        }
    }

    // VHD detection
    if (info.type == ImageType::Unknown) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray magic = file.read(8);
            file.close();
            if (magic == "conectix") info.type = ImageType::VHD;
            if (magic.mid(0, 6) == QByteArray::fromHex("657869746F6673"))  // vhdx
                info.type = ImageType::VHDX;
        }
    }

    // ISO detection by content
    if (info.type == ImageType::Unknown || info.type == ImageType::IMG)
        detectIso9660(path, info);

    // Raw bootable image detection (DD-style). Also run for ISO files: an
    // ISOHybrid is a .iso that carries an MBR at the start, so it can be
    // written in either ISO mode or DD mode.
    if (info.type == ImageType::IMG || info.type == ImageType::Unknown ||
        info.type == ImageType::ISO) {
        info.isBootableImg = isRawBootableImage(path) ? 1 : 0;
    }

    // Fallback type
    if (info.type == ImageType::Unknown)
        info.type = ImageType::IMG;

    info.isCompressed = (info.type >= ImageType::CompressedZip);
    info.isVhd = (info.type == ImageType::VHD || info.type == ImageType::VHDX);

    // Detect uncompressed size
    if (info.isCompressed) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            qint64 fileSize = file.size();
            if (info.type == ImageType::CompressedGz && fileSize >= 4) {
                if (file.seek(fileSize - 4)) {
                    QByteArray last4 = file.read(4);
                    if (last4.size() == 4) {
                        quint32 origSize = (static_cast<quint8>(last4[3]) << 24) |
                                           (static_cast<quint8>(last4[2]) << 16) |
                                           (static_cast<quint8>(last4[1]) << 8) |
                                            static_cast<quint8>(last4[0]);
                        if (origSize > 0)
                            info.uncompressedSize = origSize;
                    }
                }
            }
            else if (info.type == ImageType::CompressedXz) {
                QProcess xzProc;
                xzProc.start("xz", {"--list", "--robot", path});
                if (xzProc.waitForFinished(5000) && xzProc.exitCode() == 0) {
                    QStringList lines = QString::fromUtf8(xzProc.readAllStandardOutput())
                                            .split('\n', Qt::SkipEmptyParts);
                    for (const QString &line : lines) {
                        if (line.startsWith("totals")) {
                            QStringList parts = line.split('\t');
                            if (parts.size() >= 3)
                                info.uncompressedSize = parts[2].toLongLong();
                            break;
                        }
                    }
                }
            }
            else if (info.type == ImageType::CompressedBz2) {
                QProcess bzProc;
                bzProc.start("bzip2", {"-l", path});
                if (bzProc.waitForFinished(5000) && bzProc.exitCode() == 0) {
                    QString out = QString::fromUtf8(bzProc.readAllStandardOutput());
                    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
                        QStringList parts = line.trimmed().split(QRegularExpression("\\s+"));
                        if (parts.size() >= 5) {
                            bool ok = false;
                            qint64 val = parts.last().toLongLong(&ok);
                            if (ok && val > 0)
                                info.uncompressedSize = val;
                        }
                    }
                }
            }
            else if (info.type == ImageType::CompressedZst) {
                QProcess zstProc;
                zstProc.start("zstd", {"-l", path});
                if (zstProc.waitForFinished(5000) && zstProc.exitCode() == 0) {
                    QString out = QString::fromUtf8(zstProc.readAllStandardOutput());
                    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
                        QStringList parts = line.trimmed().split(QRegularExpression("\\s+"));
                        if (parts.size() >= 3) {
                            bool ok = false;
                            qint64 val = parts[1].toLongLong(&ok);
                            if (ok && val > 0) {
                                info.uncompressedSize = val;
                                break;
                            }
                        }
                    }
                }
            }
            file.close();
        }
    }

    // Projected size on disk: the uncompressed size for compressed images
    // (what actually gets written to the device), otherwise the file size.
    info.projectedSize = (info.uncompressedSize > 0)
        ? info.uncompressedSize
        : QFileInfo(path).size();

    // Label detection
    info.label = detectLabel(path);

    // Scan ISO contents if it's an ISO
    if (info.type == ImageType::ISO)
        scanIsoContents(path, info);

    // Determine if this is a raw disk image (DD-style)
    info.isWindowsImg = false;

    // Label-based DD-mode only images (original rufus.c suse_derivative
    // list): SUSE installers are not ISO mode compatible
    if (info.label.startsWith("Install-SUSE") ||
        info.label.startsWith("Install-LEAP") ||
        info.label.startsWith("openSUSE-Tumbleweed"))
        info.disableIso = true;

    // EFI GRUB2 images without FAT/exFAT/NTFS modules cannot be written in
    // ISO mode (original rufus.c:1427-1430; has_grub2_fs bits: 0x1=FAT,
    // 0x2=exFAT, 0x4=NTFS — exFAT or NTFS support is mandatory for 4GB+
    // files). No EFI GRUB => no scan result (hasGrub2Fs == 0), so require
    // hasEfi to limit false positives on the 7z path.
    if (info.hasGrub2 && info.hasEfi &&
        (!(info.hasGrub2Fs & 0x7) || (info.has4GBFile && !(info.hasGrub2Fs & 0x6))))
        info.disableIso = true;

    // Compute isIso flag
    if (info.type == ImageType::ISO)
        info.isIso = true;
    else if (info.type == ImageType::IMG && info.isBootableImg)
        info.isIso = false;

    // Set IS_BIOS_BOOTABLE equivalent (original rufus.h)
    info.isBiosBootable = (info.hasBootmgr || info.slVersion > 0 || info.winpe != 0 ||
                           info.hasGrub2 || info.hasGrub4Dos || info.hasKolibrios ||
                           !info.reactosPath.isEmpty());

    // Determine if UEFI bootable (original Rufus: IS_EFI_BOOTABLE == has_efi != 0)
    info.isUefiBootable = info.isUefiBootable || (info.hasEfi != 0) || info.hasBootmgrEfi;

    // DD mode is enforced when the image is bootable but not ISO mode
    // compatible (original rufus.c:1432-1438)
    if (info.isBootableImg > 0 && info.isIso &&
        (info.disableIso || (!info.isBiosBootable && !info.isUefiBootable)))
        info.isIso = false;

    // Determine recommendations
    if (info.type == ImageType::ISO) {
        if (info.hasWindows()) {
            info.recommendedFs = info.has4GBFile ? QStringLiteral("NTFS") : QStringLiteral("FAT32");
            info.recommendedBootloader = QStringLiteral("none");
            info.partitionScheme = QStringLiteral("MBR");
            info.autoBootType = BootType::Image;
        } else if (info.osType == OsType::ReactOS) {
            // Original Rufus: ReactOS is booted through Syslinux (which
            // loads FreeLoader via mboot.c32), not through FreeDOS.
            info.recommendedFs = QStringLiteral("FAT32");
            info.recommendedBootloader = QStringLiteral("syslinux");
            info.partitionScheme = QStringLiteral("MBR");
            info.autoBootType = BootType::ReactOS;
        } else if (info.osType == OsType::FreeDOS) {
            info.recommendedFs = QStringLiteral("FAT32");
            info.recommendedBootloader = QStringLiteral("freedos");
            info.partitionScheme = QStringLiteral("MBR");
            info.autoBootType = BootType::FreeDOS;
        } else if (info.hasKolibrios) {
            info.recommendedFs = QStringLiteral("FAT32");
            info.recommendedBootloader = QStringLiteral("none");
            info.partitionScheme = QStringLiteral("MBR");
            info.autoBootType = BootType::Image;
        } else if (info.hasGrub2) {
            info.recommendedFs = QStringLiteral("FAT32");
            info.recommendedBootloader = QStringLiteral("grub2");
            info.partitionScheme = QStringLiteral("GPT");
            info.autoBootType = BootType::Grub2;
        } else if (info.slVersion > 0) {
            info.recommendedFs = QStringLiteral("FAT32");
            info.recommendedBootloader = (info.slVersion >= 0x0600)
                ? QStringLiteral("syslinux") : QStringLiteral("syslinux");
            info.partitionScheme = QStringLiteral("MBR");
            info.autoBootType = (info.slVersion >= 0x0600)
                ? BootType::SyslinuxV6 : BootType::SyslinuxV4;
        } else {
            info.recommendedFs = QStringLiteral("FAT32");
            info.recommendedBootloader = QStringLiteral("none");
            info.partitionScheme = QStringLiteral("MBR");
            info.autoBootType = BootType::NonBootable;
        }

        // Original Rufus leaves the partition scheme at MBR for Linux
        // images (only Windows+EFI images get GPT as the default); the
        // user can always switch to GPT in the UI, where both options
        // stay available for ISO images.

    } else if (info.isRawDiskImage()) {
        info.partitionScheme = QStringLiteral("GPT");
        info.recommendedBootloader = QStringLiteral("none");
        info.recommendedFs = QStringLiteral("FAT32");
        info.autoBootType = BootType::Image;
    }

    // Needs NTFS?
    info.needsNtfs = (info.osType == OsType::Windows && info.has4GBFile);

    return info;
}

bool ImageHandler::extractIso(const QString &isoPath, const QString &destPath,
                              std::function<void(int)> percentCallback,
                              std::function<bool()> isCancelled,
                              std::function<void(const QString &)> fileCopied) {
    QDir().mkpath(destPath);

    // Try fuseiso first
    QTemporaryDir mountDir;
    if (!mountDir.isValid()) return false;
    mountDir.setAutoRemove(true);
    QString mp = mountDir.path();

    QProcess mountProc;
    mountProc.start("fuseiso", {isoPath, mp});
    if (mountProc.waitForFinished(10000) && mountProc.exitCode() == 0) {
        // rsync reports the cumulative transferred bytes; convert them to
        // a real percentage using the total size of the mounted tree.
        qint64 total = 0;
        {
            QDirIterator it(mp, QDir::Files | QDir::NoSymLinks,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                total += it.fileInfo().size();
            }
        }
        QProcess rsync;
        rsync.setProcessChannelMode(QProcess::SeparateChannels);
        rsync.start("rsync", {"-a", "-v", "--info=progress2",
                               mp + "/", destPath + "/"});
        if (rsync.waitForStarted(5000)) {
            QByteArray buf;
            QByteArray bufOut;
            bool cancelled = false;
            while (rsync.state() != QProcess::NotRunning) {
                if (isCancelled && isCancelled()) {
                    cancelled = true;
                    rsync.kill();
                    break;
                }
                if (!rsync.waitForReadyRead(200))
                    continue;
                buf += rsync.readAllStandardError();
                int sep = buf.lastIndexOf('\r');
                if (sep < 0) continue;
                QByteArray line = buf.mid(sep + 1).trimmed();
                buf.clear();
                if (percentCallback && !line.isEmpty()) {
                    const QList<QByteArray> fields = line.split(' ');
                    if (fields.size() >= 2) {
                        bool numOk = false;
                        QByteArray bytesField = fields[0];
                        bytesField.replace(",", "");
                        qint64 bytes = bytesField.toLongLong(&numOk);
                        if (numOk && bytes > 0)
                            percentCallback(static_cast<int>(
                                bytes * 100 / qMax<qint64>(1, total)));
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
                        if (name == QStringLiteral("sending incremental file list") ||
                            name.startsWith(QStringLiteral("sent ")) ||
                            name.startsWith(QStringLiteral("total size is ")) ||
                            name.startsWith(QStringLiteral("created directory ")))
                            continue;
                        fileCopied(name);
                    }
                }
            }
            rsync.waitForFinished(3000);
            QProcess fusermount;
            fusermount.start("fusermount", {"-u", mp});
            finishProcess(fusermount, 10000);
            if (cancelled)
                return false;
            if (rsync.exitStatus() == QProcess::NormalExit && rsync.exitCode() == 0)
                return true;
        }
        QProcess fusermount;
        fusermount.start("fusermount", {"-u", mp});
        finishProcess(fusermount, 10000);
    }

    // Try 7z
    {
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start("7z", {"x", isoPath, "-o" + destPath, "-y"});
        if (proc.waitForStarted(10000)) {
            while (!proc.waitForFinished(200)) {
                QByteArray out = proc.readAllStandardOutput();
                if (percentCallback) {
                    for (const QByteArray &line : out.split('\n')) {
                        if (line.contains('%')) {
                            bool ok = false;
                            int v = line.trimmed().left(3).trimmed().toInt(&ok);
                            if (ok) percentCallback(v);
                        }
                        // 7z prints "Extracting  <path>" for every file
                        // created: feed those to the status bar.
                        if (fileCopied && line.startsWith("Extracting")) {
                            QString name = QString::fromUtf8(line).mid(
                                line.indexOf('E') + 10).trimmed();
                            if (!name.isEmpty())
                                fileCopied(name);
                        }
                    }
                }
            }
            proc.readAllStandardOutput();
            if (proc.exitCode() == 0) return true;
        }
    }

    // Try xorriso
    {
        QProcess proc;
        proc.start("xorriso", {"-osirrox", "on", "-indev", isoPath,
                                "-extract", "/", destPath});
        if (proc.waitForFinished(120000) && proc.exitCode() == 0)
            return true;
    }

    // Final fallback: mount -o loop
    {
        QProcess sudoMount;
        sudoMount.start("mount", {"-o", "loop,ro", isoPath, mp});
        finishProcess(sudoMount, 10000);
        if (sudoMount.exitStatus() == QProcess::NormalExit && sudoMount.exitCode() == 0) {
            QProcess cp;
            cp.start("cp", {"-r", mp + "/.", destPath + "/"});
            finishProcess(cp, 120000);
            bool ok = (cp.exitStatus() == QProcess::NormalExit && cp.exitCode() == 0);
            QProcess umount;
            umount.start("umount", {mp});
            finishProcess(umount, 10000);
            return ok;
        }
    }

    return false;
}

bool ImageHandler::extractCompressed(const QString &archivePath, const QString &destPath,
                                     std::function<void(int)> percentCallback) {
    QDir().mkpath(destPath);
    QProcess proc;
    proc.start("7z", {"x", archivePath, "-o" + destPath, "-y"});
    if (proc.waitForStarted(10000)) {
        while (!proc.waitForFinished(200)) {
            QByteArray out = proc.readAllStandardOutput();
            if (percentCallback) {
                for (const QByteArray &line : out.split('\n')) {
                    if (line.contains('%')) {
                        bool ok = false;
                        int v = line.trimmed().left(3).trimmed().toInt(&ok);
                        if (ok) percentCallback(v);
                    }
                }
            }
        }
        proc.readAllStandardOutput();
        return proc.exitCode() == 0;
    }
    return false;
}

bool ImageHandler::extractMsDos(const QString &destPath) {
    // MS-DOS mode boots the bundled FreeDOS kernel like original Rufus:
    // the FreeDOS KERNEL.SYS is installed as IO.SYS (the name the MS-DOS
    // compatible boot sector loads), alongside COMMAND.COM and a plain
    // text MSDOS.SYS configuration.
    QDir dir(destPath);
    if (!dir.exists())
        return false;

    QString appDir = QCoreApplication::applicationDirPath();
    QStringList searchDirs = {
        appDir + "/freedos",
        appDir + "/../share/rufus/freedos",
        QStringLiteral("/usr/local/share/rufus/freedos"),
        QStringLiteral("/usr/share/rufus/freedos"),
    };
    QString freedosDir;
    for (const QString &d : searchDirs) {
        if (QDir(d).exists()) {
            freedosDir = d;
            break;
        }
    }
    if (freedosDir.isEmpty()) {
        qWarning("extractMsDos: FreeDOS files not found in %s",
                 qPrintable(appDir));
        return false;
    }

    if (!QFile::copy(freedosDir + "/KERNEL.SYS", destPath + "/IO.SYS"))
        return false;
    if (QFileInfo::exists(freedosDir + "/COMMAND.COM"))
        QFile::copy(freedosDir + "/COMMAND.COM", destPath + "/COMMAND.COM");

    QFile msdosSys(destPath + "/MSDOS.SYS");
    if (msdosSys.open(QIODevice::WriteOnly)) {
        msdosSys.write(";MSDOS.SYS for FreeDOS\n"
                       "[Paths]\n"
                       "WinDir=C:\\\n"
                       "WinBootDir=C:\\\n"
                       "HostWinBootDrv=C\n");
        msdosSys.close();
    }

    // Make the kernel files system/hidden read-only, like a real MS-DOS
    // installation.
    QFile::setPermissions(destPath + "/IO.SYS",
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    QFile::setPermissions(destPath + "/MSDOS.SYS",
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);

    return true;
}

bool ImageHandler::isBootableIso(const QString &path) {
    ImageInfo info = detect(path);
    return info.isBootable;
}

QString ImageHandler::detectLabel(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    // ISO9660 Volume Descriptor at offset 32768 + 40 = 32808
    if (file.size() < 33000)
        return {};

    // Only ISO9660 images have a volume descriptor: reading raw .img files
    // at this offset yields random bytes that would corrupt the volume
    // label (and break mkfs later), so require the "CD001" magic first.
    if (!file.seek(32769))
        return {};
    if (file.read(5) != QByteArrayLiteral("CD001"))
        return {};

    if (!file.seek(32808))
        return {};

    QByteArray labelData = file.read(32);
    file.close();

    if (labelData.size() < 32)
        return {};

    QString label = QString::fromLatin1(labelData).trimmed();
    if (!label.isEmpty())
        return label;

    return {};
}
