#include "DriveWriter.h"
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>

static constexpr qint64 kBufSize = 1024 * 1024;
static constexpr int kSectorSize = 512;

static char* allocAlignedBuffer() {
    void *ptr = nullptr;
    if (posix_memalign(&ptr, kSectorSize, kBufSize) != 0)
        ptr = std::malloc(kBufSize);
    return static_cast<char*>(ptr);
}

static int openDeviceForWrite(const QString &path, bool directIo = true) {
    if (directIo) {
        int fd = ::open(path.toUtf8().constData(), O_WRONLY | O_SYNC | O_DIRECT);
        if (fd >= 0)
            return fd;
        // Fallback to non-O_DIRECT if not supported
    }
    return ::open(path.toUtf8().constData(), O_WRONLY | O_SYNC);
}

static int openDeviceForRead(const QString &path) {
    return ::open(path.toUtf8().constData(), O_RDONLY);
}

WriteResult DriveWriter::writeImage(const QString &imagePath, const QString &devicePath,
                                    bool isCompressed,
                                    std::function<void(qint64)> progressCallback,
                                    std::function<bool()> isCancelled) {
    if (isCompressed) {
        // Stream through decompressor: zcat/xzcat/etc
        WriteResult result;
        result.totalBytes = QFileInfo(imagePath).size();

        QStringList cmd;
        QString ext = QFileInfo(imagePath).suffix().toLower();
        if (ext == "gz" || ext == "gzip")  cmd = {"zcat", imagePath};
        else if (ext == "xz")              cmd = {"xzcat", imagePath};
        else if (ext == "bz2")             cmd = {"bzcat", imagePath};
        else if (ext == "zst" || ext == "zstd") cmd = {"zstdcat", imagePath};
        else if (ext == "lzma")            cmd = {"lzcat", imagePath};
        else {
            // Unknown compression, try DD directly
            return writeDD(imagePath, devicePath, progressCallback, isCancelled);
        }

        // O_DIRECT requires 512-byte aligned buffers AND lengths. The
        // decompressor emits chunks of arbitrary size, so a non-aligned tail
        // write would fail with EINVAL and lose data silently. Use plain
        // buffered I/O here (O_SYNC still guarantees the data hits the disk).
        int fd = openDeviceForWrite(devicePath, /*directIo=*/false);
        if (fd < 0) {
            result.errorMessage = QStringLiteral("Cannot open device: %1").arg(strerror(errno));
            return result;
        }

        QProcess dec;
        dec.start(cmd[0], QStringList{cmd[1]});
        if (!dec.waitForStarted(5000)) {
            ::close(fd);
            result.errorMessage = QStringLiteral("Cannot start decompressor: %1").arg(cmd[0]);
            return result;
        }

        char *buffer = allocAlignedBuffer();
        QElapsedTimer timer;
        timer.start();
        qint64 totalWritten = 0;
        int writeErrors = 0;

        while (!dec.waitForFinished(100)) {
            if (isCancelled && isCancelled()) {
                dec.kill();
                dec.waitForFinished(3000);
                result.cancelled = true;
                break;
            }
            qint64 avail = dec.bytesAvailable();
            if (avail == 0) {
                if (dec.state() == QProcess::NotRunning) break;
                QThread::msleep(10);
                continue;
            }
            while (dec.bytesAvailable() > 0) {
                qint64 n = dec.read(buffer, kBufSize);
                if (n <= 0) break;
                qint64 written = 0;
                while (written < n) {
                    qint64 w = ::write(fd, buffer + written, n - written);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        writeErrors++;
                        break;
                    }
                    written += w;
                }
                totalWritten += written;
                if (progressCallback) progressCallback(totalWritten);
            }
        }

        if (result.cancelled) {
            // Drain nothing: the stream is being killed, so just make sure
            // whatever was already written hits the device before closing.
            std::free(buffer);
            ::fsync(fd);
            ::close(fd);
            result.bytesWritten = totalWritten;
            result.elapsedSeconds = timer.elapsed() / 1000.0;
            return result;
        }

        // Read any remaining data
        QByteArray remaining = dec.readAll();
        while (!remaining.isEmpty()) {
            qint64 n = qMin(static_cast<qint64>(remaining.size()), kBufSize);
            qint64 written = 0;
            while (written < n) {
                qint64 w = ::write(fd, remaining.constData() + written, n - written);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    writeErrors++;
                    break;
                }
                written += w;
            }
            totalWritten += written;
            remaining.remove(0, n);
            if (progressCallback) progressCallback(totalWritten);
        }

        std::free(buffer);
        ::fsync(fd);
        ::close(fd);

        result.bytesWritten = totalWritten;
        result.elapsedSeconds = timer.elapsed() / 1000.0;
        result.errors = writeErrors;
        if (writeErrors > 0)
            result.errorMessage = QStringLiteral("Write errors while streaming: %1").arg(writeErrors);
        result.success = (writeErrors == 0 && dec.exitCode() == 0);
        return result;
    }
    return writeDD(imagePath, devicePath, progressCallback);
}

