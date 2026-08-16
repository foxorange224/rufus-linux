#include "MainWindow.h"
#include "ThemeWatcher.h"
#include "AboutDialog.h"
#include "PreferencesDialog.h"
#include "LogDialog.h"
#include "utils/Logger.h"
#include "utils/Localization.h"
#include "utils/MsgBox.h"
#include "utils/Settings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>

#include "utils/Settings.h"
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QThread>
#include <QScrollBar>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QStyleFactory>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QRadioButton>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QDialog>
#include <QStandardItemModel>
#include <QScreen>
#include <QGuiApplication>
#include <QtConcurrent>
#include <QToolButton>
#include <QFrame>
#include <QPixmap>
#include <QIcon>
#include <cstdint>

#define SL_MAJOR(x) ((uint8_t)((x)>>8))
#define SL_MINOR(x) ((uint8_t)(x))

// ─── ISOHybrid Selection Dialog ──────────────────────────────────────
// Returns: 0 = ISO image mode, 1 = DD image mode, -1 = cancelled.
static int showIsoHybridDialog(QWidget *parent) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("ISO image mode detected"));
    dlg.setMinimumWidth(500);

    auto *layout = new QVBoxLayout(&dlg);
    auto *label = new QLabel(QObject::tr(
        "The image you have selected is an ISOHybrid, which means it can be "
        "written in either ISO image mode (recommended) or DD image mode.\n\n"
        "Rufus recommends using ISO image mode so that you have full access "
        "to the drive in Windows after formatting.\n\n"
        "If you experience problems with ISO mode, you can try rewriting the "
        "image in DD image mode.\n\n"
        "Please select the mode you would like to use to write this image:"));
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *isoRadio = new QRadioButton(
        QObject::tr("Write in ISO image mode (recommended if using Windows)"));
    auto *ddRadio = new QRadioButton(QObject::tr("Write in DD image mode"));
    isoRadio->setChecked(true);
    layout->addWidget(isoRadio);
    layout->addWidget(ddRadio);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() == QDialog::Rejected)
        return -1;
    return ddRadio->isChecked() ? 1 : 0;
}

// ─── Helpers ─────────────────────────────────────────────────────────
// TAR archives (.tar.* / .tgz) are never valid disk images: even though
// they are compressed like a single image, the archive contains multiple
// files and cannot be written or booted.
static bool isTarArchive(const QString &path) {
    const QString name = QFileInfo(path).fileName().toLower();
    return name.contains(QStringLiteral(".tar.")) ||
           name.endsWith(QStringLiteral(".tgz"));
}

// ─── Windows User Experience Dialog ──────────────────────────────────
struct WueResult {
    bool bypassTpm = false;
    bool bypassNro = false;
    bool setUser = false;
    bool noDataCollection = false;
    bool disableBitlocker = false;
    QString username;
};

static int showWueDialog(QWidget *parent, bool isWindows11, quint64 build, bool isWtg,
                          WueResult &result) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Windows User Experience"));
    dlg.setMinimumWidth(480);

    auto *layout = new QVBoxLayout(&dlg);
    auto *label = new QLabel(QObject::tr(
        "Customize Windows installation options.\n"
        "Hover over each option for details."));
    label->setWordWrap(true);
    layout->addWidget(label);

    struct Option {
        QString text;
        QString tooltip;
        bool *enabled;
    };
    QList<Option> opts;

    if (isWindows11) {
        opts << Option{
            QObject::tr("Bypass Secure Boot / TPM / 4GB+ RAM check"),
            QObject::tr("Disables compatibility checks during Windows 11 installation"),
            &result.bypassTpm
        };
        opts << Option{
            QObject::tr("Disable BitLocker automatic encryption"),
            QObject::tr("Prevents automatic device encryption"),
            &result.disableBitlocker
        };
    }
    if (build >= 22500) {
        opts << Option{
            QObject::tr("Disable online account requirement (BypassNRO)"),
            QObject::tr("Allows local account creation instead of Microsoft account"),
            &result.bypassNro
        };
    }
    opts << Option{
        QObject::tr("Create local user account"),
        QObject::tr("Creates a local user account during installation"),
        &result.setUser
    };
    opts << Option{
        QObject::tr("Disable data collection (privacy)"),
        QObject::tr("Sets privacy settings to minimum"),
        &result.noDataCollection
    };

    QList<QCheckBox *> checkboxes;
    QCheckBox *setUserCheckbox = nullptr;
    for (const auto &o : opts) {
        auto *cb = new QCheckBox(o.text);
        cb->setToolTip(o.tooltip);
        layout->addWidget(cb);
        checkboxes.append(cb);
        QObject::connect(cb, &QCheckBox::toggled, [o](bool checked) { *o.enabled = checked; });
        if (o.enabled == &result.setUser)
            setUserCheckbox = cb;
    }

    // Username field is always created but only usable when the local
    // account option is checked (the option list above is built before the
    // dialog runs, so a field gated on result.setUser would never appear).
    auto *userLayout = new QHBoxLayout;
    auto *userLabel = new QLabel(QObject::tr("Username:"));
    auto *userEdit = new QLineEdit;
    userEdit->setPlaceholderText(QObject::tr("Username"));
    userEdit->setEnabled(false);
    userLayout->addWidget(userLabel);
    userLayout->addWidget(userEdit);
    layout->addLayout(userLayout);
    QObject::connect(userEdit, &QLineEdit::textChanged, [&](const QString &t) { result.username = t; });
    if (setUserCheckbox) {
        QObject::connect(setUserCheckbox, &QCheckBox::toggled, userEdit, &QLineEdit::setEnabled);
    }

    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    int ret = dlg.exec();

    // Validate: a local account requires a non-empty username.
    if (ret == QDialog::Accepted && result.setUser && result.username.trimmed().isEmpty()) {
        MsgBox::warning(&dlg, QObject::tr("Missing username"),
            QObject::tr("Please enter a username for the local user account."));
        return QDialog::Rejected;
    }

    return ret;
}

// ─── Image path tracking ────────────────────────────────────────────
static QString g_selectedImagePath;
static QString g_archivePath;

static QString selectedImagePath() { return g_selectedImagePath; }
static void setSelectedImagePath(const QString &p) { g_selectedImagePath = p; }
static QString archivePath() { return g_archivePath; }
static void setArchivePath(const QString &p) { g_archivePath = p; }

// Mirrors FormatWorker::resolveDataFile(): data files (grldr,
// uefi-ntfs.img, ...) are searched next to the binary and in the
// standard share directories, so the pre-flight checks can tell whether
// a download would be needed for grub4dos/UEFI:NTFS.
static QString findDataFile(const QString &name) {
    const QStringList dirs = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/rufus"),
        QStringLiteral("/usr/local/share/rufus"),
        QStringLiteral("/usr/share/rufus"),
    };
    for (const QString &d : dirs) {
        const QString p = d + QLatin1Char('/') + name;
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

// ─── Section header widget (title + horizontal divider line) ────────
// Mirrors the original Rufus look: a bold title followed by a line that
// extends to the right edge of the window. The title label is passed in
// so retranslateUi() can keep updating it.
static QWidget *wrapSectionHeader(QLabel *label, QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
    auto *line = new QFrame(w);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    l->addWidget(label);
    l->addWidget(line);
    return w;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupConnections();
    loadSettings();
    populateDevices();
    updateIdleProgressBarText();
    // Follow live desktop theme changes (KDE / LXQt color scheme).
    m_themeWatcher = new ThemeWatcher(this);
    m_themeWatcher->applyNow();

    // Theme diagnostics: helps comparing the sudo and pkexec-launched
    // instances (palette + style + config dirs that drive the look).
    {
        const QPalette pal = QApplication::palette();
        Logger::info(QStringLiteral("Theme: style=%1 window=%2 button=%3 view=%4 "
                                    "highlight=%5 home=%6 XDG_CONFIG_HOME=%7 "
                                    "XDG_CURRENT_DESKTOP=%8")
            .arg(QApplication::style()->objectName(),
                 pal.color(QPalette::Window).name(),
                 pal.color(QPalette::Button).name(),
                 pal.color(QPalette::Base).name(),
                 pal.color(QPalette::Highlight).name(),
                 QDir::homePath(),
                 qEnvironmentVariable("XDG_CONFIG_HOME"),
                 qEnvironmentVariable("XDG_CURRENT_DESKTOP")));
    }
    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setInterval(3000);
    connect(m_autoRefreshTimer, &QTimer::timeout, this, &MainWindow::onAutoRefresh);
    m_autoRefreshTimer->start();

    setAcceptDrops(true);
    updateFixedSize();

    // Center on screen
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect sg = screen->availableGeometry();
        move((sg.width() - width()) / 2, (sg.height() - height()) / 2);
    }
}

