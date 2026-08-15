#include "Settings.h"
#include <QSettings>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDebug>
#include <pwd.h>
#include <unistd.h>

Settings &Settings::instance() {
    static Settings s_instance;
    return s_instance;
}

void Settings::init(const QString &appDir, const QString &userConfigDir) {
    m_userConfigDir = userConfigDir;
    m_portableMode = detectPortableMode(appDir);
    if (m_settings) {
        delete m_settings;
    }
    m_settings = createSettings(appDir);

    qDebug() << "Settings initialized:" << (m_portableMode ? "portable (rufus.ini)" : "system (QSettings)")
             << (m_userConfigDir.isEmpty() ? "" : "user config:" + m_userConfigDir);
}

bool Settings::detectPortableMode(const QString &appDir) {
    QFileInfo iniFile(appDir + QStringLiteral("/rufus.ini"));
    return iniFile.exists() && iniFile.isFile();
}

QSettings *Settings::createSettings(const QString &appDir) {
    if (m_portableMode) {
        QString iniPath = appDir + QStringLiteral("/rufus.ini");
        return new QSettings(iniPath, QSettings::IniFormat);
    } else if (!m_userConfigDir.isEmpty()) {
        // Use the original user's config directory (for sudo support)
        QDir configDir(m_userConfigDir);
        if (!configDir.exists()) {
            configDir.mkpath(".");
        }
        QString iniPath = configDir.filePath(QStringLiteral("Rufus.conf"));
        return new QSettings(iniPath, QSettings::IniFormat);
    } else {
        return new QSettings(QStringLiteral("Rufus"), QStringLiteral("Rufus"));
    }
}

QByteArray Settings::geometry() const {
    if (!m_settings) return QByteArray();
    return m_settings->value(QStringLiteral("geometry")).toByteArray();
}

void Settings::setGeometry(const QByteArray &geometry) {
    if (m_settings) m_settings->setValue(QStringLiteral("geometry"), geometry);
}

QByteArray Settings::windowState() const {
    if (!m_settings) return QByteArray();
    return m_settings->value(QStringLiteral("windowState")).toByteArray();
}

void Settings::setWindowState(const QByteArray &state) {
    if (m_settings) m_settings->setValue(QStringLiteral("windowState"), state);
}

int Settings::partitionScheme() const {
    if (!m_settings) return 0; // MBR
    return m_settings->value(QStringLiteral("scheme"), 0).toInt();
}

void Settings::setPartitionScheme(int scheme) {
    if (m_settings) m_settings->setValue(QStringLiteral("scheme"), scheme);
}

int Settings::filesystem() const {
    if (!m_settings) return 1; // FAT32
    return m_settings->value(QStringLiteral("filesystem"), 1).toInt();
}

void Settings::setFilesystem(int fs) {
    if (m_settings) m_settings->setValue(QStringLiteral("filesystem"), fs);
}

QString Settings::bootloader() const {
    if (!m_settings) return QStringLiteral("none");
    return m_settings->value(QStringLiteral("bootloader"), QStringLiteral("none")).toString();
}

void Settings::setBootloader(const QString &bootloader) {
    if (m_settings) m_settings->setValue(QStringLiteral("bootloader"), bootloader);
}

int Settings::bootType() const {
    if (!m_settings) return 3; // Image
    return m_settings->value(QStringLiteral("bootType"), 3).toInt();
}

void Settings::setBootType(int type) {
    if (m_settings) m_settings->setValue(QStringLiteral("bootType"), type);
}

bool Settings::quickFormat() const {
    if (!m_settings) return true;
    return m_settings->value(QStringLiteral("quickFormat"), true).toBool();
}

void Settings::setQuickFormat(bool enabled) {
    if (m_settings) m_settings->setValue(QStringLiteral("quickFormat"), enabled);
}

int Settings::targetSystem() const {
    if (!m_settings) return 0; // BIOS
    return m_settings->value(QStringLiteral("targetSystem"), 0).toInt();
}

