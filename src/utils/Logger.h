#pragma once

#include <QString>
#include <QFile>
#include <QMutex>
#include <QDateTime>
#include <QStringList>
#include <QtGlobal>

struct LogEntry {
    QString timestamp;
    QString message;
    int level = 0; // 0=info, 1=error, 2=warn
};

class Logger {
public:
    enum Level { Debug, Info, Warning, Error };

    static void init();
    static void shutdown();
    static void write(int level, const QString &message);
    static void info(const QString &message);
    static void warn(const QString &message);
    static void error(const QString &message);
    static void clear();
    static QString logFilePath();
    static QList<LogEntry> allLogs();
    static QString fullText();

    // With --debug, debug-level and Qt-internal messages are echoed to the
    // terminal; without it they are suppressed entirely (they never reach
    // the GUI log or the log file).
    static void setDebugEnabled(bool enabled);

private:
    static QString levelToString(Level level);
    static void messageHandler(QtMsgType type, const QMessageLogContext &context,
                               const QString &msg);
    static QFile m_file;
    static QMutex m_mutex;
    static QList<LogEntry> m_entries;
    static bool m_debugEnabled;
    static QString m_srcPrefix; // source tree prefix (for foreign-message detection)
};

inline void Logger::info(const QString &message) { write(Info, message); }
inline void Logger::warn(const QString &message) { write(Warning, message); }
inline void Logger::error(const QString &message) { write(Error, message); }
