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
#include <QVariant>

namespace {
QString buildAlignedFramesInsertSql()
{
    QString sql = QStringLiteral("INSERT INTO aligned_frames(");
    sql += QStringLiteral("frame_sequence,timestamp_unix_ms,cell_count,detect_mode,source_tag,created_at_ms");
    for (int pos = 0; pos < RealtimeSqlRecorder::kSqlAlignedChannelCount; ++pos) {
        const QString prefix = QStringLiteral(",pos%1_amp,pos%1_phase,pos%1_x,pos%1_y,pos%1_source_channel")
                                   .arg(pos, 2, 10, QLatin1Char('0'));
        sql += prefix;
    }
    sql += QStringLiteral(") VALUES(");
    const int totalParams = RealtimeSqlRecorder::alignedFramesBoundParamCount();
    for (int i = 0; i < totalParams; ++i) {
        if (i > 0) {
            sql += QLatin1Char(',');
        }
        sql += QLatin1Char('?');
    }
    sql += QLatin1Char(')');
    return sql;
}
} // namespace

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
        QString error;
        const bool ok = RealtimeSqlRecorder::ensureAlignedFramesSchema(m_db, &error);
        if (!ok) {
            qWarning() << "RealtimeSqlRecorder: ensure schema failed:" << error;
        }
        return ok;
    }

    bool prepareStatements()
    {
        m_insertAlignedFrame = QSqlQuery(m_db);
        const QString sql = RealtimeSqlRecorder::alignedFramesInsertSql();

        if (!m_insertAlignedFrame.prepare(sql)) {
            qWarning() << "RealtimeSqlRecorder: prepare insert aligned_frames failed" << m_insertAlignedFrame.lastError();
            return false;
        }

        return true;
    }

    bool insertFrame(const FrameData& frame)
    {
        m_insertAlignedFrame.bindValue(0, static_cast<qulonglong>(frame.sequence));
        m_insertAlignedFrame.bindValue(1, static_cast<qint64>(frame.timestamp));
        m_insertAlignedFrame.bindValue(2, static_cast<int>(frame.channelCount));
        m_insertAlignedFrame.bindValue(3, static_cast<int>(frame.detectMode));
        m_insertAlignedFrame.bindValue(4, QStringLiteral("device"));
        m_insertAlignedFrame.bindValue(5, QDateTime::currentMSecsSinceEpoch());

        for (int pos = 0; pos < RealtimeSqlRecorder::kSqlAlignedChannelCount; ++pos) {
            const int base = 6 + pos * 5;
            m_insertAlignedFrame.bindValue(base + 0, QVariant());
            m_insertAlignedFrame.bindValue(base + 1, QVariant());
            m_insertAlignedFrame.bindValue(base + 2, QVariant());
            m_insertAlignedFrame.bindValue(base + 3, QVariant());
            m_insertAlignedFrame.bindValue(base + 4, QVariant());
        }

        const int sampleCount = qMax(qMax(frame.channels_amp.size(), frame.channels_phase.size()),
                                     qMax(frame.channels_x.size(), frame.channels_y.size()));
        for (int i = 0; i < sampleCount; ++i) {
            int pos = i;
            if (i < frame.channels_display_index.size()) {
                pos = frame.channels_display_index.at(i);
            }
            if (pos < 0 || pos >= RealtimeSqlRecorder::kSqlAlignedChannelCount) {
                continue;
            }

            const double amp = (i < frame.channels_amp.size()) ? frame.channels_amp.at(i) : qQNaN();
            const double phase = (i < frame.channels_phase.size()) ? frame.channels_phase.at(i) : qQNaN();
            const double x = (i < frame.channels_x.size()) ? frame.channels_x.at(i) : qQNaN();
            const double y = (i < frame.channels_y.size()) ? frame.channels_y.at(i) : qQNaN();
            const int sourceChannel = (i < frame.channels_source_channel.size()) ? frame.channels_source_channel.at(i) : i;

            const int base = 6 + pos * 5;
            m_insertAlignedFrame.bindValue(base + 0, amp);
            m_insertAlignedFrame.bindValue(base + 1, phase);
            m_insertAlignedFrame.bindValue(base + 2, x);
            m_insertAlignedFrame.bindValue(base + 3, y);
            m_insertAlignedFrame.bindValue(base + 4, sourceChannel);
        }

        if (!m_insertAlignedFrame.exec()) {
            qWarning() << "RealtimeSqlRecorder: insert aligned frame failed" << m_insertAlignedFrame.lastError();
            return false;
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
            QSqlQuery delFrames(m_db);
            if (m_db.transaction()) {
                delFrames.prepare(QStringLiteral("DELETE FROM aligned_frames WHERE timestamp_unix_ms < ?"));
                delFrames.addBindValue(cutoff);
                const bool ok = delFrames.exec();
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
                QSqlQuery delFrames(m_db);
                const QString deleteOldFrameIds = QStringLiteral(
                    "SELECT id FROM aligned_frames ORDER BY timestamp_unix_ms ASC LIMIT 2000");

                const bool ok = delFrames.exec(
                    QStringLiteral("DELETE FROM aligned_frames WHERE id IN (%1)").arg(deleteOldFrameIds));

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
    QSqlQuery m_insertAlignedFrame;
    qint64 m_lastPruneMs = 0;
};

bool RealtimeSqlRecorder::ensureAlignedFramesSchema(QSqlDatabase& db, QString* errorMessage)
{
    QSqlQuery q(db);
    const QStringList ddl {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS aligned_frames ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "frame_sequence INTEGER NOT NULL,"
            "timestamp_unix_ms INTEGER NOT NULL,"
            "cell_count INTEGER NOT NULL,"
            "detect_mode INTEGER NOT NULL DEFAULT 0,"
            "source_tag TEXT DEFAULT '',"
            "created_at_ms INTEGER NOT NULL DEFAULT 0,"
            "pos00_amp REAL, pos00_phase REAL, pos00_x REAL, pos00_y REAL, pos00_source_channel INTEGER,"
            "pos01_amp REAL, pos01_phase REAL, pos01_x REAL, pos01_y REAL, pos01_source_channel INTEGER,"
            "pos02_amp REAL, pos02_phase REAL, pos02_x REAL, pos02_y REAL, pos02_source_channel INTEGER,"
            "pos03_amp REAL, pos03_phase REAL, pos03_x REAL, pos03_y REAL, pos03_source_channel INTEGER,"
            "pos04_amp REAL, pos04_phase REAL, pos04_x REAL, pos04_y REAL, pos04_source_channel INTEGER,"
            "pos05_amp REAL, pos05_phase REAL, pos05_x REAL, pos05_y REAL, pos05_source_channel INTEGER,"
            "pos06_amp REAL, pos06_phase REAL, pos06_x REAL, pos06_y REAL, pos06_source_channel INTEGER,"
            "pos07_amp REAL, pos07_phase REAL, pos07_x REAL, pos07_y REAL, pos07_source_channel INTEGER,"
            "pos08_amp REAL, pos08_phase REAL, pos08_x REAL, pos08_y REAL, pos08_source_channel INTEGER,"
            "pos09_amp REAL, pos09_phase REAL, pos09_x REAL, pos09_y REAL, pos09_source_channel INTEGER,"
            "pos10_amp REAL, pos10_phase REAL, pos10_x REAL, pos10_y REAL, pos10_source_channel INTEGER,"
            "pos11_amp REAL, pos11_phase REAL, pos11_x REAL, pos11_y REAL, pos11_source_channel INTEGER,"
            "pos12_amp REAL, pos12_phase REAL, pos12_x REAL, pos12_y REAL, pos12_source_channel INTEGER,"
            "pos13_amp REAL, pos13_phase REAL, pos13_x REAL, pos13_y REAL, pos13_source_channel INTEGER,"
            "pos14_amp REAL, pos14_phase REAL, pos14_x REAL, pos14_y REAL, pos14_source_channel INTEGER,"
            "pos15_amp REAL, pos15_phase REAL, pos15_x REAL, pos15_y REAL, pos15_source_channel INTEGER,"
            "pos16_amp REAL, pos16_phase REAL, pos16_x REAL, pos16_y REAL, pos16_source_channel INTEGER,"
            "pos17_amp REAL, pos17_phase REAL, pos17_x REAL, pos17_y REAL, pos17_source_channel INTEGER,"
            "pos18_amp REAL, pos18_phase REAL, pos18_x REAL, pos18_y REAL, pos18_source_channel INTEGER,"
            "pos19_amp REAL, pos19_phase REAL, pos19_x REAL, pos19_y REAL, pos19_source_channel INTEGER,"
            "pos20_amp REAL, pos20_phase REAL, pos20_x REAL, pos20_y REAL, pos20_source_channel INTEGER,"
            "pos21_amp REAL, pos21_phase REAL, pos21_x REAL, pos21_y REAL, pos21_source_channel INTEGER,"
            "pos22_amp REAL, pos22_phase REAL, pos22_x REAL, pos22_y REAL, pos22_source_channel INTEGER,"
            "pos23_amp REAL, pos23_phase REAL, pos23_x REAL, pos23_y REAL, pos23_source_channel INTEGER,"
            "pos24_amp REAL, pos24_phase REAL, pos24_x REAL, pos24_y REAL, pos24_source_channel INTEGER,"
            "pos25_amp REAL, pos25_phase REAL, pos25_x REAL, pos25_y REAL, pos25_source_channel INTEGER,"
            "pos26_amp REAL, pos26_phase REAL, pos26_x REAL, pos26_y REAL, pos26_source_channel INTEGER,"
            "pos27_amp REAL, pos27_phase REAL, pos27_x REAL, pos27_y REAL, pos27_source_channel INTEGER,"
            "pos28_amp REAL, pos28_phase REAL, pos28_x REAL, pos28_y REAL, pos28_source_channel INTEGER,"
            "pos29_amp REAL, pos29_phase REAL, pos29_x REAL, pos29_y REAL, pos29_source_channel INTEGER,"
            "pos30_amp REAL, pos30_phase REAL, pos30_x REAL, pos30_y REAL, pos30_source_channel INTEGER,"
            "pos31_amp REAL, pos31_phase REAL, pos31_x REAL, pos31_y REAL, pos31_source_channel INTEGER,"
            "pos32_amp REAL, pos32_phase REAL, pos32_x REAL, pos32_y REAL, pos32_source_channel INTEGER,"
            "pos33_amp REAL, pos33_phase REAL, pos33_x REAL, pos33_y REAL, pos33_source_channel INTEGER,"
            "pos34_amp REAL, pos34_phase REAL, pos34_x REAL, pos34_y REAL, pos34_source_channel INTEGER,"
            "pos35_amp REAL, pos35_phase REAL, pos35_x REAL, pos35_y REAL, pos35_source_channel INTEGER,"
            "pos36_amp REAL, pos36_phase REAL, pos36_x REAL, pos36_y REAL, pos36_source_channel INTEGER,"
            "pos37_amp REAL, pos37_phase REAL, pos37_x REAL, pos37_y REAL, pos37_source_channel INTEGER,"
            "pos38_amp REAL, pos38_phase REAL, pos38_x REAL, pos38_y REAL, pos38_source_channel INTEGER,"
            "pos39_amp REAL, pos39_phase REAL, pos39_x REAL, pos39_y REAL, pos39_source_channel INTEGER"
            ")"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_aligned_frames_sequence ON aligned_frames(frame_sequence)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_aligned_frames_timestamp ON aligned_frames(timestamp_unix_ms)")
    };
    for (const QString& sql : ddl) {
        if (!q.exec(sql)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("DDL 执行失败: %1").arg(q.lastError().text());
            }
            return false;
        }
    }
    return true;
}

QString RealtimeSqlRecorder::alignedFramesInsertSql()
{
    return buildAlignedFramesInsertSql();
}

int RealtimeSqlRecorder::alignedFramesBoundParamCount()
{
    return 6 + kSqlAlignedChannelCount * 5;
}

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
