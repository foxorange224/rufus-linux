#include "BadBlocks.h"
#include <QElapsedTimer>
#include <QThread>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>

// Pattern types matching original Rufus: SLC, MLC, TLC
static const uint8_t kPatternSLC[] = { 0x00, 0xFF, 0x55, 0xAA };
static const uint8_t kPatternMLC[] = { 0x00, 0xFF, 0x33, 0xCC };
static const uint8_t kPatternTLC[] = { 0x00, 0xFF, 0x1C, 0x71, 0xC7, 0xE3, 0x8E, 0x38 };
static const uint8_t kPatternOnePass[] = { 0x55, 0x00, 0x00, 0x00 };
static const uint8_t kPatternTwoPass[] = { 0x55, 0xAA, 0x00, 0x00 };

static constexpr qint64 kBlockSize = 512 * 1024;  // 512KB blocks like original

static const uint8_t* patternForPass(int pass, int totalPasses) {
    if (totalPasses <= 1)
        return kPatternOnePass;
    if (totalPasses == 2)
        return kPatternTwoPass;
    if (totalPasses >= 3) {
        switch (pass) {
        case 0: return kPatternSLC;
        case 1: return kPatternMLC;
        case 2: return kPatternTLC;
        default: return kPatternSLC;
        }
    }
    return kPatternOnePass;
}

static int patternLength(int totalPasses) {
    if (totalPasses <= 1) return 4;
    if (totalPasses == 2) return 4;
    if (totalPasses >= 3) return 8;
    return 4;
}

BadBlockResult BadBlocks::check(const QString &devicePath, qint64 numSectors,
                                Mode mode, std::function<void(int)> progressCallback,
                                int numPasses) {
    BadBlockResult result;

    if (numPasses < 1) numPasses = 1;

    int fd = ::open(devicePath.toUtf8().constData(), O_RDWR | O_SYNC);
    if (fd < 0) {
        result.summary = QStringLiteral("Cannot open device: %1").arg(strerror(errno));
        return result;
    }

    qint64 sectorSize = 512;
    qint64 deviceSize = ::lseek(fd, 0, SEEK_END);
    if (deviceSize < 0) { ::close(fd); return result; }
    ::lseek(fd, 0, SEEK_SET);

    if (numSectors <= 0 || numSectors > deviceSize / sectorSize)
        numSectors = deviceSize / sectorSize;

    result.totalSectors = numSectors;

    // Use block-sized buffer for better performance
    qint64 blockSize = kBlockSize;
    qint64 numBlocks = (numSectors * sectorSize + blockSize - 1) / blockSize;

    char *writeBuf = nullptr, *readBuf = nullptr;
    if (posix_memalign((void**)&writeBuf, 512, blockSize) != 0) writeBuf = nullptr;
    if (posix_memalign((void**)&readBuf, 512, blockSize) != 0) readBuf = nullptr;
    if (!writeBuf || !readBuf) {
        std::free(writeBuf); std::free(readBuf);
        result.summary = QStringLiteral("Buffer allocation failed");
        return result;
    }

    QElapsedTimer timer;
    timer.start();

    // One pass per pattern: SLC -> MLC -> TLC (like original Rufus).
    // Read-only checks always run a single pass.
    int passes = (mode == Mode::Read) ? 1 : numPasses;
    for (int pass = 0; pass < passes; pass++) {
        const uint8_t *pattern = patternForPass(pass, numPasses);
        const int plen = patternLength(numPasses);

        for (qint64 block = 0; block < numBlocks; block++) {
            if (progressCallback && (block % 8 == 0)) {
                int pct = static_cast<int>((pass * 100 + block * 100 / numBlocks) / passes);
                progressCallback(pct);
            }

            qint64 offset = block * blockSize;
            qint64 thisBlockSize = qMin(blockSize, (numSectors * sectorSize) - offset);
            if (thisBlockSize <= 0) break;

            if (mode == Mode::Write || mode == Mode::ReadWrite) {
                // Fill with the current pass' pattern
                for (qint64 i = 0; i < thisBlockSize; i += plen)
                    ::memcpy(writeBuf + i, pattern, qMin<qint64>(plen, thisBlockSize - i));

                qint64 written = 0;
                while (written < thisBlockSize) {
                    qint64 w = ::pwrite(fd, writeBuf + written, thisBlockSize - written, offset + written);
                    if (w < 0) {
                        result.writeErrors++;
                        result.badSectors++;
                        result.badSectorList.append(offset / 512 + written / 512);
                        break;
                    }
                    written += w;
                }
                ::fsync(fd);
            }

            if (mode == Mode::Read || mode == Mode::ReadWrite) {
                qint64 read = 0;
                while (read < thisBlockSize) {
                    qint64 r = ::pread(fd, readBuf + read, thisBlockSize - read, offset + read);
                    if (r < 0) {
                        result.readErrors++;
                        result.badSectors++;
                        result.badSectorList.append(offset / 512 + read / 512);
                        break;
                    }
                    read += r;
                }

                // Verify data for destructive test
                if (mode == Mode::ReadWrite) {
                    for (qint64 i = 0; i < thisBlockSize; i += 512) {
                        if (::memcmp(writeBuf + i, readBuf + i, 512) != 0) {
                            result.badSectors++;
                            result.badSectorList.append(offset / 512 + i / 512);
                        }
                    }
                }
            }
        }
    }

    std::free(writeBuf); std::free(readBuf);
    ::close(fd);

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.summary = formatResult(result);
    return result;
}

