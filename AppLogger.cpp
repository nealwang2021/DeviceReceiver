#include "AppLogger.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QMutexLocker>

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <vector>

namespace {

constexpr const char* kLoggerName = "DeviceReceiver";

spdlog::level::level_enum toSpdlogLevel(AppLogLevel level)
{
    switch (level) {
    case AppLogLevel::Debug:
        return spdlog::level::debug;
    case AppLogLevel::Info:
        return spdlog::level::info;
    case AppLogLevel::Warning:
        return spdlog::level::warn;
    case AppLogLevel::Critical:
        return spdlog::level::err;
    case AppLogLevel::Fatal:
        return spdlog::level::critical;
    }
    return spdlog::level::info;
}

std::string toUtf8StdString(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

QString contextSuffix(const QMessageLogContext& context)
{
    QStringList parts;
    if (context.category && context.category[0] != '\0') {
        parts << QStringLiteral("category=%1").arg(QString::fromUtf8(context.category));
    }
    if (context.file && context.file[0] != '\0') {
        QString file = QString::fromUtf8(context.file);
        const int slash = qMax(file.lastIndexOf(QLatin1Char('/')), file.lastIndexOf(QLatin1Char('\\')));
        if (slash >= 0) {
            file = file.mid(slash + 1);
        }
        parts << QStringLiteral("file=%1:%2").arg(file).arg(context.line);
    }
    if (parts.isEmpty()) {
        return QString();
    }
    return QStringLiteral(" (%1)").arg(parts.join(QStringLiteral(", ")));
}

} // namespace

AppLogger::AppLogger(QObject* parent)
    : QObject(parent)
{
}

AppLogger* AppLogger::instance()
{
    static AppLogger* logger = nullptr;
    if (!logger) {
        logger = new AppLogger(QCoreApplication::instance());
    }
    return logger;
}

QString AppLogger::levelName(AppLogLevel level)
{
    switch (level) {
    case AppLogLevel::Debug:
        return QStringLiteral("DEBUG");
    case AppLogLevel::Info:
        return QStringLiteral("INFO");
    case AppLogLevel::Warning:
        return QStringLiteral("WARNING");
    case AppLogLevel::Critical:
        return QStringLiteral("CRITICAL");
    case AppLogLevel::Fatal:
        return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

QString AppLogger::levelColor(AppLogLevel level)
{
    switch (level) {
    case AppLogLevel::Debug:
        return QStringLiteral("#8a8f98");
    case AppLogLevel::Info:
        return QString();
    case AppLogLevel::Warning:
        return QStringLiteral("#d19000");
    case AppLogLevel::Critical:
    case AppLogLevel::Fatal:
        return QStringLiteral("red");
    }
    return QString();
}

AppLogLevel AppLogger::levelFromQt(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return AppLogLevel::Debug;
    case QtInfoMsg:
        return AppLogLevel::Info;
    case QtWarningMsg:
        return AppLogLevel::Warning;
    case QtCriticalMsg:
        return AppLogLevel::Critical;
    case QtFatalMsg:
        return AppLogLevel::Fatal;
    }
    return AppLogLevel::Info;
}

AppLogLevel AppLogger::levelFromString(const QString& value, AppLogLevel fallback)
{
    const QString v = value.trimmed().toUpper();
    if (v == QStringLiteral("DEBUG")) {
        return AppLogLevel::Debug;
    }
    if (v == QStringLiteral("INFO")) {
        return AppLogLevel::Info;
    }
    if (v == QStringLiteral("WARNING") || v == QStringLiteral("WARN")) {
        return AppLogLevel::Warning;
    }
    if (v == QStringLiteral("CRITICAL") || v == QStringLiteral("ERROR")) {
        return AppLogLevel::Critical;
    }
    if (v == QStringLiteral("FATAL")) {
        return AppLogLevel::Fatal;
    }
    return fallback;
}

QString AppLogger::levelToConfigString(AppLogLevel level)
{
    return levelName(level);
}

bool AppLogger::passesLevel(AppLogLevel level, AppLogLevel minimum)
{
    return static_cast<int>(level) >= static_cast<int>(minimum);
}

bool AppLogger::initialize(const QString& logFilePath)
{
    if (m_initialized) {
        return true;
    }

    try {
        std::vector<spdlog::sink_ptr> sinks;
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(toUtf8StdString(logFilePath), true);
        auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        sinks.push_back(fileSink);
        sinks.push_back(consoleSink);

        m_logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
        m_logger->set_level(spdlog::level::debug);
        m_logger->flush_on(spdlog::level::trace);
        m_logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
        spdlog::register_logger(m_logger);

        m_logFilePath = logFilePath;
        m_initialized = true;
        return true;
    } catch (const spdlog::spdlog_ex& e) {
        fprintf(stderr, "AppLogger initialize failed: %s\n", e.what());
        return false;
    }
}

void AppLogger::installQtMessageHandler()
{
    qRegisterMetaType<AppLogLevel>("AppLogLevel");
    qRegisterMetaType<AppLogRecord>("AppLogRecord");
    qInstallMessageHandler(&AppLogger::qtMessageHandler);
}

void AppLogger::shutdown()
{
    qInstallMessageHandler(nullptr);
    if (m_logger) {
        m_logger->flush();
        spdlog::drop(kLoggerName);
        m_logger.reset();
    }
    m_initialized = false;
}

QVector<AppLogRecord> AppLogger::recentRecords() const
{
    QMutexLocker locker(&m_mutex);
    return m_recentRecords;
}

void AppLogger::logCrashLine(const QString& line)
{
    if (!m_initialized || !m_logger) {
        fprintf(stderr, "%s\n", line.toUtf8().constData());
        return;
    }

    m_logger->critical(toUtf8StdString(QStringLiteral("[CrashHandler] %1").arg(line)));
    m_logger->flush();
}

void AppLogger::handleQtMessage(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    const AppLogLevel level = levelFromQt(type);
    const QDateTime now = QDateTime::currentDateTime();
    const QString decoratedMessage = msg + contextSuffix(context);

    AppLogRecord record;
    record.timestamp = now;
    record.level = level;
    record.message = decoratedMessage;
    record.category = context.category ? QString::fromUtf8(context.category) : QString();
    record.file = context.file ? QString::fromUtf8(context.file) : QString();
    record.function = context.function ? QString::fromUtf8(context.function) : QString();
    record.line = context.line;

    if (m_logger) {
        m_logger->log(toSpdlogLevel(level), "{}", toUtf8StdString(decoratedMessage));
        if (level >= AppLogLevel::Warning) {
            m_logger->flush();
        }
    } else {
        const QString fallback = QStringLiteral("%1 [%2] %3")
            .arg(now.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")),
                 levelName(level),
                 decoratedMessage);
        fprintf(stderr, "%s\n", fallback.toLocal8Bit().constData());
    }

    appendRecord(record);
}

void AppLogger::appendRecord(const AppLogRecord& record)
{
    {
        QMutexLocker locker(&m_mutex);
        m_recentRecords.append(record);
        while (m_recentRecords.size() > m_recentLimit) {
            m_recentRecords.removeFirst();
        }
    }

    emit logRecordEmitted(record);
}

void AppLogger::qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    AppLogger::instance()->handleQtMessage(type, context, msg);
}
