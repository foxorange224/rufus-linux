#include "Logger.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QCoreApplication>
#include <QDebug>
#include <cstdio>

QFile Logger::m_file;
QMutex Logger::m_mutex;
QList<LogEntry> Logger::m_entries;
bool Logger::m_debugEnabled = false;
QString Logger::m_srcPrefix;

void Logger::messageHandler(QtMsgType type, const QMessageLogContext &context,
                            const QString &msg) {
    Level level;
    switch (type) {
    case QtDebugMsg:   level = Debug;   break;
    case QtInfoMsg:    level = Info;    break;
    case QtWarningMsg: level = Warning; break;
    default:           level = Error;   break;
    }

    // QtDebugMsg: only shown in the terminal with --debug, never stored.
    if (level == Debug) {
        if (m_debugEnabled)
            fprintf(stderr, "%s\n", qPrintable(msg));
        return;
    }

    // Messages from Qt internals (e.g. "XDG_RUNTIME_DIR not set...") have
    // a context outside our source tree: they are terminal-only noise with
    // --debug and must not pollute the GUI log.
    const char *file = context.file;
    if (!file || !m_srcPrefix.isEmpty() &&
                     !QString::fromUtf8(file).startsWith(m_srcPrefix)) {
        if (m_debugEnabled)
            fprintf(stderr, "%s\n", qPrintable(msg));
        return;
    }

    write(level, msg);

    // Without --debug only warnings/errors reach the terminal; the full
    // stream appears with --debug.
    if (m_debugEnabled || level >= Warning)
        fprintf(stderr, "%s\n", qPrintable(msg));
}

void Logger::init() {
    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dirPath);
    QString path = dirPath + QStringLiteral("/rufus.log");

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    // Source tree prefix of this file, used to tell our own messages apart
    // from Qt-internal ones in messageHandler().
    m_srcPrefix = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();

    // Route qDebug()/qWarning()/... from the whole application into the log
    qInstallMessageHandler(messageHandler);

    info(QStringLiteral("=== Rufus started === version %1")
         .arg(QCoreApplication::applicationVersion()));
}

void Logger::shutdown() {
    info(QStringLiteral("=== Rufus stopped ==="));
    if (m_file.isOpen())
        m_file.close();
    qInstallMessageHandler(nullptr);
}

void Logger::setDebugEnabled(bool enabled) {
    m_debugEnabled = enabled;
}

void Logger::write(int level, const QString &message) {
    QMutexLocker locker(&m_mutex);

    QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));

    LogEntry entry;
    entry.timestamp = timestamp;
    entry.message = message;
    entry.level = level;
    m_entries.append(entry);

    if (m_entries.size() > 2000)
        m_entries.removeFirst();

    if (m_file.isOpen()) {
        QTextStream stream(&m_file);
        stream << QStringLiteral("[%1] %2: %3\n")
            .arg(timestamp, levelToString(static_cast<Level>(level)), message);
        stream.flush();
    }
}

void Logger::clear() {
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
    // The on-disk log must be emptied too, otherwise the file keeps growing
    // with entries that no longer appear in the dialog.
    if (m_file.isOpen()) {
        m_file.resize(0);
        m_file.flush();
    }
}

QString Logger::logFilePath() {
    return m_file.fileName();
}

QList<LogEntry> Logger::allLogs() {
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

QString Logger::fullText() {
    QMutexLocker locker(&m_mutex);
    QStringList lines;
    for (const LogEntry &e : m_entries)
        lines << e.message;
    return lines.join(QStringLiteral("\n"));
}

QString Logger::levelToString(Level level) {
    switch (level) {
    case Debug:   return QStringLiteral("DEBUG");
    case Info:    return QStringLiteral("INFO");
    case Warning: return QStringLiteral("WARN");
    case Error:   return QStringLiteral("ERROR");
    }
    return {};
}
