#pragma once

#include <QString>
#include <QList>
#include <functional>

struct BadBlockResult {
    qint64 totalSectors = 0;
    qint64 badSectors = 0;
    qint64 readErrors = 0;
    qint64 writeErrors = 0;
    qint64 slowSectors = 0;
    QList<qint64> badSectorList;
    bool isFakeFlash = false;
    bool cancelled = false;
    double elapsedSeconds = 0.0;
    QString summary;
};

class BadBlocks {
public:
    enum class Mode {
        Read,
        Write,
        ReadWrite
    };

    static BadBlockResult check(const QString &devicePath, qint64 numSectors = 0,
                                Mode mode = Mode::Read,
                                std::function<void(int)> progressCallback = nullptr,
                                int numPasses = 1,
                                std::function<bool()> isCancelled = nullptr);

    static bool detectFakeFlash(const QString &devicePath, qint64 reportedSize,
                                std::function<void(int)> progressCallback = nullptr,
                                std::function<bool()> isCancelled = nullptr);

    static QString formatResult(const BadBlockResult &result);
};
