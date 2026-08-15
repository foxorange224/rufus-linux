#include "BootloaderInstaller.h"
#include "core/QProc.h"
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <QCoreApplication>

static QString findMbrBin() {
    static const char *paths[] = {
        "/usr/lib/syslinux/bios/mbr.bin",
        "/usr/lib/syslinux/mbr.bin",
        "/usr/share/syslinux/mbr.bin",
        "/usr/lib/ISOLINUX/mbr.bin",
        nullptr
    };
    for (const char **p = paths; *p; p++) {
        if (QFileInfo::exists(*p))
            return QString::fromLatin1(*p);
    }
    // Search with find command
    QProcess which;
    which.start("find", {"/usr", "-name", "mbr.bin", "-type", "f"});
    if (which.waitForFinished(10000)) {
        QString res = QString::fromUtf8(which.readAllStandardOutput()).trimmed();
        if (!res.isEmpty())
            return res.split('\n').first().trimmed();
    }
    return {};
}

static QString findGptMbrBin() {
    static const char *paths[] = {
        "/usr/lib/syslinux/bios/gptmbr.bin",
        "/usr/lib/syslinux/gptmbr.bin",
        "/usr/share/syslinux/gptmbr.bin",
        nullptr
    };
    for (const char **p = paths; *p; p++) {
        if (QFileInfo::exists(*p))
            return QString::fromLatin1(*p);
    }
    return {};
}

static bool writeMbr(const QString &devicePath, const QString &mbrFile) {
    // Write first 440 bytes of MBR
    QProcess dd;
    dd.start("dd", {"if=" + mbrFile, "of=" + devicePath,
                     "bs=440", "count=1", "status=none", "conv=notrunc"});
    if (!dd.waitForFinished(10000))
        return false;
    return dd.exitCode() == 0;
}

BootloaderResult BootloaderInstaller::installSyslinux(const QString &devicePath,
                                                       const QString &mountPoint,
                                                       std::function<void(int)> progressCallback,
                                                       std::function<bool()> isCancelled) {
    BootloaderResult result;
    result.type = "syslinux";

    if (!isSyslinuxAvailable()) {
        result.errorMessage = "syslinux not found on this system";
        return result;
    }

    if (progressCallback) progressCallback(10);

    QProcess proc;
    proc.start("syslinux", {"--install", mountPoint});
    if (finishProcess(proc, 30000, isCancelled))
        return result;   // cancelled: failed result, caller checks isCancelled
    if (proc.exitStatus() != QProcess::NormalExit) {
        result.errorMessage = "syslinux --install timed out";
        return result;
    }

    if (proc.exitCode() != 0) {
        result.errorMessage = QStringLiteral("syslinux --install failed: %1")
            .arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return result;
    }

    if (progressCallback) progressCallback(50);

    // Write MBR bootstrap code
    QString mbrBin = findMbrBin();
    if (mbrBin.isEmpty()) {
        result.errorMessage = "syslinux MBR file (mbr.bin) not found";
        return result;
    }

    if (!writeMbr(devicePath, mbrBin)) {
        result.errorMessage = "Failed to write syslinux MBR";
        return result;
    }

    if (progressCallback) progressCallback(100);

    result.success = true;
    result.targetPath = devicePath;
    return result;
}

