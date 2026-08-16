#include "AboutDialog.h"
#include "utils/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QDialog>
#include <QApplication>
#include <QPixmap>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
#include <QFileInfo>
#include <QFile>
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

    // ── Top: small pendrive icon (left) + main text block (right) ──
    auto *top = new QHBoxLayout;
    top->setSpacing(16);

    auto *iconLabel = new QLabel;
    QPixmap iconPix(QStringLiteral(":/icons/icons/rufus-128.png"));
    if (!iconPix.isNull())
        iconLabel->setPixmap(iconPix.scaled(48, 48, Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    top->addWidget(iconLabel);

    auto *text = new QVBoxLayout;
    text->setSpacing(2);

    // Header (like the original: "Rufus, the trustworthy USB formatting tool")
    auto *title = new QLabel(tr("Rufus - The Reliable USB Formatting Utility"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    title->setWordWrap(true);
    text->addWidget(title);

    auto *version = new QLabel(tr("Version %1")
        .arg(QApplication::applicationVersion()));
    text->addWidget(version);

    auto *website = new QLabel(tr("Official website: "
        "<a href='https://rufus.ie'>https://rufus.ie</a>"));
    website->setTextInteractionFlags(Qt::TextBrowserInteraction);
    website->setOpenExternalLinks(false);
    text->addWidget(website);

    text->addSpacing(6);

    auto *copyright = new QLabel(tr("Copyright © 2011-2026 Pete Batard"));
    copyright->setWordWrap(true);
    text->addWidget(copyright);

    auto *portCredit = new QLabel(tr("Linux port by FoxOrange224"));
    portCredit->setWordWrap(true);
    text->addWidget(portCredit);

    auto *credits = new QLabel(tr("Credits: Pete Batard (original Rufus "
        "author), FoxOrange224 (Linux port)"));
    credits->setWordWrap(true);
    text->addWidget(credits);

    auto *translations = new QLabel(tr("Translations: Arabic, Chinese "
        "(Simplified), English, Spanish, Persian, French, German, Indonesian, "
        "Japanese, Korean, Portuguese (Brazil), Russian, Turkish, Vietnamese"));
    translations->setWordWrap(true);
    text->addWidget(translations);

    text->addSpacing(6);

    auto *bugsLabel = new QLabel(tr("Report bugs or request enhancements at:"));
    bugsLabel->setWordWrap(true);
    text->addWidget(bugsLabel);

    auto *bugsLink = new QLabel(QStringLiteral("<a href=\"https://github.com/"
        "foxorange224/rufus-linux/issues\">"
        "https://github.com/foxorange224/rufus-linux/issues</a>"));
    bugsLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    bugsLink->setOpenExternalLinks(false);
    text->addWidget(bugsLink);

    top->addLayout(text, 1);
    layout->addLayout(top);

    // ── Middle: additional copyrights box (scrollable, like the original) ──
    auto *copyrightsTitle = new QLabel(tr("Additional Copyrights:"));
    layout->addWidget(copyrightsTitle);

    auto *copyrightsEdit = new QTextEdit;
    copyrightsEdit->setReadOnly(true);
    copyrightsEdit->setPlainText(QStringLiteral(
        "Rufus is licensed under the GNU General Public License v3 (GPLv3).\n"
        "See the LICENSE file or click the \"License\" button for the full "
        "text.\n\n"
        "This Linux port bundles or links against the following third-party "
        "components:\n\n"
        "- Qt 6, Copyright (c) The Qt Company Ltd and contributors.\n"
        "  Licensed under the GNU Lesser General Public License v3 (LGPLv3).\n\n"
        "- zstd, xxhash, FSE and HUF (libs/bled), Copyright (c) Meta "
        "Platforms, Inc. and affiliates; xxhash also Copyright (c) Yann "
        "Collet.\n"
        "  Dual-licensed under the BSD-style license and the GNU General "
        "Public License v2 (GPLv2).\n\n"
        "- FreeDOS (res/freedos), Copyright (c) Jim Hall and the FreeDOS "
        "Project.\n"
        "  Licensed under the GNU General Public License v2 (GPLv2).\n\n"
        "- SYSLINUX (res/syslinux), Copyright (c) H. Peter Anvin and "
        "contributors.\n"
        "  Licensed under the GNU General Public License v2 (GPLv2).\n\n"
        "- GRUB 2 (res/grub2), Copyright (c) the Free Software Foundation.\n"
        "  Licensed under the GNU General Public License v3 (GPLv3).\n\n"
        "- UEFI:NTFS (res/uefi), a UEFI bootloader that provides read/write "
        "access to NTFS drives.\n\n"
        "- MBR boot records and templates (res/mbr), from the original Rufus "
        "project.\n\n"
        "The original Rufus for Windows is Copyright (c) 2011-2026 Pete "
        "Batard <pete@akeo.ie>."));
    layout->addWidget(copyrightsEdit, 1);

    // ── Bottom: License (left) / OK (right), like the original ──
    auto *buttons = new QHBoxLayout;

    auto *licenseBtn = new QPushButton(tr("License"));
    buttons->addWidget(licenseBtn);

    buttons->addStretch();

    auto *okBtn = new QPushButton(tr("OK"));
    okBtn->setDefault(true);
    buttons->addWidget(okBtn);
    layout->addLayout(buttons);

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
    connect(website, &QLabel::linkActivated, openLink);
    connect(bugsLink, &QLabel::linkActivated, openLink);

    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(licenseBtn, &QPushButton::clicked, this, [this]() {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("License"));
        auto *dlgLayout = new QVBoxLayout(&dlg);
        auto *edit = new QTextEdit;
        edit->setReadOnly(true);
        QFile licenseFile(QStringLiteral(":/LICENSE"));
        if (licenseFile.open(QIODevice::ReadOnly))
            edit->setPlainText(QString::fromUtf8(licenseFile.readAll()));
        else
            edit->setPlainText(tr("License file not found."));
        dlgLayout->addWidget(edit);
        auto *closeBtn = new QPushButton(tr("Close"));
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlgLayout->addWidget(closeBtn, 0, Qt::AlignRight);
        dlg.resize(640, 480);
        dlg.exec();
    });

    // Size the dialog to its content (never clips the description or the
    // links, even with larger fonts or after a language change).
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    resize(520, 540);
}