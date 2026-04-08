#ifndef DATACACHEMANAGER_H
#define DATACACHEMANAGER_H

#include <QObject>
#include <QVector>
#include <QReadWriteLock>
#include "FrameData.h"

// 线程安全的实时数据缓存管理器（单例模式）
class DataCacheManager : public QObject
{
    Q_OBJECT
public:
    // 获取全局唯一实例
    static DataCacheManager* instance();

    // ========== 数据写入接口（串口/USB线程调用） ==========
    void addFrame(const FrameData& frame);

    // ========== 数据读取接口（应用层调用） ==========
    FrameData getLatestFrame();                      // 获取最新一帧
    QVector<FrameData> getLastNFrames(int n);        // 获取最近N帧
    QVector<FrameData> getFramesInTimeRange(qint64 startTimeMs, qint64 endTimeMs); // 时间范围帧
    QVector<FrameData> getAllFrames();               // 获取所有帧
    int getCacheSize();                              // 当前缓存帧数
    qint64 getTotalFrameCount();                     // 累计接收帧数
    int getMaxCacheSize();                           // 最大缓存帧数

    // ========== 缓存配置接口 ==========
    void setMaxCacheSize(int maxSize);               // 最大缓存帧数
    void setExpireTimeMs(qint64 expireMs);           // 数据过期时间（毫秒）
    void clearCache();                               // 清空缓存
    void cleanExpiredFrames();                       // 清理过期数据

signals:
    void dataUpdated();                              // 数据更新通知（轻量，无参数）
    void criticalFrameReceived(const FrameData& frame); // 关键报警帧通知

private:
    explicit DataCacheManager(QObject *parent = nullptr);
    static DataCacheManager* m_instance;

    int physicalIndex(int logicalIndex) const;
    int lowerBoundTimestamp(qint64 timestamp) const;
    int upperBoundTimestamp(qint64 timestamp) const;
    QVector<FrameData> snapshotLocked() const;

    QVector<FrameData> m_frameCache;                 // 环形缓存容器
    QReadWriteLock m_rwLock;                         // 读写锁（读共享/写独占）
    int m_maxCacheSize = 10000;                      // 默认最大缓存1万帧
    qint64 m_expireTimeMs = 600000;                  // 默认10分钟过期
    int m_head = 0;                                  // 逻辑起点
    int m_size = 0;                                  // 当前有效帧数
    qint64 m_totalFrameCount = 0;                    // 累计接收帧数
};

#endif // DATACACHEMANAGER_H