#include "RealtimeSqlRecorder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

class RealtimeSqlRecorderWorker : public QObject
{
public:
    explicit RealtimeSqlRecorderWorker(RealtimeSqlRecorder* owner)
        : QObject(nullptr)
        , m_owner(owner)
    {
    }

public slots:
    void initialize()
    {
        if (!m_owner) {
            return;
        }

        if (!openDatabase()) {
            qWarning() << "RealtimeSqlRecorder: 打开数据库失败" << m_owner->m_databaseFilePath;
            return;
        }

        m_flushTimer = new QTimer(this);
        m_flushTimer->setInterval(m_owner->m_flushIntervalMs);
        connect(m_flushTimer, &QTimer::timeout, this, &RealtimeSqlRecorderWorker::flushOnce);
        m_flushTimer->start();

        m_lastPruneMs = QDateTime::currentMSecsSinceEpoch();
        qInfo() << "RealtimeSqlRecorder: 启动，db=" << m_owner->m_databaseFilePath;
    }

    void flushOnce()
    {
        if (!m_db.isValid() || !m_db.isOpen() || !m_owner) {
            return;
        }

        const QVector<FrameData> frames = m_owner->takeBatch(m_owner->m_batchSize);
        if (frames.isEmpty()) {
            maybePrune();
            return;
        }

        if (!m_db.transaction()) {
            qWarning() << "RealtimeSqlRecorder: transaction begin failed" << m_db.lastError();
            reportDatabaseDropped(frames, QStringLiteral("transaction begin failed"));
            return;
        }

        bool ok = true;
        for (const FrameData& frame : frames) {
            if (!insertFrame(frame)) {
                ok = false;
                break;
            }
        }

        if (ok) {
            if (!m_db.commit()) {
                qWarning() << "RealtimeSqlRecorder: commit failed" << m_db.lastError();
                m_db.rollback();
                reportDatabaseDropped(frames, QStringLiteral("commit failed"));
            }
        } else {
            m_db.rollback();
            reportDatabaseDropped(frames, QStringLiteral("insert failed / rollback"));
        }

        maybePrune();
    }

    void shutdown()
    {
        if (m_flushTimer) {
            m_flushTimer->stop();
        }

        for (int i = 0; i < 10; ++i) {
            if (!m_owner || !m_owner->hasPendingFrames()) {
                break;
            }
            flushOnce();
        }

        if (m_db.isValid() && m_db.isOpen()) {
            QSqlQuery pragma(m_db);
            pragma.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            m_db.close();
        }
        if (!m_connectionName.isEmpty()) {
            QSqlDatabase::removeDatabase(m_connectionName);
            m_connectionName.clear();
        }
    }

private:
    void reportDatabaseDropped(const QVector<FrameData>& frames, const QString& reason)
    {
        if (!m_owner || frames.isEmpty()) {
            return;
        }

        const int frameCount = frames.size();
        const quint64 firstSeq = frames.first().sequence;
        const quint64 lastSeq = frames.last().sequence;
        const quint32 firstTs = frames.first().timestamp;
        const quint32 lastTs = frames.last().timestamp;

        const quint64 dbDropped = m_owner->m_droppedByDatabase.fetch_add(
                                   static_cast<quint64>(frameCount),
                                   std::memory_order_relaxed)
            + static_cast<quint64>(frameCount);
        const quint64 totalDropped = m_owner->m_droppedFrames.fetch_add(
                                     static_cast<quint64>(frameCount),
                                     std::memory_order_relaxed)
            + static_cast<quint64>(frameCount);

        qWarning() << "RealtimeSqlRecorder: 写库失败导致丢帧"
                   << "count=" << frameCount
                   << "seqRange=[" << firstSeq << "," << lastSeq << "]"
                   << "tsRange=[" << firstTs << "," << lastTs << "]"
                   << "dbDroppedTotal=" << dbDropped
                   << "allDroppedTotal=" << totalDropped
                   << "reason=" << reason;

        emit m_owner->dropFrameAlert(
            QStringLiteral("写库失败导致丢帧 count=%1 seqRange=[%2,%3] tsRange=[%4,%5] dbDroppedTotal=%6 allDroppedTotal=%7 reason=%8")
                .arg(frameCount)
                .arg(firstSeq)
                .arg(lastSeq)
                .arg(firstTs)
                .arg(lastTs)
                .arg(dbDropped)
                .arg(totalDropped)
                .arg(reason));
    }

    bool openDatabase()
    {
        if (!m_owner) {
            return false;
        }

        const QFileInfo dbFileInfo(m_owner->m_databaseFilePath);
        QDir dir = dbFileInfo.dir();
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            qWarning() << "RealtimeSqlRecorder: 创建数据库目录失败" << dir.absolutePath();
            return false;
        }

