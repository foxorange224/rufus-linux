#include "DeviceManager.h"
#include "Mounter.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStorageInfo>
#include <QRegularExpression>

QString DeviceManager::readSysAttr(const QString &sysPath, const QString &attr) {
    QFile f(sysPath + "/" + attr);
    if (f.open(QIODevice::ReadOnly)) {
        QString val = QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        return val;
    }
    return {};
}

DeviceInfo DeviceManager::parseSysBlock(const QString &sysPath) {
    DeviceInfo dev;
    QString entry = QFileInfo(sysPath).fileName();
    dev.path = QStringLiteral("/dev/") + entry;
    dev.name = entry;

    dev.isRemovable = (readSysAttr(sysPath, "removable") == "1");
    dev.model = readSysAttr(sysPath, "device/model");
    dev.vendor = readSysAttr(sysPath, "device/vendor");
    dev.serial = readSysAttr(sysPath, "serial").simplified();

    QString sizeStr = readSysAttr(sysPath, "size");
    qint64 sectors = sizeStr.toLongLong();
    dev.size = sectors * 512;

    QString ro = readSysAttr(sysPath, "ro");
    dev.isReadOnly = (ro == "1");

    if (entry.startsWith("nvme")) {
        dev.isNvme = true;
        // nvme0n1 -> index 0, extract controller number
        QRegularExpression re("nvme(\\d+)n\\d+");
        auto m = re.match(entry);
        if (m.hasMatch()) dev.index = m.captured(1).toInt();
    } else if (entry.startsWith("mmcblk")) {
        dev.isMmc = true;
        QRegularExpression re("mmcblk(\\d+)");
        auto m = re.match(entry);
        if (m.hasMatch()) dev.index = m.captured(1).toInt();
    } else if (entry.startsWith("sd")) {
        QRegularExpression re("sd([a-z]+)");
        auto m = re.match(entry);
        if (m.hasMatch()) {
            QString letters = m.captured(1);
            int idx = 0;
            for (int i = 0; i < letters.length(); i++)
                idx = idx * 26 + (letters[i].toLatin1() - 'a' + 1);
            dev.index = idx;
        }
    } else if (entry.startsWith("loop")) {
        dev.isLoop = true;
        // Backing file (only present while the loop device is attached).
        // Used to show user-created test drives (e.g. /tmp virtual USB).
        dev.loopBackingFile = readSysAttr(sysPath + "/loop", "backing_file").trimmed();
    }

    // Detect USB by walking up sysfs until we find usb driver
    QString searchPath = sysPath;
    while (searchPath != "/sys") {
        QString uevent = readSysAttr(searchPath, "uevent");
        if (uevent.contains("DEVTYPE=usb_interface") || uevent.contains("DRIVER=usb")) {
            dev.isUsb = true;
            break;
        }
        if (uevent.contains("DRIVER=sd") || uevent.contains("DRIVER=nvme") ||
            uevent.contains("DRIVER=mmcblk"))
            break;
        searchPath = QFileInfo(searchPath).path();
    }

    // Detect if this is the system disk (contains rootfs mount)
    if (dev.size > 0) {
        QStorageInfo rootInfo("/");
        QString rootDev = rootInfo.device();
        // rootDev might be /dev/sda2, /dev/nvme0n1p2, etc.
        if (rootDev.startsWith(dev.path) && rootDev != dev.path)
            dev.isSystem = true;
    }

    // Enumerate partitions
    QDir partDir(sysPath);
    for (const QString &part : partDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (part.startsWith(entry) && part != entry) {
            QString partPath = QStringLiteral("/dev/") + part;
            dev.partitions.append(partPath);
            // Check if mounted
            QString mp = Mounter::findMountPoint(partPath);
            if (!mp.isEmpty())
                dev.mountedVolumes.append(mp);
        }
    }

    return dev;
}

