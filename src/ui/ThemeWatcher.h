#pragma once

#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QPalette>

// Watches the active desktop environment's theme configuration (KDE Plasma's
// kdeglobals, or LXQt's lxqt.conf) and reapplies a matching QPalette whenever
// the user switches the global color scheme, so Rufus follows theme changes
// without a restart. Changes are debounced to avoid thrashing the palette
// while the config file is being rewritten.
class ThemeWatcher : public QObject {
    Q_OBJECT
public:
    explicit ThemeWatcher(QObject *parent = nullptr);
    ~ThemeWatcher() override;

    // Re-reads the current desktop theme config and applies a palette now.
    void applyNow();

private:
    void schedule();
    void onConfigChanged(const QString &path);
    QPalette paletteFromKde() const;
    QPalette paletteFromLxqt() const;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};