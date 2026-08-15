#include "LogDialog.h"
#include "utils/Logger.h"
#include "utils/Localization.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QCloseEvent>
#include <QStyle>
#include <QFileDialog>
#include <QMessageBox>
#include "utils/MsgBox.h"
#include <QTextStream>
#include <QFile>
#include <QSysInfo>
#include <QLocale>
#include <QProcess>
#include <QScrollBar>

LogDialog::LogDialog(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(tr("Log"));
    setMinimumSize(360, 200);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);
    layout->setContentsMargins(8, 8, 8, 8);

    m_textEdit = new QTextEdit;
    m_textEdit->setReadOnly(true);
    m_textEdit->document()->setMaximumBlockCount(5000);
    updateLogColors();
    layout->addWidget(m_textEdit, 1);

    auto *btnLayout = new QHBoxLayout;

    m_detachBtn = new QToolButton;
    m_detachBtn->setAutoRaise(true);
    m_detachBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
    m_detachBtn->setIconSize(QSize(16, 16));
    m_detachBtn->setFixedSize(24, 24);
    m_detachBtn->setToolTip(tr("Detach from main window"));
    btnLayout->addWidget(m_detachBtn);
    btnLayout->addStretch();

    m_clearBtn = new QPushButton(tr("Clear"));
    m_clearBtn->setFixedWidth(80);
    btnLayout->addWidget(m_clearBtn);

    m_saveBtn = new QPushButton(tr("Save"));
    m_saveBtn->setFixedWidth(80);
    btnLayout->addWidget(m_saveBtn);
    layout->addLayout(btnLayout);

    connect(m_clearBtn, &QPushButton::clicked, this, &LogDialog::onClear);
    connect(m_saveBtn, &QPushButton::clicked, this, &LogDialog::onSave);
    connect(m_detachBtn, &QToolButton::clicked, this, &LogDialog::detachClicked);

    // Build header once and cache it
    m_headerText = buildHeader();
    // Show the header immediately when the log is opened.
    m_textEdit->setPlainText(m_headerText);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(500);
    connect(m_refreshTimer, &QTimer::timeout, this, &LogDialog::onRefresh);
    m_refreshTimer->start();
}

void LogDialog::setDetached(bool detached) {
    m_detached = detached;
    m_detachBtn->setToolTip(detached ? tr("Attach to main window")
                                     : tr("Detach from main window"));
    if (detached)
        setWindowTitle(tr("Log"));
}

void LogDialog::updateLogColors() {
    QPalette pal = palette();
    QColor bg = pal.color(QPalette::Base);
    QColor fg = pal.color(QPalette::Text);
    QColor border = pal.color(QPalette::Mid);
    m_textEdit->setStyleSheet(
        QStringLiteral("QTextEdit { background-color: %1; color: %2; "
                       "font-family: 'Monospace', 'Courier New', monospace; font-size: 11px; "
                       "border: 1px solid %3; }")
            .arg(bg.name(), fg.name(), border.name()));
}

void LogDialog::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        m_clearBtn->setText(tr("Clear"));
        m_saveBtn->setText(tr("Save"));
        setWindowTitle(tr("Log"));
        m_detachBtn->setToolTip(m_detached ? tr("Attach to main window")
                                           : tr("Detach from main window"));
    }
    if (event->type() == QEvent::PaletteChange)
        updateLogColors();
    QWidget::changeEvent(event);
}

void LogDialog::closeEvent(QCloseEvent *event) {
    // Closing the detached window re-attaches the panel to the main window.
    // Deferred: close() would hide the widget again right after this event,
    // so the re-attach (which shows it) must run after the close completes.
    if (m_detached)
        QTimer::singleShot(0, this, &LogDialog::detachClicked);
    event->accept();
}