QList<DeviceInfo> DeviceManager::enumerate() {
    QList<DeviceInfo> devices;
    QDir sysBlock("/sys/block");
    QString homeDir = QDir::homePath();

    for (const QString &entry : sysBlock.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        // Skip system loop devices; only show user-created ones whose
        // backing file lives in /tmp or the home directory (virtual USB
        // drives for testing, e.g. losetup /tmp/rufus-vusb.img).
        if (entry.startsWith("loop")) {
            DeviceInfo dev = parseSysBlock(sysBlock.absoluteFilePath(entry));
            if (dev.size <= 0)
                continue;
            if (dev.loopBackingFile.startsWith("/tmp/") ||
                dev.loopBackingFile.startsWith(homeDir + "/"))
                devices.append(dev);
            continue;
        }
        if (entry.startsWith("ram") || entry.startsWith("zram"))
            continue;

        DeviceInfo dev = parseSysBlock(sysBlock.absoluteFilePath(entry));
        if (dev.size <= 0)
            continue;

        // Only show removable devices + non-system fixed drives that are USB/MMC
        if (dev.isRemovable || dev.isUsb || dev.isMmc)
            devices.append(dev);
    }

    // Sort: USB devices first, then by size descending
    std::sort(devices.begin(), devices.end(), [](const DeviceInfo &a, const DeviceInfo &b) {
        if (a.isUsb != b.isUsb) return a.isUsb;
        if (a.isMmc != b.isMmc) return a.isMmc;
        return a.size > b.size;
    });

    return devices;
}

DeviceInfo DeviceManager::getDevice(const QString &path) {
    for (const DeviceInfo &dev : enumerate()) {
        if (dev.path == path)
            return dev;
    }
    return {};
}

DeviceInfo DeviceManager::getDeviceByPath(const QString &path) {
    return getDevice(path);
}

bool DeviceManager::isDeviceBusy(const QString &path) {
    QProcess proc;
    proc.start("lsof", {path, "+D", path});
    if (!proc.waitForFinished(3000))
        return true;
    QByteArray out = proc.readAllStandardOutput().trimmed();
    if (!out.isEmpty()) return true;

    // Also check if any partitions are mounted
    DeviceInfo dev = getDevice(path);
    for (const QString &part : dev.partitions) {
        QProcess mp;
        mp.start("findmnt", {"-n", part});
        if (mp.waitForFinished(3000) && mp.exitCode() == 0)
            return true;
    }
    return false;
}

QStringList DeviceManager::getBlockingProcesses(const QString &path) {
    QStringList procs;
    QProcess lsof;
    lsof.start("lsof", {"-F", "c", path});
    if (lsof.waitForFinished(3000)) {
        for (const QByteArray &line : lsof.readAllStandardOutput().split('\n')) {
            if (line.startsWith('c'))
                procs << QString::fromUtf8(line.mid(1));
        }
    }

    // Also check mount points
    DeviceInfo dev = getDevice(path);
    for (const QString &part : dev.partitions) {
        QProcess fuser;
        fuser.start("fuser", {"-m", part});
        if (fuser.waitForFinished(3000)) {
            QByteArray out = fuser.readAllStandardError();
            // fuser outputs PIDs, we just note that it's in use
            if (!out.trimmed().isEmpty())
                procs << QStringLiteral("(process using %1)").arg(part);
        }
    }

    procs.removeDuplicates();
    return procs;
}

bool DeviceManager::unmountPartitions(const QString &devicePath) {
    DeviceInfo dev = getDevice(devicePath);
    bool allOk = true;
    for (const QString &part : dev.partitions) {
        QString mp = Mounter::findMountPoint(part);
        if (!mp.isEmpty()) {
            if (!Mounter::unmount(mp, true))
                allOk = false;
        }
    }
    // Also try unmounting the device itself (for whole-device mounts)
    QString wholeMp = Mounter::findMountPoint(devicePath);
    if (!wholeMp.isEmpty()) {
        if (!Mounter::unmount(wholeMp, true))
            allOk = false;
    }
    return allOk;
}