        m_connectionName = QStringLiteral("realtime_sql_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_db.setDatabaseName(m_owner->m_databaseFilePath);
        if (!m_db.open()) {
            qWarning() << "RealtimeSqlRecorder: sqlite open failed" << m_db.lastError();
            return false;
        }

        if (!applyPragmas()) {
            return false;
        }
        if (!ensureSchema()) {
            return false;
        }
        return prepareStatements();
    }

    bool applyPragmas()
    {
        QSqlQuery q(m_db);
        const QStringList pragmas {
            QStringLiteral("PRAGMA journal_mode=WAL"),
            QStringLiteral("PRAGMA synchronous=NORMAL"),
            QStringLiteral("PRAGMA temp_store=MEMORY"),
            QStringLiteral("PRAGMA cache_size=-32768"),
            QStringLiteral("PRAGMA busy_timeout=3000"),
            QStringLiteral("PRAGMA wal_autocheckpoint=1000")
        };
        for (const QString& sql : pragmas) {
            if (!q.exec(sql)) {
                qWarning() << "RealtimeSqlRecorder: pragma failed" << sql << q.lastError();
                return false;
            }
        }
        return true;
    }

    bool ensureSchema()
    {
        QSqlQuery q(m_db);
        const QStringList ddl {
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS frames ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "timestamp_ms INTEGER NOT NULL,"
                "sequence INTEGER NOT NULL,"
                "frame_id INTEGER NOT NULL,"
                "detect_mode INTEGER NOT NULL,"
                "channel_count INTEGER NOT NULL,"
                "has_stage INTEGER NOT NULL,"
                "stage_timestamp_ms INTEGER,"
                "stage_x_mm REAL, stage_y_mm REAL, stage_z_mm REAL,"
                "stage_x_pulse INTEGER, stage_y_pulse INTEGER, stage_z_pulse INTEGER"
                ")"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS frame_samples ("
                "frame_row_id INTEGER NOT NULL,"
                "timestamp_ms INTEGER NOT NULL,"
                "channel_index INTEGER NOT NULL,"
                "source_channel INTEGER NOT NULL,"
                "amp REAL, phase REAL, x REAL, y REAL,"
                "comp0 REAL, comp1 REAL,"
                "PRIMARY KEY(frame_row_id, channel_index)"
                ")"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_frames_timestamp ON frames(timestamp_ms)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_frames_sequence ON frames(sequence)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_samples_channel_time ON frame_samples(channel_index, timestamp_ms)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_samples_time ON frame_samples(timestamp_ms)")
        };
        for (const QString& sql : ddl) {
            if (!q.exec(sql)) {
                qWarning() << "RealtimeSqlRecorder: ddl failed" << sql << q.lastError();
                return false;
            }
        }
        return true;
    }

    bool prepareStatements()
    {
        m_insertFrame = QSqlQuery(m_db);
        if (!m_insertFrame.prepare(
                QStringLiteral(
                    "INSERT INTO frames("
                    "timestamp_ms, sequence, frame_id, detect_mode, channel_count,"
                    "has_stage, stage_timestamp_ms, stage_x_mm, stage_y_mm, stage_z_mm,"
                    "stage_x_pulse, stage_y_pulse, stage_z_pulse"
                    ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"))) {
            qWarning() << "RealtimeSqlRecorder: prepare insert frames failed" << m_insertFrame.lastError();
            return false;
        }

        m_insertSample = QSqlQuery(m_db);
        if (!m_insertSample.prepare(
                QStringLiteral(
                    "INSERT INTO frame_samples("
                    "frame_row_id, timestamp_ms, channel_index, source_channel,"
                    "amp, phase, x, y, comp0, comp1"
                    ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"))) {
            qWarning() << "RealtimeSqlRecorder: prepare insert samples failed" << m_insertSample.lastError();
            return false;
        }

        return true;
    }

    bool insertFrame(const FrameData& frame)
    {
        m_insertFrame.bindValue(0, static_cast<qint64>(frame.timestamp));
        m_insertFrame.bindValue(1, static_cast<qulonglong>(frame.sequence));
        m_insertFrame.bindValue(2, static_cast<int>(frame.frameId));
        m_insertFrame.bindValue(3, static_cast<int>(frame.detectMode));
        m_insertFrame.bindValue(4, static_cast<int>(frame.channelCount));
        m_insertFrame.bindValue(5, frame.hasStagePose ? 1 : 0);
        m_insertFrame.bindValue(6, frame.hasStagePose ? frame.stageTimestampMs : QVariant(QVariant::LongLong));
        m_insertFrame.bindValue(7, frame.hasStagePose ? frame.stageXMm : QVariant(QVariant::Double));
        m_insertFrame.bindValue(8, frame.hasStagePose ? frame.stageYMm : QVariant(QVariant::Double));
        m_insertFrame.bindValue(9, frame.hasStagePose ? frame.stageZMm : QVariant(QVariant::Double));
        m_insertFrame.bindValue(10, frame.hasStagePose ? frame.stageXPulse : QVariant(QVariant::Int));
        m_insertFrame.bindValue(11, frame.hasStagePose ? frame.stageYPulse : QVariant(QVariant::Int));
        m_insertFrame.bindValue(12, frame.hasStagePose ? frame.stageZPulse : QVariant(QVariant::Int));

        if (!m_insertFrame.exec()) {
            qWarning() << "RealtimeSqlRecorder: insert frame failed" << m_insertFrame.lastError();
            return false;
        }

        const qint64 frameRowId = m_insertFrame.lastInsertId().toLongLong();
        if (frameRowId <= 0) {
            qWarning() << "RealtimeSqlRecorder: invalid frame row id" << frameRowId;
            return false;
        }

        const int channelCount = qMax<int>(
            static_cast<int>(frame.channelCount),
            qMax(
                qMax(frame.channels_amp.size(), frame.channels_x.size()),
                qMax(frame.channels_comp0.size(), frame.channels_comp1.size())));

        for (int i = 0; i < channelCount; ++i) {
            const double amp = (i < frame.channels_amp.size()) ? frame.channels_amp.at(i)
                : ((i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN());
            const double phase = (i < frame.channels_phase.size()) ? frame.channels_phase.at(i)
                : ((i < frame.channels_comp1.size()) ? frame.channels_comp1.at(i) : qQNaN());
            const double x = (i < frame.channels_x.size()) ? frame.channels_x.at(i)
                : ((i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN());
            const double y = (i < frame.channels_y.size()) ? frame.channels_y.at(i)
                : ((i < frame.channels_comp1.size()) ? frame.channels_comp1.at(i) : qQNaN());
            const double comp0 = (i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN();
            const double comp1 = (i < frame.channels_comp1.size()) ? frame.channels_comp1.at(i) : qQNaN();

            m_insertSample.bindValue(0, frameRowId);
            m_insertSample.bindValue(1, static_cast<qint64>(frame.timestamp));
            m_insertSample.bindValue(2, i);
            m_insertSample.bindValue(3, i);
            m_insertSample.bindValue(4, amp);
            m_insertSample.bindValue(5, phase);
            m_insertSample.bindValue(6, x);
            m_insertSample.bindValue(7, y);
            m_insertSample.bindValue(8, comp0);
            m_insertSample.bindValue(9, comp1);

            if (!m_insertSample.exec()) {
                qWarning() << "RealtimeSqlRecorder: insert sample failed" << m_insertSample.lastError();
                return false;
            }
        }

        return true;
    }

    void maybePrune()
    {
        if (!m_owner || !m_db.isOpen()) {
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_lastPruneMs < 30000) {
            return;
        }
        m_lastPruneMs = nowMs;

        const qint64 cutoff = nowMs - m_owner->m_retentionMs;
        if (m_owner->m_retentionMs > 0) {
            QSqlQuery delSamples(m_db);
            QSqlQuery delFrames(m_db);
            if (m_db.transaction()) {
                delSamples.prepare(QStringLiteral("DELETE FROM frame_samples WHERE timestamp_ms < ?"));
                delSamples.addBindValue(cutoff);
                delFrames.prepare(QStringLiteral("DELETE FROM frames WHERE timestamp_ms < ?"));
                delFrames.addBindValue(cutoff);
                const bool ok = delSamples.exec() && delFrames.exec();
                if (ok) {
                    m_db.commit();
                } else {
                    m_db.rollback();
                }
            }
        }

        if (m_owner->m_maxDatabaseBytes > 0) {
            int round = 0;
            while (databaseBytes() > m_owner->m_maxDatabaseBytes && round < 20) {
                ++round;
                if (!m_db.transaction()) {
                    break;
                }
                QSqlQuery delSamples(m_db);
                QSqlQuery delFrames(m_db);
                const QString deleteOldFrameIds = QStringLiteral(
                    "SELECT id FROM frames ORDER BY timestamp_ms ASC LIMIT 2000");

                const bool ok = delSamples.exec(
                    QStringLiteral("DELETE FROM frame_samples WHERE frame_row_id IN (%1)").arg(deleteOldFrameIds))
                    && delFrames.exec(
                        QStringLiteral("DELETE FROM frames WHERE id IN (%1)").arg(deleteOldFrameIds));

                if (ok) {
                    m_db.commit();
                } else {
                    m_db.rollback();
                    break;
                }
            }

            QSqlQuery checkpoint(m_db);
            checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)"));
        }
    }

    qint64 databaseBytes() const
    {
        if (!m_owner) {
            return 0;
        }
        const QString dbPath = m_owner->m_databaseFilePath;
        const QFileInfo mainInfo(dbPath);
        const QFileInfo walInfo(dbPath + QStringLiteral("-wal"));
        return mainInfo.size() + walInfo.size();
    }

private:
    RealtimeSqlRecorder* m_owner = nullptr;
    QTimer* m_flushTimer = nullptr;
    QSqlDatabase m_db;
    QString m_connectionName;
    QSqlQuery m_insertFrame;
    QSqlQuery m_insertSample;
    qint64 m_lastPruneMs = 0;
};

RealtimeSqlRecorder::RealtimeSqlRecorder(QObject* parent)
    : QObject(parent)
{
}

RealtimeSqlRecorder::~RealtimeSqlRecorder()
{
    stop();
}

bool RealtimeSqlRecorder::start(const QString& databaseFilePath)
{
    if (m_running.load()) {
        return true;
    }

    m_databaseFilePath = databaseFilePath;
    m_worker = new RealtimeSqlRecorderWorker(this);
    m_worker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::started, m_worker, &RealtimeSqlRecorderWorker::initialize);
    connect(this, &RealtimeSqlRecorder::stopWorker, m_worker, &RealtimeSqlRecorderWorker::shutdown, Qt::QueuedConnection);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_workerThread.start();
    m_running.store(true);
    return true;
}

void RealtimeSqlRecorder::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }

    emit stopWorker();
    m_workerThread.quit();
    m_workerThread.wait(5000);
    m_worker = nullptr;

    const quint64 droppedAll = m_droppedFrames.load(std::memory_order_relaxed);
    if (droppedAll > 0) {
        qWarning() << "RealtimeSqlRecorder: 停止时丢帧统计"
                   << "allDropped=" << droppedAll
                   << "queueDropped=" << m_droppedByQueue.load(std::memory_order_relaxed)
                   << "dbDropped=" << m_droppedByDatabase.load(std::memory_order_relaxed);
    }
}

void RealtimeSqlRecorder::enqueueFrame(const FrameData& frame)
{
    if (!m_running.load(std::memory_order_relaxed)) {
        return;
    }

    QMutexLocker locker(&m_queueMutex);
    int droppedNow = 0;
    while (m_queue.size() >= m_maxQueueSize) {
        m_queue.dequeue();
        ++droppedNow;
    }
    m_queue.enqueue(frame);

    if (droppedNow > 0) {
        const quint64 queueDroppedTotal = m_droppedByQueue.fetch_add(
                                            static_cast<quint64>(droppedNow),
                                            std::memory_order_relaxed)
            + static_cast<quint64>(droppedNow);
        const quint64 allDroppedTotal = m_droppedFrames.fetch_add(
                                          static_cast<quint64>(droppedNow),
                                          std::memory_order_relaxed)
            + static_cast<quint64>(droppedNow);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        qint64 lastMs = m_lastQueueDropLogMs.load(std::memory_order_relaxed);
        const bool shouldLog = (nowMs - lastMs >= 1000) || (droppedNow >= 100) || (queueDroppedTotal % 1000 == 0);
        if (shouldLog
            && m_lastQueueDropLogMs.compare_exchange_strong(lastMs, nowMs, std::memory_order_relaxed)) {
            qWarning() << "RealtimeSqlRecorder: 队列溢出丢帧"
                       << "droppedNow=" << droppedNow
                       << "queueDroppedTotal=" << queueDroppedTotal
                       << "allDroppedTotal=" << allDroppedTotal
                       << "queueSize=" << m_queue.size()
                       << "maxQueueSize=" << m_maxQueueSize;

            emit dropFrameAlert(
                QStringLiteral("队列溢出丢帧 droppedNow=%1 queueDroppedTotal=%2 allDroppedTotal=%3 queueSize=%4 maxQueueSize=%5")
                    .arg(droppedNow)
                    .arg(queueDroppedTotal)
                    .arg(allDroppedTotal)
                    .arg(m_queue.size())
                    .arg(m_maxQueueSize));
        }
    }
}

QVector<FrameData> RealtimeSqlRecorder::takeBatch(int maxBatchSize)
{
    QVector<FrameData> batch;
    batch.reserve(maxBatchSize);

    QMutexLocker locker(&m_queueMutex);
    const int count = qMin(maxBatchSize, m_queue.size());
    for (int i = 0; i < count; ++i) {
        batch.append(m_queue.dequeue());
    }
    return batch;
}

bool RealtimeSqlRecorder::hasPendingFrames() const
{
    QMutexLocker locker(&m_queueMutex);
    return !m_queue.isEmpty();
}