MainWindow::~MainWindow() {
    saveSettings();
    if (m_workerThread) {
        // Cancel the worker first (it polls isCancelled() between chunks),
        // then stop the loop and wait for it to wind down. The worker is a
        // child of the thread and is deleteLater'd on QThread::finished; the
        // thread itself is a child of MainWindow and is cleaned up here by
        // deleteChildren — so no deleteLater on the thread object (deleting
        // a QThread from its own finished signal is a documented crash: the
        // thread is still winding down and qFatal aborts).
        if (m_workerThread->isRunning()) {
            // Only touch m_worker while the thread runs: once it finishes,
            // the worker has been deleteLater'd and the pointer is stale.
            if (m_worker)
                m_worker->cancel();
            m_workerThread->quit();
            // The worker cancels promptly when requested; wait a generous
            // amount of time so the thread completes first.
            m_workerThread->wait(30000);
            if (m_workerThread->isRunning()) {
                m_workerThread->requestInterruption();
                m_workerThread->wait(30000);
            }
        }
    }
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    // The layout hints are stale while the window is hidden: updateFixedSize()
    // ran in the constructor with the wrong values and locked the window at
    // the old height (extra space then spreads between the labels). Recompute
    // once the window is actually shown so the size matches the content.
    updateFixedSize();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            QString path = url.toLocalFile().toLower();
            if (path.endsWith(".iso") || path.endsWith(".img") ||
                path.endsWith(".vhd") || path.endsWith(".vhdx") ||
                path.endsWith(".wim") || path.endsWith(".esd") ||
                path.endsWith(".gz") || path.endsWith(".bz2") ||
                path.endsWith(".xz") || path.endsWith(".zst")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    if (event->mimeData()->hasUrls()) {
        QString path = event->mimeData()->urls().first().toLocalFile();
        onImageChanged(path);
        event->acceptProposedAction();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_isRunning) {
        auto reply = MsgBox::question(this, tr("Confirm"),
            tr("Operation in progress. Cancel and exit?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        // Cancel the worker and keep the window open until the operation
        // fully finishes. Closing now would destroy the QThread while its
        // run() is still executing ("QThread: Destroyed while thread is
        // still running") and lose the worker's cleanup.
        m_pendingClose = true;
        m_startBtn->setText(tr("ESPERE..."));
        m_startBtn->setEnabled(false);
        statusBar()->showMessage(tr("Cancelling - Please wait..."));
        if (m_worker) m_worker->cancel();
        event->ignore();
        return;
    }
    // A detached log window must not outlive the main window.
    if (m_logDialog && !m_logAttached)
        m_logDialog->close();
    saveSettings();
    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
    case Qt::Key_F5:
        onRefreshDevices();
        break;
    case Qt::Key_F:
        // Ctrl+F toggles "List USB HDD" (as advertised in the tooltip);
        // show the advanced drive section if it is currently hidden so
        // the user can see the change.
        if (event->modifiers() & Qt::ControlModifier) {
            if (m_listUsbHddCheck) {
                if (!m_listUsbHddCheck->isVisible())
                    onAdvancedDriveToggled();
                m_listUsbHddCheck->setChecked(!m_listUsbHddCheck->isChecked());
            }
            break;
        }
        QMainWindow::keyPressEvent(event);
        break;
    case Qt::Key_Escape:
        if (m_isRunning && m_worker) m_worker->cancel();
        break;
    case Qt::Key_O:
        if (event->modifiers() & Qt::ControlModifier)
            onSelectImage();
        break;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void MainWindow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    if (event->type() == QEvent::PaletteChange && m_hashBtn)
        updateHashButtonStyle();
    QMainWindow::changeEvent(event);
}

// ─── setupUi ─────────────────────────────────────────────────────────
void MainWindow::setupUi() {
    setWindowTitle(tr("Rufus %1").arg(QApplication::applicationVersion()));
    // Fixed layout: no maximize button and a locked size, so the window can
    // never be stretched into a size where widgets would be squeezed or
    // ballooned (original Rufus window is fixed-size).
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    // Horizontal container: form on the left, log panel on the right.
    auto *mainHBox = new QHBoxLayout(central);
    mainHBox->setSpacing(4);
    mainHBox->setContentsMargins(10, 10, 10, 4);
    m_mainHBox = mainHBox;
    m_mainLayout = new QVBoxLayout;
    m_mainLayout->setSpacing(4);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    // Wrap the form in a container widget so its width can be locked (and
    // released) independently when the log panel is toggled, preventing the
    // form from being squeezed horizontally. The form is locked to a fixed
    // 455 px width for good so it never changes size when the log is toggled
    // (otherwise the buttons shift).
    m_formWidget = new QWidget(central);
    m_formWidget->setLayout(m_mainLayout);
    m_formWidget->setFixedWidth(455);
    mainHBox->addWidget(m_formWidget, 1);
    auto *mainLayout = m_mainLayout;

    QFont sectionFont = font();
    sectionFont.setBold(true);
    sectionFont.setPointSize(sectionFont.pointSize() + 1);

    // ══════════════════════════════════════════════════════════════════
    // DRIVE PROPERTIES
    // ══════════════════════════════════════════════════════════════════
    m_driveHeader = new QLabel(tr("Drive Properties"));
    m_driveHeader->setFont(sectionFont);
    mainLayout->addWidget(wrapSectionHeader(m_driveHeader, central));

    auto *deviceCol = new QVBoxLayout;
    deviceCol->setSpacing(2);
    m_deviceLabel = new QLabel(tr("Device"));
    deviceCol->addWidget(m_deviceLabel);
    m_deviceCombo = new QComboBox;
    m_deviceCombo->setMinimumWidth(350);
    m_deviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_deviceCombo->setToolTip(tr("Select the USB drive to write to.\n"
        "Only removable USB drives are listed by default. Press Ctrl+F or check\n"
        "'List USB Hard Drives' to show fixed drives."));
    m_saveBtn = new QPushButton(QStringLiteral("\xe2\x80\xa6"));
    m_saveBtn->setFixedSize(18, 18);
    m_saveBtn->setFlat(true);
    m_saveBtn->setToolTip(tr("Save current settings to INI file"));
    m_refreshBtn = new QPushButton;
    m_refreshBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshBtn->setFixedWidth(26);
    m_refreshBtn->setToolTip(tr("Refresh devices (F5)"));
    auto *deviceRow = new QHBoxLayout;
    deviceRow->addWidget(m_deviceCombo, 1);
    deviceRow->addWidget(m_saveBtn);
    deviceRow->addWidget(m_refreshBtn);
    deviceCol->addLayout(deviceRow);
    mainLayout->addLayout(deviceCol);

    // Boot selection row (label above the combo, like original Rufus)
    auto *bootCol = new QVBoxLayout;
    bootCol->setSpacing(2);
    m_bootSelectionLabel = new QLabel(tr("Boot selection"));
    m_bootSelectionCombo = new QComboBox;
    m_bootSelectionCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_bootSelectionCombo->setToolTip(tr("Select the type of bootable USB to create.\n"
        "• Non bootable: Just format the drive\n"
        "• Disk or ISO image: Create from an ISO/IMG file\n"
        "• FreeDOS: Create a FreeDOS bootable drive\n"
        "• MS-DOS: Create an MS-DOS bootable drive"));
    rebuildBootSelectionCombo();
    // Check button shown once a valid image is selected (like original Rufus,
    // which displays a ✓ between the combo and the SELECT button). Clicking it
    // computes the image hashes (it replaces the old "#" button).
    m_hashBtn = new QPushButton;
    QIcon checkIcon = QIcon::fromTheme(QStringLiteral("dialog-ok"));
    if (checkIcon.isNull())
        checkIcon = style()->standardIcon(QStyle::SP_DialogApplyButton);
    m_hashBtn->setIcon(checkIcon);
    m_hashBtn->setIconSize(QSize(16, 16));
    m_hashBtn->setFixedSize(18, 18);
    m_hashBtn->setFlat(true);
    m_hashBtn->setToolTip(tr("Compute MD5, SHA-1 and SHA-256 hashes for the selected image"));
    updateHashButtonStyle();

    // SELECT is a dropdown button: "Select image…" plus the recently used
    // images, like the original Rufus SELECT menu.
    m_selectBtn = new QToolButton;
    m_selectBtn->setText(tr("SELECT"));
    m_selectBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_selectBtn->setPopupMode(QToolButton::InstantPopup);
    m_selectBtn->setMenu(new QMenu(m_selectBtn));
    m_selectBtn->setFixedWidth(116);
    m_selectBtn->setToolTip(tr("Select a disk image (ISO, IMG, VHD, etc.)"));
    m_selectMenu = m_selectBtn->menu();
    connect(m_selectMenu, &QMenu::aboutToShow, this, [this]() { populateSelectMenu(); });

    auto *bootRow = new QHBoxLayout;
    bootCol->addWidget(m_bootSelectionLabel);
    bootRow->addWidget(m_bootSelectionCombo, 1);
    bootRow->addWidget(m_hashBtn);
    bootRow->addWidget(m_selectBtn);
    bootCol->addLayout(bootRow);
    mainLayout->addLayout(bootCol);

    // Image option row (label above the combo, like the other fields —
    // only shown for Windows images)
    auto *imageOptCol = new QVBoxLayout;
    imageOptCol->setSpacing(2);
    m_imageOptionLabel = new QLabel(tr("Image option"));
    auto *imageOptRow = new QHBoxLayout;
    m_imageOptionCombo = new QComboBox;
    m_imageOptionCombo->addItem(tr("Standard Windows installation"));
    m_imageOptionCombo->addItem(tr("Windows To Go"));
    m_imageOptionCombo->setToolTip(tr("Image option:\n"
        "• Standard: Regular Windows installation\n"
        "• Windows To Go: Run Windows from USB"));
    imageOptRow->addWidget(m_imageOptionCombo);
    imageOptRow->addStretch();
    imageOptCol->addWidget(m_imageOptionLabel);
    imageOptCol->addLayout(imageOptRow);
    mainLayout->addLayout(imageOptCol);

    // Partition scheme + Target system
    auto *schemeTargetRow = new QHBoxLayout;
    auto *schemeCol = new QVBoxLayout;
    m_partitionSchemeLabel = new QLabel(tr("Partition scheme"));
    m_schemeCombo = new QComboBox;
    m_schemeCombo->addItem(QStringLiteral("MBR"), static_cast<int>(PartitionScheme::MBR));
    m_schemeCombo->addItem(QStringLiteral("GPT"), static_cast<int>(PartitionScheme::GPT));
    m_schemeCombo->setToolTip(tr("Partition scheme:\n"
        "• MBR: Master Boot Record (compatible, BIOS + UEFI-CSM)\n"
        "• GPT: GUID Partition Table (modern, native UEFI)"));
    schemeCol->addWidget(m_partitionSchemeLabel);
    schemeCol->addWidget(m_schemeCombo);
    schemeTargetRow->addLayout(schemeCol);
    schemeTargetRow->addSpacing(12);

    auto *targetCol = new QVBoxLayout;
    auto *targetLabelRow = new QHBoxLayout;
    m_targetSystemLabel = new QLabel(tr("Target system"));
    m_csmHelpLabel = new QLabel(QStringLiteral("<a href='#' style='text-decoration:none; color:#2d89ef;'>?</a>"));
    m_csmHelpLabel->setToolTip(tr("Click for information about UEFI-CSM (Compatibility Support Module)"));
    m_csmHelpLabel->setCursor(Qt::PointingHandCursor);
    targetLabelRow->addWidget(m_targetSystemLabel);
    targetLabelRow->addStretch();
    m_targetSystemCombo = new QComboBox;
    m_targetSystemCombo->addItem(tr("BIOS (or UEFI-CSM)"), 0);
    m_targetSystemCombo->addItem(tr("UEFI (non CSM)"), 1);
    m_targetSystemCombo->setCurrentIndex(0);
    m_targetSystemCombo->setToolTip(tr("Target system type:\n"
        "• BIOS/UEFI-CSM: For legacy BIOS or UEFI in CSM mode\n"
        "• UEFI (non CSM): For native UEFI boot"));
    // The default scheme is MBR: only BIOS (or UEFI-CSM) is offered then.
    updateTargetSystemForScheme();
    // The combo (not its label) spans the available space and reaches the
    // right margin, like original Rufus. The blue "?" sits right next to
    // the combo, not beside the "Target system" caption.
    m_targetSystemCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    targetCol->addLayout(targetLabelRow);
    auto *targetComboRow = new QHBoxLayout;
    targetComboRow->addWidget(m_targetSystemCombo);
    targetComboRow->addWidget(m_csmHelpLabel);
    targetCol->addLayout(targetComboRow);
    schemeTargetRow->addLayout(targetCol, 1);
    mainLayout->addLayout(schemeTargetRow);

    // Advanced drive properties toggle (hidden by default)
    m_advancedDriveToggle = new QPushButton(tr("Show advanced drive properties"));
    m_advancedDriveToggle->setFlat(true);
    m_advancedDriveToggle->setCursor(Qt::PointingHandCursor);
    m_advancedDriveToggle->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    m_advancedDriveToggle->setStyleSheet("QPushButton { text-align: left; padding: 2px 0; border: none; color: palette(text); }");
    mainLayout->addWidget(m_advancedDriveToggle);

    m_listUsbHddCheck = new QCheckBox(tr("List USB Hard Drives"));
    m_listUsbHddCheck->setToolTip(tr("Also list USB hard drives (not just removable flash drives)"));
    m_listUsbHddCheck->setVisible(false);
    mainLayout->addWidget(m_listUsbHddCheck);

    m_oldBiosFixCheck = new QCheckBox(tr("Add fixes for old BIOSes (extra partition, align, etc.)"));
    m_oldBiosFixCheck->setToolTip(tr("Add an extra alignment partition and other fixes for old BIOS"));
    m_oldBiosFixCheck->setVisible(false);
    mainLayout->addWidget(m_oldBiosFixCheck);

    m_uefiMediaCheck = new QCheckBox(tr("Enable runtime UEFI media validation"));
    m_uefiMediaCheck->setToolTip(tr("Validate UEFI boot media at runtime (may affect compatibility)"));
    m_uefiMediaCheck->setVisible(false);
    mainLayout->addWidget(m_uefiMediaCheck);

    // ══════════════════════════════════════════════════════════════════
    // FORMAT OPTIONS
    // ══════════════════════════════════════════════════════════════════
    m_formatHeader = new QLabel(tr("Format Options"));
    m_formatHeader->setFont(sectionFont);
    mainLayout->addWidget(wrapSectionHeader(m_formatHeader, central));

    m_volumeLabelLabel = new QLabel(tr("Volume label"));
    mainLayout->addWidget(m_volumeLabelLabel);
    m_labelEdit = new QLineEdit;
    m_labelEdit->setMaxLength(11);
    m_labelEdit->setPlaceholderText(tr("None"));
    m_labelEdit->setToolTip(tr("Volume label (up to 11 characters for FAT/FAT32,\n"
        "up to 32 characters for NTFS/exFAT)"));
    mainLayout->addWidget(m_labelEdit);

    // File system + Cluster size
    auto *fsRow = new QHBoxLayout;
    auto *fsCol = new QVBoxLayout;
    m_fileSystemLabel = new QLabel(tr("File system"));
    m_fsCombo = new QComboBox;
    m_fsCombo->setToolTip(tr("File system type for the USB drive.\n"
        "FAT32 is recommended for maximum compatibility.\n"
        "NTFS is required for files larger than 4GB.\n"
        "exFAT is good for large files without NTFS overhead.\n"
        "ext2/3/4 are Linux native filesystems."));
    populateFsCombo(BootType::Image);
    fsCol->addWidget(m_fileSystemLabel);
    fsCol->addWidget(m_fsCombo);
    fsRow->addLayout(fsCol);
    fsRow->addSpacing(12);

    auto *clusterCol = new QVBoxLayout;
    m_clusterSizeLabel = new QLabel(tr("Cluster size"));
    m_clusterSizeCombo = new QComboBox;
    m_clusterSizeCombo->setToolTip(tr("Allocation unit size. Default is recommended."));
    // The combo (not its label) spans the available space and reaches the
    // right margin, like original Rufus.
    m_clusterSizeCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    clusterCol->addWidget(m_clusterSizeLabel);
    clusterCol->addWidget(m_clusterSizeCombo);
    fsRow->addLayout(clusterCol, 1);
    mainLayout->addLayout(fsRow);

    // Advanced format options toggle (hidden by default, matches original Rufus)
    m_advancedFormatToggle = new QPushButton(tr("Show advanced format options"));
    m_advancedFormatToggle->setFlat(true);
    m_advancedFormatToggle->setCursor(Qt::PointingHandCursor);
    m_advancedFormatToggle->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    m_advancedFormatToggle->setStyleSheet("QPushButton { text-align: left; padding: 2px 0; border: none; color: palette(text); }");
    mainLayout->addWidget(m_advancedFormatToggle);

    m_quickFormatCheck = new QCheckBox(tr("Quick format"));
    m_quickFormatCheck->setChecked(true);
    m_quickFormatCheck->setToolTip(tr("Quick format (just writes filesystem structures).\n"
        "Uncheck for full format (wipes all data)"));
    m_quickFormatCheck->setVisible(false);
    mainLayout->addWidget(m_quickFormatCheck);

    m_extendedLabelCheck = new QCheckBox(tr("Create extended label and icon files"));
    m_extendedLabelCheck->setChecked(true);
    m_extendedLabelCheck->setToolTip(tr("Creates autorun.inf and icon files for the drive"));
    m_extendedLabelCheck->setVisible(false);
    mainLayout->addWidget(m_extendedLabelCheck);

    auto *bbRow = new QHBoxLayout;
    m_badBlocksCheck = new QCheckBox(tr("Check device for bad blocks"));
    m_badBlocksCheck->setToolTip(tr("Scan the device for bad blocks before writing.\n"
        "This can take a long time on large drives."));
    m_nbPassesCombo = new QComboBox;
    m_nbPassesCombo->addItem(tr("1 pass (default)"));
    m_nbPassesCombo->addItem(tr("2 passes (SLC pattern)"));
    m_nbPassesCombo->addItem(tr("3 passes (MLC pattern)"));
    m_nbPassesCombo->addItem(tr("4 passes (TLC pattern)"));
    m_nbPassesCombo->addItem(tr("5 passes (TLC pattern)"));
    m_nbPassesCombo->setEnabled(false);
    m_nbPassesCombo->setToolTip(tr("Number of bad block scan passes.\n"
        "Multiple passes with different patterns detect more types of NAND defects."));
    bbRow->addWidget(m_badBlocksCheck);
    bbRow->addWidget(m_nbPassesCombo);
    bbRow->addStretch();
    m_badBlocksCheck->setVisible(false);
    m_nbPassesCombo->setVisible(false);
    mainLayout->addLayout(bbRow);

    // Internal-only widgets (always hidden, keep logic working)
    m_verifyWriteCheck = new QCheckBox;
    m_verifyWriteCheck->setVisible(false);
    m_verifyWriteCheck->setChecked(true);
    mainLayout->addWidget(m_verifyWriteCheck);

    m_espCheck = new QCheckBox;
    m_espCheck->setVisible(false);
    mainLayout->addWidget(m_espCheck);

    m_uefiNtfsCheck = new QCheckBox;
    m_uefiNtfsCheck->setVisible(false);
    mainLayout->addWidget(m_uefiNtfsCheck);

    m_bootloaderLabel = new QLabel;
    m_bootloaderLabel->setVisible(false);
    mainLayout->addWidget(m_bootloaderLabel);

    m_bootloaderCombo = new QComboBox;
    m_bootloaderCombo->addItem(QStringLiteral("none"), QStringLiteral("none"));
    m_bootloaderCombo->addItem(QStringLiteral("syslinux"), QStringLiteral("syslinux"));
    m_bootloaderCombo->addItem(QStringLiteral("grub2"), QStringLiteral("grub2"));
    m_bootloaderCombo->addItem(QStringLiteral("mbr"), QStringLiteral("mbr"));
    m_bootloaderCombo->addItem(QStringLiteral("freedos"), QStringLiteral("freedos"));
    m_bootloaderCombo->addItem(QStringLiteral("msdos"), QStringLiteral("msdos"));
    m_bootloaderCombo->addItem(QStringLiteral("grub4dos"), QStringLiteral("grub4dos"));
    m_bootloaderCombo->setVisible(false);
    mainLayout->addWidget(m_bootloaderCombo);

    // ══════════════════════════════════════════════════════════════════
    // STATUS
    // ══════════════════════════════════════════════════════════════════
    m_statusHeader = new QLabel(tr("Status"));
    m_statusHeader->setFont(sectionFont);
    mainLayout->addWidget(wrapSectionHeader(m_statusHeader, central));

    m_progressBar = new QProgressBar;
    m_progressBar->setMinimumHeight(24);
    m_progressBar->setTextVisible(true);
    m_progressBar->setAlignment(Qt::AlignCenter);
    // Blank until an operation starts (original Rufus shows nothing until
    // START is pressed); all status text is rendered centered inside the
    // bar via setFormat(), using the palette colors so it stays readable
    // on any theme.
    m_progressBar->setFormat(QString());
    m_progressBar->setValue(0);
    m_progressBar->setToolTip(tr("Operation progress"));
    mainLayout->addWidget(m_progressBar);

    // Log panel: embedded at the right side of the window (vertical divider
    // + panel); the window widens when it is shown. It can be detached into
    // its own window via the detach button (or closed to re-attach).
    m_logSeparator = new QFrame(central);
    m_logSeparator->setFrameShape(QFrame::VLine);
    m_logSeparator->setFrameShadow(QFrame::Sunken);
    m_logSeparator->setVisible(false);
    mainHBox->addWidget(m_logSeparator);

    m_logDialog = new LogDialog(central);
    m_logDialog->setWindowIcon(windowIcon());
    m_logDialog->setVisible(false);
    mainHBox->addWidget(m_logDialog);

    // Note: all status/elapsed information lives in the native QStatusBar
    // (device count left, elapsed time right), like original Rufus — no
    // extra labels below the progress bar.

    // ══════════════════════════════════════════════════════════════════
    // BOTTOM: Multi-toolbar + START + CLOSE
    // ══════════════════════════════════════════════════════════════════
    auto *bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(10);
    bottomRow->setContentsMargins(0, 16, 0, 16);

    // Icon-only toolbar like original Rufus: small flat square QToolButtons
    // (Language, About, Log) with icons from the active icon theme, no text
    // labels. Falls back to Qt standard icons when the theme has no match.
    auto *toolRow = new QHBoxLayout;
    toolRow->setSpacing(6);

    auto themedIcon = [this](const QList<QString> &names, QStyle::StandardPixmap fallback) {
        for (const QString &name : names) {
            QIcon icon = QIcon::fromTheme(name);
            if (!icon.isNull())
                return icon;
        }
        return style()->standardIcon(fallback);
    };

    m_langBtn = new QToolButton;
    m_langBtn->setIcon(themedIcon({QStringLiteral("preferences-desktop-locale"),
                                   QStringLiteral("config-language")},
                                  QStyle::SP_FileDialogListView));
    m_langBtn->setToolTip(tr("Select interface language"));

    m_aboutBtn = new QToolButton;
    m_aboutBtn->setIcon(themedIcon({QStringLiteral("help-about")},
                                   QStyle::SP_MessageBoxInformation));
    m_aboutBtn->setToolTip(tr("About Rufus"));

    // Preferences button sits right next to About, like the original.
    m_prefsBtn = new QToolButton;
    m_prefsBtn->setIcon(themedIcon({QStringLiteral("preferences-system"),
                                    QStringLiteral("configure")},
                                   QStyle::SP_FileDialogDetailedView));
    m_prefsBtn->setToolTip(tr("Preferences"));

    m_logBtn = new QToolButton;
    m_logBtn->setIcon(themedIcon({QStringLiteral("utilities-log-viewer"),
                                  QStringLiteral("text-x-generic")},
                                 QStyle::SP_FileDialogDetailedView));
    m_logBtn->setToolTip(tr("Open log window"));

    for (QToolButton *b : {m_langBtn, m_aboutBtn, m_prefsBtn, m_logBtn}) {
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(24, 24);
        toolRow->addWidget(b);
    }

    bottomRow->addLayout(toolRow);
    bottomRow->addStretch();

    m_startBtn = new QPushButton(tr("START"));
    m_startBtn->setFixedWidth(90);
    m_startBtn->setDefault(true);
    m_startBtn->setToolTip(tr("Start the USB formatting/writing operation"));
    bottomRow->addWidget(m_startBtn);

    m_closeBtn = new QPushButton(tr("CLOSE"));
    m_closeBtn->setFixedWidth(90);
    m_closeBtn->setToolTip(tr("Close Rufus"));
    bottomRow->addWidget(m_closeBtn);
    mainLayout->addLayout(bottomRow);

    // Status bar: device count on the left, operation elapsed time on the
    // right — like the original Rufus bottom bar (e.g. "1 device found" and
    // a running 00:00:29 clock).
    m_elapsedLabel = new QLabel(QStringLiteral("00:00:00"));
    m_elapsedLabel->setStyleSheet("color: #888;");
    statusBar()->addPermanentWidget(m_elapsedLabel);
    m_elapsedTick = new QTimer(this);
    m_elapsedTick->setInterval(1000);
    connect(m_elapsedTick, &QTimer::timeout, this, &MainWindow::updateElapsedLabel);
    updateDeviceCountStatus();

    // Language menu
    m_langMenu = new QMenu(this);
    m_langGroup = new QActionGroup(this);
    m_langGroup->setExclusive(true);

    // Use available locales from Localization instead of hardcoded list
    QStringList availableLocales = Localization::availableLocales();
    // The checked entry must match the language actually in use (which at
    // startup is the desktop session's, detected in main.cpp) — not the
    // value saved in settings, which may differ after a manual switch.
    QString currentLang = Localization::currentLanguage();
    currentLang.replace(QChar('_'), QChar('-'));

    if (availableLocales.isEmpty()) {
        // Fallback: minimal set
        struct LangEntry { const char *label; const char *code; };
        LangEntry langs[] = {
            {"English", "en"}, {"Portuguese (Brazil)", "pt-BR"},
            {"Russian", "ru-RU"}, {"Spanish", "es-ES"},
        };
        for (const auto &le : langs) {
            auto *action = m_langMenu->addAction(le.label);
            action->setData(le.code);
            action->setCheckable(true);
            if (currentLang.startsWith(le.code))
                action->setChecked(true);
            m_langGroup->addAction(action);
        }
    } else {
        for (const QString &code : availableLocales) {
            QLocale loc(code);
            QString label = QStringLiteral("%1 (%2)").arg(loc.nativeLanguageName()).arg(code);
            auto *action = m_langMenu->addAction(label);
            action->setData(code);
            action->setCheckable(true);
            if (currentLang == code)
                action->setChecked(true);
            m_langGroup->addAction(action);
        }
    }

    // Show hash/save only when needed
    m_hashBtn->setVisible(false);
    m_saveBtn->setVisible(false);
    m_imageOptionLabel->setVisible(false);
    m_imageOptionCombo->setVisible(false);

    // Window size is locked programmatically (updateFixedSize()); the
    // height changes only when advanced sections expand/collapse, exactly
    // like the fixed-size original Rufus window.
}

void MainWindow::populateFsCombo(BootType bootType) {
    // Remember the current selection so it survives repopulation
    // (device/boot type changes must not silently reset the file system).
    FileSystem prevFs = (m_fsCombo->count() > 0)
        ? static_cast<FileSystem>(m_fsCombo->currentData().toInt())
        : FileSystem::FAT32;

    m_fsCombo->blockSignals(true);
    m_fsCombo->clear();

    QList<FileSystem> allowed;
    if (!m_lastDevice.path.isEmpty()) {
        allowed = PartitionManager::getAllowedFileSystems(bootType,
            selectedImagePath(), m_quickFormatCheck->isVisible());
    } else {
        allowed = { FileSystem::FAT32, FileSystem::NTFS, FileSystem::exFAT,
                    FileSystem::ext4, FileSystem::ext3, FileSystem::ext2,
                    FileSystem::FAT16, FileSystem::UDF, FileSystem::btrfs, FileSystem::XFS };
    }

    // Image-aware filtering: only offer file systems the selected image
    // can actually boot from (like original Rufus).
    const ImageInfo &info = m_lastImageInfo;
    if (bootType == BootType::Image && !selectedImagePath().isEmpty() && !info.disableIso) {
        if (info.has4GBFile) {
            // FAT16/FAT32 cannot hold files > 4GB
            allowed.removeAll(FileSystem::FAT16);
            allowed.removeAll(FileSystem::FAT32);
        }
        if (info.needsNtfs) {
            // Windows image with 4GB+ files: only NTFS/exFAT are viable
            allowed = { FileSystem::NTFS, FileSystem::exFAT };
        }
    }

    // MS-DOS boot mode only works with FAT16 (as in original Rufus); the
    // format step forces FAT16 anyway, so offering anything else here would
    // be misleading — restrict the combo and lock it instead.
    if (isMsdosBootloader())
        allowed = { FileSystem::FAT16 };

    for (FileSystem fs : allowed) {
        if (!PartitionManager::isSupportedOnLinux(fs)) continue;
        m_fsCombo->addItem(PartitionManager::fsToString(fs), static_cast<int>(fs));
    }

    m_fsCombo->blockSignals(false);

    // Restore the previous selection when it is still allowed, otherwise
    // fall back to the original Rufus default (FAT32 when available).
    int restoreIdx = m_fsCombo->findData(static_cast<int>(prevFs));
    if (restoreIdx < 0) {
        restoreIdx = m_fsCombo->findData(static_cast<int>(FileSystem::FAT32));
        if (restoreIdx < 0 && m_fsCombo->count() > 0)
            restoreIdx = 0;
    }
    if (restoreIdx >= 0)
        m_fsCombo->setCurrentIndex(restoreIdx);
}

void MainWindow::populateClusterSizes(FileSystem fs) {
    m_clusterSizeCombo->blockSignals(true);
    m_clusterSizeCombo->clear();

    if (!m_lastDevice.path.isEmpty()) {
        qint64 diskSize = m_lastDevice.size;
        QStringList labels = PartitionManager::getClusterSizeLabels(fs, diskSize);
        for (const QString &l : labels)
            m_clusterSizeCombo->addItem(l);
        int defIdx = PartitionManager::getDefaultClusterIndex(fs, diskSize);
        m_clusterSizeCombo->setCurrentIndex(defIdx >= 0 ? defIdx : 0);
    } else {
        m_clusterSizeCombo->addItem(tr("Default"));
        m_clusterSizeCombo->addItem(QStringLiteral("512 bytes"));
        m_clusterSizeCombo->addItem(QStringLiteral("4096 bytes"));
        m_clusterSizeCombo->setCurrentIndex(0);
    }

    m_clusterSizeCombo->blockSignals(false);
}

// ─── SELECT dropdown menu (recent images) ────────────────────────────
void MainWindow::populateSelectMenu() {
    if (!m_selectMenu)
        return;
    m_selectMenu->clear();
    m_selectMenu->addAction(tr("Select image..."), this, &MainWindow::onSelectImage);
    if (!m_recentImages.isEmpty()) {
        m_selectMenu->addSeparator();
        for (const QString &path : m_recentImages) {
            auto *act = m_selectMenu->addAction(QFileInfo(path).fileName());
            act->setToolTip(path);
            connect(act, &QAction::triggered, this, [this, path]() {
                onImageChanged(path);
            });
        }
        m_selectMenu->addSeparator();
        // "Clear" wipes the recent-images history only; the current image
        // stays selected (deselecting it had bugs with Windows ISOs).
        m_selectMenu->addAction(tr("Clear"), this, [this]() {
            m_recentImages.clear();
        });
    }
}

void MainWindow::updateElapsedLabel() {
    if (!m_elapsedLabel)
        return;
    qint64 secs = m_elapsedTimer.elapsed() / 1000;
    m_elapsedLabel->setText(QStringLiteral("%1:%2:%3")
        .arg(secs / 3600, 2, 10, QLatin1Char('0'))
        .arg((secs % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(secs % 60, 2, 10, QLatin1Char('0')));
}

// ─── Fixed window size (original Rufus behavior) ─────────────────────
// The window cannot be resized or maximized by the user; it is locked to
// the natural size of its content. The height changes only when a section
// is expanded/collapsed at runtime (advanced options, image info row),
// mirroring the compact fixed window of original Rufus
// (~455-460 px wide, ~620-630 px tall collapsed, ~700-720 px expanded).
void MainWindow::updateFixedSize() {
    // Unlock first: adjustSize() cannot shrink a widget whose minimum and
    // maximum sizes are both locked by the previous setFixedSize().
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setMinimumSize(0, 0);
    // Toggling widget visibility (advanced sections, image rows)
    // only SCHEDULES the layout update asynchronously. If we read sizeHint()
    // right away it still reports the OLD (expanded) size, so the window would
    // fail to shrink back when a section is collapsed and the extra space would
    // spread between the labels. Force the nested layouts to recompute now and
    // refresh the form widget's cached item geometry so sizeHint() is current.
    layout()->invalidate();
    if (m_mainHBox)
        m_mainHBox->invalidate();
    if (m_formWidget)
        m_formWidget->updateGeometry();
    adjustSize();
    // Original Rufus is ~455-460 px wide; keep at least that even if the
    // content would naturally fit in less.
    QSize natural = sizeHint();
    natural.setWidth(qMax(natural.width(), 455));
    setFixedSize(natural);
}

// ─── Log panel size (width only) ──────────────────────────────────────
// Toggling the log panel must change ONLY the window width (growing to the
// right, keeping the left edge fixed) and preserve the current height. Using
// updateFixedSize() here would recompute the full sizeHint() and shrink the
// window vertically, which makes the WM (e.g. KWin) reposition it and shift
// the title bar buttons.
//
// When the log is shown (attached), the form keeps its current width and the
// log panel is locked to exactly 455 px (the same width as the Rufus window),
// so the window becomes ~2x (form on the left, log on the right) without ever
// squeezing the form. When the log is hidden/detached, both width locks are
// released so the form returns to its natural width and a detached log can be
// freely resized.
void MainWindow::updateLogPanelSize() {
    if (!m_mainHBox || !m_formWidget)
        return;
    QPoint topLeft = pos();
    int curHeight = height();
    // The log/separator visibility was just toggled, but that only schedules a
    // layout update asynchronously. Force the nested layout and the form/log
    // cached item geometry to refresh now so sizeHint() reflects the new state.
    layout()->invalidate();
    if (m_mainHBox)
        m_mainHBox->invalidate();
    if (m_formWidget)
        m_formWidget->updateGeometry();
    if (m_logDialog)
        m_logDialog->updateGeometry();

    bool logInWindow = m_logDialog->isVisible() && m_logAttached;
    int formWidth = 455;
    if (logInWindow) {
        m_logDialog->setFixedWidth(formWidth);
    } else {
        m_logDialog->setMinimumSize(360, 200);
        m_logDialog->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    int newWidth = qMax(m_mainHBox->sizeHint().width(), formWidth);
    setFixedSize(newWidth, curHeight);
    move(topLeft);
}

// ─── Status bar: device count ───────────────────────────────────────
void MainWindow::updateDeviceCountStatus() {
    if (m_isRunning)
        return;
    // While hashing, the "Computing image hashes..." message must stay on
    // the status bar; the periodic device count message can wait.
    if (m_isHashing)
        return;
    int count = m_deviceCombo->count();
    if (count == 1 && m_deviceCombo->itemData(0).toString().isEmpty())
        count = 0;
    statusBar()->showMessage(count == 1
        ? tr("1 device found")
        : tr("%1 devices found").arg(count));
}

// ─── setupConnections ────────────────────────────────────────────────
void MainWindow::setupConnections() {
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(m_selectBtn, &QToolButton::clicked, this, &MainWindow::onSelectImage);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshDevices);
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDeviceChanged);
    connect(m_bootSelectionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBootSelectionChanged);
    connect(m_bootloaderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBootloaderChanged);
    connect(m_advancedFormatToggle, &QPushButton::clicked, this, &MainWindow::onAdvancedFormatToggled);
    connect(m_advancedDriveToggle, &QPushButton::clicked, this, &MainWindow::onAdvancedDriveToggled);
    connect(m_badBlocksCheck, &QCheckBox::toggled,
            m_nbPassesCombo, &QComboBox::setEnabled);
    connect(m_langBtn, &QToolButton::clicked, this, [this]() {
        if (m_langMenu && m_langBtn)
            m_langMenu->exec(m_langBtn->mapToGlobal(QPoint(0, m_langBtn->height())));
    });
    connect(m_logDialog, &LogDialog::detachClicked,
            this, &MainWindow::onLogDetachClicked);
    connect(m_aboutBtn, &QToolButton::clicked, this, [this]() {
        AboutDialog dlg(this);
        dlg.exec();
    });
    connect(m_prefsBtn, &QToolButton::clicked, this, [this]() {
        PreferencesDialog dlg(this);
        dlg.exec();
    });
    connect(m_logBtn, &QToolButton::clicked, this, &MainWindow::onOpenLog);
    connect(m_langGroup, &QActionGroup::triggered, this, &MainWindow::onLanguageChanged);
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::close);
    connect(m_schemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSchemeChanged);
    connect(m_targetSystemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTargetSystemChanged);
    // The "?" next to "Target system" was a dead link before: clicking it
    // now explains what UEFI-CSM is.
    connect(m_csmHelpLabel, &QLabel::linkActivated, this, [this](const QString &) {
        MsgBox::information(this, tr("UEFI-CSM"),
            tr("UEFI-CSM (Compatibility Support Module) is a component of the "
               "UEFI firmware that emulates a legacy BIOS environment, so that "
               "operating systems and bootloaders that only support BIOS boot "
               "can still run on UEFI machines.\n\n"
               "Choose 'BIOS (or UEFI-CSM)' for legacy BIOS firmware, or for "
               "UEFI firmware with CSM enabled. Choose 'UEFI (non CSM)' for "
               "native UEFI boot, e.g. on Windows 11 certified machines or "
               "when Secure Boot is used."));
    });
    connect(m_fsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFsChanged);
    connect(m_imageOptionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (idx == 1)
            m_verifyWriteCheck->setChecked(false);
        updateFixedSize();
    });
    connect(m_saveBtn, &QPushButton::clicked, this, [this]() {
        saveSettings();
        MsgBox::information(this, tr("Save Settings"),
            tr("Settings saved."));
    });
    connect(m_hashBtn, &QPushButton::clicked, this, [this]() {
        QString imgPath = selectedImagePath();
        if (imgPath.isEmpty() || m_hashWatcher) return;   // already computing

        Logger::info(QStringLiteral("Computing MD5, SHA-1 and SHA-256 hashes for %1")
            .arg(imgPath));
        // Keep this message until hashing completes: the periodic device
        // count message must not overwrite it (see updateDeviceCountStatus).
        m_isHashing = true;
        statusBar()->showMessage(tr("Computing image hashes..."));
        m_hashBtn->setEnabled(false);
        m_hashWatcher = new QFutureWatcher<QString>(this);
        connect(m_hashWatcher, &QFutureWatcher<QString>::finished,
                this, &MainWindow::onHashFinished);
        m_hashWatcher->setFuture(QtConcurrent::run([imgPath]() {
            QByteArray md5 = Hash::compute(imgPath, HashType::MD5, nullptr);
            QByteArray sha1 = Hash::compute(imgPath, HashType::SHA1, nullptr);
            QByteArray sha256 = Hash::compute(imgPath, HashType::SHA256, nullptr);
            return QStringLiteral("MD5: %1\nSHA-1: %2\nSHA-256: %3")
                .arg(Hash::toString(md5))
                .arg(Hash::toString(sha1))
                .arg(Hash::toString(sha256));
        }));
    });
    connect(m_listUsbHddCheck, &QCheckBox::toggled, this, [this](bool) {
        populateDevices();
    });
    connect(m_uefiMediaCheck, &QCheckBox::toggled, this, [this](bool) {
        if (m_schemeCombo->currentData().toInt() == static_cast<int>(PartitionScheme::GPT))
            m_espCheck->setChecked(m_uefiMediaCheck->isChecked());
    });
}

void MainWindow::populateDevices() {
    QString prevData = m_deviceCombo->currentData().toString();
    m_deviceCombo->blockSignals(true);
    m_deviceCombo->clear();
    QList<DeviceInfo> devices = DeviceManager::enumerate();

    bool listHdd = m_listUsbHddCheck ? m_listUsbHddCheck->isChecked() : false;

    for (const DeviceInfo &dev : devices) {
        // Loop devices are user-created virtual drives, always show them.
        if (!listHdd && !dev.isUsb && !dev.isRemovable && !dev.isLoop)
            continue;

        QString label = QStringLiteral("%1 (%2)")
            .arg(dev.name)
            .arg(formatSize(dev.size));

        if (dev.isUsb) label.prepend(QStringLiteral("USB: "));
        else if (dev.isMmc) label.prepend(QStringLiteral("MMC: "));
        else if (dev.isNvme) label.prepend(QStringLiteral("NVMe: "));
        else if (dev.isLoop) label.prepend(QStringLiteral("Loop: "));

        if (!dev.model.isEmpty())
            label += QStringLiteral(" - %1").arg(dev.model);

        if (dev.isSystem)
            label += QStringLiteral(" [SYSTEM]");

        m_deviceCombo->addItem(label, dev.path);
    }

    if (m_deviceCombo->count() == 0)
        m_deviceCombo->addItem(tr("No removable devices found"), QString());

    if (!prevData.isEmpty()) {
        int idx = m_deviceCombo->findData(prevData);
        if (idx >= 0) m_deviceCombo->setCurrentIndex(idx);
    }

    m_deviceCombo->blockSignals(false);

    if (m_deviceCombo->currentIndex() >= 0)
        onDeviceChanged(m_deviceCombo->currentIndex());

    updateDeviceCountStatus();
    updateContextualStates();
}

bool MainWindow::runBootChecks() {
    if (m_deviceCombo->currentData().toString().isEmpty())
        return false;

    DeviceInfo dev = DeviceManager::getDevice(m_deviceCombo->currentData().toString());
    if (dev.path.isEmpty())
        return false;

    // Check: image not on target drive
    QString imgPath = selectedImagePath();
    if (!imgPath.isEmpty()) {
        // ZIP/TAR archives are never valid disk images: refuse to write
        // them so the operation cannot corrupt the device.
        if (m_lastImageInfo.type == ImageType::CompressedZip ||
            isTarArchive(imgPath)) {
            MsgBox::critical(this, tr("Error"),
                tr("The selected file is not a valid disk image.\n"
                   "Please select an ISO or IMG file."));
            return false;
        }
        // WIM/ESD are Windows imaging formats, not disk images: writing them
        // would produce a blank drive that only reports "success".
        if (m_lastImageInfo.type == ImageType::WIM ||
            m_lastImageInfo.type == ImageType::ESD) {
            MsgBox::critical(this, tr("Error"),
                tr("WIM/ESD files are not supported.\n"
                   "Please use an ISO or IMG file."));
            return false;
        }
        QFileInfo fi(imgPath);
        QString imgCanonical = fi.canonicalPath();
        for (const QString &mount : dev.mountedVolumes) {
            QFileInfo mnt(mount);
            if (mnt.exists() && imgCanonical.startsWith(mnt.canonicalPath())) {
                MsgBox::critical(this, tr("Error"),
                    tr("The image file is located on the target drive!\n"
                       "Please copy the image to a different drive first."));
                return false;
            }
        }
    }

    // Check: image size vs device size
    if (!imgPath.isEmpty() && m_lastImageInfo.projectedSize > 0) {
        if (m_lastImageInfo.projectedSize > dev.size) {
            MsgBox::critical(this, tr("Error"),
                tr("The image is too large for this device!\n"
                   "Image size: %1\nDevice size: %2")
                .arg(formatSize(m_lastImageInfo.projectedSize))
                .arg(formatSize(dev.size)));
            return false;
        }
    }

    // Checks below depend on the format options (filesystem/cluster), which
    // are not applicable to raw disk images (DD mode) — the combos then hold
    // the "Not applicable" placeholder, so skip them entirely.
    bool fmtApplicable = !m_formatNotApplicable;

    // Check: MS-DOS requires cluster size < 64KB (compare by actual size,
    // not by a hardcoded combo index which depends on FS/disk geometry)
    int bootType = m_bootSelectionCombo->currentData().toInt();
    if (fmtApplicable && bootType == static_cast<int>(BootType::MSDOS)) {
        FileSystem fs = static_cast<FileSystem>(m_fsCombo->currentData().toInt());
        int csKB = PartitionManager::getClusterSizeFromIndex(
            fs, m_lastDevice.size, m_clusterSizeCombo->currentIndex());
        if (csKB >= 64) { // 64K or larger
            MsgBox::critical(this, tr("Error"),
                tr("MS-DOS cannot boot from a drive with a 64KB cluster size.\n"
                   "Please select a smaller cluster size or a different filesystem."));
            return false;
        }
    }

    // Check: FAT32 + >4GB files
    if (fmtApplicable && bootType == static_cast<int>(BootType::Image) && !imgPath.isEmpty()) {
        FileSystem fs = static_cast<FileSystem>(m_fsCombo->currentData().toInt());
        if ((fs == FileSystem::FAT16 || fs == FileSystem::FAT32) &&
            m_lastImageInfo.has4GBFile) {
            MsgBox::critical(this, tr("Error"),
                tr("The image contains a file larger than 4GB.\n"
                   "FAT32 does not support files larger than 4GB.\n"
                   "Please select NTFS or exFAT."));
            return false;
        }
    }

    // Check: UEFI target requires EFI bootable image
    int targetIdx = m_targetSystemCombo->currentData().toInt();
    if (targetIdx == 1 && bootType == static_cast<int>(BootType::Image) && !imgPath.isEmpty()) {
        if (!m_lastImageInfo.isUefiBootable) {
            QMessageBox::StandardButton ret = MsgBox::warning(this, tr("Warning"),
                tr("The selected image does not appear to be UEFI-bootable.\n"
                   "Continue anyway?"),
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes) return false;
        }
    }

    // Check: UEFI:NTFS requires NTFS or exFAT
    if (fmtApplicable && bootType == static_cast<int>(BootType::UefiNtfs)) {
        FileSystem fs = static_cast<FileSystem>(m_fsCombo->currentData().toInt());
        if (fs != FileSystem::NTFS && fs != FileSystem::exFAT) {
            MsgBox::critical(this, tr("Error"),
                tr("UEFI:NTFS requires NTFS or exFAT filesystem."));
            return false;
        }
    }

    // Check: the device is about to be overwritten — if it is still in
    // use (open files, mounted partitions), ask the user before continuing.
    if (DeviceManager::isDeviceBusy(dev.path)) {
        QMessageBox::StandardButton ret = MsgBox::warning(this, tr("Warning"),
            tr("The target device is in use by another application.\n"
               "Continuing may fail or corrupt data.\n\n"
               "Continue anyway?"),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) return false;
    }

    // Pre-flight: verify that the external tools required by the selected
    // options are installed, so the operation fails with a clear message
    // before touching the device instead of midway with a cryptic one.
    if (fmtApplicable) {
        FileSystem fs = static_cast<FileSystem>(m_fsCombo->currentData().toInt());
        QString tool;
        switch (fs) {
        case FileSystem::FAT16:
        case FileSystem::FAT32: tool = QStringLiteral("mkfs.fat"); break;
        case FileSystem::NTFS:  tool = QStringLiteral("mkfs.ntfs"); break;
        case FileSystem::exFAT: tool = QStringLiteral("mkfs.exfat"); break;
        case FileSystem::ext2:  tool = QStringLiteral("mkfs.ext2"); break;
        case FileSystem::ext3:  tool = QStringLiteral("mkfs.ext3"); break;
        case FileSystem::ext4:  tool = QStringLiteral("mkfs.ext4"); break;
        case FileSystem::btrfs: tool = QStringLiteral("mkfs.btrfs"); break;
        case FileSystem::XFS:   tool = QStringLiteral("mkfs.xfs"); break;
        case FileSystem::F2FS:  tool = QStringLiteral("mkfs.f2fs"); break;
        default: break;
        }
        if (!tool.isEmpty() && QStandardPaths::findExecutable(tool).isEmpty()) {
            MsgBox::critical(this, tr("Error"),
                tr("%1 is not installed on this system.\n"
                   "Please install the '%2' package and try again.")
                .arg(tool, tool));
            return false;
        }
        if (!archivePath().isEmpty() &&
            QStandardPaths::findExecutable(QStringLiteral("7z")).isEmpty()) {
            MsgBox::critical(this, tr("Error"),
                tr("7z is not installed on this system.\n"
                   "It is needed to extract the additional file.\n"
                   "Please install the 'p7zip' package and try again."));
            return false;
        }
    }

    // Bootloader tools: only when a bootloader is actually installed onto
    // the drive (raw DD images and plain ISO copies don't need them).
    if (fmtApplicable && bootType != static_cast<int>(BootType::Image)) {
        QString bootloader = m_bootloaderCombo->currentData().toString();
        if (bootType == static_cast<int>(BootType::UefiNtfs))
            bootloader = QStringLiteral("uefintfs");
        QString tool;
        if (bootloader == QStringLiteral("syslinux") ||
            bootloader == QStringLiteral("freedos")) {
            tool = QStringLiteral("syslinux");
        } else if (bootloader == QStringLiteral("grub2")) {
            tool = QStringLiteral("grub-install");
        } else if (bootloader == QStringLiteral("grub4dos") ||
                   bootloader == QStringLiteral("uefintfs")) {
            // These can download their missing files, but only if wget
            // exists; otherwise they would fail in the middle of the run.
            const QString file = (bootloader == QStringLiteral("grub4dos"))
                ? QStringLiteral("grldr") : QStringLiteral("uefi-ntfs.img");
            if (findDataFile(file).isEmpty())
                tool = QStringLiteral("wget");
        }
        if (!tool.isEmpty() && QStandardPaths::findExecutable(tool).isEmpty()) {
            MsgBox::critical(this, tr("Error"),
                tr("%1 is not installed on this system.\n"
                   "Please install the '%2' package and try again.")
                .arg(tool, tool));
            return false;
        }
    }

    return true;
}

void MainWindow::onStartStop() {
    if (m_isRunning) {
        if (m_worker) {
            m_worker->cancel();
            m_startBtn->setText(tr("ESPERE..."));
            m_startBtn->setEnabled(false);
            statusBar()->showMessage(tr("Cancelling - Please wait..."));
        }
        return;
    }

    if (m_deviceCombo->currentData().toString().isEmpty()) {
        MsgBox::warning(this, tr("Error"), tr("Please select a target device."));
        return;
    }

    QString imagePath = selectedImagePath();
    int bootTypeVal = m_bootSelectionCombo->currentData().toInt();

    // In "Disk or ISO image" mode with no image selected, guide the user
    // instead of silently formatting or installing a bootloader without
    // knowing which image to write.
    if (bootTypeVal == static_cast<int>(BootType::Image) && imagePath.isEmpty()) {
        MsgBox::warning(this, tr("No image selected"),
            tr("To continue, please select an image or disk (IMG or other "
               "format), or if you only want to format, select \"Non bootable\" "
               "in the boot selection."));
        return;
    }

    DeviceInfo dev = DeviceManager::getDevice(m_deviceCombo->currentData().toString());
    if (dev.path.isEmpty()) {
        MsgBox::warning(this, tr("Error"), tr("Could not read device info."));
        return;
    }

    // Run pre-flight boot checks
    if (!runBootChecks())
        return;

    // ISOHybrid: if the image is an ISOHybrid (both DD and ISO bootable), ask
    // the user which mode to use (ISO recommended, DD alternative).
    int isoHybridChoice = -1;
    if (bootTypeVal == static_cast<int>(BootType::Image) && !imagePath.isEmpty()) {
        ImageInfo info = m_lastImageInfo;
        if (info.isBootableImg > 0 && info.isIso) {
            isoHybridChoice = showIsoHybridDialog(this);
            if (isoHybridChoice < 0) return; // cancelled
        }
    }

    // Windows User Experience dialog for Windows images
    WueResult wueResult;
    QString unattendXmlPath;
    bool showWue = (bootTypeVal == static_cast<int>(BootType::Image) && !imagePath.isEmpty() &&
        m_lastImageInfo.hasWindows() && m_lastImageInfo.winVersion.major >= 6);
    if (showWue) {
        bool isWin11 = m_lastImageInfo.winVersion.major >= 10 &&
            m_lastImageInfo.winVersion.build >= 22000;
        int ret = showWueDialog(this, isWin11, m_lastImageInfo.winVersion.build,
            (m_imageOptionCombo->currentIndex() == 1), wueResult);
        if (ret == QDialog::Rejected) return;
    }

    FormatWorker::Config config;
    config.targetDevice = dev;
    config.imagePath = imagePath;

    // Determine mode
    if (imagePath.isEmpty()) {
        config.mode = FormatWorker::Mode::FormatOnly;
    } else {
        ImageInfo imgInfo = m_lastImageInfo;
        bool isRawImg = imgInfo.isRawDiskImage() || imgInfo.isDDOnly();

        // Handle ISOHybrid selection: honor the user's choice from the
        // dialog (ISO image mode must stay ISO, original Rufus keeps the
        // dialog answer).
        if (isoHybridChoice == 1) {
            // DD image mode
            config.mode = FormatWorker::Mode::WriteImage;
        } else if (isoHybridChoice == 0) {
            // ISO image mode — write as ISO even though the image is
            // also DD bootable.
            config.mode = FormatWorker::Mode::WriteImageIso;
        } else {
            if (bootTypeVal == static_cast<int>(BootType::Image)) {
                config.mode = isRawImg
                    ? FormatWorker::Mode::WriteImage
                    : FormatWorker::Mode::WriteImageIso;
            } else {
                config.mode = FormatWorker::Mode::CreateBootable;
            }
        }
    }

    // Format options
    config.scheme = static_cast<PartitionScheme>(m_schemeCombo->currentData().toInt());
    config.filesystem = static_cast<FileSystem>(m_fsCombo->currentData().toInt());
    config.volumeLabel = m_labelEdit->text();
    config.quickFormat = m_quickFormatCheck->isChecked();
    config.checkBadBlocks = m_badBlocksCheck->isChecked();
    config.badBlocksPasses = m_nbPassesCombo->currentIndex() + 1;
    config.verifyAfterWrite = m_verifyWriteCheck->isChecked();
    config.bootloaderType = m_bootloaderCombo->currentData().toString();
    if (bootTypeVal == static_cast<int>(BootType::UefiNtfs))
        config.bootloaderType = QStringLiteral("uefintfs");
    config.clusterSizeKB = PartitionManager::getClusterSizeFromIndex(
        config.filesystem, m_lastDevice.size, m_clusterSizeCombo->currentIndex());

    // MS-DOS requires FAT16 (as in original Rufus); the format step will
    // upgrade to FAT32 with a warning if the volume exceeds 2GB.
    if (config.bootloaderType == QStringLiteral("msdos") &&
        config.filesystem == FileSystem::FAT32) {
        config.filesystem = FileSystem::FAT16;
        Logger::warn(tr("Bootloader is MS-DOS: forcing FAT16 file system"));
    }

    int targetIdx = m_targetSystemCombo->currentData().toInt();
    config.targetType = (targetIdx == 1) ? TargetSystemType::UEFI : TargetSystemType::BIOS;

    config.extraParts.persistence = false;
    // Uncompressed size: used as the DD progress total so compressed
    // images don't jump to 100% before the write actually finishes.
    config.projectedSize = static_cast<qint64>(m_lastImageInfo.projectedSize);
    config.extraParts.esp = m_espCheck->isChecked();
    config.extraParts.uefiNtfs = m_uefiNtfsCheck->isChecked();
    config.extraParts.compatibility = m_oldBiosFixCheck->isChecked();
    config.extendedLabel = m_extendedLabelCheck->isChecked();
    config.archivePath = archivePath();

    // Map WUE result to config
    if (showWue && (wueResult.bypassTpm || wueResult.bypassNro || wueResult.disableBitlocker ||
                    wueResult.setUser || wueResult.noDataCollection)) {
        config.wue.enabled = true;
        config.wue.bypassTpm = wueResult.bypassTpm;
        config.wue.bypassSecureBoot = wueResult.bypassTpm;  // Original Rufus couples these
        config.wue.bypassRam = wueResult.bypassTpm;
        config.wue.bypassNro = wueResult.bypassNro;
        config.wue.disableBitLocker = wueResult.disableBitlocker;
        config.wue.skipMicrosoftAccount = wueResult.bypassNro;
        config.wue.disablePrivacySettings = wueResult.noDataCollection;
        config.wue.enableLocalAccount = wueResult.setUser;
        config.wue.localAccountName = wueResult.username;
        config.wue.localAccountPassword.clear();
    }

    // Confirmation dialog
    QString modeStr;
    switch (config.mode) {
    case FormatWorker::Mode::WriteImage: modeStr = tr("DD Image Write"); break;
    case FormatWorker::Mode::FormatOnly: modeStr = tr("Format Only"); break;
    case FormatWorker::Mode::CreateBootable: modeStr = tr("Create Bootable"); break;
    case FormatWorker::Mode::WriteImageIso: modeStr = tr("ISO Mode"); break;
    }

    auto reply = MsgBox::question(this, tr("Confirm"),
        tr("This will DESTROY ALL DATA on:\n%1 (%2)\n\nMode: %3\nFile system: %4\nProceed?")
            .arg(config.targetDevice.path)
            .arg(formatSize(config.targetDevice.size))
            .arg(modeStr)
            .arg(PartitionManager::fsToString(config.filesystem)),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    Logger::info(QStringLiteral("Operation started: mode=%1, device=%2, fs=%3, scheme=%4, image=%5")
        .arg(modeStr)
        .arg(config.targetDevice.path)
        .arg(PartitionManager::fsToString(config.filesystem))
        .arg(config.scheme == PartitionScheme::GPT ? QStringLiteral("GPT") : QStringLiteral("MBR"))
        .arg(config.imagePath.isEmpty() ? QStringLiteral("(none)") : config.imagePath));

    // Setup UI for operation
    setControlsEnabled(false);
    m_isRunning = true;
    m_startBtn->setText(tr("CANCELAR"));
    m_startBtn->setEnabled(true);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    m_progressBar->setValue(0);
    m_elapsedLabel->setText(QStringLiteral("00:00:00"));
    m_elapsedTimer.restart();
    m_elapsedTick->start();

    m_workerThread = new QThread(this);
    m_worker = new FormatWorker;
    m_worker->setConfig(config);
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, m_worker, &FormatWorker::run);
    connect(m_worker, &FormatWorker::finished, this, &MainWindow::onFormatFinished);
    connect(m_worker, &FormatWorker::progressChanged, this, &MainWindow::onProgressChanged);
    connect(m_worker, &FormatWorker::deviceProgress, this, &MainWindow::onDeviceProgress);
    connect(m_worker, &FormatWorker::logMessage, this, &MainWindow::onLogMessage);
    connect(m_worker, &FormatWorker::statusChanged, this, [this](const QString &s) {
        // Like original Rufus, the status message is rendered inside the
        // progress bar ("Extracting files to USB drive: 42%"), never as
        // an orphaned message in the window's status bar. The trailing
        // ellipsis of the raw message is dropped so it does not end up
        // double-punctuated next to the percentage.
        QString msg = s;
        while (msg.endsWith(QStringLiteral("...")) || msg.endsWith(QStringLiteral("…"))) {
            if (msg.endsWith(QStringLiteral("...")))
                msg.chop(3);
            else
                msg.chop(1);
        }
        m_progressBar->setFormat(QStringLiteral("%1: %p%").arg(msg));
    });
    connect(m_worker, &FormatWorker::statusBarMessage, this, [this](const QString &s) {
        // Detail line in the bottom status bar while the operation runs:
        // "Usando la imagen: arch.iso", "Extrayendo: <ruta>/<archivo>" or
        // "Trabajando con la unidad...". Stays until the operation ends.
        statusBar()->showMessage(s);
    });
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_autoRefreshTimer->stop();
    m_workerThread->start();
}

void MainWindow::onSelectImage() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select bootable image"),
        selectedImagePath().isEmpty() ? QString() : QFileInfo(selectedImagePath()).absolutePath(),
        tr("Disk Images (*.iso *.img *.vhd *.vhdx *.wim *.esd *.gz *.bz2 *.xz *.zst);;All Files (*)"));
    if (!path.isEmpty())
        onImageChanged(path);
}

