#pragma once

#include <QString>
#include <QByteArray>
#include <QCryptographicHash>
#include <functional>

enum class HashType {
    MD5,
    SHA1,
    SHA256,
    SHA512
};

class Hash {
public:
    static QByteArray compute(const QString &filePath, HashType type,
                              std::function<void(int)> progressCallback = nullptr);
    static QByteArray computeRaw(const QByteArray &data, HashType type);
    static bool verify(const QString &filePath, const QByteArray &expected, HashType type);
    static QString toString(const QByteArray &hash);
    static QString typeName(HashType type);
    static HashType detectFromFileName(const QString &fileName);
    static int hashLength(HashType type);

private:
    static QCryptographicHash::Algorithm toQtAlgorithm(HashType type);
};
