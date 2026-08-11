#include "Hash.h"
#include <QFile>
#include <QFileInfo>

QCryptographicHash::Algorithm Hash::toQtAlgorithm(HashType type) {
    switch (type) {
    case HashType::MD5:    return QCryptographicHash::Md5;
    case HashType::SHA1:   return QCryptographicHash::Sha1;
    case HashType::SHA256: return QCryptographicHash::Sha256;
    case HashType::SHA512: return QCryptographicHash::Sha512;
    }
    return QCryptographicHash::Sha256;
}

QByteArray Hash::compute(const QString &filePath, HashType type,
                         std::function<void(int)> progressCallback) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QCryptographicHash hash(toQtAlgorithm(type));
    qint64 totalSize = file.size();
    qint64 readSize = 0;
    QByteArray buffer;
    buffer.resize(64 * 1024);

    while (!file.atEnd()) {
        qint64 n = file.read(buffer.data(), buffer.size());
        if (n <= 0) break;
        hash.addData(QByteArrayView(buffer.constData(), n));
        readSize += n;
        if (progressCallback)
            progressCallback(static_cast<int>(readSize * 100 / totalSize));
    }

    return hash.result();
}

QByteArray Hash::computeRaw(const QByteArray &data, HashType type) {
    QCryptographicHash hash(toQtAlgorithm(type));
    hash.addData(data);
    return hash.result();
}

bool Hash::verify(const QString &filePath, const QByteArray &expected, HashType type) {
    QByteArray computed = compute(filePath, type);
    return !computed.isEmpty() && computed == expected;
}

QString Hash::toString(const QByteArray &hash) {
    return QString::fromLatin1(hash.toHex().toLower());
}

QString Hash::typeName(HashType type) {
    switch (type) {
    case HashType::MD5:    return QStringLiteral("MD5");
    case HashType::SHA1:   return QStringLiteral("SHA-1");
    case HashType::SHA256: return QStringLiteral("SHA-256");
    case HashType::SHA512: return QStringLiteral("SHA-512");
    }
    return {};
}

HashType Hash::detectFromFileName(const QString &fileName) {
    QString lower = fileName.toLower();
    if (lower.contains("md5"))   return HashType::MD5;
    if (lower.contains("sha1") || lower.contains("sha-1")) return HashType::SHA1;
    if (lower.contains("sha256") || lower.contains("sha-256")) return HashType::SHA256;
    if (lower.contains("sha512") || lower.contains("sha-512")) return HashType::SHA512;
    return HashType::SHA256;
}

int Hash::hashLength(HashType type) {
    switch (type) {
    case HashType::MD5:    return 32;
    case HashType::SHA1:   return 40;
    case HashType::SHA256: return 64;
    case HashType::SHA512: return 128;
    }
    return 64;
}
