#include "MainWindow.h"
#include "utils/Logger.h"
#include "utils/Localization.h"
#include "utils/MsgBox.h"
#include "utils/Settings.h"
#include "backend/DeviceManager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <unistd.h>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QProcess>
#include <QPixmap>
#include <pwd.h>

static bool g_debugEnabled = false;

static void debugLog(const QString &msg) {
    if (!g_debugEnabled)
        return;
    QFile f(QStringLiteral("/tmp/rufus_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&f) << msg << "\n";
    }
}

static void printVersion() {
    QTextStream out(stdout);
    out << QStringLiteral("Rufus %1 - Create bootable USB drives\n")
            .arg(QApplication::applicationVersion())
        << QStringLiteral("Linux port using Qt6 and C++\n")
        << QStringLiteral("GNU GPL v3 License\n");
}

static void printHelp(const QCommandLineParser &parser) {
    QTextStream out(stdout);
    out << QStringLiteral("Usage: rufus [options]\n\n")
        << parser.helpText();
}

static int listDevices() {
    QList<DeviceInfo> devices = DeviceManager::enumerate();
    QTextStream out(stdout);
    for (const DeviceInfo &dev : devices) {
        double sizeGiB = dev.size / (1024.0 * 1024 * 1024);
        out << dev.path << QStringLiteral("  ")
            << QString::number(sizeGiB, 'f', 1) << QStringLiteral("G  ")
            << dev.name;
        if (!dev.model.isEmpty())
            out << QStringLiteral("  [") << dev.model << QStringLiteral("]");
        if (dev.isUsb)
            out << QStringLiteral("  USB");
        if (dev.isSystem)
            out << QStringLiteral("  SYSTEM");
        out << QStringLiteral("\n");
    }
    return 0;
}

static QIcon makeAppIcon() {
    QIcon icon;
    static const int sizes[] = {16, 24, 32, 44, 48, 64, 72, 128, 150, 256, 512};
    for (int s : sizes) {
        QIcon sub(QStringLiteral(":/icons/icons/rufus-%1.png").arg(s));
        if (!sub.isNull())
            icon.addPixmap(sub.pixmap(s, s));
    }
    if (icon.isNull())
        icon = QIcon(QStringLiteral(":/icons/icons/rufus.ico"));
    return icon;
}

// Get the original user's home directory when running under sudo
static QString getOriginalUserHomeDir() {
    const char *sudoUser = qgetenv("SUDO_USER").constData();
    if (sudoUser && sudoUser[0] != '\0') {
        struct passwd *pw = getpwnam(sudoUser);
        if (pw && pw->pw_dir) {
            return QString::fromUtf8(pw->pw_dir);
        }
    }

    const char *sudoUid = qgetenv("SUDO_UID").constData();
    if (sudoUid && sudoUid[0] != '\0') {
        uid_t uid = static_cast<uid_t>(atoi(sudoUid));
        struct passwd *pw = getpwuid(uid);
        if (pw && pw->pw_dir) {
            return QString::fromUtf8(pw->pw_dir);
        }
    }

    // pkexec (used by the .desktop entry) sets PKEXEC_UID instead of SUDO_*
    const char *pkexecUid = qgetenv("PKEXEC_UID").constData();
    if (pkexecUid && pkexecUid[0] != '\0') {
        uid_t uid = static_cast<uid_t>(atoi(pkexecUid));
        struct passwd *pw = getpwuid(uid);
        if (pw && pw->pw_dir) {
            return QString::fromUtf8(pw->pw_dir);
        }
    }

    return QString();
}

// When Rufus runs as root through sudo/pkexec, Qt's platform theme must see
// the original user's configuration (kdeglobals, icon theme, color scheme,
// fonts) instead of root's, otherwise the style, icons and dark/light mode
// of the desktop are lost. Must be called before QApplication is created.
static void useOriginalUserEnvironment(const QString &homeDir) {
    if (homeDir.isEmpty())
        return;
    qputenv("HOME", homeDir.toUtf8());
    if (qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME"))
        qputenv("XDG_CONFIG_HOME", (homeDir + QStringLiteral("/.config")).toUtf8());
    if (qEnvironmentVariableIsEmpty("XDG_DATA_HOME"))
        qputenv("XDG_DATA_HOME", (homeDir + QStringLiteral("/.local/share")).toUtf8());
    if (qEnvironmentVariableIsEmpty("XDG_CACHE_HOME"))
        qputenv("XDG_CACHE_HOME", (homeDir + QStringLiteral("/.cache")).toUtf8());
}

static QStringList checkMissingDependencies() {
    static const char *tools[] = {"syslinux", "grub-install", "7z", "fuseiso"};
    QStringList missing;
    for (const char *t : tools) {
        QProcess p;
        p.start(QStringLiteral("which"), {QString::fromLatin1(t)});
        if (!p.waitForFinished(3000) || p.exitCode() != 0)
            missing << QString::fromLatin1(t);
    }
    return missing;
}

static void showDependencyError(const QStringList &missing) {
    QString joined = missing.join(QStringLiteral(", "));
    MsgBox::critical(nullptr,
        QCoreApplication::translate("main", "Rufus - Missing dependencies"),
        QCoreApplication::translate("main",
            "Rufus cannot run because required dependencies are missing from your "
            "system.\n\n"
            "Missing dependencies:\n%1\n\n"
            "Install them using the package manager that comes by default with "
            "your distribution.").arg(joined));
}

static void showFirstRunWelcome() {
    QDialog dlg;
    dlg.setWindowTitle(QCoreApplication::translate("main", "Welcome to Rufus"));
    dlg.setWindowIcon(QApplication::windowIcon());
    dlg.setWindowFlag(Qt::WindowCloseButtonHint, false);
    dlg.setMinimumWidth(440);

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(14);

    QLabel *icon = new QLabel;
    icon->setPixmap(QApplication::windowIcon().pixmap(96, 96));
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);

    QLabel *text = new QLabel(
        QCoreApplication::translate("main",
            "Welcome to Rufus! Thank you for using this tool. We are thrilled that "
            "you care about our work as much as we enjoy working on it.\n\n"
            "Rufus requires administrator privileges to access storage devices.\n\n"
            "To continue, please choose your language and press Accept."));
    text->setWordWrap(true);
    text->setAlignment(Qt::AlignCenter);
    layout->addWidget(text);

    QComboBox *lang = new QComboBox;
    for (const QString &code : Localization::availableLocales())
        lang->addItem(QLocale(code).nativeLanguageName(), code);

    QString currentLang = Localization::currentLanguage();
    currentLang.replace(QChar('_'), QChar('-'));

    int cur = lang->findData(currentLang);
    if (cur < 0) {
        QString langOnly = currentLang.left(2);
        cur = lang->findData(langOnly);
    }
    if (cur < 0) {
        cur = lang->findData(QStringLiteral("en"));
    }
    if (cur >= 0)
        lang->setCurrentIndex(cur);
    layout->addWidget(lang);

    auto *footer = new QHBoxLayout;
    QLabel *footerLabel = new QLabel(QStringLiteral("by FoxOrange224"));
    footer->addWidget(footerLabel);
    footer->addStretch();
    QPushButton *accept = new QPushButton(QCoreApplication::translate("main", "Accept"));
    accept->setDefault(true);
    footer->addWidget(accept);
    layout->addLayout(footer);

    QObject::connect(accept, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() == QDialog::Accepted) {
        QString code = lang->currentData().toString();
        Localization::setLanguage(code);
        Settings::instance().setLanguage(code);
        Settings::instance().setFirstRun(false);
        Settings::instance().sync();

        dlg.setWindowTitle(QCoreApplication::translate("main", "Welcome to Rufus"));
        text->setText(QCoreApplication::translate("main",
            "Welcome to Rufus! Thank you for using this tool. We are thrilled that "
            "you care about our work as much as we enjoy working on it.\n\n"
            "Rufus requires administrator privileges to access storage devices.\n\n"
            "To continue, please choose your language and press Accept."));
        accept->setText(QCoreApplication::translate("main", "Accept"));
    }
}

int main(int argc, char *argv[]) {
    bool welcomeDone = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--welcome-done") == 0) welcomeDone = true;
        if (strcmp(argv[i], "--debug") == 0) g_debugEnabled = true;
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: rufus [options]\n\n"
                   "Options:\n"
                   "  --help              Show this help\n"
                   "  --version           Show version\n"
                   "  --list-devices      List available devices and exit\n"
                   "  --welcome-done      Internal: welcome dialog already shown\n"
                   "  --debug             Enable debug logging to /tmp/rufus_debug.log\n");
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("Rufus 1.0.1 - Create bootable USB drives\n");
            printf("Linux port using Qt6 and C++\n");
            printf("GNU GPL v3 License\n");
            return 0;
        }
        if (strcmp(argv[i], "--list-devices") == 0) {
        }
    }

    QString userConfigDir;
    if (geteuid() != 0) {
        // Not running as root - show error and exit
        userConfigDir = getOriginalUserHomeDir();
        if (!userConfigDir.isEmpty()) {
            userConfigDir += QStringLiteral("/.config");
        }

        QApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("Rufus"));
        app.setApplicationVersion(QStringLiteral("1.0.1"));
        app.setOrganizationName(QStringLiteral("Rufus"));
        app.setOrganizationDomain(QStringLiteral("rufus.ie"));
        app.setQuitOnLastWindowClosed(true);

        // Initialize localization with system language for the error dialog
        Localization::init(QApplication::applicationDirPath());

        QString msg = QCoreApplication::translate("main",
            "Cannot run Rufus without administrator privileges.\n\n"
            "Please run with sudo:\n  sudo %1").arg(QApplication::applicationFilePath());

        QMessageBox::critical(nullptr,
            QCoreApplication::translate("main", "Rufus - Administrator Required"),
            msg);
        return 1;
    }

    // Running as root - use original user's config directory
    userConfigDir = getOriginalUserHomeDir();
    // Point Qt's platform theme at the user's config (theme, icons, dark/light)
    useOriginalUserEnvironment(userConfigDir);
    if (!userConfigDir.isEmpty()) {
        userConfigDir += QStringLiteral("/.config");
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Rufus"));
    app.setApplicationVersion(QStringLiteral("1.0.1"));
    app.setOrganizationName(QStringLiteral("Rufus"));
    app.setOrganizationDomain(QStringLiteral("rufus.ie"));
    app.setQuitOnLastWindowClosed(true);

    // No forced style: let the platform theme pick the desktop style so the
    // application follows the system theme (dark/light/colorful).
    app.setWindowIcon(makeAppIcon());

    Localization::init(QApplication::applicationDirPath());
    Settings::instance().init(QApplication::applicationDirPath(), userConfigDir);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Create bootable USB drives from ISO/IMG files"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption listOpt(QStringLiteral("list-devices"),
        QStringLiteral("List available devices and exit"));
    parser.addOption(listOpt);

    QCommandLineOption debugOpt(QStringLiteral("debug"),
        QStringLiteral("Enable debug logging to /tmp/rufus_debug.log"));
    parser.addOption(debugOpt);

    QCommandLineOption welcomeDoneOpt(QStringLiteral("welcome-done"),
        QStringLiteral("Internal: welcome dialog already shown"));
    parser.addOption(welcomeDoneOpt);

    QCommandLineOption elevatedOpt(QStringLiteral("elevated"),
        QStringLiteral("Internal: already running with administrator privileges"));
    parser.addOption(elevatedOpt);

    parser.process(app);

    bool showHelp = parser.isSet(QStringLiteral("help"));
    bool showVersion = parser.isSet(QStringLiteral("version"));
    bool listDev = parser.isSet(QStringLiteral("list-devices"));

    if (parser.isSet(QStringLiteral("welcome-done"))) {
        welcomeDone = true;
    }

    if (showHelp) {
        printHelp(parser);
        return 0;
    }

    if (showVersion) {
        printVersion();
        return 0;
    }

    if (listDev) {
        return listDevices();
    }

    if (!welcomeDone) {
        if (Settings::instance().firstRun())
            showFirstRunWelcome();

        QStringList missing = checkMissingDependencies();
        if (!missing.isEmpty()) {
            showDependencyError(missing);
            return 1;
        }
    }

    Logger::init();

    // Log detected system language
    Logger::info(QCoreApplication::translate("main", "Detected language: %1")
                     .arg(Localization::detectedSystemLanguage()));

    MainWindow window;
    window.show();

    debugLog(QStringLiteral("Rufus started (uid=%1)").arg(geteuid()));

    int ret = app.exec();

    Logger::shutdown();
    return ret;
}
