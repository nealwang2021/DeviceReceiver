#ifndef APPLOGGER_H
#define APPLOGGER_H

#include <QDateTime>
#include <QDebug>
#include <QMutex>
#include <QObject>
#include <QVector>

#include <memory>

namespace spdlog {
class logger;
}

enum class AppLogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Critical = 3,
    Fatal = 4,
};

struct AppLogRecord
{
    QDateTime timestamp;
    AppLogLevel level{AppLogLevel::Info};
    QString message;
    QString category;
    QString file;
    QString function;
    int line{0};
};

Q_DECLARE_METATYPE(AppLogLevel)
Q_DECLARE_METATYPE(AppLogRecord)

class AppLogger : public QObject
{
    Q_OBJECT
public:
    static AppLogger* instance();

    static QString levelName(AppLogLevel level);
    static QString levelColor(AppLogLevel level);
    static AppLogLevel levelFromQt(QtMsgType type);
    static AppLogLevel levelFromString(const QString& value, AppLogLevel fallback = AppLogLevel::Info);
    static QString levelToConfigString(AppLogLevel level);
    static bool passesLevel(AppLogLevel level, AppLogLevel minimum);

    bool initialize(const QString& logFilePath);
    void installQtMessageHandler();
    void shutdown();

    QString logFilePath() const { return m_logFilePath; }
    bool initialized() const { return m_initialized; }

    QVector<AppLogRecord> recentRecords() const;

    /// Used by CrashHandlerWin callback. Safe before initialize(); falls back to stderr.
    void logCrashLine(const QString& line);

signals:
    void logRecordEmitted(const AppLogRecord& record);

private:
    explicit AppLogger(QObject* parent = nullptr);
    void handleQtMessage(QtMsgType type, const QMessageLogContext& context, const QString& msg);
    void appendRecord(const AppLogRecord& record);

    static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);

    mutable QMutex m_mutex;
    QVector<AppLogRecord> m_recentRecords;
    int m_recentLimit{1500};
    QString m_logFilePath;
    std::shared_ptr<spdlog::logger> m_logger;
    bool m_initialized{false};
};

#endif // APPLOGGER_H