BootloaderResult BootloaderInstaller::installGrub2(const QString &devicePath,
                                                     const QString &mountPoint,
                                                     std::function<void(int)> progressCallback,
                                                     std::function<bool()> isCancelled) {
    BootloaderResult result;
    result.type = "grub2";

    if (!isGrub2Available()) {
        result.errorMessage = "grub-install not found on this system";
        return result;
    }

    if (progressCallback) progressCallback(10);

    // Try --removable first (for USB sticks), fallback to regular install
    QStringList args;
    QString bootDir = mountPoint + "/boot";
    QDir().mkpath(bootDir);

    // Determine if BIOS or UEFI
    bool hasUefi = QFileInfo::exists("/usr/lib/grub/x86_64-efi") ||
                   QFileInfo::exists("/usr/lib/grub/i386-efi");

    // BIOS boot
    QString biosPath = QStringLiteral("/usr/lib/grub/i386-pc");
    if (QFileInfo::exists(biosPath)) {
        args = {"--target=i386-pc", "--boot-directory=" + bootDir,
                "--recheck", "--force"};
        if (progressCallback) progressCallback(20);

        QProcess proc;
        proc.start("grub-install", args + QStringList{devicePath});
        if (finishProcess(proc, 60000, isCancelled))
            return result;   // cancelled
        if (proc.exitStatus() != QProcess::NormalExit) {
            result.errorMessage = "grub-install (BIOS) timed out";
            return result;
        }

        if (proc.exitCode() != 0) {
            // Try without --force
            proc.start("grub-install",
                       {"--target=i386-pc", "--boot-directory=" + bootDir,
                        devicePath});
            if (finishProcess(proc, 60000, isCancelled))
                return result;   // cancelled
        }

        if (proc.exitCode() != 0) {
            result.errorMessage = QStringLiteral("grub-install (BIOS) failed: %1")
                .arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
            return result;
        }
    }

    // UEFI boot (--removable installs to /EFI/BOOT/BOOTX64.EFI)
    if (hasUefi) {
        if (progressCallback) progressCallback(60);

        QProcess proc;
        proc.start("grub-install",
                   {"--target=x86_64-efi", "--efi-directory=" + mountPoint,
                    "--removable", "--boot-directory=" + bootDir,
                    "--recheck", devicePath});
        if (finishProcess(proc, 60000, isCancelled))
            return result;   // cancelled
        if (proc.exitStatus() != QProcess::NormalExit) {
            result.errorMessage = "grub-install (UEFI) timed out";
            return result;
        }

        if (proc.exitCode() != 0) {
            // Non-fatal: UEFI may not be supported on this system
            result.errorMessage = QStringLiteral("grub-install (UEFI) note: %1")
                .arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        }
    }

    if (progressCallback) progressCallback(100);
    result.success = true;
    result.targetPath = devicePath;
    return result;
}

BootloaderResult BootloaderInstaller::installMbr(const QString &devicePath,
                                                  std::function<void(int)> progressCallback) {
    BootloaderResult result;
    result.type = "mbr";

    // Try ms-sys first
    QProcess proc;
    proc.start("ms-sys", {"--mbr", devicePath});
    if (proc.waitForFinished(10000) && proc.exitCode() == 0) {
        if (progressCallback) progressCallback(100);
        result.success = true;
        result.targetPath = devicePath;
        return result;
    }

    // Fallback: write generic MBR
    QString mbrBin = findMbrBin();
    if (mbrBin.isEmpty()) {
        result.errorMessage = "No MBR template found (install ms-sys or syslinux)";
        return result;
    }

    if (!writeMbr(devicePath, mbrBin)) {
        result.errorMessage = "Failed to write MBR";
        return result;
    }

    if (progressCallback) progressCallback(100);
    result.success = true;
    result.targetPath = devicePath;
    return result;
}

BootloaderResult BootloaderInstaller::writeZeroMbr(const QString &devicePath) {
    BootloaderResult result;
    result.type = "zero";
    QProcess dd;
    dd.start("dd", {"if=/dev/zero", "of=" + devicePath, "bs=440", "count=1", "status=none", "conv=notrunc"});
    if (dd.waitForFinished(10000) && dd.exitCode() == 0) {
        result.success = true;
        result.targetPath = devicePath;
    } else {
        result.errorMessage = "Failed to write zero MBR";
    }
    return result;
}

BootloaderResult BootloaderInstaller::writeGptProtectiveMbr(const QString &devicePath) {
    BootloaderResult result;
    result.type = "gpt_protective";
    // Write a protective MBR with partition type 0xEE covering the whole disk
    // This is the standard GPT protective MBR, no special message
    QByteArray mbr(512, 0);
    mbr[0x1BE] = 0x00; // status: not bootable
    mbr[0x1BF] = 0x00; // CHS start: 0/0/1
    mbr[0x1C0] = 0x02;
    mbr[0x1C1] = 0x00;
    mbr[0x1C2] = 0xEE; // GPT protective partition type
    mbr[0x1C3] = 0xFF; // CHS end: 1023/254/63
    mbr[0x1C4] = 0xFF;
    mbr[0x1C5] = 0xFF;
    mbr[0x1C6] = 0x01; // LBA start = 1
    mbr[0x1C7] = 0x00;
    mbr[0x1C8] = 0x00;
    mbr[0x1C9] = 0x00;
    // LBA size = 0xFFFFFFFF (max)
    mbr[0x1CA] = 0xFF;
    mbr[0x1CB] = 0xFF;
    mbr[0x1CC] = 0xFF;
    mbr[0x1CD] = 0xFF;
    // Boot signature
    mbr[0x1FE] = 0x55;
    mbr[0x1FF] = 0xAA;

    QFile f(devicePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(mbr);
        f.close();
        result.success = true;
        result.targetPath = devicePath;
    } else {
        result.errorMessage = "Failed to write GPT protective MBR";
    }
    return result;
}

