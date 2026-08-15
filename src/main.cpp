#include "MainWindow.h"
#include "utils/Logger.h"
#include "utils/Localization.h"
#include "utils/MsgBox.h"
#include "utils/Settings.h"
#include "backend/DeviceManager.h"
#include "backend/PartitionManager.h"
#include "core/ImageHandler.h"
#include "worker/FormatWorker.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <QStyle>
#include <QStyleFactory>
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
    out << QCoreApplication::translate("main", "List of removable devices:")
        << QStringLiteral("\n\n");
    if (devices.isEmpty()) {
        out << QCoreApplication::translate("main", "No removable devices found")
            << QStringLiteral("\n");
        return 0;
    }
    for (const DeviceInfo &dev : devices) {
        double sizeGiB = dev.size / (1024.0 * 1024 * 1024);
        QString sizeText = QString::number(sizeGiB, 'f', 0);
        if (sizeText.isEmpty())
            sizeText = QStringLiteral("0");
        out << QStringLiteral("   %1 (%2, %3 GB) - %4")
            .arg(QFileInfo(dev.path).fileName())
            .arg(dev.path)
            .arg(sizeText)
            .arg(dev.name);
        if (dev.isReadOnly)
            out << QCoreApplication::translate("main", " (read only)");
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

// Read the LANG setting of the desktop session (KDE keeps it in
// plasma-localerc; systemd systems default to /etc/locale.conf). Running
// through sudo/pkexec loses the user's locale, so Rufus must re-discover
// it to honor "always start in the session's language".
static QString detectSessionLanguage(const QString &homeDir) {
    auto readLang = [](const QString &path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QChar('#')) || !line.contains(QChar('=')))
                continue;
            const QString key = line.section(QChar('='), 0, 0).trimmed();
            const QString val = line.section(QChar('='), 1).trimmed();
            if (key == QStringLiteral("LANG") && !val.isEmpty() && !val.startsWith(QChar('"')))
                return val;
        }
        return QString();
    };

    if (!homeDir.isEmpty()) {
        const QString sessionFile = homeDir + QStringLiteral("/.config/plasma-localerc");
        if (QFileInfo::exists(sessionFile)) {
            // [Formats] LANG holds the session language; if that section is
            // missing, [Translations] LANGUAGE is the UI language.
            QString lang = readLang(sessionFile);
            if (!lang.isEmpty())
                return lang;
        }
    }
    if (QFileInfo::exists(QStringLiteral("/etc/locale.conf")))
        return readLang(QStringLiteral("/etc/locale.conf"));
    return QString();
}

static QStringList checkMissingDependencies() {
    static const char *tools[] = {"syslinux", "grub-install", "7z", "fuseiso"};
    QStringList missing;
    for (const char *t : tools) {
        QProcess p;
        p.start(QStringLiteral("which"), {QString::fromLatin1(t)});
        if (!p.waitForFinished(5000) || p.exitCode() != 0)
            missing << QString::fromLatin1(t);
    }
    return missing;
}