void MainWindow::onRefreshDevices() {
    Logger::info("Refreshing device list");
    populateDevices();
    statusBar()->showMessage(tr("Devices refreshed"), 3000);
}

void MainWindow::onDeviceChanged(int index) {
    if (index < 0) return;
    QString devPath = m_deviceCombo->itemData(index).toString();
    m_lastDevice = DeviceManager::getDevice(devPath);

    if (devPath.isEmpty() || m_lastDevice.path.isEmpty()) {
        // No valid device: gray out all target-dependent controls and keep
        // START disabled (original Rufus behavior).
        updateContextualStates();
        return;
    }

    DeviceInfo dev = m_lastDevice;
    Logger::info(QStringLiteral("Device selected: %1 (%2, %3)")
        .arg(dev.name.isEmpty() ? dev.path : dev.name)
        .arg(dev.path)
        .arg(formatSize(dev.size)));

    // Update FS and cluster sizes based on device
    populateFsCombo(static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()));
    onFsChanged(m_fsCombo->currentIndex());

    // Device change repopulates the FS combo: re-assert the "Not
    // applicable" state if an incompatible image is still selected, and
    // re-apply the contextual enable/disable rules.
    updateContextualStates();
}

void MainWindow::onImageChanged(const QString &path) {
    setSelectedImagePath(path);

    if (path.isEmpty() || !QFileInfo::exists(path)) {
        // Stale image info must not leak into later operations (WUE dialog,
        // size checks, DD/ISO mode selection).
        m_lastImageInfo = ImageInfo();
        setFormatNotApplicable(false);
        int imgIdx = m_bootSelectionCombo->findData(static_cast<int>(BootType::Image));
        if (imgIdx >= 0) {
            m_bootSelectionCombo->blockSignals(true);
            m_bootSelectionCombo->setItemText(imgIdx, tr("Disk or ISO image (Please select a file)"));
            m_bootSelectionCombo->blockSignals(false);
        }
        // No image anymore: MS-DOS becomes available again.
        updateBootloaderItemState();
        m_hashBtn->setVisible(false);
        // Reset the image option row so no stale Windows To Go state leaks
        // into the next image (or a format-only operation).
        m_imageOptionLabel->setVisible(false);
        m_imageOptionCombo->setVisible(false);
        m_imageOptionCombo->setCurrentIndex(0);
        updateFixedSize();
        updateIdleProgressBarText();
        return;
    }

    // Image detection (ISO mount + content scan) can take seconds, so it
    // runs off the UI thread with a size/mtime-keyed cache.
    Logger::info(QStringLiteral("Detecting image: %1").arg(path));
    statusBar()->showMessage(tr("Analyzing image..."));
    m_pendingImagePath = path;
    QFileInfo fi(path);
    QString cacheKey = QStringLiteral("%1|%2|%3")
        .arg(path).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    auto it = m_imageInfoCache.find(cacheKey);
    if (it != m_imageInfoCache.end()) {
        m_lastImageInfo = it.value();
        applyImageInfo(path);
        return;
    }

    // If a previous detection is still running, its result is stale anyway
    // (the selected image has changed) - discard it without blocking the UI
    // thread. The pending-path check in onImageDetectFinished() discards any
    // result that arrives after a newer selection.
    delete m_imageWatcher;
    m_imageWatcher = new QFutureWatcher<ImageInfo>(this);
    connect(m_imageWatcher, &QFutureWatcher<ImageInfo>::finished,
            this, &MainWindow::onImageDetectFinished);
    m_imageWatcher->setFuture(QtConcurrent::run([path]() {
        return ImageHandler::detect(path);
    }));
}

