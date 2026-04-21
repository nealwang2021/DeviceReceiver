#include "SqlHistoryQuery.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <cmath>

namespace {
constexpr int kSqlAlignedChannelCount = 40;
}

SqlHistoryQuery::SqlHistoryQuery(QObject* parent)
    : QObject(parent)
{
    m_connectionName = QStringLiteral("SqlHistoryQuery_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

SqlHistoryQuery::~SqlHistoryQuery()
{
    close();
}

QString SqlHistoryQuery::componentColumnSuffix(Component c)
{
    switch (c) {
    case Component::Amplitude:
        return QStringLiteral("amp");
    case Component::Phase:
        return QStringLiteral("phase");
    case Component::Real:
        return QStringLiteral("x");
    case Component::Imag:
        return QStringLiteral("y");
    }
    return QStringLiteral("amp");
}

QString SqlHistoryQuery::channelColumnName(int channelIndex, Component c)
{
    return QStringLiteral("pos%1_%2")
        .arg(channelIndex, 2, 10, QLatin1Char('0'))
        .arg(componentColumnSuffix(c));
}

bool SqlHistoryQuery::open(const QString& databaseFilePath)
{
    close();
    if (databaseFilePath.isEmpty()) {
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(databaseFilePath);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=3000"));
    if (!db.open()) {
        qWarning() << "SqlHistoryQuery: 打开只读连接失败" << databaseFilePath << db.lastError();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA query_only=1"));
    pragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));

    m_databasePath = databaseFilePath;
    m_isOpen = true;
    return true;
}

void SqlHistoryQuery::close()
{
    if (m_isOpen) {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName);
        if (db.isValid() && db.isOpen()) {
            db.close();
        }
        // 必须先销毁最后一个连接句柄，再 removeDatabase；否则 Qt 文档明确提示未定义行为。
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_isOpen = false;
        m_databasePath.clear();
    }
}

bool SqlHistoryQuery::queryTimeBoundsFast(qint64& minTimestampMs, qint64& maxTimestampMs) const
{
    if (!m_isOpen) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) {
        return false;
    }
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT MIN(timestamp_unix_ms), MAX(timestamp_unix_ms) FROM aligned_frames"))) {
        qWarning() << "SqlHistoryQuery::queryTimeBoundsFast failed" << q.lastError();
        return false;
    }
    if (!q.next()) {
        return false;
    }
    const QVariant vMin = q.value(0);
    const QVariant vMax = q.value(1);
    if (vMin.isNull() || vMax.isNull()) {
        return false;
    }
    minTimestampMs = vMin.toLongLong();
    maxTimestampMs = vMax.toLongLong();
    return true;
}

bool SqlHistoryQuery::queryAlignedFrameRowCount(qint64* outRowCount) const
{
    if (!outRowCount || !m_isOpen) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) {
        return false;
    }
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM aligned_frames"))) {
        qWarning() << "SqlHistoryQuery::queryAlignedFrameRowCount failed" << q.lastError();
        return false;
    }
    if (!q.next()) {
        return false;
    }
    *outRowCount = q.value(0).toLongLong();
    return true;
}

bool SqlHistoryQuery::queryTimeBounds(qint64& minTimestampMs, qint64& maxTimestampMs, qint64* rowCount) const
{
    if (!queryTimeBoundsFast(minTimestampMs, maxTimestampMs)) {
        return false;
    }
    if (rowCount) {
        return queryAlignedFrameRowCount(rowCount);
    }
    return true;
}

