#include "DataCacheManager.h"
#include <QDateTime>
#include <QDebug>

DataCacheManager* DataCacheManager::m_instance = nullptr;

DataCacheManager::DataCacheManager(QObject *parent) : QObject(parent)
{
}

DataCacheManager* DataCacheManager::instance()
{
    if (!m_instance) {
        m_instance = new DataCacheManager;
    }
    return m_instance;
}

int DataCacheManager::physicalIndex(int logicalIndex) const
{
    if (m_frameCache.isEmpty()) {
        return 0;
    }
    return (m_head + logicalIndex) % m_frameCache.size();
}

int DataCacheManager::lowerBoundTimestamp(qint64 timestamp) const
{
    int left = 0;
    int right = m_size;
    while (left < right) {
        const int mid = left + (right - left) / 2;
        const qint64 midTs = m_frameCache[physicalIndex(mid)].timestamp;
        if (midTs < timestamp) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

int DataCacheManager::upperBoundTimestamp(qint64 timestamp) const
{
    int left = 0;
    int right = m_size;
    while (left < right) {
        const int mid = left + (right - left) / 2;
        const qint64 midTs = m_frameCache[physicalIndex(mid)].timestamp;
        if (midTs <= timestamp) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

QVector<FrameData> DataCacheManager::snapshotLocked() const
{
    QVector<FrameData> result;
    result.reserve(m_size);
    for (int index = 0; index < m_size; ++index) {
        result.append(m_frameCache[physicalIndex(index)]);
    }
    return result;
}

void DataCacheManager::addFrame(const FrameData& frame)
{
    QWriteLocker locker(&m_rwLock); // 写锁：独占访问

    if (m_maxCacheSize <= 0) {
        return;
    }

    if (m_frameCache.size() != m_maxCacheSize) {
        QVector<FrameData> oldFrames = snapshotLocked();
        m_frameCache.resize(m_maxCacheSize);
        m_head = 0;
        m_size = 0;
        const int start = qMax(0, oldFrames.size() - m_maxCacheSize);
        for (int i = start; i < oldFrames.size(); ++i) {
            m_frameCache[m_size++] = oldFrames[i];
        }
    }

    if (m_size < m_frameCache.size()) {
        m_frameCache[physicalIndex(m_size)] = frame;
        ++m_size;
    } else {
        m_frameCache[m_head] = frame;
        m_head = (m_head + 1) % m_frameCache.size();
    }

    ++m_totalFrameCount;

    // 清理过期数据
    cleanExpiredFrames();

    // 发送轻量更新通知
    emit dataUpdated();

    // 报警功能已移除
}

FrameData DataCacheManager::getLatestFrame()
{
    QReadLocker locker(&m_rwLock); // 读锁：共享访问

    if (m_size <= 0) {
        FrameData emptyFrame;
        emptyFrame.timestamp = 0;
        emptyFrame.frameId = UINT16_MAX; // 无效帧ID
        return emptyFrame;
    }
    return m_frameCache[physicalIndex(m_size - 1)];
}

QVector<FrameData> DataCacheManager::getLastNFrames(int n)
{
    QReadLocker locker(&m_rwLock);

    if (m_size <= 0 || n <= 0) {
        return {};
    }

    const int begin = qMax(0, m_size - n);
    QVector<FrameData> result;
    result.reserve(m_size - begin);
    for (int i = begin; i < m_size; ++i) {
        result.append(m_frameCache[physicalIndex(i)]);
    }
    return result;
}

QVector<FrameData> DataCacheManager::getFramesInTimeRange(qint64 startTimeMs, qint64 endTimeMs)
{
    QReadLocker locker(&m_rwLock);

    if (m_size <= 0 || endTimeMs < startTimeMs) {
        return {};
    }

    const int begin = lowerBoundTimestamp(startTimeMs);
    const int end = upperBoundTimestamp(endTimeMs);
    if (begin >= end) {
        return {};
    }

    QVector<FrameData> result;
    result.reserve(end - begin);
    for (int i = begin; i < end; ++i) {
        result.append(m_frameCache[physicalIndex(i)]);
    }
    return result;
}

QVector<FrameData> DataCacheManager::getAllFrames()
{
    QReadLocker locker(&m_rwLock);
    return snapshotLocked();
}

int DataCacheManager::getCacheSize()
{
    QReadLocker locker(&m_rwLock);
    return m_size;
}

qint64 DataCacheManager::getTotalFrameCount()
{
    QReadLocker locker(&m_rwLock);
    return m_totalFrameCount;
}

int DataCacheManager::getMaxCacheSize()
{
    QReadLocker locker(&m_rwLock);
    return m_maxCacheSize;
}

void DataCacheManager::setMaxCacheSize(int maxSize)
{
    QWriteLocker locker(&m_rwLock);
    m_maxCacheSize = qMax(1, maxSize);

    QVector<FrameData> oldFrames = snapshotLocked();
    m_frameCache.resize(m_maxCacheSize);
    m_head = 0;
    m_size = 0;
    const int start = qMax(0, oldFrames.size() - m_maxCacheSize);
    for (int i = start; i < oldFrames.size(); ++i) {
        m_frameCache[m_size++] = oldFrames[i];
    }
}

void DataCacheManager::setExpireTimeMs(qint64 expireMs)
{
    QWriteLocker locker(&m_rwLock);
    m_expireTimeMs = expireMs;
}

void DataCacheManager::clearCache()
{
    QWriteLocker locker(&m_rwLock);
    m_frameCache.clear();
    m_head = 0;
    m_size = 0;
    m_totalFrameCount = 0;
}

void DataCacheManager::cleanExpiredFrames()
{
    if (m_expireTimeMs <= 0) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (m_size > 0) {
        const FrameData& oldest = m_frameCache[m_head];
        if (now - oldest.timestamp > m_expireTimeMs) {
            m_head = (m_head + 1) % m_frameCache.size();
            --m_size;
            continue;
        }
        break;
    }
}