void MainWindow::onImageDetectFinished() {
    if (!m_imageWatcher)
        return;
    m_lastImageInfo = m_imageWatcher->result();
    QFileInfo fi(m_pendingImagePath);
    m_imageInfoCache.insert(QStringLiteral("%1|%2|%3")
        .arg(m_pendingImagePath).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()),
        m_lastImageInfo);
    m_imageWatcher->deleteLater();
    m_imageWatcher = nullptr;

    // The image was cleared (boot mode changed) while detection was running:
    // discard the stale result instead of re-applying it.
    if (m_pendingImagePath != selectedImagePath())
        return;

    const ImageInfo &info = m_lastImageInfo;
    Logger::info(QStringLiteral("Image: %1, %2, UEFI-bootable=%3, BIOS-bootable=%4")
        .arg(info.type == ImageType::ISO ? QStringLiteral("ISO") : QStringLiteral("IMG"))
        .arg(formatSize(info.size))
        .arg(info.isUefiBootable ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(info.isBiosBootable ? QStringLiteral("yes") : QStringLiteral("no")));
    if (info.has4GBFile)
        Logger::warn("Image contains files larger than 4GB (NTFS/exFAT required)");
    if (info.isCompressed)
        Logger::info(QStringLiteral("Image is compressed, uncompressed size: %1")
            .arg(formatSize(static_cast<qint64>(info.projectedSize))));
    statusBar()->showMessage(QStringLiteral("Image ready: %1").arg(fi.fileName()), 5000);

    applyImageInfo(m_pendingImagePath);

    // Warn about files that can never be written (ZIP/TAR archives), and
    // about disk images with a partition table but no bootloader.
    const QString fileName = QFileInfo(m_pendingImagePath).fileName();
    const bool isTar = fileName.contains(QStringLiteral(".tar."), Qt::CaseInsensitive) ||
                       fileName.endsWith(QStringLiteral(".tgz"), Qt::CaseInsensitive);
    if (m_lastImageInfo.type == ImageType::CompressedZip || isTar) {
        MsgBox::warning(this, tr("Invalid Image"),
            tr("This file is not a valid disk image and cannot be written "
               "or booted.\nPlease check your file!"));
    } else if (m_lastImageInfo.type == ImageType::IMG &&
               !m_lastImageInfo.isBootableImg &&
               ImageHandler::hasPartitionTable(m_pendingImagePath)) {
        MsgBox::information(this, tr("Image not bootable"),
            tr("This image has a defined partition scheme, but no bootloader, "
               "so it can only be written as a disk clone."));
    }
}