bool SqlHistoryQuery::queryOverviewEnvelope(qint64 startMs,
                                            qint64 endMs,
                                            qint64 bucketMs,
                                            Component component,
                                            QVector<BucketRow>* outBuckets) const
{
    if (!outBuckets || !m_isOpen || bucketMs <= 0 || endMs < startMs) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) {
        return false;
    }

    // 构造：桶起始时间 + 每通道 MIN/MAX 列（共 40 * 2 个表达式）。
    QStringList selectCols;
    selectCols.reserve(1 + kSqlAlignedChannelCount * 2);
    selectCols << QStringLiteral("(timestamp_unix_ms / :bucket) * :bucket AS bucket_start");
    for (int i = 0; i < kSqlAlignedChannelCount; ++i) {
        const QString col = channelColumnName(i, component);
        selectCols << QStringLiteral("MIN(%1)").arg(col);
        selectCols << QStringLiteral("MAX(%1)").arg(col);
    }

    const QString sql = QStringLiteral(
        "SELECT %1 FROM aligned_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "GROUP BY bucket_start "
        "ORDER BY bucket_start ASC")
        .arg(selectCols.join(QStringLiteral(", ")));

    QSqlQuery q(db);
    q.prepare(sql);
    q.bindValue(QStringLiteral(":bucket"), bucketMs);
    q.bindValue(QStringLiteral(":start"), startMs);
    q.bindValue(QStringLiteral(":end"), endMs);
    if (!q.exec()) {
        qWarning() << "SqlHistoryQuery::queryOverviewEnvelope failed" << q.lastError();
        return false;
    }

    outBuckets->clear();
    while (q.next()) {
        BucketRow row;
        row.bucketStartMs = q.value(0).toLongLong();
        bool hasAny = false;
        double localMin = 0.0;
        double localMax = 0.0;
        for (int i = 0; i < kSqlAlignedChannelCount; ++i) {
            const QVariant vMin = q.value(1 + i * 2);
            const QVariant vMax = q.value(1 + i * 2 + 1);
            if (vMin.isNull() || vMax.isNull()) {
                continue;
            }
            const double dMin = vMin.toDouble();
            const double dMax = vMax.toDouble();
            if (!std::isfinite(dMin) || !std::isfinite(dMax)) {
                continue;
            }
            if (!hasAny) {
                localMin = dMin;
                localMax = dMax;
                hasAny = true;
            } else {
                if (dMin < localMin) localMin = dMin;
                if (dMax > localMax) localMax = dMax;
            }
        }
        if (hasAny) {
            row.minValue = localMin;
            row.maxValue = localMax;
            row.hasData = true;
            outBuckets->append(row);
        }
    }
    return true;
}

bool SqlHistoryQuery::queryChannelEnvelope(qint64 startMs,
                                           qint64 endMs,
                                           qint64 bucketMs,
                                           int channelIndex,
                                           Component component,
                                           QVector<BucketRow>* outBuckets) const
{
    if (!outBuckets || !m_isOpen || bucketMs <= 0 || endMs < startMs) {
        return false;
    }
    if (channelIndex < 0 || channelIndex >= kSqlAlignedChannelCount) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) {
        return false;
    }

    const QString col = channelColumnName(channelIndex, component);
    const QString sql = QStringLiteral(
        "SELECT (timestamp_unix_ms / :bucket) * :bucket AS bucket_start, "
        "MIN(%1), MAX(%1) FROM aligned_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "GROUP BY bucket_start ORDER BY bucket_start ASC")
        .arg(col);

    QSqlQuery q(db);
    q.prepare(sql);
    q.bindValue(QStringLiteral(":bucket"), bucketMs);
    q.bindValue(QStringLiteral(":start"), startMs);
    q.bindValue(QStringLiteral(":end"), endMs);
    if (!q.exec()) {
        qWarning() << "SqlHistoryQuery::queryChannelEnvelope failed" << q.lastError();
        return false;
    }

    outBuckets->clear();
    while (q.next()) {
        BucketRow row;
        row.bucketStartMs = q.value(0).toLongLong();
        const QVariant vMin = q.value(1);
        const QVariant vMax = q.value(2);
        if (vMin.isNull() || vMax.isNull()) {
            continue;
        }
        const double dMin = vMin.toDouble();
        const double dMax = vMax.toDouble();
        if (!std::isfinite(dMin) || !std::isfinite(dMax)) {
            continue;
        }
        row.minValue = dMin;
        row.maxValue = dMax;
        row.hasData = true;
        outBuckets->append(row);
    }
    return true;
}
