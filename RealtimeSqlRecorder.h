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

class RealtimeSqlRecorder : public QObject
{
    Q_OBJECT
public:
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

private:
    QVector<FrameData> takeBatch(int maxBatchSize);
    bool hasPendingFrames() const;

signals:
    void stopWorker();
    void dropFrameAlert(const QString& message);

private:
    friend class RealtimeSqlRecorderWorker;

    static constexpr int kSqlAlignedChannelCount = 40;

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
