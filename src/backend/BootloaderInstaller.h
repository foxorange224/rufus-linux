#pragma once

#include <QString>
#include <functional>

#include "PartitionManager.h"

struct BootloaderResult {
    bool success = false;
    QString type;        // syslinux, grub, mbr, freedos
    QString targetPath;
    QString errorMessage;
};

class BootloaderInstaller {
public:
    static BootloaderResult installSyslinux(const QString &devicePath,
                                            const QString &mountPoint,
                                            std::function<void(int)> progressCallback = nullptr);
    static BootloaderResult installGrub2(const QString &devicePath,
                                         const QString &mountPoint,
                                         std::function<void(int)> progressCallback = nullptr);
    static BootloaderResult installMbr(const QString &devicePath,
                                        std::function<void(int)> progressCallback = nullptr);
    static BootloaderResult writeMbrForBootType(const QString &devicePath,
                                                  const QString &bootloaderType,
                                                  PartitionScheme scheme,
                                                  std::function<void(int)> progressCallback = nullptr);
    static BootloaderResult writeZeroMbr(const QString &devicePath);
    static BootloaderResult writeGptProtectiveMbr(const QString &devicePath);
    static BootloaderResult installFreeDos(const QString &devicePath,
                                           const QString &mountPoint,
                                           std::function<void(int)> progressCallback = nullptr);
    static BootloaderResult installGrub4Dos(const QString &devicePath,
                                            const QString &mountPoint,
                                            std::function<void(int)> progressCallback = nullptr);
    static BootloaderResult installUefiNtfs(const QString &devicePath,
                                            const QString &mountPoint,
                                            std::function<void(int)> progressCallback = nullptr);

    static bool isSyslinuxAvailable();
    static bool isGrub2Available();

private:
    static QString findSyslinux();
    static QString findGrubInstall();
    static bool extractBootFiles(const QString &sourceDir, const QString &destDir);
};
