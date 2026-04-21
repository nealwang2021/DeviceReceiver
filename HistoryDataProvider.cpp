#include "HistoryDataProvider.h"

#include <QDebug>
#include <algorithm>

HistoryDataProvider* HistoryDataProvider::s_instance = nullptr;

HistoryDataProvider* HistoryDataProvider::instance()
{
    if (!s_instance) {
        s_instance = new HistoryDataProvider();
    }
    return s_instance;
}

HistoryDataProvider::HistoryDataProvider(QObject* parent)
    : QObject(parent)
{
    m_query = new SqlHistoryQuery(this);
}

HistoryDataProvider::~HistoryDataProvider()
{
    if (m_query) {
        m_query->close();
    }
}

bool HistoryDataProvider::openDatabase(const QString& databaseFilePath)
{
    if (!m_query) {
        return false;
    }
    if (m_query->isOpen()) {
        m_query->close();
        emit databaseClosed();
    }
    if (databaseFilePath.isEmpty()) {
        return false;
    }
    const bool ok = m_query->open(databaseFilePath);
    if (ok) {
        m_databasePath = databaseFilePath;
        emit databaseOpened(databaseFilePath);
    } else {
        qWarning() << "HistoryDataProvider::openDatabase failed:" << databaseFilePath;
    }
    return ok;
}

void HistoryDataProvider::closeDatabase()
{
    if (m_query && m_query->isOpen()) {
        m_query->close();
        m_databasePath.clear();
        emit databaseClosed();
    }
}

bool HistoryDataProvider::isDatabaseOpen() const
{
    return m_query && m_query->isOpen();
}

bool HistoryDataProvider::openOfflineDatabase(const QString& databaseFilePath)
{
    m_sourceMode = HistorySourceMode::OfflineExternal;
    return openDatabase(databaseFilePath);
}

bool HistoryDataProvider::reopenSessionDatabase()
{
    if (m_sessionDatabasePath.isEmpty()) {
        return false;
    }
    m_sourceMode = HistorySourceMode::SessionRealtime;
    return openDatabase(m_sessionDatabasePath);
}

bool HistoryDataProvider::queryTimeBounds(qint64& minMs, qint64& maxMs, qint64* rowCount) const
{
    if (!isDatabaseOpen()) {
        return false;
    }
    return m_query->queryTimeBounds(minMs, maxMs, rowCount);
}

bool HistoryDataProvider::queryTimeBoundsFast(qint64& minMs, qint64& maxMs) const
{
    if (!isDatabaseOpen()) {
        return false;
    }
    return m_query->queryTimeBoundsFast(minMs, maxMs);
}

bool HistoryDataProvider::queryAlignedFrameRowCount(qint64* outRowCount) const
{
    if (!isDatabaseOpen()) {
        return false;
    }
    return m_query->queryAlignedFrameRowCount(outRowCount);
}

qint64 HistoryDataProvider::suggestBucketMs(qint64 startMs, qint64 endMs, int targetBuckets)
{
    if (endMs <= startMs) {
        return 50;
    }
    if (targetBuckets < 16) {
        targetBuckets = 16;
    }
    const qint64 span = endMs - startMs;
    qint64 bucket = span / targetBuckets;
    if (bucket < 50) bucket = 50;                     // 最小 50ms 桶
    if (bucket > 60LL * 60LL * 1000LL) bucket = 60LL * 60LL * 1000LL; // 最大 1h 桶
    return bucket;
}

bool HistoryDataProvider::queryOverviewEnvelope(qint64 startMs,
                                                qint64 endMs,
                                                qint64 bucketMs,
                                                SqlHistoryQuery::Component component,
                                                QVector<SqlHistoryQuery::BucketRow>* outBuckets) const
{
    if (!outBuckets || !isDatabaseOpen()) {
        return false;
    }
    return m_query->queryOverviewEnvelope(startMs, endMs, bucketMs, component, outBuckets);
}

bool HistoryDataProvider::queryChannelEnvelope(qint64 startMs,
                                               qint64 endMs,
                                               qint64 bucketMs,
                                               int channelIndex,
                                               SqlHistoryQuery::Component component,
                                               QVector<SqlHistoryQuery::BucketRow>* outBuckets) const
{
    if (!outBuckets || !isDatabaseOpen()) {
        return false;
    }
    return m_query->queryChannelEnvelope(startMs, endMs, bucketMs, channelIndex, component, outBuckets);
}