BootloaderResult BootloaderInstaller::writeMbrForBootType(const QString &devicePath,
                                                           const QString &bootloaderType,
                                                           PartitionScheme scheme,
                                                           std::function<void(int)> progressCallback) {
    BootloaderResult result;
    result.targetPath = devicePath;

    if (scheme == PartitionScheme::GPT) {
        // For GPT, write protective MBR (non-bootable)
        return writeGptProtectiveMbr(devicePath);
    }

    // Non-bootable or UEFI target → zero MBR
    if (bootloaderType == "none" || bootloaderType.isEmpty()) {
        if (progressCallback) progressCallback(100);
        return writeZeroMbr(devicePath);
    }

    // For syslinux → write syslinux MBR
    if (bootloaderType == "syslinux" || bootloaderType == "freedos") {
        QString mbrBin = findMbrBin();
        if (!mbrBin.isEmpty() && writeMbr(devicePath, mbrBin)) {
            result.type = "syslinux_mbr";
            result.success = true;
            if (progressCallback) progressCallback(100);
            return result;
        }
        // Fallback: ms-sys
    }

    // For grub2 → grub-install writes its own MBR, skip
    if (bootloaderType == "grub2") {
        result.type = "grub2";
        result.success = true;
        if (progressCallback) progressCallback(100);
        return result;
    }

    // For grub4dos → grub4dos MBR (try to find grldr.mbr)
    if (bootloaderType == "grub4dos") {
        QString grub4dosMbr = QCoreApplication::applicationDirPath() + "/grldr.mbr";
        if (QFileInfo::exists(grub4dosMbr) && writeMbr(devicePath, grub4dosMbr)) {
            result.type = "grub4dos_mbr";
            result.success = true;
            if (progressCallback) progressCallback(100);
            return result;
        }
        // Fallthrough to default MBR
    }

    // Default: Windows-compatible MBR via ms-sys
    QProcess proc;
    proc.start("ms-sys", {"--mbr", devicePath});
    if (proc.waitForFinished(10000) && proc.exitCode() == 0) {
        result.type = "windows_mbr";
        result.success = true;
        if (progressCallback) progressCallback(100);
        return result;
    }

    // Fallback: syslinux MBR
    QString mbrBin = findMbrBin();
    if (!mbrBin.isEmpty() && writeMbr(devicePath, mbrBin)) {
        result.type = "syslinux_mbr";
        result.success = true;
        if (progressCallback) progressCallback(100);
        return result;
    }

    result.errorMessage = "No suitable MBR found (install ms-sys or syslinux)";
    return result;
}

BootloaderResult BootloaderInstaller::installFreeDos(const QString &devicePath,
                                                      const QString &mountPoint,
                                                      std::function<void(int)> progressCallback,
                                                      std::function<bool()> isCancelled) {
    BootloaderResult result;
    result.type = "freedos";

    if (progressCallback) progressCallback(10);

    QDir dest(mountPoint);
    if (!dest.exists()) {
        result.errorMessage = "Mount point does not exist";
        return result;
    }

    // FreeDOS is bundled with the application; check the build tree and
    // the installed locations as well.
    QStringList searchDirs = {
        QCoreApplication::applicationDirPath() + "/freedos",
        QCoreApplication::applicationDirPath() + "/../share/rufus/freedos",
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
        result.errorMessage = "FreeDOS files not found. Install the rufus package or place the files in 'freedos/' next to the binary";
        return result;
    }

    if (progressCallback) progressCallback(50);

    // Copy FreeDOS files
    QProcess cp;
    cp.start("cp", {"-r", freedosDir + "/.", mountPoint});
    if (finishProcess(cp, 30000, isCancelled))
        return result;   // cancelled
    if (cp.exitStatus() != QProcess::NormalExit) {
        result.errorMessage = "Failed to copy FreeDOS files";
        return result;
    }

    // Install syslinux for boot
    BootloaderResult syslinuxResult = installSyslinux(devicePath, mountPoint, nullptr, isCancelled);
    if (!syslinuxResult.success) {
        result.errorMessage = syslinuxResult.errorMessage;
        return result;
    }

    if (progressCallback) progressCallback(100);
    result.success = true;
    result.targetPath = mountPoint;
    return result;
}

