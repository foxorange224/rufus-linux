#include "ThemeWatcher.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStyle>
#include <QTextStream>

namespace {
QString configDir() {
    QString home = QDir::homePath();
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (!xdg.isEmpty())
        return xdg;
    return home + QStringLiteral("/.config");
}

// Parses "R,G,B" (or "#RRGGBB") into a color.
QColor parseRgb(const QString &value) {
    const QStringList parts = value.split(',', Qt::SkipEmptyParts);
    if (parts.size() == 3) {
        bool ok = false;
        int r = parts[0].trimmed().toInt(&ok);
        if (ok) {
            int g = parts[1].trimmed().toInt();
            int b = parts[2].trimmed().toInt();
            return QColor(r, g, b);
        }
    }
    return QColor(value.trimmed());
}

void setRole(QPalette &pal, QPalette::ColorRole role, const QColor &color) {
    if (color.isValid()) {
        pal.setColor(role, color);
        pal.setColor(QPalette::Inactive, role, color);
        pal.setColor(QPalette::Disabled, role, color);
    }
}
}

// KDE stores its color scheme in ~/.config/kdeglobals as [Colors:<Role>]
// groups; the standard role names are Window, Button, View and Selection.
QPalette ThemeWatcher::paletteFromKde() const {
    QPalette pal = QApplication::style()->standardPalette();
    const QString path = configDir() + QStringLiteral("/kdeglobals");
    QSettings s(path, QSettings::IniFormat);

    auto groupColor = [&s](const QString &group, const QString &key) {
        s.beginGroup(group);
        QColor c = parseRgb(s.value(key).toString());
        s.endGroup();
        return c;
    };

    setRole(pal, QPalette::Window, groupColor(QStringLiteral("Colors:Window"),
                                              QStringLiteral("BackgroundNormal")));
    setRole(pal, QPalette::WindowText, groupColor(QStringLiteral("Colors:Window"),
                                                  QStringLiteral("ForegroundNormal")));
    setRole(pal, QPalette::Base, groupColor(QStringLiteral("Colors:View"),
                                            QStringLiteral("BackgroundNormal")));
    setRole(pal, QPalette::AlternateBase, groupColor(QStringLiteral("Colors:View"),
                                                     QStringLiteral("BackgroundAlternate")));
    setRole(pal, QPalette::Text, groupColor(QStringLiteral("Colors:View"),
                                            QStringLiteral("ForegroundNormal")));
    setRole(pal, QPalette::Button, groupColor(QStringLiteral("Colors:Button"),
                                              QStringLiteral("BackgroundNormal")));
    setRole(pal, QPalette::ButtonText, groupColor(QStringLiteral("Colors:Button"),
                                                  QStringLiteral("ForegroundNormal")));
    setRole(pal, QPalette::Highlight, groupColor(QStringLiteral("Colors:Selection"),
                                                 QStringLiteral("BackgroundNormal")));
    setRole(pal, QPalette::HighlightedText, groupColor(QStringLiteral("Colors:Selection"),
                                                       QStringLiteral("ForegroundNormal")));
    setRole(pal, QPalette::Link, groupColor(QStringLiteral("Colors:Selection"),
                                            QStringLiteral("ForegroundLink")));
    setRole(pal, QPalette::ToolTipBase, pal.color(QPalette::Base));
    setRole(pal, QPalette::ToolTipText, pal.color(QPalette::Text));
    setRole(pal, QPalette::PlaceholderText, pal.color(QPalette::Text));
    return pal;
}

// LXQt stores its palette in ~/.config/lxqt/lxqt.conf under [Palette].
QPalette ThemeWatcher::paletteFromLxqt() const {
    QPalette pal = QApplication::style()->standardPalette();
    const QString path = configDir() + QStringLiteral("/lxqt/lxqt.conf");
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("Palette"));

    auto color = [&s](const QString &key) { return parseRgb(s.value(key).toString()); };

    setRole(pal, QPalette::Window, color(QStringLiteral("window_color")));
    setRole(pal, QPalette::WindowText, color(QStringLiteral("window_text_color")));
    setRole(pal, QPalette::Base, color(QStringLiteral("base_color")));
    setRole(pal, QPalette::Text, color(QStringLiteral("text_color")));
    setRole(pal, QPalette::Highlight, color(QStringLiteral("highlight_color")));
    setRole(pal, QPalette::HighlightedText, color(QStringLiteral("highlighted_text_color")));
    setRole(pal, QPalette::ToolTipBase, color(QStringLiteral("tooltip_base_color")));
    setRole(pal, QPalette::ToolTipText, color(QStringLiteral("tooltip_text_color")));
    setRole(pal, QPalette::Link, color(QStringLiteral("link_color")));
    s.endGroup();
    return pal;
}

ThemeWatcher::ThemeWatcher(QObject *parent)
    : QObject(parent), m_debounce(this) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &ThemeWatcher::applyNow);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &ThemeWatcher::onConfigChanged);

    // Watch the theme config files; Qt may drop a watch if the file is
    // replaced, so paths are re-added on each change notification.
    for (const QString &path : {
         configDir() + QStringLiteral("/kdeglobals"),
         configDir() + QStringLiteral("/lxqt/lxqt.conf")}) {
        if (QFile::exists(path))
            m_watcher.addPath(path);
    }
}

ThemeWatcher::~ThemeWatcher() = default;

void ThemeWatcher::onConfigChanged(const QString &path) {
    if (!m_watcher.files().contains(path) && QFile::exists(path))
        m_watcher.addPath(path);
    schedule();
}

void ThemeWatcher::schedule() {
    m_debounce.start();
}

void ThemeWatcher::applyNow() {
    QPalette pal;
    const QString kde = configDir() + QStringLiteral("/kdeglobals");
    const QString lxqt = configDir() + QStringLiteral("/lxqt/lxqt.conf");
    const QString de = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();

    if (de.contains(QStringLiteral("lxqt")) || (!de.contains(QStringLiteral("kde")) &&
                                               QFile::exists(lxqt) &&
                                               !QFile::exists(kde)))
        pal = paletteFromLxqt();
    else if (de.contains(QStringLiteral("kde")) || QFile::exists(kde))
        pal = paletteFromKde();
    else if (QFile::exists(lxqt))
        pal = paletteFromLxqt();

    if (pal != QApplication::palette())
        QApplication::setPalette(pal);
}