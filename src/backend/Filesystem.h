#pragma once

#include <QString>
#include "PartitionManager.h"

struct FormatResult {
    bool success = false;
    bool cancelled = false;
    QString fsType;
    QString label;
    double elapsedSeconds = 0.0;
    QString errorMessage;
    QString note;   // Non-fatal informational message (e.g. FAT16->FAT32 upgrade)
};

class Filesystem {
public:
    static FormatResult format(const QString &partitionPath, FileSystem fs,
                               const QString &label = QString(),
                               bool quick = true,
                               int clusterSizeKB = 0,
                               std::function<void(int)> progressCallback = nullptr,
                               std::function<bool()> isCancelled = nullptr);

    static bool checkFilesystem(const QString &partitionPath, FileSystem fs);

    static QString mkfsTool(FileSystem fs);

    static FileSystem detectExisting(const QString &partitionPath);

private:
    static FormatResult formatVfat(const QString &partitionPath, FileSystem fs,
                                   const QString &label, bool quick, int clusterSizeKB,
                                   std::function<bool()> isCancelled = nullptr);
    static FormatResult formatExt(const QString &partitionPath, FileSystem fs,
                                  const QString &label, bool quick, int clusterSizeKB,
                                  std::function<bool()> isCancelled = nullptr);
    static FormatResult formatNtfs(const QString &partitionPath, const QString &label,
                                   bool quick, int clusterSizeKB,
                                   std::function<bool()> isCancelled = nullptr);
    static FormatResult formatExfat(const QString &partitionPath, const QString &label,
                                    bool quick, int clusterSizeKB,
                                    std::function<bool()> isCancelled = nullptr);
};