bool BadBlocks::detectFakeFlash(const QString &devicePath, qint64 reportedSize,
                                std::function<void(int)> progressCallback) {
    int fd = ::open(devicePath.toUtf8().constData(), O_RDWR | O_SYNC);
    if (fd < 0) return false;

    qint64 testSize = qMin(reportedSize, static_cast<qint64>(2LL * 1024 * 1024 * 1024));
    qint64 sectorSize = 512;
    int numSamples = 100;

    char *writeBuf = new char[sectorSize];
    char *readBuf = new char[sectorSize];

    bool isFake = false;

    for (int i = 0; i < numSamples; i++) {
        if (progressCallback)
            progressCallback(i * 100 / numSamples);

        // Write different patterns at each sample point
        qint64 offset = (static_cast<qint64>(i) * testSize / numSamples);
        offset = (offset / sectorSize) * sectorSize;

        // Use alternating patterns (0xAA, 0x55, 0xFF, 0x00)
        uint8_t pattern = (i % 4 == 0) ? 0xAA : (i % 4 == 1) ? 0x55 :
                          (i % 4 == 2) ? 0xFF : 0x00;
        ::memset(writeBuf, pattern, sectorSize);

        if (::pwrite(fd, writeBuf, sectorSize, offset) != sectorSize) {
            isFake = true;
            break;
        }
        ::fsync(fd);

        ::memset(readBuf, 0, sectorSize);
        if (::pread(fd, readBuf, sectorSize, offset) != sectorSize) {
            isFake = true;
            break;
        }

        if (::memcmp(writeBuf, readBuf, sectorSize) != 0) {
            isFake = true;
            break;
        }
    }

    delete[] writeBuf;
    delete[] readBuf;
    ::close(fd);
    return isFake;
}

QString BadBlocks::formatResult(const BadBlockResult &result) {
    QString s = QStringLiteral("Bad block check completed: %1 sectors checked, "
                               "%2 bad sectors, %3 read errors, %4 write errors")
        .arg(result.totalSectors)
        .arg(result.badSectors)
        .arg(result.readErrors)
        .arg(result.writeErrors);

    if (result.badSectors > 0) {
        s += QStringLiteral("\nBad sectors at: ");
        QStringList sectors;
        for (int i = 0; i < qMin(result.badSectorList.size(), 20); i++)
            sectors << QString::number(result.badSectorList[i]);
        s += sectors.join(QStringLiteral(", "));
        if (result.badSectorList.size() > 20)
            s += QStringLiteral("... (%1 total)").arg(result.badSectorList.size());
    }

    return s;
}
