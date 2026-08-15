#include "AboutDialog.h"
#include "utils/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QApplication>
#include <QPixmap>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
#include <QFileInfo>
#include <pwd.h>
#include <unistd.h>
#include <cstdlib>

// Name of the user that launched Rufus through sudo/pkexec, if any.
static QString originalUserName() {
    const char *sudoUser = qgetenv("SUDO_USER").constData();
    if (sudoUser && sudoUser[0] != '\0')
        return QString::fromUtf8(sudoUser);
    const char *uidStr = qgetenv("SUDO_UID").constData();
    if (!uidStr || uidStr[0] == '\0')
        uidStr = qgetenv("PKEXEC_UID").constData();
    if (uidStr && uidStr[0] != '\0') {
        struct passwd *pw = getpwuid(static_cast<uid_t>(atoi(uidStr)));
        if (pw && pw->pw_name)
            return QString::fromUtf8(pw->pw_name);
    }
    return QString();
}

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About Rufus"));

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // Program icon (from the rufus-*.png set embedded in the resources)
    auto *iconLabel = new QLabel;
    QPixmap iconPix(QStringLiteral(":/icons/icons/rufus-256.png"));
    if (!iconPix.isNull())
        iconLabel->setPixmap(iconPix.scaled(72, 72, Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel);

    auto *title = new QLabel(QStringLiteral("<h2>Rufus</h2>"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto *version = new QLabel(tr("Version %1 (Linux Port)")
        .arg(QApplication::applicationVersion()));
    version->setAlignment(Qt::AlignCenter);
    layout->addWidget(version);

    layout->addSpacing(10);

    auto *desc = new QLabel(tr(
        "Create bootable USB drives from ISO/IMG files.\n\n"
        "Originally created by Pete Batard (Akeo)\n"
        "GNU GPL v3 License\n\n"
        "Linux port using Qt6 and C++"));
    desc->setWordWrap(true);
    desc->setMinimumWidth(380);
    desc->setAlignment(Qt::AlignCenter);
    layout->addWidget(desc);

    auto *linkPort = new QLabel(
        QStringLiteral("<p><a href=\"https://www.github.com/foxorange224/rufus-linux\">%1</a></p>")
        .arg(tr("Linux port on GitHub")));
    linkPort->setWordWrap(true);
    linkPort->setAlignment(Qt::AlignCenter);
    linkPort->setTextInteractionFlags(Qt::TextBrowserInteraction);
    linkPort->setOpenExternalLinks(false);
    layout->addWidget(linkPort);

    auto *linkOriginal = new QLabel(
        QStringLiteral("<p><a href=\"https://github.com/pbatard/rufus\">%1</a></p>")
        .arg(tr("Original Rufus (Windows) on GitHub")));
    linkOriginal->setWordWrap(true);
    linkOriginal->setAlignment(Qt::AlignCenter);
    linkOriginal->setTextInteractionFlags(Qt::TextBrowserInteraction);
    linkOriginal->setOpenExternalLinks(false);
    layout->addWidget(linkOriginal);

    // Opening links from a root process: QDesktopServices and xdg-open run
    // as root, where browsers refuse to start, and xdg-open on Plasma 6
    // silently fails when kfmclient is missing. The reliable path is the
    // desktop portal: `gio open` executed as the ORIGINAL user with that
    // user's session environment (DISPLAY/DBus). Everything runs detached,
    // so the dialog never blocks.
    auto openAsUser = [](const QStringList &cmd) {
        const QString user = originalUserName();
        if (user.isEmpty()) {
            QProcess::startDetached(cmd[0], cmd.mid(1));
            return;
        }
        struct passwd *pw = getpwnam(user.toUtf8().constData());
        QString runtimeDir;
        if (pw) {
            const QString rt = QStringLiteral("/run/user/%1").arg(pw->pw_uid);
            if (QFileInfo::exists(rt))
                runtimeDir = rt;
        }
        QString dbusAddr = qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS");
        if (dbusAddr.isEmpty() && !runtimeDir.isEmpty())
            dbusAddr = QStringLiteral("unix:path=%1/bus").arg(runtimeDir);
        const QString display = qEnvironmentVariable("DISPLAY").isEmpty()
            ? QStringLiteral(":0") : qEnvironmentVariable("DISPLAY");

        QStringList args = {QStringLiteral("-u"), user, QStringLiteral("env"),
                            QStringLiteral("DISPLAY=%1").arg(display)};
        if (!runtimeDir.isEmpty())
            args << QStringLiteral("XDG_RUNTIME_DIR=%1").arg(runtimeDir);
        if (!dbusAddr.isEmpty())
            args << QStringLiteral("DBUS_SESSION_BUS_ADDRESS=%1").arg(dbusAddr);
        args << cmd;
        QProcess::startDetached(QStringLiteral("sudo"), args);
    };

    auto openLink = [openAsUser](const QString &url) {
        // As root, QDesktopServices::openUrl "succeeds" by spawning
        // xdg-open as root, which silently fails to open anything, so
        // skip it and go straight to the user's session.
        if (geteuid() != 0 && QDesktopServices::openUrl(QUrl(url)))
            return;
        Logger::warn(QStringLiteral("QDesktopServices::openUrl failed for %1, "
                                    "opening via the desktop portal").arg(url));
        openAsUser({QStringLiteral("gio"), QStringLiteral("open"), url});
        // If the portal is unavailable, fall back to xdg-open in the
        // user's session (both detached, so this is harmless).
        openAsUser({QStringLiteral("xdg-open"), url});
    };
    connect(linkPort, &QLabel::linkActivated, openLink);
    connect(linkOriginal, &QLabel::linkActivated, openLink);

    layout->addStretch();

    auto *closeBtn = new QPushButton(tr("Close"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(closeBtn);

    // Size the dialog to its content (never clips the description or the
    // links, even with larger fonts or after a language change).
    layout->setSizeConstraint(QLayout::SetFixedSize);
}