BootloaderResult BootloaderInstaller::installGrub4Dos(const QString &devicePath,
                                                       const QString &mountPoint,
                                                       std::function<void(int)> progressCallback) {
    BootloaderResult result;
    result.type = "grub4dos";

    if (progressCallback) progressCallback(10);

    // Write GRLDR.MBR to device MBR (only first 440 bytes, not full 512)
    QString grldrMbr = QCoreApplication::applicationDirPath() + "/grldr.mbr";
    QString grldr = QCoreApplication::applicationDirPath() + "/grldr";

    // Check if bundled GRLDR files exist, try to find them
    if (!QFileInfo::exists(grldrMbr)) {
        // Search in common locations
        QStringList searchPaths = {
            "/usr/share/grub4dos/grldr.mbr",
            "/usr/local/share/grub4dos/grldr.mbr",
            QCoreApplication::applicationDirPath() + "/../share/rufus/grldr.mbr"
        };
        for (const QString &p : searchPaths) {
            if (QFileInfo::exists(p)) {
                grldrMbr = p;
                break;
            }
        }
    }

    if (!QFileInfo::exists(grldr)) {
        QStringList searchPaths = {
            "/usr/share/grub4dos/grldr",
            "/usr/local/share/grub4dos/grldr",
            QCoreApplication::applicationDirPath() + "/../share/rufus/grldr"
        };
        for (const QString &p : searchPaths) {
            if (QFileInfo::exists(p)) {
                grldr = p;
                break;
            }
        }
    }

    // Write GRLDR.MBR to the device
    if (QFileInfo::exists(grldrMbr)) {
        QProcess dd;
        dd.start("dd", {"if=" + grldrMbr, "of=" + devicePath,
                         "bs=440", "count=1", "status=none", "conv=notrunc"});
        if (!dd.waitForFinished(10000) || dd.exitCode() != 0) {
            // Try writing full 512 bytes
            dd.start("dd", {"if=" + grldrMbr, "of=" + devicePath,
                             "bs=512", "count=1", "status=none", "conv=notrunc"});
            dd.waitForFinished(10000);
        }

        if (dd.exitCode() == 0) {
            result.success = true;
            result.targetPath = devicePath;
            if (progressCallback) progressCallback(50);
        } else {
            result.errorMessage = "Failed to write GRLDR.MBR to device";
            return result;
        }
    } else {
        // MBR write failure is non-fatal; we can still copy GRLDR
        result.errorMessage = "grldr.mbr not found, MBR not written";
    }

    // Copy GRLDR to the partition root
    if (QFileInfo::exists(grldr) && !mountPoint.isEmpty()) {
        QFile::copy(grldr, mountPoint + "/grldr");
        if (progressCallback) progressCallback(80);

        // Also copy menu.lst if exists
        QString menuLst = QCoreApplication::applicationDirPath() + "/menu.lst";
        if (QFileInfo::exists(menuLst)) {
            QFile::copy(menuLst, mountPoint + "/menu.lst");
        }
    } else if (!mountPoint.isEmpty()) {
        result.errorMessage = "grldr not found, bootloader files not copied to partition";
    }

    if (progressCallback) progressCallback(100);
    result.success = true;
    result.targetPath = devicePath;
    return result;
}

