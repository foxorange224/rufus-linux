#pragma once

#include <QString>
#include <QStringList>

struct MountInfo {
    QString device;
    QString mountPoint;
    QString fsType;
    QStringList options;
    bool isReadOnly = false;
};

class Mounter {
public:
    static bool mount(const QString &devicePath, const QString &mountPoint,
                      const QString &fsType = QString(),
                      const QStringList &options = {});
    static bool unmount(const QString &path, bool force = false);
    static bool isMounted(const QString &path);
    static QString findMountPoint(const QString &devicePath);
    static QList<MountInfo> getAllMounts();
    static QString createTempMountPoint();
    static bool removeMountPoint(const QString &mountPoint);

private:
    static bool execMount(const QStringList &args);
    static bool parseMountInfo(const QString &line, MountInfo &info);
};