void MainWindow::applyImageInfo(const QString &path) {
    m_hashBtn->setVisible(true);

    // Remember the image for the SELECT dropdown (most recent first).
    m_recentImages.removeAll(path);
    m_recentImages.prepend(path);
    while (m_recentImages.size() > 8)
        m_recentImages.takeLast();

    // The scheme list (MBR/GPT) depends on the detected image, so rebuild
    // it now that detection finished, then apply the recommendations.
    updatePartitionSchemeForBootType(static_cast<BootType>(
        m_bootSelectionCombo->currentData().toInt()));
    updateRecommendedSettings(path);

    int imgIdx = m_bootSelectionCombo->findData(static_cast<int>(BootType::Image));
    if (imgIdx >= 0) {
        m_bootSelectionCombo->blockSignals(true);
        m_bootSelectionCombo->setItemText(imgIdx, QFileInfo(path).fileName());
        m_bootSelectionCombo->setCurrentIndex(imgIdx);
        m_bootSelectionCombo->blockSignals(false);
    }

    const bool isTar = isTarArchive(path);
    const bool notApplicable = m_lastImageInfo.isRawDiskImage() ||
                               m_lastImageInfo.type == ImageType::CompressedZip ||
                               isTar;
    setFormatNotApplicable(notApplicable);

    // The image may have been selected while MS-DOS was active: resync the
    // bootloader item state and the file system combo (setBootloaderFromImage
    // ran with signals blocked, so the combo may still be locked to FAT16).
    onBootloaderChanged();
    updateFixedSize();
    updateIdleProgressBarText();
}

void MainWindow::onHashFinished() {
    if (!m_hashWatcher)
        return;
    QString result = m_hashWatcher->result();
    m_hashWatcher->deleteLater();
    m_hashWatcher = nullptr;
    m_hashBtn->setEnabled(true);
    m_isHashing = false;
    // Keep the newlines so the log shows one hash per line (MD5 / SHA-1 /
    // SHA-256) instead of a single comma-separated entry.
    Logger::info(QStringLiteral("Hashes computed:\n%1").arg(result));
    statusBar()->showMessage(tr("Image hashes computed"), 5000);
    MsgBox::information(this, tr("Image Hashes"), result);
}

void MainWindow::onFormatFinished(bool success, const QString &message, bool fakeFlash) {
    m_isRunning = false;
    m_autoRefreshTimer->start();
    setControlsEnabled(true);

    // Stop the worker thread: each operation spawns a fresh thread, and
    // leaving its event loop running leaks a live QThread child of this
    // window — destroying a still-running QThread aborts with qFatal at
    // close. quit() lets run()'s loop exit; the worker is deleteLater'd
    // when the thread finishes (deferred deletes are processed on exit).
    if (m_workerThread && m_workerThread->isRunning())
        m_workerThread->quit();

    if (success)
        Logger::info("Operation finished successfully: " + message);
    else
        Logger::error("Operation failed: " + message);

    QString imgPath = selectedImagePath();
    if (!imgPath.isEmpty() && QFileInfo::exists(imgPath))
        setFormatNotApplicable(m_lastImageInfo.isRawDiskImage() ||
                               m_lastImageInfo.type == ImageType::CompressedZip ||
                               isTarArchive(imgPath));

    m_startBtn->setText(tr("START"));
    updateContextualStates();
    // Success: like original Rufus the bar stays visible, filled to 100%
    // with "PREPARED" centered inside it. It is NOT reset to 0.
    if (success) {
        m_progressBar->setValue(100);
        m_progressBar->setFormat(tr("PREPARED"));
    }

    // Freeze the elapsed clock at the final value (like original Rufus).
    m_elapsedTick->stop();
    updateElapsedLabel();
    // The operation detail ("Usando la imagen: ..." etc.) is gone: the
    // device count message takes over the status bar again.
    statusBar()->clearMessage();
    updateDeviceCountStatus();

    // The user asked to close while the operation was running: the worker is
    // done now, so honor the close request without interrupting with dialogs.
    if (m_pendingClose) {
        m_pendingClose = false;
        saveSettings();
        close();
        return;
    }

    QProcess::startDetached("notify-send", {
        QStringLiteral("--app-name=Rufus"),
        QStringLiteral("--icon=") + (success ? QStringLiteral("media-optical") : QStringLiteral("dialog-error")),
        success ? tr("Rufus: Completed") : tr("Rufus: Failed"),
        message
    });

    if (success) {
        // No success popup and no orphan status message: like original
        // Rufus, success is communicated silently by the filled progress
        // bar showing "PREPARED".
        if (fakeFlash) {
            MsgBox::warning(this, tr("Warning"),
                tr("This device appears to be a fake flash drive:\n"
                   "it reports more storage capacity than it actually has.\n"
                   "Data written beyond the real capacity will be lost.\n"
                   "Use a genuine drive from a reputable brand."));
        }
    } else if (message.contains("cancelled", Qt::CaseInsensitive)) {
        MsgBox::information(this, tr("Cancelled"), message);
    } else {
        MsgBox::critical(this, tr("Error"), message);
    }
}

void MainWindow::onProgressChanged(int percent) {
    m_progressBar->setValue(percent);
}

void MainWindow::onDeviceProgress(qint64 current, qint64 total) {
    if (total > 0) {
        int pct = qBound(0, static_cast<int>(current * 100 / total), 100);
        m_progressBar->setValue(pct);
    }
}

void MainWindow::onLogMessage(const QString &message, int type) {
    switch (type) {
    case 0: Logger::info(message); break;
    case 1: Logger::error(message); break;
    case 2: Logger::warn(message); break;
    default: Logger::info(message); break;
    }
}