BootloaderResult BootloaderInstaller::installUefiNtfs(const QString &devicePath,
                                                       const QString &mountPoint,
                                                       std::function<void(int)> progressCallback) {
    BootloaderResult result;
    result.type = "uefintfs";

    if (progressCallback) progressCallback(10);

    // UEFI:NTFS provides a bootx64.efi that can read NTFS from UEFI environment
    // This is used when writing NTFS images for UEFI boot
    QString uefiNtfsImg = QCoreApplication::applicationDirPath() + "/uefi-ntfs.img";

    // Search for the image in common locations
    if (!QFileInfo::exists(uefiNtfsImg)) {
        QStringList searchPaths = {
            "/usr/share/rufus/uefi-ntfs.img",
            "/usr/local/share/rufus/uefi-ntfs.img",
            QCoreApplication::applicationDirPath() + "/../share/rufus/uefi-ntfs.img"
        };
        for (const QString &p : searchPaths) {
            if (QFileInfo::exists(p)) {
                uefiNtfsImg = p;
                break;
            }
        }
    }

    if (!QFileInfo::exists(uefiNtfsImg)) {
        result.errorMessage = "UEFI:NTFS image not found. Download from: "
                              "https://github.com/pbatard/UEFI-NTFS/releases";
        return result;
    }

    if (mountPoint.isEmpty()) {
        result.errorMessage = "Mount point required for UEFI:NTFS installation";
        return result;
    }

    if (progressCallback) progressCallback(30);

    // Mount the UEFI:NTFS image to extract its contents
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        result.errorMessage = "Failed to create temp directory";
        return result;
    }

    QProcess mountImg;
    mountImg.start("mount", QStringList({"-o", "loop,ro", uefiNtfsImg, tmpDir.path()}));
    if (!mountImg.waitForFinished(10000) || mountImg.exitCode() != 0) {
        // Try fuseiso
        mountImg.start("fuseiso", QStringList({uefiNtfsImg, tmpDir.path()}));
        mountImg.waitForFinished(10000);
    }

    // Create EFI boot directory
    QString efiBootDir = mountPoint + "/EFI/BOOT";
    QDir().mkpath(efiBootDir);

    // Copy UEFI:NTFS boot files
    // The image contains a FAT partition with bootx64.efi, bootia32.efi
    if (QDir(tmpDir.path()).exists()) {
        QStringList efiFiles = {"bootx64.efi", "bootia32.efi"};
        for (const QString &efi : efiFiles) {
            if (QFileInfo::exists(tmpDir.path() + "/" + efi)) {
                QFile::copy(tmpDir.path() + "/" + efi, efiBootDir + "/" + efi);
            }
        }
    }

    // Also copy the image itself for drive letter assignment
    QFile::copy(uefiNtfsImg, mountPoint + "/uefi-ntfs.img");

    if (progressCallback) progressCallback(70);

    // Cleanup temp mount
    QProcess umount;
    umount.start("umount", QStringList({tmpDir.path()}));
    finishProcess(umount, 10000);

    // Verify bootx64.efi was copied
    if (!QFileInfo::exists(efiBootDir + "/bootx64.efi")) {
        result.errorMessage = "Failed to install UEFI:NTFS bootloader";
        return result;
    }

    if (progressCallback) progressCallback(100);
    result.success = true;
    result.targetPath = efiBootDir;
    return result;
}

bool BootloaderInstaller::isSyslinuxAvailable() {
    return !findSyslinux().isEmpty();
}

bool BootloaderInstaller::isGrub2Available() {
    return !findGrubInstall().isEmpty();
}

QString BootloaderInstaller::findSyslinux() {
    QProcess which;
    which.start("which", {"syslinux"});
    if (which.waitForFinished(3000) && which.exitCode() == 0)
        return QString::fromUtf8(which.readAllStandardOutput()).trimmed();
    return {};
}

QString BootloaderInstaller::findGrubInstall() {
    QProcess which;
    which.start("which", {"grub-install"});
    if (which.waitForFinished(3000) && which.exitCode() == 0)
        return QString::fromUtf8(which.readAllStandardOutput()).trimmed();
    return {};
}

bool BootloaderInstaller::extractBootFiles(const QString &sourceDir, const QString &destDir) {
    QDir dest(destDir);
    if (!dest.exists())
        dest.mkpath(".");

    QProcess cp;
    cp.start("cp", {"-r", sourceDir + "/.", destDir});
    if (!cp.waitForFinished(30000))
        return false;
    return cp.exitCode() == 0;
}
