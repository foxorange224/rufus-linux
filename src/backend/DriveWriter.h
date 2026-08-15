#pragma once

#include <QString>
#include <functional>

struct WriteResult {
    bool success = false;
    bool cancelled = false;
    qint64 bytesWritten = 0;
    qint64 totalBytes = 0;
    int errors = 0;
    double elapsedSeconds = 0.0;
    QString errorMessage;
};

class DriveWriter {
public:
    static WriteResult writeImage(const QString &imagePath, const QString &devicePath,
                                  bool isCompressed = false,
                                  std::function<void(qint64)> progressCallback = nullptr,
                                  std::function<bool()> isCancelled = nullptr);
    static WriteResult writeDD(const QString &imagePath, const QString &devicePath,
                               std::function<void(qint64)> progressCallback = nullptr,
                               std::function<bool()> isCancelled = nullptr);
    static WriteResult writeZeros(const QString &devicePath, qint64 numBytes = 0,
                                  std::function<void(qint64)> progressCallback = nullptr,
                                  std::function<bool()> isCancelled = nullptr);
    static WriteResult writeSparse(const QString &imagePath, const QString &devicePath,
                                   std::function<void(qint64)> progressCallback = nullptr);

    static bool syncDevice(const QString &devicePath);

private:
    static int openDeviceRaw(const QString &path, bool write = true);
    static bool isSectorAligned(qint64 offset, int sectorSize);
};