void MainWindow::onOpenLog() {
    if (!m_logDialog)
        return;
    if (!m_logAttached) {
        // Detached: toggle the top-level log window from Rufus so it can be
        // both opened and deactivated here.
        if (m_logDialog->isVisible()) {
            m_logDialog->hide();
        } else {
            m_logDialog->show();
            m_logDialog->raise();
            m_logDialog->activateWindow();
        }
    } else {
        bool visible = !m_logDialog->isVisible();
        m_logDialog->setVisible(visible);
        if (m_logSeparator)
            m_logSeparator->setVisible(visible);
        updateLogPanelSize();
    }
    m_logBtn->setToolTip(m_logDialog->isVisible()
                             ? tr("Disable the log window")
                             : tr("Enable the log window"));
}

void MainWindow::onLogDetachClicked() {
    if (!m_logDialog || !m_mainHBox)
        return;

    if (m_logAttached) {
        // Detach: take the panel out of the layout and show it as a
        // top-level window exactly where it was (to the right of the form,
        // same size it had while attached).
        m_logDialog->setParent(nullptr, Qt::Window);
        m_logDialog->setDetached(true);
        m_logDialog->setWindowIcon(windowIcon());
        if (m_logSeparator)
            m_logSeparator->setVisible(false);

        // Same width the panel had while attached (455 px) and the same
        // height as the main window so it appears in the exact spot it left.
        int lw = 455;
        int lh = height();
        m_logDialog->resize(lw, lh);

        // Place it just to the right of the form (where the log lived), at
        // the same vertical position. If it would overflow the screen, put it
        // to the left of the form instead; never on top of Rufus.
        QPoint formTopLeft = m_formWidget->mapToGlobal(QPoint(0, 0));
        int x = formTopLeft.x() + m_formWidget->width() + 4;
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect avail = screen->availableGeometry();
            if (x + lw > avail.width())
                x = formTopLeft.x() - lw - 4;
            if (x < avail.x())
                x = avail.x();
        }
        m_logDialog->show();
        m_logDialog->move(x, formTopLeft.y());
        m_logAttached = false;
    } else {
        // Attach: put the panel back at the right side of the window.
        m_logDialog->hide();
        m_logDialog->setParent(centralWidget());
        m_logDialog->setDetached(false);
        m_mainHBox->addWidget(m_logDialog);
        m_logDialog->show();
        if (m_logSeparator)
            m_logSeparator->setVisible(true);
        m_logAttached = true;
    }
    updateLogPanelSize();
}

void MainWindow::onLanguageChanged(QAction *action) {
    QString langCode = action->data().toString();
    Logger::info(QStringLiteral("Language changed to: %1").arg(langCode));
    Settings::instance().setLanguage(langCode);
    Settings::instance().sync();

    Localization::setLanguage(langCode);
    QEvent langEvent(QEvent::LanguageChange);
    QApplication::sendEvent(this, &langEvent);
    if (m_logDialog)
        QApplication::sendEvent(m_logDialog, &langEvent);
}

void MainWindow::onAdvancedFormatToggled() {
    bool visible = !m_quickFormatCheck->isVisible();
    m_quickFormatCheck->setVisible(visible);
    m_extendedLabelCheck->setVisible(visible);
    m_badBlocksCheck->setVisible(visible);
    m_nbPassesCombo->setVisible(visible);
    // Internal-only widgets stay hidden — visible never toggled
    m_advancedFormatToggle->setText(visible ? tr("Hide advanced format options")
                                             : tr("Show advanced format options"));
    m_advancedFormatToggle->setIcon(style()->standardIcon(
        visible ? QStyle::SP_ArrowDown : QStyle::SP_ArrowRight));

    // Rebuild boot selection combo to show/hide advanced boot types
    rebuildBootSelectionCombo();
    // Original Rufus: the fixed-size window grows when sections expand and
    // shrinks back when they collapse.
    updateFixedSize();
}

void MainWindow::onAdvancedDriveToggled() {
    bool visible = !m_listUsbHddCheck->isVisible();
    m_listUsbHddCheck->setVisible(visible);
    m_oldBiosFixCheck->setVisible(visible);
    m_uefiMediaCheck->setVisible(visible);
    m_advancedDriveToggle->setText(visible ? tr("Hide advanced drive properties")
                                            : tr("Show advanced drive properties"));
    m_advancedDriveToggle->setIcon(style()->standardIcon(
        visible ? QStyle::SP_ArrowDown : QStyle::SP_ArrowRight));
    updateFixedSize();
}

void MainWindow::onAutoRefresh() {
    if (m_isRunning) return;

    QString prevPath;
    if (m_deviceCombo->currentIndex() >= 0)
        prevPath = m_deviceCombo->currentData().toString();

    QList<DeviceInfo> devices = DeviceManager::enumerate();
    bool listHdd = m_listUsbHddCheck ? m_listUsbHddCheck->isChecked() : false;

    int itemCount = m_deviceCombo->count();
    bool hasPlaceholder = (itemCount == 1 && m_deviceCombo->itemData(0).toString().isEmpty());

    int filteredCount = 0;
    for (const DeviceInfo &dev : devices) {
        // Mirror populateDevices(): loop devices are always shown,
        // so they must be counted here too or the combo repopulates
        // on every 3s tick (infinite refresh loop).
        if (listHdd || dev.isUsb || dev.isRemovable || dev.isLoop)
            filteredCount++;
    }

    if (filteredCount != itemCount - (hasPlaceholder ? 1 : 0)) {
        populateDevices();
        return;
    }

    if (!prevPath.isEmpty()) {
        bool found = false;
        for (const DeviceInfo &dev : devices) {
            if (dev.path == prevPath) { found = true; break; }
        }
        if (!found) populateDevices();
    }

    updateDeviceCountStatus();
}

void MainWindow::onSchemeChanged(int index) {
    int scheme = m_schemeCombo->currentData().toInt();
    // Like original Rufus: MBR goes with BIOS (or UEFI-CSM) and UEFI
    // (non CSM) is not offered for an MBR scheme; GPT defaults to UEFI.
    // The one exception is an image that cannot boot through MBR at all
    // (EFI-bootable, not BIOS-bootable, e.g. a Windows ISO): then MBR is
    // reverted back to GPT instead of offering an impossible combination.
    bool imageMode = static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()) ==
                         BootType::Image &&
                     !selectedImagePath().isEmpty();
    bool imageEfiOnly = imageMode && m_lastImageInfo.isUefiBootable &&
                        !m_lastImageInfo.isBiosBootable;
    if (scheme == static_cast<int>(PartitionScheme::MBR) && imageEfiOnly) {
        int gptIdx = m_schemeCombo->findData(static_cast<int>(PartitionScheme::GPT));
        if (gptIdx >= 0) {
            m_schemeCombo->blockSignals(true);
            m_schemeCombo->setCurrentIndex(gptIdx);
            m_schemeCombo->blockSignals(false);
        }
        scheme = static_cast<int>(PartitionScheme::GPT);
    }
    int want = (scheme == static_cast<int>(PartitionScheme::GPT)) ? 1 : 0;
    int wantIdx = m_targetSystemCombo->findData(want);
    if (wantIdx >= 0) {
        m_targetSystemCombo->blockSignals(true);
        m_targetSystemCombo->setCurrentIndex(wantIdx);
        m_targetSystemCombo->blockSignals(false);
        // The index was set with signals blocked, so the "?" help link
        // next to the combo would not be updated by onTargetSystemChanged.
        m_csmHelpLabel->setVisible(want == 0);
    }
    updateTargetSystemForScheme();
    m_targetSystemCombo->setEnabled(!m_formatNotApplicable);

    // ESP partition is only available with GPT (or UEFI:NTFS)
    int bt = m_bootSelectionCombo->currentData().toInt();
    m_espCheck->setEnabled(
        scheme == static_cast<int>(PartitionScheme::GPT) ||
        bt == static_cast<int>(BootType::UefiNtfs));

    // Update old BIOS fix availability
    m_oldBiosFixCheck->setEnabled(
        scheme != static_cast<int>(PartitionScheme::GPT));
}

void MainWindow::onTargetSystemChanged(int index) {
    int target = m_targetSystemCombo->itemData(index).toInt();
    if (target == 0) {
        if (m_schemeCombo->currentData().toInt() == static_cast<int>(PartitionScheme::GPT)) {
            m_schemeCombo->setCurrentIndex(
                m_schemeCombo->findData(static_cast<int>(PartitionScheme::MBR)));
        }
    }

    // Show the blue "?" help link next to the combo whenever the
    // "BIOS (or UEFI-CSM)" target is selected.
    m_csmHelpLabel->setVisible(target == 0);
}

void MainWindow::onFsChanged(int index) {
    if (index < 0) return;
    FileSystem fs = static_cast<FileSystem>(m_fsCombo->itemData(index).toInt());
    bool hasClusterSize = (fs == FileSystem::FAT16 || fs == FileSystem::FAT32 ||
                           fs == FileSystem::NTFS || fs == FileSystem::exFAT);

    m_clusterSizeCombo->setEnabled(hasClusterSize && !m_formatNotApplicable);
    m_clusterSizeLabel->setEnabled(hasClusterSize && !m_formatNotApplicable);

    // Always populate so the combo never looks empty: file systems
    // without a cluster size option show a single "Default" entry.
    populateClusterSizes(fs);

    // Enable/disable extended label
    bool isExt = (fs == FileSystem::ext2 || fs == FileSystem::ext3 ||
                  fs == FileSystem::ext4 || fs == FileSystem::btrfs ||
                  fs == FileSystem::XFS || fs == FileSystem::F2FS);
    m_extendedLabelCheck->setEnabled(!isExt && !m_formatNotApplicable);

    // Volume label max length
    int maxLen = (fs == FileSystem::NTFS || fs == FileSystem::exFAT) ? 32 : 11;
    m_labelEdit->setMaxLength(maxLen);
}

void MainWindow::onBootSelectionChanged(int index) {
    Q_UNUSED(index);
    int bt = m_bootSelectionCombo->currentData().toInt();

    // Any non-image boot mode has no use for the selected ISO: drop it so
    // stale image info cannot leak into a format-only operation.
    if (bt != static_cast<int>(BootType::Image) && !selectedImagePath().isEmpty())
        onImageChanged(QString());

    // Update bootloader selection based on boot type
    switch (static_cast<BootType>(bt)) {
    case BootType::NonBootable:
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("none"));
        // A plain format has no prior volume label and, like original
        // Rufus' defaults, leaves the extended label/icon option off.
        m_labelEdit->clear();
        m_extendedLabelCheck->setChecked(false);
        break;
    case BootType::MSDOS:
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("msdos"));
        break;
    case BootType::FreeDOS:
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("freedos"));
        break;
    case BootType::SyslinuxV4:
    case BootType::SyslinuxV6:
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("syslinux"));
        break;
    case BootType::Grub2:
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("grub2"));
        break;
    case BootType::Grub4Dos:
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("grub4dos"));
        break;
    case BootType::UefiNtfs:
        // UEFI:NTFS is implemented by FormatWorker itself via
        // bootloaderType "uefintfs"; the bootloader combo has no entry
        // for it, so leave the combo untouched and map it in onStartStop.
        break;
    case BootType::ReactOS:
        // Original Rufus uses Syslinux for ReactOS (BT_REACTOS installs
        // Syslinux and boots FreeLoader via mboot.c32), never FreeDOS.
        m_bootloaderCombo->setCurrentIndex(m_bootloaderCombo->findData("syslinux"));
        break;
    case BootType::Image:
        if (selectedImagePath().isEmpty())
            onSelectImage();
        break;
    default:
        break;
    }

    // Update allowed filesystems
    populateFsCombo(static_cast<BootType>(bt));

    // Update partition scheme availability
    updatePartitionSchemeForBootType(static_cast<BootType>(bt));

    // MS-DOS is not compatible with image boot mode: update the item state
    // (and fall back to "none" if it was active) whenever the boot type
    // or the selected image changes.
    updateBootloaderItemState();

    // The combos were repopulated: re-assert the "Not applicable" state
    // if an incompatible image is still selected.
    if (m_formatNotApplicable)
        setFormatNotApplicable(true);
}

void MainWindow::onBootloaderChanged() {
    bool msdos = isMsdosBootloader();

    // Rebuild the file system list: in MS-DOS mode only FAT16 is shown
    // (and locked), so the UI never offers a format that onStartStop would
    // silently override.
    populateFsCombo(static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()));
    if (msdos) {
        int fsIdx = m_fsCombo->findData(static_cast<int>(FileSystem::FAT16));
        if (fsIdx >= 0)
            m_fsCombo->setCurrentIndex(fsIdx);
        m_fsCombo->setEnabled(false);
        m_fsCombo->setToolTip(tr("MS-DOS boot mode only supports FAT16."));
    } else {
        // Raw disk images (DD mode) keep all format options disabled.
        bool ddMode = m_lastImageInfo.isRawDiskImage() || m_formatNotApplicable;
        m_fsCombo->setEnabled(!ddMode);
        m_fsCombo->setToolTip({});
    }

    updateBootloaderItemState();

    // The file system list was rebuilt: re-assert the "Not applicable"
    // state so the placeholder item stays selected.
    if (m_formatNotApplicable)
        setFormatNotApplicable(true);
}

void MainWindow::updateBootloaderItemState() {
    bool imageMode = !selectedImagePath().isEmpty() &&
        (static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()) == BootType::Image);

    // In image mode the ISO provides its own boot files; MS-DOS cannot be
    // used and is grayed out so the user cannot pick a combination that
    // would end up unbootable (Windows ISOs cannot boot from FAT16).
    int msdosIdx = m_bootloaderCombo->findData(QStringLiteral("msdos"));
    if (msdosIdx >= 0) {
        // QComboBox uses a QStandardItemModel internally: disabling the
        // item (Qt::ItemIsEnabled flag) grays it out and makes it
        // unselectable in the popup without removing it from the list.
        auto *model = qobject_cast<QStandardItemModel *>(m_bootloaderCombo->model());
        if (model) {
            QStandardItem *item = model->item(msdosIdx);
            if (item)
                item->setEnabled(!imageMode);
        }
    }

    // An image was selected while MS-DOS was active: fall back to "none"
    // instead of silently writing an unbootable FAT16 drive.
    if (imageMode && isMsdosBootloader()) {
        int noneIdx = m_bootloaderCombo->findData(QStringLiteral("none"));
        if (noneIdx >= 0)
            m_bootloaderCombo->setCurrentIndex(noneIdx);
    }
    updateFixedSize();
    // The idle text in the progress bar depends on the boot selection.
    updateIdleProgressBarText();
}

void MainWindow::updatePartitionSchemeForBootType(BootType bt) {
    m_schemeCombo->blockSignals(true);
    int prevScheme = m_schemeCombo->currentData().toInt();

    m_schemeCombo->clear();
    m_schemeCombo->addItem(QStringLiteral("MBR"), static_cast<int>(PartitionScheme::MBR));

    // QComboBox uses a QStandardItemModel internally: disabling an item
    // (Qt::ItemIsEnabled flag) grays it out and makes it unselectable in
    // the popup without removing it from the list.
    auto setTargetEnabled = [this](int dataVal, bool en) {
        int idx = m_targetSystemCombo->findData(dataVal);
        if (idx < 0)
            return;
        if (auto *model = qobject_cast<QStandardItemModel *>(m_targetSystemCombo->model())) {
            if (auto *item = model->item(idx))
                item->setEnabled(en);
        }
    };

    // Non-image boot types always support both BIOS and UEFI.
    setTargetEnabled(0, true);
    setTargetEnabled(1, true);

    switch (bt) {
    case BootType::MSDOS:
    case BootType::FreeDOS:
    case BootType::SyslinuxV4:
    case BootType::SyslinuxV6:
    case BootType::ReactOS:
    case BootType::Grub4Dos:
    case BootType::Grub2:
        // MBR only
        break;
    case BootType::Image: {
        // Like original Rufus, an ISO image always keeps both MBR and GPT
        // available (almost any modern ISO can boot through legacy MBR
        // BIOS), except BIOS-only images which cannot use GPT at all.
        const ImageInfo &info = m_lastImageInfo;
        bool imageKnown = !selectedImagePath().isEmpty() &&
                          (info.isUefiBootable || info.isBiosBootable);
        bool biosOnly = imageKnown && info.isBiosBootable && !info.isUefiBootable;
        if (!biosOnly)
            m_schemeCombo->addItem(QStringLiteral("GPT"), static_cast<int>(PartitionScheme::GPT));
        // Like original Rufus, the target system list is narrowed to what
        // the image supports: an EFI-bootable image cannot be installed as
        // plain BIOS (and vice versa), so the disallowed entry is grayed
        // out. Default is UEFI for any EFI-bootable image.
        if (imageKnown) {
            setTargetEnabled(0, info.isBiosBootable);
            setTargetEnabled(1, info.isUefiBootable);
            int want = info.isUefiBootable ? 1 : 0;
            int wantIdx = m_targetSystemCombo->findData(want);
            if (wantIdx >= 0) {
                m_targetSystemCombo->blockSignals(true);
                m_targetSystemCombo->setCurrentIndex(wantIdx);
                m_targetSystemCombo->blockSignals(false);
            }
        }
        break;
    }
    default:
        m_schemeCombo->addItem(QStringLiteral("GPT"), static_cast<int>(PartitionScheme::GPT));
        break;
    }

    // Try to restore previous selection. An image that cannot boot through
    // MBR at all (EFI-bootable, not BIOS-bootable) makes MBR impossible:
    // fall back to GPT, like onSchemeChanged does for manual MBR picks.
    if (static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()) ==
            BootType::Image && !selectedImagePath().isEmpty() &&
        m_lastImageInfo.isUefiBootable && !m_lastImageInfo.isBiosBootable &&
        prevScheme == static_cast<int>(PartitionScheme::MBR))
        prevScheme = static_cast<int>(PartitionScheme::GPT);
    int newIdx = m_schemeCombo->findData(prevScheme);
    if (newIdx >= 0)
        m_schemeCombo->setCurrentIndex(newIdx);

    updateTargetSystemForScheme();

    m_schemeCombo->blockSignals(false);
}