void Settings::setTargetSystem(int system) {
    if (m_settings) m_settings->setValue(QStringLiteral("targetSystem"), system);
}

bool Settings::verifyWrite() const {
    if (!m_settings) return true;
    return m_settings->value(QStringLiteral("verifyWrite"), true).toBool();
}

void Settings::setVerifyWrite(bool enabled) {
    if (m_settings) m_settings->setValue(QStringLiteral("verifyWrite"), enabled);
}

bool Settings::badBlocks() const {
    if (!m_settings) return false;
    return m_settings->value(QStringLiteral("badBlocks"), false).toBool();
}

void Settings::setBadBlocks(bool enabled) {
    if (m_settings) m_settings->setValue(QStringLiteral("badBlocks"), enabled);
}

bool Settings::persistence() const {
    if (!m_settings) return false;
    return m_settings->value(QStringLiteral("persistence"), false).toBool();
}

void Settings::setPersistence(bool enabled) {
    if (m_settings) m_settings->setValue(QStringLiteral("persistence"), enabled);
}

QString Settings::volumeLabel() const {
    if (!m_settings) return QString();
    return m_settings->value(QStringLiteral("volumeLabel"), QString()).toString();
}

void Settings::setVolumeLabel(const QString &label) {
    if (m_settings) m_settings->setValue(QStringLiteral("volumeLabel"), label);
}

bool Settings::listUsbHdd() const {
    if (!m_settings) return false;
    return m_settings->value(QStringLiteral("listUsbHdd"), false).toBool();
}

void Settings::setListUsbHdd(bool enabled) {
    if (m_settings) m_settings->setValue(QStringLiteral("listUsbHdd"), enabled);
}

QStringList Settings::recentImages() const {
    if (!m_settings) return QStringList();
    return m_settings->value(QStringLiteral("recentImages")).toStringList();
}

void Settings::setRecentImages(const QStringList &images) {
    if (m_settings) m_settings->setValue(QStringLiteral("recentImages"), images);
}

int Settings::imageOption() const {
    if (!m_settings) return 0; // Standard Windows installation
    return m_settings->value(QStringLiteral("imageOption"), 0).toInt();
}

void Settings::setImageOption(int option) {
    if (m_settings) m_settings->setValue(QStringLiteral("imageOption"), option);
}

bool Settings::advancedFormatVisible() const {
    if (!m_settings) return false;
    return m_settings->value(QStringLiteral("advancedFormat"), false).toBool();
}

void Settings::setAdvancedFormatVisible(bool visible) {
    if (m_settings) m_settings->setValue(QStringLiteral("advancedFormat"), visible);
}

bool Settings::advancedDriveVisible() const {
    if (!m_settings) return false;
    return m_settings->value(QStringLiteral("advancedDrive"), false).toBool();
}

void Settings::setAdvancedDriveVisible(bool visible) {
    if (m_settings) m_settings->setValue(QStringLiteral("advancedDrive"), visible);
}

QString Settings::language() const {
    if (!m_settings) return QString();
    QString lang = m_settings->value(QStringLiteral("language"), QString()).toString();
    lang.replace(QChar('_'), QChar('-'));
    return lang;
}

void Settings::setLanguage(const QString &code) {
    if (m_settings) m_settings->setValue(QStringLiteral("language"), code);
}

QString Settings::style() const {
    if (!m_settings) return QStringLiteral("fusion");
    return m_settings->value(QStringLiteral("style"), QStringLiteral("fusion")).toString();
}

void Settings::setStyle(const QString &name) {
    if (m_settings) m_settings->setValue(QStringLiteral("style"), name);
}

bool Settings::firstRun() const {
    if (!m_settings) return true;
    return m_settings->value(QStringLiteral("firstRun"), true).toBool();
}

void Settings::setFirstRun(bool done) {
    if (m_settings) m_settings->setValue(QStringLiteral("firstRun"), done);
}

void Settings::sync() {
    if (m_settings) m_settings->sync();
}