WriteResult DriveWriter::writeDD(const QString &imagePath, const QString &devicePath,
                                 std::function<void(qint64)> progressCallback,
                                 std::function<bool()> isCancelled) {
    WriteResult result;

    QFile image(imagePath);
    if (!image.open(QIODevice::ReadOnly)) {
        result.errorMessage = QStringLiteral("Cannot open image: %1").arg(image.errorString());
        return result;
    }

    int fd = openDeviceForWrite(devicePath);
    if (fd < 0) {
        result.errorMessage = QStringLiteral("Cannot open device: %1").arg(strerror(errno));
        return result;
    }
    bool directIo = (::fcntl(fd, F_GETFL) & O_DIRECT) != 0;

    // O_DIRECT rejects writes whose length is not a multiple of the sector
    // size, so an image whose size is not sector-aligned would lose its tail.
    // Open a non-O_DIRECT descriptor to flush such a tail safely.
    int fdPlain = -1;
    if (directIo) {
        fdPlain = ::open(devicePath.toUtf8().constData(), O_WRONLY | O_SYNC);
    }
    auto writeAll = [&](int targetFd, const char *data, qint64 len) -> bool {
        qint64 written = 0;
        while (written < len) {
            qint64 w = ::write(targetFd, data + written, len - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            written += w;
        }
        return true;
    };

    result.totalBytes = image.size();
    QElapsedTimer timer;
    timer.start();

    char *buffer = allocAlignedBuffer();
    qint64 totalWritten = 0;
    int writeErrors = 0;

    while (!image.atEnd()) {
        if (isCancelled && isCancelled()) {
            result.cancelled = true;
            break;
        }
        qint64 n = image.read(buffer, kBufSize);
        if (n <= 0) break;

        if (directIo && (n % kSectorSize) != 0) {
            // Tail chunk: not sector aligned. Write the aligned part through
            // the O_DIRECT descriptor and the remainder through a plain one.
            qint64 aligned = (n / kSectorSize) * kSectorSize;
            bool ok = true;
            if (aligned > 0)
                ok = writeAll(fd, buffer, aligned);
            totalWritten += aligned;
            if (ok && fdPlain >= 0 && n > aligned)
                ok = ::pwrite(fdPlain, buffer + aligned, n - aligned, totalWritten);
            else if (ok && n > aligned)
                ok = writeAll(fd, buffer + aligned, n - aligned);
            if (!ok) writeErrors++;
            totalWritten += (ok ? (n - aligned) : 0);
        } else {
            if (!writeAll(fd, buffer, n)) writeErrors++;
            totalWritten += n;
        }

        if (progressCallback) progressCallback(totalWritten);
    }

    std::free(buffer);
    ::fsync(fd);
    ::close(fd);
    if (fdPlain >= 0) {
        ::fsync(fdPlain);
        ::close(fdPlain);
    }
    image.close();

    result.bytesWritten = totalWritten;
    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.errors = writeErrors;
    if (writeErrors > 0)
        result.errorMessage = QStringLiteral("Write errors: %1").arg(writeErrors);
    result.success = !result.cancelled && (writeErrors == 0 &&
                                           totalWritten == result.totalBytes);

    return result;
}

WriteResult DriveWriter::writeZeros(const QString &devicePath, qint64 numBytes,
                                    std::function<void(qint64)> progressCallback,
                                    std::function<bool()> isCancelled) {
    WriteResult result;

    int fd = openDeviceForWrite(devicePath);
    if (fd < 0) {
        result.errorMessage = QStringLiteral("Cannot open device: %1").arg(strerror(errno));
        return result;
    }

    if (numBytes <= 0) {
        numBytes = ::lseek(fd, 0, SEEK_END);
        ::lseek(fd, 0, SEEK_SET);
    }

    result.totalBytes = numBytes;
    QElapsedTimer timer;
    timer.start();

    char *buffer = allocAlignedBuffer();
    std::memset(buffer, 0, kBufSize);
    qint64 totalWritten = 0;

    while (totalWritten < numBytes) {
        if (isCancelled && isCancelled()) {
            result.cancelled = true;
            break;
        }
        qint64 toWrite = qMin(kBufSize, numBytes - totalWritten);
        qint64 w = ::write(fd, buffer, toWrite);
        if (w <= 0) break;
        totalWritten += w;
        if (progressCallback) progressCallback(totalWritten);
    }

    std::free(buffer);
    ::fsync(fd);
    ::close(fd);

    result.bytesWritten = totalWritten;
    result.elapsedSeconds = timer.elapsed() / 1000.0;
    result.success = (totalWritten >= numBytes);

    return result;
}

WriteResult DriveWriter::writeSparse(const QString &imagePath, const QString &devicePath,
                                     std::function<void(qint64)> progressCallback) {
    // For sparse files, we can use seek to skip zero blocks
    // For now, just do regular write
    return writeDD(imagePath, devicePath, progressCallback);
}

bool DriveWriter::syncDevice(const QString &devicePath) {
    int fd = ::open(devicePath.toUtf8().constData(), O_RDONLY);
    if (fd < 0) return false;
    int ret = ::fsync(fd);
    ::close(fd);
    return ret == 0;
}

int DriveWriter::openDeviceRaw(const QString &path, bool write) {
    int flags = write ? (O_WRONLY | O_SYNC) : O_RDONLY;
    return ::open(path.toUtf8().constData(), flags);
}

bool DriveWriter::isSectorAligned(qint64 offset, int sectorSize) {
    return (offset % sectorSize) == 0;
}
