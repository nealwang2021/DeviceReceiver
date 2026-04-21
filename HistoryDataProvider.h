#ifndef HISTORYDATAPROVIDER_H
#define HISTORYDATAPROVIDER_H

#include <QObject>
#include <QVector>

#include "SqlHistoryQuery.h"

/**
 * 历史数据聚合层（单例）。
 *
 * 目标：为 UI 层提供「按时间范围 + 分量」取数的唯一入口。
 *  - 总览包络：优先来自 SQL 聚合（全历史，较大时段）
 *  - 单通道/多通道详图：MVP 版直接调用 SQL 聚合；PlotDataHub 内存侧由 ArrayPlotWindow 自行配合
 *
 * 线程模型：MVP 同步调用（桶聚合点数有限，主线程阻塞可接受）；后续可移到 worker 线程。
 */
class HistoryDataProvider : public QObject
{
    Q_OBJECT
public:
    /// 会话实时库（与当前记录器一致）vs 用户打开的离线外部库
    enum class HistorySourceMode {
        SessionRealtime = 0,
        OfflineExternal = 1,
    };
    Q_ENUM(HistorySourceMode)

    static HistoryDataProvider* instance();

    HistorySourceMode sourceMode() const { return m_sourceMode; }
    void setSourceMode(HistorySourceMode mode) { m_sourceMode = mode; }

    /// 当前会话记录器 DB 路径（由 ApplicationController 在启动记录成功后设置）。
    QString sessionDatabasePath() const { return m_sessionDatabasePath; }
    void setSessionDatabasePath(const QString& path) { m_sessionDatabasePath = path; }

    /// 打开离线库并切换到 OfflineExternal。
    bool openOfflineDatabase(const QString& databaseFilePath);

    /// 回到会话实时库（SessionRealtime + openDatabase(sessionPath)）。
    bool reopenSessionDatabase();

    /// 打开/切换当前只读 DB（不改变 sourceMode；由 openOfflineDatabase / reopenSessionDatabase 封装语义）。
    bool openDatabase(const QString& databaseFilePath);
    void closeDatabase();
    bool isDatabaseOpen() const;
    QString currentDatabasePath() const { return m_databasePath; }

    /// 查询全表时间范围；rowCount 非空时会额外 COUNT（较慢）。
    bool queryTimeBounds(qint64& minMs, qint64& maxMs, qint64* rowCount = nullptr) const;

    /// 仅 MIN/MAX，无 COUNT。
    bool queryTimeBoundsFast(qint64& minMs, qint64& maxMs) const;

    /// 行数统计（大表慎用）。
    bool queryAlignedFrameRowCount(qint64* outRowCount) const;

    /// 根据「目标桶数」自动选择 bucketMs（不小于 50ms）。
    static qint64 suggestBucketMs(qint64 startMs, qint64 endMs, int targetBuckets);

    bool queryOverviewEnvelope(qint64 startMs,
                               qint64 endMs,
                               qint64 bucketMs,
                               SqlHistoryQuery::Component component,
                               QVector<SqlHistoryQuery::BucketRow>* outBuckets) const;

    bool queryChannelEnvelope(qint64 startMs,
                              qint64 endMs,
                              qint64 bucketMs,
                              int channelIndex,
                              SqlHistoryQuery::Component component,
                              QVector<SqlHistoryQuery::BucketRow>* outBuckets) const;

signals:
    /// 数据库可用或切换时触发，UI 侧可借此刷新总览。
    void databaseOpened(const QString& path);
    void databaseClosed();

private:
    explicit HistoryDataProvider(QObject* parent = nullptr);
    ~HistoryDataProvider() override;

    SqlHistoryQuery* m_query = nullptr;
    QString m_databasePath;
    QString m_sessionDatabasePath;
    HistorySourceMode m_sourceMode = HistorySourceMode::SessionRealtime;
    static HistoryDataProvider* s_instance;
};

#endif // HISTORYDATAPROVIDER_H
