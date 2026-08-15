#pragma once

#include <QSettings>
#include <QByteArray>
#include <QStringList>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>

class Settings {
public:
    static Settings &instance();

    // Initialize settings (portable mode detection)
    // If userConfigDir is provided, use it instead of the default (for sudo support)
    void init(const QString &appDir, const QString &userConfigDir = QString());

    // Settings accessors
    QByteArray geometry() const;
    void setGeometry(const QByteArray &geometry);

    QByteArray windowState() const;
    void setWindowState(const QByteArray &state);

    int partitionScheme() const;
    void setPartitionScheme(int scheme);

    int filesystem() const;
    void setFilesystem(int fs);

    QString bootloader() const;
    void setBootloader(const QString &bootloader);

    int bootType() const;
    void setBootType(int type);

    bool quickFormat() const;
    void setQuickFormat(bool enabled);

    int targetSystem() const;
    void setTargetSystem(int system);

    bool verifyWrite() const;
    void setVerifyWrite(bool enabled);

    bool badBlocks() const;
    void setBadBlocks(bool enabled);

    bool persistence() const;
    void setPersistence(bool enabled);

    QString volumeLabel() const;
    void setVolumeLabel(const QString &label);

    bool listUsbHdd() const;
    void setListUsbHdd(bool enabled);

    QStringList recentImages() const;
    void setRecentImages(const QStringList &images);

    int imageOption() const;
    void setImageOption(int option);

    bool advancedFormatVisible() const;
    void setAdvancedFormatVisible(bool visible);

    bool advancedDriveVisible() const;
    void setAdvancedDriveVisible(bool visible);

    QString language() const;
    void setLanguage(const QString &code);

    // Qt style preference: a style name from QStyleFactory::keys()
    // (defaults to "fusion").
    QString style() const;
    void setStyle(const QString &name);

    bool firstRun() const;
    void setFirstRun(bool done);

    // Sync all pending changes to disk
    void sync();

private:
    Settings() = default;
    ~Settings() = default;
    Settings(const Settings &) = delete;
    Settings &operator=(const Settings &) = delete;

    QSettings *m_settings = nullptr;
    bool m_portableMode = false;
    QString m_userConfigDir;

    // Detect portable mode (rufus.ini in executable directory)
    bool detectPortableMode(const QString &appDir);
    QSettings *createSettings(const QString &appDir);
};