void MainWindow::updateTargetSystemForScheme() {
    // Like original Rufus, "UEFI (non CSM)" is hidden while the partition
    // scheme is MBR (only BIOS / UEFI-CSM applies then) and comes back
    // when switching to GPT. QComboBox hides items whose Qt::UserRole-1
    // data is 0; a non-zero value restores visibility.
    int uefiIdx = m_targetSystemCombo->findData(1);
    if (uefiIdx < 0)
        return;
    bool mbr = m_schemeCombo->currentData().toInt() ==
               static_cast<int>(PartitionScheme::MBR);
    m_targetSystemCombo->blockSignals(true);
    m_targetSystemCombo->setItemData(uefiIdx, mbr ? 0 : 1, Qt::UserRole - 1);
    if (mbr && m_targetSystemCombo->currentData().toInt() == 1) {
        int biosIdx = m_targetSystemCombo->findData(0);
        if (biosIdx >= 0)
            m_targetSystemCombo->setCurrentIndex(biosIdx);
    }
    m_targetSystemCombo->blockSignals(false);
    // The combo may have been changed with signals blocked above: keep the
    // blue "?" help link in sync with the actually selected target.
    m_csmHelpLabel->setVisible(m_targetSystemCombo->currentData().toInt() == 0);
}

void MainWindow::setControlsEnabled(bool enabled) {
    m_deviceCombo->setEnabled(enabled);
    m_langBtn->setEnabled(enabled);
    m_refreshBtn->setEnabled(enabled);
    m_bootSelectionCombo->setEnabled(enabled);
    m_selectBtn->setEnabled(enabled);
    m_advancedFormatToggle->setEnabled(enabled);
    m_advancedDriveToggle->setEnabled(enabled);
    m_quickFormatCheck->setEnabled(enabled);
    m_extendedLabelCheck->setEnabled(enabled);
    m_verifyWriteCheck->setEnabled(enabled);
    m_badBlocksCheck->setEnabled(enabled);
    m_nbPassesCombo->setEnabled(enabled && m_badBlocksCheck->isChecked());
    m_espCheck->setEnabled(enabled);
    m_uefiNtfsCheck->setEnabled(enabled);
    m_oldBiosFixCheck->setEnabled(enabled);
    m_listUsbHddCheck->setEnabled(enabled);
    m_uefiMediaCheck->setEnabled(enabled);
    m_schemeCombo->setEnabled(enabled);
    m_targetSystemCombo->setEnabled(enabled);
    m_fsCombo->setEnabled(enabled);
    m_clusterSizeCombo->setEnabled(enabled);
    m_labelEdit->setEnabled(enabled);
    m_bootloaderCombo->setEnabled(enabled);
    m_saveBtn->setEnabled(enabled);
    m_hashBtn->setEnabled(enabled);
}

void MainWindow::updateContextualStates() {
    // Like original Rufus, all target-dependent controls stay grayed out
    // until a real device is selected (the combo only holds the "No
    // removable devices found" placeholder), and START stays disabled
    // until then too.
    const bool validDevice = !m_deviceCombo->currentData().toString().isEmpty();
    const bool enabled = !m_isRunning && validDevice;

    m_schemeCombo->setEnabled(enabled);
    m_targetSystemCombo->setEnabled(enabled);
    m_fsCombo->setEnabled(enabled);
    m_clusterSizeCombo->setEnabled(enabled);
    m_labelEdit->setEnabled(enabled);
    m_bootloaderCombo->setEnabled(enabled);
    m_partitionSchemeLabel->setEnabled(enabled);
    m_fileSystemLabel->setEnabled(enabled);
    m_volumeLabelLabel->setEnabled(enabled);
    m_bootloaderLabel->setEnabled(enabled);
    m_quickFormatCheck->setEnabled(enabled);
    m_extendedLabelCheck->setEnabled(enabled);
    m_verifyWriteCheck->setEnabled(enabled);
    m_badBlocksCheck->setEnabled(enabled);
    m_nbPassesCombo->setEnabled(enabled && m_badBlocksCheck->isChecked());
    m_espCheck->setEnabled(enabled);
    m_uefiNtfsCheck->setEnabled(enabled);
    m_oldBiosFixCheck->setEnabled(enabled);
    m_uefiMediaCheck->setEnabled(enabled);

    // START needs a real device. (Without an image in "Disk or ISO image"
    // mode the button stays enabled and onStartStop() shows guidance
    // instead of starting, so the user is told how to proceed.)
    m_startBtn->setEnabled(enabled);

    // Re-assert the per-image sub-states (Not applicable combos, MS-DOS
    // bootloader rules, advanced options) so the device gating above does
    // not accidentally loosen them.
    if (validDevice) {
        if (m_formatNotApplicable)
            setFormatNotApplicable(true);
        else
            setFormatNotApplicable(false);
        updateBootloaderItemState();
        updateAdvancedFromImage(m_lastImageInfo);
        populateFsCombo(static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()));
    }
}

void MainWindow::updateIdleProgressBarText() {
    if (m_isRunning)
        return;
    // A fresh selection invalidates the finished state (100% + PREPARED).
    m_progressBar->setValue(0);
    // Like original Rufus, the bar is never blank: it shows a hint when
    // the user still has to pick an image (boot selection = "Disk or ISO
    // image" with no file), or "PREPARED" once the selection is ready to
    // write (image loaded, or any other boot selection). All status text
    // is rendered centered inside the bar (theme-compatible, palette
    // colors — never right-aligned).
    bool imageSlot = static_cast<BootType>(m_bootSelectionCombo->currentData().toInt()) ==
                     BootType::Image;
    bool hasImage = imageSlot && !selectedImagePath().isEmpty();
    if (imageSlot && !hasImage) {
        m_progressBar->setFormat(tr("SELECT IMAGE"));
    } else {
        m_progressBar->setFormat(tr("PREPARED"));
    }
}

void MainWindow::updateHashButtonStyle() {
    if (!m_hashBtn)
        return;
    m_hashBtn->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; background: transparent; }"
        "QPushButton:hover { background-color: %1; border-radius: 3px; }")
        .arg(palette().color(QPalette::Highlight).name()));
}

void MainWindow::updateRecommendedSettings(const QString &imagePath) {
    if (imagePath.isEmpty() || !QFileInfo::exists(imagePath)) return;

    ImageInfo info = m_lastImageInfo;

    // Partition scheme
    setPartitionSchemeFromImage(info);

    // File system
    if (!info.recommendedFs.isEmpty()) {
        FileSystem fs = PartitionManager::fsFromString(info.recommendedFs);
        int fsIdx = m_fsCombo->findData(static_cast<int>(fs));
        if (fsIdx >= 0) {
            m_fsCombo->blockSignals(true);
            m_fsCombo->setCurrentIndex(fsIdx);
            m_fsCombo->blockSignals(false);
        }
    }

    // Volume label from ISO (only for ISO9660 images: raw .img files have
    // no volume descriptor, and random bytes read as a label would corrupt
    // the saved label and make the format step fail later). When the ISO has
    // no label we explicitly clear it so it stays empty ("Ninguna").
    if (info.type == ImageType::ISO) {
        m_labelEdit->setText(info.label);
    }

    // Boot type
    if (info.autoBootType != BootType::Max) {
        int btIdx = m_bootSelectionCombo->findData(static_cast<int>(info.autoBootType));
        if (btIdx >= 0) {
            m_bootSelectionCombo->blockSignals(true);
            m_bootSelectionCombo->setCurrentIndex(btIdx);
            m_bootSelectionCombo->blockSignals(false);
        }
    }

    // Bootloader
    setBootloaderFromImage(info);

    // Advanced options based on image
    updateAdvancedFromImage(info);
}

void MainWindow::updateAdvancedFromImage(const ImageInfo &info) {
    m_espCheck->setEnabled(info.isUefiBootable);

    bool needsUefiNtfs = info.hasWindows() && info.has4GBFile &&
                         info.partitionScheme.contains("GPT", Qt::CaseInsensitive);
    m_uefiNtfsCheck->setEnabled(needsUefiNtfs);

    // Show the image option row only for Windows images (Windows To Go is
    // a Windows-only option; Linux ISOs never get an image option).
    bool showImageOpt = info.hasWindows() && info.wininstIndex > 0;
    m_imageOptionLabel->setVisible(showImageOpt);
    m_imageOptionCombo->setVisible(showImageOpt);

    updateFixedSize();
}

void MainWindow::setBootloaderFromImage(const ImageInfo &info) {
    if (info.recommendedBootloader.isEmpty())
        return;

    int blIdx = m_bootloaderCombo->findData(info.recommendedBootloader);
    if (blIdx >= 0) {
        m_bootloaderCombo->blockSignals(true);
        m_bootloaderCombo->setCurrentIndex(blIdx);
        m_bootloaderCombo->blockSignals(false);
    }
}

void MainWindow::setPartitionSchemeFromImage(const ImageInfo &info) {
    if (info.partitionScheme.isEmpty())
        return;

    QString upper = info.partitionScheme.toUpper();
    int idx = -1;
    if (upper == "GPT") {
        idx = m_schemeCombo->findData(static_cast<int>(PartitionScheme::GPT));
    } else if (upper == "MBR_FOR_UEFI" || upper.contains("UEFI")) {
        idx = m_schemeCombo->findData(static_cast<int>(PartitionScheme::MBR));
        int uefiIdx = m_targetSystemCombo->findData(1);
        if (uefiIdx >= 0)
            m_targetSystemCombo->setCurrentIndex(uefiIdx);
    } else {
        idx = m_schemeCombo->findData(static_cast<int>(PartitionScheme::MBR));
    }

    if (idx >= 0) {
        m_schemeCombo->blockSignals(true);
        m_schemeCombo->setCurrentIndex(idx);
        m_schemeCombo->blockSignals(false);
    }
    // The scheme change is applied with signals blocked, so re-sync the
    // "UEFI (non CSM)" target visibility, which depends on the scheme.
    if (upper == "GPT")
        updateTargetSystemForScheme();
}

void MainWindow::setFormatNotApplicable(bool notApplicable) {
    const bool wasNotApplicable = m_formatNotApplicable;
    m_formatNotApplicable = notApplicable;
    const QString naText = tr("Not applicable");
    const QString naSuffix = QStringLiteral(" (%1)").arg(naText);

    // Add (or remove) the "Not applicable" placeholder item in a combo,
    // saving the previous selection so it can be restored. The placeholder
    // is matched by its text (findData(QVariant()) is unreliable: combos
    // whose items carry no user data, like the cluster size combo, report
    // an empty QVariant for every item). Idempotent: if the combo was
    // repopulated while the state was active, re-inserting the placeholder
    // keeps the state consistent.
    auto applyCombo = [&](QComboBox *combo, QVariant *savedData, int *savedIndex) {
        int naIdx = combo->findText(naText);
        if (notApplicable) {
            if (naIdx < 0) {
                if (!wasNotApplicable) {
                    *savedData = combo->currentData();
                    *savedIndex = combo->currentIndex();
                }
                combo->insertItem(0, naText, QVariant());
                naIdx = 0;
            }
            combo->blockSignals(true);
            combo->setCurrentIndex(naIdx);
            combo->blockSignals(false);
            combo->setEnabled(false);
        } else {
            if (naIdx >= 0) {
                combo->removeItem(naIdx);
                int restoreIdx = combo->findData(*savedData);
                if (restoreIdx < 0 && *savedIndex >= 0)
                    restoreIdx = qMin(*savedIndex, combo->count() - 1);
                if (restoreIdx >= 0) {
                    combo->blockSignals(true);
                    combo->setCurrentIndex(restoreIdx);
                    combo->blockSignals(false);
                }
            }
            combo->setEnabled(true);
        }
    };

    // Append " (Not applicable)" to a checkbox text while the state is
    // active; strip it back off when the state is cleared.
    auto applyCheck = [&](QCheckBox *check) {
        QString text = check->text();
        if (notApplicable) {
            if (!text.endsWith(naSuffix)) {
                check->setText(text + naSuffix);
                text = check->text();
            }
            check->setEnabled(false);
        } else {
            if (text.endsWith(naSuffix))
                check->setText(text.left(text.size() - naSuffix.size()));
            check->setEnabled(true);
        }
    };

    applyCombo(m_schemeCombo, &m_savedSchemeData, &m_savedSchemeIndex);
    applyCombo(m_targetSystemCombo, &m_savedTargetData, &m_savedTargetIndex);
    applyCombo(m_fsCombo, &m_savedFsData, &m_savedFsIndex);
    applyCombo(m_clusterSizeCombo, &m_savedClusterData, &m_savedClusterIndex);

    // Volume label: keep the text but disable it (and clear it) while the
    // state is active so it cannot look editable.
    if (notApplicable) {
        if (!wasNotApplicable)
            m_savedLabelText = m_labelEdit->text();
        m_labelEdit->clear();
        m_labelEdit->setEnabled(false);
    } else {
        if (wasNotApplicable)
            m_labelEdit->setText(m_savedLabelText);
        m_labelEdit->setEnabled(true);
    }

    applyCheck(m_quickFormatCheck);
    applyCheck(m_extendedLabelCheck);

    m_partitionSchemeLabel->setEnabled(!notApplicable);
    m_fileSystemLabel->setEnabled(!notApplicable);
    m_volumeLabelLabel->setEnabled(!notApplicable);
    m_bootloaderLabel->setEnabled(!notApplicable);
    m_bootloaderCombo->setEnabled(!notApplicable);
    // "Boot selection" is never disabled: even for DD images the user must
    // be able to switch back to a normal boot type.

    if (!notApplicable) {
        // Re-apply the per-filesystem rules (cluster size, extended label)
        // that onFsChanged would have set for a normal state.
        m_fsCombo->setEnabled(!isMsdosBootloader());
        onFsChanged(m_fsCombo->currentIndex());
    }
}

