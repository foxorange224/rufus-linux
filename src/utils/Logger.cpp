#include "Logger.h"
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <QCoreApplication>
#include <QDebug>

QFile Logger::m_file;
QMutex Logger::m_mutex;
QList<LogEntry> Logger::m_entries;

void Logger::messageHandler(QtMsgType type, const QMessageLogContext &context,
                            const QString &msg) {
    Q_UNUSED(context);
    Level level;
    switch (type) {
    case QtDebugMsg:   level = Debug;   break;
    case QtInfoMsg:    level = Info;    break;
    case QtWarningMsg: level = Warning; break;
    default:           level = Error;   break;
    }
    write(level, msg);

    // Keep the original terminal output visible
    QTextStream stream(stderr);
    stream << msg << QLatin1Char('\n');
    stream.flush();
}

void Logger::init() {
    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dirPath);
    QString path = dirPath + QStringLiteral("/rufus.log");

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

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

void Logger::write(int level, const QString &message) {
    QMutexLocker locker(&m_mutex);

    QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));

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
