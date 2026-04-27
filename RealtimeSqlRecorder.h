#ifndef REALTIMESQLRECORDER_H
#define REALTIMESQLRECORDER_H

#include <QObject>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QMutex>
#include <QSqlQuery>
#include <atomic>

#include "FrameData.h"

class RealtimeSqlRecorderWorker;
class QSqlDatabase;

class RealtimeSqlRecorder : public QObject
{
    Q_OBJECT
public:
    static constexpr int kSqlAlignedChannelCount = 40;

    explicit RealtimeSqlRecorder(QObject* parent = nullptr);
    ~RealtimeSqlRecorder() override;

    bool start(const QString& databaseFilePath);
    void stop();
    void enqueueFrame(const FrameData& frame);

    /// 当前会话写入的 SQLite 文件路径；未 start 时返回空串。
    QString currentDatabasePath() const { return m_databaseFilePath; }

    quint64 droppedFrameCount() const { return m_droppedFrames.load(); }
    quint64 droppedByQueueCount() const { return m_droppedByQueue.load(); }
    quint64 droppedByDatabaseCount() const { return m_droppedByDatabase.load(); }

    /// 导入模块复用：创建 aligned_frames 表与索引（与记录器写入 schema 保持一致）。
    static bool ensureAlignedFramesSchema(QSqlDatabase& db, QString* errorMessage = nullptr);
    /// 导入模块复用：aligned_frames 的 INSERT 语句（占位符顺序与记录器 bind 完全一致）。
    static QString alignedFramesInsertSql();
    /// 导入模块复用：INSERT 占位符总数（6 + 40*5）。
    static int alignedFramesBoundParamCount();

private:
    QVector<FrameData> takeBatch(int maxBatchSize);
    bool hasPendingFrames() const;

signals:
    void stopWorker();
    void dropFrameAlert(const QString& message);

private:
    friend class RealtimeSqlRecorderWorker;

    mutable QMutex m_queueMutex;
    QQueue<FrameData> m_queue;

    int m_maxQueueSize = 5000;
    int m_batchSize = 500;
    int m_flushIntervalMs = 20;

    qint64 m_retentionMs = 24ll * 60ll * 60ll * 1000ll;
    qint64 m_maxDatabaseBytes = 1024ll * 1024ll * 1024ll;

    QString m_databaseFilePath;
    std::atomic<quint64> m_droppedFrames {0};
    std::atomic<quint64> m_droppedByQueue {0};
    std::atomic<quint64> m_droppedByDatabase {0};
    std::atomic<qint64> m_lastQueueDropLogMs {0};
    std::atomic<bool> m_running {false};

    QThread m_workerThread;
    RealtimeSqlRecorderWorker* m_worker = nullptr;
};

#endif // REALTIMESQLRECORDER_H
