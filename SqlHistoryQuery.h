#ifndef SQLHISTORYQUERY_H
#define SQLHISTORYQUERY_H

#include <QObject>
#include <QString>
#include <QVector>

/**
 * 只读 SQLite 历史查询（针对 `aligned_frames` 表）。
 *
 * - 与 `RealtimeSqlRecorder` 使用不同的 QSqlDatabase 连接名，避免并发冲突。
 * - 所有接口均为同步阻塞；建议放到专用线程（由 HistoryDataProvider 统一调度）。
 * - 分量名（component）到列后缀映射：Amplitude->amp, Phase->phase, Real->x, Imag->y。
 */
class SqlHistoryQuery : public QObject
{
    Q_OBJECT
public:
    enum class Component {
        Amplitude = 0,
        Phase = 1,
        Real = 2,
        Imag = 3,
    };

    struct BucketRow {
        qint64 bucketStartMs = 0; // 桶起始时间戳
        double minValue = 0.0;    // 聚合后的最小值
        double maxValue = 0.0;    // 聚合后的最大值
        bool hasData = false;     // 桶内是否有有效数据
    };

    explicit SqlHistoryQuery(QObject* parent = nullptr);
    ~SqlHistoryQuery() override;

    /// 打开指定 sqlite 文件（只读）；可以多次调用切换目标（用于新会话）。
    bool open(const QString& databaseFilePath);
    void close();
    bool isOpen() const { return m_isOpen; }

    /// 查询全表时间范围（仅 MIN/MAX，避免 COUNT 全表扫描）。
    bool queryTimeBoundsFast(qint64& minTimestampMs, qint64& maxTimestampMs) const;

    /// 行数统计（全表 COUNT，大库较慢）；仅在需要提示/统计时调用。
    bool queryAlignedFrameRowCount(qint64* outRowCount) const;

    /// 查询全表时间范围；rowCount 非空时会额外执行 COUNT 查询。
    bool queryTimeBounds(qint64& minTimestampMs, qint64& maxTimestampMs, qint64* rowCount = nullptr) const;

    /**
     * 查询「所有 40 通道 min/max 包络」在给定时间段内按 bucketMs 聚合的结果。
     * 返回 buckets 按时间升序；空桶不返回。
     */
    bool queryOverviewEnvelope(qint64 startMs,
                               qint64 endMs,
                               qint64 bucketMs,
                               Component component,
                               QVector<BucketRow>* outBuckets) const;

    /**
     * 查询「单通道」在给定时间段内按 bucketMs 聚合的 min/max。
     * channelIndex 范围 [0, 39]；超出返回 false。
     */
    bool queryChannelEnvelope(qint64 startMs,
                              qint64 endMs,
                              qint64 bucketMs,
                              int channelIndex,
                              Component component,
                              QVector<BucketRow>* outBuckets) const;

private:
    static QString componentColumnSuffix(Component c);
    static QString channelColumnName(int channelIndex, Component c);

    QString m_connectionName;
    QString m_databasePath;
    bool m_isOpen = false;
};

#endif // SQLHISTORYQUERY_H