QString LogDialog::buildHeader() {
    QStringList header;
    QSysInfo sysInfo;
    QString kernelVer = sysInfo.kernelVersion();
    QString arch = QSysInfo::currentCpuArchitecture();
    header << QStringLiteral("Rufus v%1 (%2, Qt %3)")
        .arg(QApplication::applicationVersion())
        .arg(arch)
        .arg(QStringLiteral(QT_VERSION_STR));
    header << QStringLiteral("System: %1, %2 (%3-bit, Kernel %4)")
        .arg(detectDistroString(), arch,
             QString::number(QSysInfo::WordSize), kernelVer);

    auto checkTool = [](const QString &name) -> QString {
        QProcess p;
        p.start("which", {name});
        if (p.waitForFinished(3000) && p.exitCode() == 0)
            return QStringLiteral("available");
        return QStringLiteral("not found");
    };
    header << QStringLiteral("Syslinux: %1").arg(checkTool(QStringLiteral("syslinux")));
    header << QStringLiteral("GRUB: %1").arg(checkTool(QStringLiteral("grub-install")));
    header << QStringLiteral("7z: %1").arg(checkTool(QStringLiteral("7z")));
    header << QStringLiteral("fuseiso: %1").arg(checkTool(QStringLiteral("fuseiso")));

    header << QStringLiteral("---");

    return header.join(QStringLiteral("\n")) + QStringLiteral("\n");
}

QString LogDialog::detectDistroString() {
    // Prefer /etc/os-release for accurate distro info. Rolling-release
    // distros (Arch, etc.) carry no VERSION_ID, so annotate them as such.
    QFile f(QStringLiteral("/etc/os-release"));
    if (f.open(QIODevice::ReadOnly)) {
        QString name, version;
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        f.close();
        for (const QString &line : lines) {
            if (line.startsWith(QStringLiteral("PRETTY_NAME=")))
                name = line.section('=', 1).remove('"');
            else if (line.startsWith(QStringLiteral("VERSION_ID=")))
                version = line.section('=', 1).remove('"');
        }
        if (!name.isEmpty()) {
            if (!version.isEmpty())
                name += QStringLiteral(" %1").arg(version);
            else if (name.contains(QStringLiteral("Arch"), Qt::CaseInsensitive))
                name += QStringLiteral(" (rolling release)");
            return name;
        }
    }

    // Fallback to QSysInfo (which reports "unknown" for rolling distros).
    QString type = QSysInfo::productType();
    QString ver = QSysInfo::productVersion();
    if (ver.isEmpty() || ver == QLatin1String("unknown"))
        ver = QStringLiteral("(rolling release)");
    return QStringLiteral("%1 %2").arg(type, ver);
}

void LogDialog::onClear() {
    Logger::clear();
    m_textEdit->clear();
    m_textEdit->setPlainText(m_headerText);
    m_lastLogCount = 0;
}

void LogDialog::onSave() {
    QString path = QFileDialog::getSaveFileName(this, tr("Save log"),
        QStringLiteral("rufus.log"),
        QStringLiteral("Log files (*.log);;Text files (*.txt);;All Files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        MsgBox::critical(this, tr("Error"),
            tr("Cannot write to %1").arg(path));
        return;
    }

    QTextStream out(&file);
    out << m_textEdit->toPlainText();
    file.close();

    MsgBox::information(this, tr("Saved"),
        tr("Log saved to %1").arg(path));
}

void LogDialog::onRefresh() {
    QList<LogEntry> entries = Logger::allLogs();
    int count = entries.size();

    if (count > m_lastLogCount) {
        // Save cursor state (so selection survives)
        bool atBottom = (m_textEdit->verticalScrollBar()->value()
                         >= m_textEdit->verticalScrollBar()->maximum() - 2);

        // Only append new entries, colored by level (no timestamps: the
        // GUI log shows plain messages only)
        for (int i = m_lastLogCount; i < count; ++i) {
            const LogEntry &entry = entries[i];
            QString color;
            switch (entry.level) {
            case Logger::Error:   color = QStringLiteral("#ff6b6b"); break;
            case Logger::Warning: color = QStringLiteral("#ffcc66"); break;
            case Logger::Debug:   color = QStringLiteral("#8a8a8a"); break;
            default:              color = QStringLiteral("#d4d4d4"); break;
            }
            QString line = QStringLiteral("<span style=\"color:%1\">%2</span>")
                .arg(color, entry.message.toHtmlEscaped());
            m_textEdit->append(line);
        }

        m_lastLogCount = count;

        // If was at bottom, scroll to bottom; otherwise restore cursor
        if (atBottom)
            m_textEdit->verticalScrollBar()->setValue(
                m_textEdit->verticalScrollBar()->maximum());
    }
}