static void showDependencyWarning(const QStringList &missing) {
    QString joined = missing.join(QStringLiteral(", "));
    MsgBox::warning(nullptr,
        QCoreApplication::translate("main", "Rufus - Missing optional dependencies"),
        QCoreApplication::translate("main",
            "Some optional dependencies are missing from your system.\n\n"
            "Missing:\n%1\n\n"
            "Rufus can still run, but the following operations will not be "
            "available:\n"
            "- syslinux: BIOS bootloader installation\n"
            "- grub-install: GRUB bootloader installation\n"
            "- 7z: extraction of compressed (ZIP/7z) images\n"
            "- fuseiso: ISO extraction\n\n"
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
        }
}

// Headless CLI write mode:
//   sudo ./rufus --image=/path/to/image.iso [--boot=mbr|gpt]
//                 [--filesystem=default|name] [--device=sdX]
// Runs the same FormatWorker pipeline as the GUI, printing the log to
// stdout and exiting with 0 on success, 1 on failure.
static int cliWrite(const QString &imagePath, const QString &bootOpt,
                    const QString &fsOpt, const QString &deviceOpt) {
    if (deviceOpt.isEmpty()) {
        fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
            "Please specify a device with --device (e.g. --device=sdc).")));
        return 1;
    }
    if (imagePath.isEmpty()) {
        fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
            "Please specify an image with --image.")));
        return 1;
    }
    if (!QFileInfo::exists(imagePath)) {
        fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
            "Image not found: %1").arg(imagePath)));
        return 1;
    }

    DeviceInfo dev;
    for (const DeviceInfo &d : DeviceManager::enumerate()) {
        if (d.path == deviceOpt || QFileInfo(d.path).fileName() == deviceOpt) {
            dev = d;
            break;
        }
    }
    if (dev.path.isEmpty()) {
        fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
            "Device '%1' not found.").arg(deviceOpt)));
        return 1;
    }
    if (dev.isReadOnly) {
        fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
            "Device '%1' is read only.").arg(dev.path)));
        return 1;
    }
    if (dev.isSystem) {
        fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
            "Device '%1' is the system disk and will not be overwritten.")
            .arg(dev.path)));
        return 1;
    }

    ImageInfo info = ImageHandler::detect(imagePath);
    const bool rawDiskImage = info.isDDOnly() || info.isRawDiskImage();

    printf("%s\n", qPrintable(QCoreApplication::translate("main",
        "Using image: %1...").arg(imagePath)));
    fflush(stdout);

    FormatWorker::Config config;
    config.targetDevice = dev;
    config.imagePath = imagePath;
    config.verifyAfterWrite = true;

    if (rawDiskImage) {
        // Whole-disk (DD) write: partition scheme and file system do not
        // apply, so warn when the user asked for them.
        config.mode = FormatWorker::Mode::WriteImage;
        if (!bootOpt.isEmpty() || !fsOpt.isEmpty()) {
            const QString ignored = QCoreApplication::translate("main",
                "The '--filesystem=%1' and '--boot=%2' instructions were ignored, "
                "as they do not work on disk image formats (IMG/VHD).")
                .arg(fsOpt.isEmpty() ? QStringLiteral("default") : fsOpt,
                     bootOpt.isEmpty() ? QStringLiteral("default") : bootOpt);
            printf("%s\n", qPrintable(ignored));
            fflush(stdout);
        }
    } else {
        config.mode = FormatWorker::Mode::WriteImageIso;
        if (bootOpt.compare(QStringLiteral("gpt"), Qt::CaseInsensitive) == 0) {
            config.scheme = PartitionScheme::GPT;
            config.targetType = TargetSystemType::UEFI;
        } else {
            config.scheme = PartitionScheme::MBR;
            config.targetType = TargetSystemType::BIOS;
        }
        if (fsOpt.isEmpty() || fsOpt.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0) {
            config.filesystem = FileSystem::FAT32;
        } else {
            config.filesystem = PartitionManager::fsFromString(fsOpt);
            if (config.filesystem == FileSystem::Unknown) {
                fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate("main",
                    "Unknown file system '%1'.").arg(fsOpt)));
                return 1;
            }
        }
        config.bootloaderType = info.recommendedBootloader.isEmpty()
            ? QStringLiteral("syslinux") : info.recommendedBootloader;
    }

    bool success = false;
    QString finalMessage;
    FormatWorker worker;
    worker.setConfig(config);
    QObject::connect(&worker, &FormatWorker::logMessage,
        [](const QString &msg, int type) {
            // Warnings (type 2) go to stderr so scripts can separate
            // them from the normal progress output on stdout.
            if (type == 2)
                fprintf(stderr, "%s\n", qPrintable(msg));
            else
                printf("%s\n", qPrintable(msg));
            fflush(stdout);
        });
    QObject::connect(&worker, &FormatWorker::statusChanged,
        [](const QString &status) {
            printf("%s\n", qPrintable(status));
            fflush(stdout);
        });
    QObject::connect(&worker, &FormatWorker::finished,
        [&](bool ok, const QString &message) {
            success = ok;
            finalMessage = message;
            printf("\n%s\n", qPrintable(message));
            fflush(stdout);
        });
    worker.run();
    return success ? 0 : 1;
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
                   "  --devices           Alias for --list-devices\n"
                   "  --image=<path>      Write an ISO/IMG image (headless mode)\n"
                   "  --boot=mbr|gpt      Partition scheme for --image (default: mbr)\n"
                   "  --filesystem=<name> File system for --image (default, FAT16, FAT32,\n"
                   "                      NTFS, exFAT, ext2/3/4, btrfs, XFS, UDF)\n"
                   "  --device=sdX        Target device for --image\n"
                   "  --welcome-done      Internal: welcome dialog already shown\n"
                   "  --debug             Enable debug logging to /tmp/rufus_debug.log\n");
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("Rufus 1.0.2 - Create bootable USB drives\n");
            printf("Linux port using Qt6 and C++\n");
            printf("GNU GPL v3 License\n");
            return 0;
        }
    }

    QString userConfigDir;
    // Always start in the desktop session's language when a translation
    // for it exists (see Localization::init).
    const QString sessionLang = detectSessionLanguage(getOriginalUserHomeDir());
    if (!sessionLang.isEmpty())
        qputenv("LANG", sessionLang.toUtf8());

    // Running as root through sudo/pkexec loses XDG_RUNTIME_DIR, which
    // makes Qt print "QStandardPaths: XDG_RUNTIME_DIR not set" warnings
    // and can confuse session helpers: point it at the original user's
    // runtime directory (readable by root).
    if (qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR")) {
        const char *sudoUid = qgetenv("SUDO_UID").constData();
        const char *pkexecUid = qgetenv("PKEXEC_UID").constData();
        const char *uidStr = (sudoUid && sudoUid[0]) ? sudoUid
                                                      : (pkexecUid && pkexecUid[0]) ? pkexecUid : nullptr;
        if (uidStr) {
            QString rtDir = QStringLiteral("/run/user/%1").arg(QString::fromUtf8(uidStr));
            if (QFileInfo::exists(rtDir))
                qputenv("XDG_RUNTIME_DIR", rtDir.toUtf8());
        }
    }

    if (geteuid() != 0) {
        // Not running as root - show error and exit
        userConfigDir = getOriginalUserHomeDir();
        if (!userConfigDir.isEmpty()) {
            userConfigDir += QStringLiteral("/.config");
        }

        QApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("Rufus"));
        app.setApplicationVersion(QStringLiteral("1.0.2"));
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

    // Headless CLI mode: --image (write), --list-devices/--devices (list).
    // Detected before QApplication so no display is required; runs the
    // same write pipeline as the GUI and prints progress to stdout.
    {
        bool cliMode = false;
        for (int i = 1; i < argc; i++) {
            const QByteArray arg(argv[i]);
            if (arg.startsWith("--image") || arg == "--list-devices" || arg == "--devices") {
                cliMode = true;
                break;
            }
        }
        if (cliMode) {
            // Same user environment as the GUI path: HOME/XDG point at the
            // original user so virtual loop drives and translations resolve
            // identically (must run before QCoreApplication).
            const QString cliHome = getOriginalUserHomeDir();
            useOriginalUserEnvironment(cliHome);

            QCoreApplication app(argc, argv);
            app.setApplicationName(QStringLiteral("Rufus"));
            app.setApplicationVersion(QStringLiteral("1.0.2"));
            app.setOrganizationName(QStringLiteral("Rufus"));
            app.setOrganizationDomain(QStringLiteral("rufus.ie"));

            Localization::init(QCoreApplication::applicationDirPath());

            QCommandLineParser parser;
            parser.setApplicationDescription(QStringLiteral("Create bootable USB drives from ISO/IMG files"));
            parser.addHelpOption();
            parser.addVersionOption();

            QCommandLineOption imageOpt(QStringLiteral("image"),
                QStringLiteral("Image (ISO/IMG) to write to the device"), QStringLiteral("path"));
            parser.addOption(imageOpt);
            QCommandLineOption bootOpt(QStringLiteral("boot"),
                QStringLiteral("Partition scheme: mbr or gpt"), QStringLiteral("scheme"));
            parser.addOption(bootOpt);
            QCommandLineOption fsOpt(QStringLiteral("filesystem"),
                QStringLiteral("File system: default, FAT16, FAT32, NTFS, exFAT, ext2/3/4, btrfs, XFS, UDF"),
                QStringLiteral("name"));
            parser.addOption(fsOpt);
            QCommandLineOption deviceOpt(QStringLiteral("device"),
                QStringLiteral("Target device (e.g. sdc)"), QStringLiteral("sdX"));
            parser.addOption(deviceOpt);
            QCommandLineOption listOpt(QStringLiteral("list-devices"),
                QStringLiteral("List available devices and exit"));
            parser.addOption(listOpt);
            QCommandLineOption listAliasOpt(QStringLiteral("devices"),
                QStringLiteral("Alias for --list-devices"));
            parser.addOption(listAliasOpt);

            parser.process(app);

            if (parser.isSet(QStringLiteral("list-devices")) ||
                parser.isSet(QStringLiteral("devices")))
                return listDevices();
            if (parser.isSet(QStringLiteral("image")))
                return cliWrite(parser.value(QStringLiteral("image")),
                    parser.value(QStringLiteral("boot")),
                    parser.value(QStringLiteral("filesystem")),
                    parser.value(QStringLiteral("device")));
            parser.showHelp(0);
            return 0;
        }
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
    app.setApplicationVersion(QStringLiteral("1.0.2"));
    app.setOrganizationName(QStringLiteral("Rufus"));
    app.setOrganizationDomain(QStringLiteral("rufus.ie"));
    app.setQuitOnLastWindowClosed(true);

    // A style chosen in Preferences (Settings "style", default "fusion")
    // is always applied, so the look is predictable across desktops.
    app.setWindowIcon(makeAppIcon());

    Localization::init(QApplication::applicationDirPath());
    Settings::instance().init(QApplication::applicationDirPath(), userConfigDir);
    QStyle *style = QStyleFactory::create(Settings::instance().style());
    if (style)
        QApplication::setStyle(style);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Create bootable USB drives from ISO/IMG files"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption listOpt(QStringLiteral("list-devices"),
        QStringLiteral("List available devices and exit"));
    parser.addOption(listOpt);
    QCommandLineOption listAliasOpt(QStringLiteral("devices"),
        QStringLiteral("Alias for --list-devices"));
    parser.addOption(listAliasOpt);

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
    bool listDev = parser.isSet(QStringLiteral("list-devices")) ||
                   parser.isSet(QStringLiteral("devices"));

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
        if (!missing.isEmpty())
            showDependencyWarning(missing);
    }

    Logger::init();
    Logger::setDebugEnabled(g_debugEnabled);

    // Log detected system language
    Logger::info(QStringLiteral("Detected language: %1")
                     .arg(Localization::detectedSystemLanguage()));

    MainWindow window;
    window.show();

    debugLog(QStringLiteral("Rufus started (uid=%1)").arg(geteuid()));

    int ret = app.exec();

    Logger::shutdown();
    return ret;
}
