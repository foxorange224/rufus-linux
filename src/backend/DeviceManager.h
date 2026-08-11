#pragma once

#include <QString>
#include <QList>
#include <QStringList>

struct DeviceInfo {
    QString path;
    QString name;
    QString model;
    QString vendor;
    QString serial;
    qint64 size = 0;
    int sectorSize = 512;
    bool isRemovable = false;
    bool isUsb = false;
    bool isMmc = false;
    bool isNvme = false;
    bool isSsd = false;
    bool isLoop = false;
    QString loopBackingFile;   // backing file while attached (loop devices)
    bool isReadOnly = false;
    bool isSystem = false;
    int index = -1;
    QStringList partitions;
    QStringList mountedVolumes;

    bool isNvmePath() const { return path.contains("nvme"); }
    QString partitionPath(int num) const;
};

class DeviceManager {
public:
    static QList<DeviceInfo> enumerate();
    static DeviceInfo getDevice(const QString &path);
    static DeviceInfo getDeviceByPath(const QString &path);
    static bool isDeviceBusy(const QString &path);
    static QStringList getBlockingProcesses(const QString &path);
    static bool unmountPartitions(const QString &devicePath);

private:
    static DeviceInfo parseSysBlock(const QString &sysPath);
    static QString readSysAttr(const QString &sysPath, const QString &attr);
    static QString resolveDevicePath(const QString &entry);
};

inline QString DeviceInfo::partitionPath(int num) const {
    if (isNvmePath())
        return path + QStringLiteral("p%1").arg(num);
    if (path.contains("mmcblk"))
        return path + QStringLiteral("p%1").arg(num);
    return path + QString::number(num);
}