void MainWindow::loadSettings() {
    auto &settings = Settings::instance();
    restoreGeometry(settings.geometry());
    restoreState(settings.windowState());

    // Restore combos by value (data), not by index: the combo contents
    // depend on the selected boot type and can be reordered/truncated.
    int savedScheme = settings.partitionScheme();
    int schemeIdx = m_schemeCombo->findData(savedScheme);
    if (schemeIdx >= 0)
        m_schemeCombo->setCurrentIndex(schemeIdx);

    int savedFs = settings.filesystem();
    int fsIdx = m_fsCombo->findData(savedFs);
    if (fsIdx >= 0)
        m_fsCombo->setCurrentIndex(fsIdx);

    QString savedBootloader = settings.bootloader();
    int blIdx = m_bootloaderCombo->findData(savedBootloader);
    if (blIdx >= 0)
        m_bootloaderCombo->setCurrentIndex(blIdx);

    int savedBootType = settings.bootType();
    // Block signals: restoring the saved "Disk or ISO image" boot type must
    // not trigger onBootSelectionChanged, which would open a file dialog at
    // startup before the window is even shown.
    m_bootSelectionCombo->blockSignals(true);
    int btIdx = m_bootSelectionCombo->findData(savedBootType);
    if (btIdx >= 0)
        m_bootSelectionCombo->setCurrentIndex(btIdx);
    else
        m_bootSelectionCombo->setCurrentIndex(
            m_bootSelectionCombo->findData(static_cast<int>(BootType::Image)));
    m_bootSelectionCombo->blockSignals(false);

    m_quickFormatCheck->setChecked(settings.quickFormat());
    m_targetSystemCombo->setCurrentIndex(settings.targetSystem());
    // A saved MBR + UEFI combination is impossible: re-apply the
    // scheme-based target visibility after restoring the settings.
    updateTargetSystemForScheme();
    m_verifyWriteCheck->setChecked(settings.verifyWrite());
    m_badBlocksCheck->setChecked(settings.badBlocks());
    m_labelEdit->setText(settings.volumeLabel());
    m_listUsbHddCheck->setChecked(settings.listUsbHdd());
    m_recentImages = settings.recentImages();
    m_recentImages.removeAll(QString());
    while (m_recentImages.size() > 8)
        m_recentImages.takeLast();
    int savedOption = settings.imageOption();
    if (savedOption >= 0 && savedOption < m_imageOptionCombo->count())
        m_imageOptionCombo->setCurrentIndex(savedOption);

    // Advanced sections visibility (original Rufus starts with both
    // sections collapsed for a compact fixed window)
    bool advFormatVisible = settings.advancedFormatVisible();
    if (!advFormatVisible) {
        m_quickFormatCheck->setVisible(false);
        m_extendedLabelCheck->setVisible(false);
        m_badBlocksCheck->setVisible(false);
        m_nbPassesCombo->setVisible(false);
        m_verifyWriteCheck->setVisible(false);
        m_espCheck->setVisible(false);
        m_uefiNtfsCheck->setVisible(false);
        m_bootloaderLabel->setVisible(false);
        m_bootloaderCombo->setVisible(false);
        m_advancedFormatToggle->setText(tr("Show advanced format options"));
        m_advancedFormatToggle->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    } else {
        // setupUi() leaves the advanced widgets hidden by default; when the
        // saved (or default) state is "visible", restore them explicitly or
        // the toggle would claim "Hide advanced..." while nothing is shown.
        m_quickFormatCheck->setVisible(true);
        m_extendedLabelCheck->setVisible(true);
        m_badBlocksCheck->setVisible(true);
        m_nbPassesCombo->setVisible(true);
        m_verifyWriteCheck->setVisible(true);
        m_espCheck->setVisible(true);
        m_uefiNtfsCheck->setVisible(true);
        m_bootloaderLabel->setVisible(true);
        m_bootloaderCombo->setVisible(true);
        m_advancedFormatToggle->setText(tr("Hide advanced format options"));
        m_advancedFormatToggle->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    }

    bool advDriveVisible = settings.advancedDriveVisible();
    if (!advDriveVisible) {
        m_listUsbHddCheck->setVisible(false);
        m_oldBiosFixCheck->setVisible(false);
        m_uefiMediaCheck->setVisible(false);
        m_advancedDriveToggle->setText(tr("Show advanced drive properties"));
        m_advancedDriveToggle->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    } else {
        m_advancedDriveToggle->setText(tr("Hide advanced drive properties"));
        m_advancedDriveToggle->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    }

    // The interface language is detected from the desktop session at
    // startup (see main.cpp): the saved preference is not re-applied here,
    // so the app always follows the session language on launch. The
    // language menu still switches it for the current session.

    // Combos were restored with signals blocked, so normalize the
    // MS-DOS/image interaction explicitly (e.g. a saved msdos bootloader
    // combined with image boot mode must fall back to "none").
    updateBootloaderItemState();
}

void MainWindow::saveSettings() {
    auto &settings = Settings::instance();
    settings.setGeometry(saveGeometry());
    settings.setWindowState(saveState());
    // Save combo selections by value so they survive combo rebuilds.
    settings.setPartitionScheme(m_schemeCombo->currentData().toInt());
    settings.setFilesystem(m_fsCombo->currentData().toInt());
    settings.setBootloader(m_bootloaderCombo->currentData().toString());
    settings.setBootType(m_bootSelectionCombo->currentData().toInt());
    settings.setQuickFormat(m_quickFormatCheck->isChecked());
    settings.setTargetSystem(m_targetSystemCombo->currentData().toInt());
    settings.setVerifyWrite(m_verifyWriteCheck->isChecked());
    settings.setAdvancedFormatVisible(m_quickFormatCheck->isVisible());
    settings.setAdvancedDriveVisible(m_listUsbHddCheck->isVisible());
    settings.setListUsbHdd(m_listUsbHddCheck->isChecked());
    settings.setVolumeLabel(m_labelEdit->text());
    settings.setBadBlocks(m_badBlocksCheck->isChecked());
    settings.setImageOption(m_imageOptionCombo->currentIndex());
    settings.setRecentImages(m_recentImages);
    settings.sync();
    Logger::info(QStringLiteral("Settings saved"));
}

void MainWindow::rebuildBootSelectionCombo() {
    int prevData = m_bootSelectionCombo->currentData().toInt();

    m_bootSelectionCombo->blockSignals(true);
    m_bootSelectionCombo->clear();

    m_bootSelectionCombo->addItem(tr("Non bootable"), static_cast<int>(BootType::NonBootable));
    m_bootSelectionCombo->addItem(QStringLiteral("FreeDOS"), static_cast<int>(BootType::FreeDOS));
    m_bootSelectionCombo->addItem(QStringLiteral("MS-DOS"), static_cast<int>(BootType::MSDOS));

    QString imgText = tr("Disk or ISO image (Please select a file)");
    QString selPath = selectedImagePath();
    if (!selPath.isEmpty())
        imgText = QFileInfo(selPath).fileName();
    m_bootSelectionCombo->addItem(imgText, static_cast<int>(BootType::Image));

    int newIdx = m_bootSelectionCombo->findData(prevData);
    if (newIdx >= 0)
        m_bootSelectionCombo->setCurrentIndex(newIdx);
    else
        m_bootSelectionCombo->setCurrentIndex(
            m_bootSelectionCombo->findData(static_cast<int>(BootType::Image)));

    m_bootSelectionCombo->blockSignals(false);
}

void MainWindow::retranslateUi() {
    setWindowTitle(tr("Rufus %1").arg(QApplication::applicationVersion()));

    if (m_deviceLabel)
        m_deviceLabel->setText(tr("Device"));
    if (m_targetSystemLabel)
        m_targetSystemLabel->setText(tr("Target system"));
    if (m_driveHeader)
        m_driveHeader->setText(tr("Drive Properties"));
    if (m_formatHeader)
        m_formatHeader->setText(tr("Format Options"));
    if (m_statusHeader)
        m_statusHeader->setText(tr("Status"));

    m_deviceCombo->setToolTip(tr("Select the USB drive to write to.\n"
        "Only removable USB drives are listed by default. Press Ctrl+F or check\n"
        "'List USB Hard Drives' to show fixed drives."));
    m_refreshBtn->setToolTip(tr("Refresh devices (F5)"));

    if (m_bootSelectionLabel)
        m_bootSelectionLabel->setText(tr("Boot selection"));

    m_labelEdit->setPlaceholderText(tr("None"));

    m_langBtn->setToolTip(tr("Select interface language"));
    m_aboutBtn->setToolTip(tr("About Rufus"));
    m_logBtn->setToolTip(tr("Open log window"));

    m_startBtn->setText(m_isRunning ? tr("CANCELAR") : tr("START"));
    m_closeBtn->setText(tr("CLOSE"));

    // The progress bar shows the (translated) idle hint — "SELECCIONE
    // IMAGEN", "PREPARADO" — or keeps "PREPARADO" at 100% when a previous
    // operation finished.
    if (m_progressBar && !m_isRunning) {
        if (m_progressBar->value() >= 100) {
            m_progressBar->setFormat(tr("PREPARED"));
        } else {
            updateIdleProgressBarText();
        }
    }

    if (m_partitionSchemeLabel)
        m_partitionSchemeLabel->setText(tr("Partition scheme"));
    if (m_fileSystemLabel)
        m_fileSystemLabel->setText(tr("File system"));
    if (m_clusterSizeLabel)
        m_clusterSizeLabel->setText(tr("Cluster size"));
    if (m_volumeLabelLabel)
        m_volumeLabelLabel->setText(tr("Volume label"));
    if (m_imageOptionLabel)
        m_imageOptionLabel->setText(tr("Image option"));

    if (m_targetSystemCombo) {
        m_targetSystemCombo->blockSignals(true);
        int idx = m_targetSystemCombo->currentIndex();
        m_targetSystemCombo->clear();
        m_targetSystemCombo->addItem(tr("BIOS (or UEFI-CSM)"), 0);
        m_targetSystemCombo->addItem(tr("UEFI (non CSM)"), 1);
        m_targetSystemCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        m_targetSystemCombo->blockSignals(false);
        // The combo was rebuilt: re-apply the scheme-based visibility.
        updateTargetSystemForScheme();
    }

    rebuildBootSelectionCombo();

    if (m_clusterSizeCombo) {
        if (m_clusterSizeCombo->count() > 0) {
            m_clusterSizeCombo->blockSignals(true);
            m_clusterSizeCombo->setItemText(0, tr("Default"));
            m_clusterSizeCombo->blockSignals(false);
        }
    }

    bool advVisible = m_quickFormatCheck && m_quickFormatCheck->isVisible();
    m_advancedFormatToggle->setText(advVisible ? tr("Hide advanced format options")
                                                : tr("Show advanced format options"));
    m_advancedFormatToggle->setIcon(style()->standardIcon(
        advVisible ? QStyle::SP_ArrowDown : QStyle::SP_ArrowRight));
    bool driveAdvVisible = m_listUsbHddCheck && m_listUsbHddCheck->isVisible();
    m_advancedDriveToggle->setText(driveAdvVisible ? tr("Hide advanced drive properties")
                                                    : tr("Show advanced drive properties"));
    m_advancedDriveToggle->setIcon(style()->standardIcon(
        driveAdvVisible ? QStyle::SP_ArrowDown : QStyle::SP_ArrowRight));

    m_quickFormatCheck->setText(tr("Quick format"));
    m_extendedLabelCheck->setText(tr("Create extended label and icon files"));
    m_badBlocksCheck->setText(tr("Check device for bad blocks"));

    if (m_imageOptionCombo && m_imageOptionCombo->count() == 2) {
        m_imageOptionCombo->blockSignals(true);
        m_imageOptionCombo->setItemText(0, tr("Standard Windows installation"));
        m_imageOptionCombo->setItemText(1, tr("Windows To Go"));
        m_imageOptionCombo->blockSignals(false);
    }

    if (m_nbPassesCombo && m_nbPassesCombo->count() == 5) {
        m_nbPassesCombo->blockSignals(true);
        m_nbPassesCombo->setItemText(0, tr("1 pass (default)"));
        m_nbPassesCombo->setItemText(1, tr("2 passes (SLC pattern)"));
        m_nbPassesCombo->setItemText(2, tr("3 passes (MLC pattern)"));
        m_nbPassesCombo->setItemText(3, tr("4 passes (TLC pattern)"));
        m_nbPassesCombo->setItemText(4, tr("5 passes (TLC pattern)"));
        m_nbPassesCombo->blockSignals(false);
    }

    if (m_csmHelpLabel)
        m_csmHelpLabel->setToolTip(tr("Click for information about UEFI-CSM (Compatibility Support Module)"));

    if (m_verifyWriteCheck)
        m_verifyWriteCheck->setText(tr("Verify written data (read back and hash)"));

    if (m_bootloaderLabel)
        m_bootloaderLabel->setText(tr("Bootloader"));

    if (m_bootloaderCombo) {
        m_bootloaderCombo->blockSignals(true);
        int blIdx = m_bootloaderCombo->currentIndex();
        m_bootloaderCombo->setItemText(0, tr("None"));
        m_bootloaderCombo->blockSignals(false);
        m_bootloaderCombo->setCurrentIndex(blIdx);
    }

    if (m_listUsbHddCheck)
        m_listUsbHddCheck->setText(tr("List USB Hard Drives"));
    if (m_oldBiosFixCheck)
        m_oldBiosFixCheck->setText(tr("Add fixes for old BIOSes (extra partition, align, etc.)"));
    if (m_uefiMediaCheck)
        m_uefiMediaCheck->setText(tr("Enable runtime UEFI media validation"));

    if (m_espCheck)
        m_espCheck->setText(tr("EFI System Partition (ESP)"));
    if (m_uefiNtfsCheck)
        m_uefiNtfsCheck->setText(tr("UEFI:NTFS"));

    m_saveBtn->setToolTip(tr("Save current settings to INI file"));
    m_hashBtn->setToolTip(tr("Compute MD5, SHA-1 and SHA-256 hashes for the selected image"));
    m_selectBtn->setText(tr("SELECT"));
    m_selectBtn->setToolTip(tr("Select a disk image (ISO, IMG, VHD, etc.)"));

    updateDeviceCountStatus();

    // Tooltips that are only set once in setupUi() must be retranslated too.
    m_bootSelectionCombo->setToolTip(tr("Select the type of bootable USB to create.\n"
        "• Non bootable: Just format the drive\n• Disk or ISO image: Create from an ISO/IMG file\n"
        "• FreeDOS: Create a FreeDOS bootable drive\n• MS-DOS: Create an MS-DOS bootable drive"));
    m_imageOptionCombo->setToolTip(tr("Image option:\n"
        "• Standard: Regular Windows installation\n• Windows To Go: Run Windows from USB"));
    m_schemeCombo->setToolTip(tr("Partition scheme:\n"
        "• MBR: Master Boot Record (compatible, BIOS + UEFI-CSM)\n"
        "• GPT: GUID Partition Table (modern, native UEFI)"));
    m_targetSystemCombo->setToolTip(tr("Target system type:\n"
        "• BIOS/UEFI-CSM: For legacy BIOS or UEFI in CSM mode\n"
        "• UEFI (non CSM): For native UEFI boot"));
    m_listUsbHddCheck->setToolTip(tr("Also list USB hard drives (not just removable flash drives)"));
    m_oldBiosFixCheck->setToolTip(tr("Add an extra alignment partition and other fixes for old BIOS"));
    m_uefiMediaCheck->setToolTip(tr("Validate UEFI boot media at runtime (may affect compatibility)"));
    m_labelEdit->setToolTip(tr("Volume label (up to 11 characters for FAT/FAT32,\n"
        "up to 32 characters for NTFS/exFAT)"));
    m_fsCombo->setToolTip(tr("File system type for the USB drive.\n"
        "FAT32 is recommended for maximum compatibility.\nNTFS is required for files larger than 4GB.\n"
        "exFAT is good for large files without NTFS overhead.\next2/3/4 are Linux native filesystems."));
    m_clusterSizeCombo->setToolTip(tr("Allocation unit size. Default is recommended."));
    m_quickFormatCheck->setToolTip(tr("Quick format (just writes filesystem structures).\n"
        "Uncheck for full format (wipes all data)"));
    m_extendedLabelCheck->setToolTip(tr("Creates autorun.inf and icon files for the drive"));
    m_badBlocksCheck->setToolTip(tr("Scan the device for bad blocks before writing.\n"
        "This can take a long time on large drives."));
    m_nbPassesCombo->setToolTip(tr("Number of bad block scan passes.\n"
        "Multiple passes with different patterns detect more types of NAND defects."));
    m_progressBar->setToolTip(tr("Operation progress"));
    m_startBtn->setToolTip(tr("Start the USB formatting/writing operation"));
    m_closeBtn->setToolTip(tr("Close Rufus"));

    if (m_deviceCombo->count() == 1 && m_deviceCombo->itemData(0).toString().isEmpty())
        m_deviceCombo->setItemText(0, tr("No removable devices found"));

    updateFixedSize();
}

QString MainWindow::formatSize(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    else if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    else if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    else if (bytes < 1024LL * 1024 * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / (1024 * 1024 * 1024));
    else
        return QStringLiteral("%1 TB").arg(bytes / (1024LL * 1024 * 1024 * 1024));
}
