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
static_assert(kSqlAlignedChannelCount == SqlHistoryQuery::kAlignedChannelCount,
              "kAlignedChannelCount mismatch");
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
    qint64 gMin = std::numeric_limits<qint64>::max();
    qint64 gMax = std::numeric_limits<qint64>::min();
    // 同时检查 aligned_frames 和 multifreq_frames
    const QStringList tables = {QStringLiteral("aligned_frames"), QStringLiteral("multifreq_frames")};
    for (const QString& table : tables) {
        QSqlQuery tq(db);
        if (tq.exec(QStringLiteral("SELECT MIN(timestamp_unix_ms), MAX(timestamp_unix_ms) FROM %1").arg(table))
            && tq.next()) {
            const QVariant vMin = tq.value(0);
            const QVariant vMax = tq.value(1);
            if (!vMin.isNull() && !vMax.isNull()) {
                gMin = qMin(gMin, vMin.toLongLong());
                gMax = qMax(gMax, vMax.toLongLong());
            }
        }
    }
    if (gMin > gMax) {
        return false;
    }
    minTimestampMs = gMin;
    maxTimestampMs = gMax;
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

qint64 SqlHistoryQuery::estimateRowCount(qint64 startMs, qint64 endMs) const
{
    if (!m_isOpen || endMs < startMs) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) {
        return 0;
    }

    constexpr qint64 kOneHourMs = 60LL * 60LL * 1000LL;
    const qint64 spanMs = endMs - startMs;

    if (spanMs <= kOneHourMs) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM aligned_frames "
            "WHERE timestamp_unix_ms BETWEEN :start AND :end"));
        q.bindValue(QStringLiteral(":start"), startMs);
        q.bindValue(QStringLiteral(":end"), endMs);
        if (!q.exec() || !q.next()) {
            qWarning() << "SqlHistoryQuery::estimateRowCount COUNT failed" << q.lastError();
            return 0;
        }
        return q.value(0).toLongLong();
    }

    // 长时段：用经验帧率（100 fps）粗估，避免全表扫描。
    constexpr qint64 kAssumedFps = 100;
    const qint64 spanSec = (spanMs + 999) / 1000;
    return spanSec * kAssumedFps;
}

bool SqlHistoryQuery::fetchRawChunk(qint64 startMs,
                                    qint64 endMs,
                                    qint64 lastTimestampMs,
                                    qint64 lastRowId,
                                    int chunkSize,
                                    QVector<AlignedFrameRow>* outRows,
                                    QString* errorMessage) const
{
    if (!outRows) {
        if (errorMessage) *errorMessage = QStringLiteral("outRows 为空");
        return false;
    }
    outRows->clear();

    if (!m_isOpen) {
        if (errorMessage) *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }
    if (chunkSize <= 0) {
        chunkSize = 8192;
    }
    if (endMs < startMs) {
        return true; // 空结果
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) {
        if (errorMessage) *errorMessage = QStringLiteral("数据库连接无效");
        return false;
    }

    // 组装 SELECT 列：基础字段 + 40 × (amp,phase,x,y,source_channel)
    QString sql = QStringLiteral(
        "SELECT id, timestamp_unix_ms, frame_sequence, detect_mode, cell_count, source_tag");
    for (int i = 0; i < kSqlAlignedChannelCount; ++i) {
        const QString prefix = QStringLiteral("pos%1").arg(i, 2, 10, QLatin1Char('0'));
        sql += QStringLiteral(", %1_amp, %1_phase, %1_x, %1_y, %1_source_channel").arg(prefix);
    }
    // (timestamp, id) 元组游标：避免 frame_sequence 重复时分页歧义，同时规避 OFFSET 的 O(N^2)
    sql += QStringLiteral(
        " FROM aligned_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "AND (timestamp_unix_ms > :lastTs "
        "     OR (timestamp_unix_ms = :lastTsEq AND id > :lastRowId)) "
        "ORDER BY timestamp_unix_ms ASC, id ASC "
        "LIMIT :chunkSize");

    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        const QString msg = QStringLiteral("fetchRawChunk prepare 失败: %1").arg(q.lastError().text());
        qWarning() << msg;
        if (errorMessage) *errorMessage = msg;
        return false;
    }
    q.bindValue(QStringLiteral(":start"), startMs);
    q.bindValue(QStringLiteral(":end"), endMs);
    q.bindValue(QStringLiteral(":lastTs"), lastTimestampMs);
    q.bindValue(QStringLiteral(":lastTsEq"), lastTimestampMs);
    q.bindValue(QStringLiteral(":lastRowId"), lastRowId);
    q.bindValue(QStringLiteral(":chunkSize"), chunkSize);

    if (!q.exec()) {
        const QString msg = QStringLiteral("fetchRawChunk exec 失败: %1").arg(q.lastError().text());
        qWarning() << msg;
        if (errorMessage) *errorMessage = msg;
        return false;
    }

    outRows->reserve(chunkSize);
    while (q.next()) {
        AlignedFrameRow row;
        row.rowId          = q.value(0).toLongLong();
        row.timestampMs    = q.value(1).toLongLong();
        row.frameSequence  = q.value(2).toLongLong();
        row.detectMode     = q.value(3).toInt();
        row.cellCount      = q.value(4).toInt();
        row.sourceTag      = q.value(5).toString();

        // 基础 6 列后，每槽位 5 列，共 40*5=200 列。
        int col = 6;
        for (int i = 0; i < kSqlAlignedChannelCount; ++i) {
            row.amp[i]           = q.value(col + 0);
            row.phase[i]         = q.value(col + 1);
            row.x[i]             = q.value(col + 2);
            row.y[i]             = q.value(col + 3);
            row.sourceChannel[i] = q.value(col + 4);
            col += 5;
        }
        outRows->append(row);
    }
    return true;
}

QVector<SqlHistoryQuery::MultiFreqFrameRow> SqlHistoryQuery::fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs,
    qint64 lastTimestampMs, qint64 lastRowId, int chunkSize)
{
    QVector<MultiFreqFrameRow> rows;
    if (!m_isOpen) return rows;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) return rows;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, timestamp_unix_ms, frame_index, frequency_factor, frequency_hz, "
        "impedance_real, impedance_imag, impedance_magnitude, impedance_phase_deg, "
        "normalized_impedance_real, normalized_impedance_imag, "
        "voltage_magnitude, current_magnitude, valid "
        "FROM multifreq_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "AND (timestamp_unix_ms > :lastTs "
        "OR (timestamp_unix_ms = :lastTsEq AND id > :lastRowId)) "
        "ORDER BY timestamp_unix_ms ASC, id ASC "
        "LIMIT :chunkSize"));
    q.bindValue(":start", startMs);
    q.bindValue(":end", endMs);
    q.bindValue(":lastTs", lastTimestampMs);
    q.bindValue(":lastTsEq", lastTimestampMs);
    q.bindValue(":lastRowId", lastRowId);
    q.bindValue(":chunkSize", chunkSize);

    if (!q.exec()) return rows;

    while (q.next()) {
        MultiFreqFrameRow r;
        r.rowId = q.value(0).toLongLong();
        r.timestampMs = q.value(1).toLongLong();
        r.frameIndex = q.value(2).toLongLong();
        r.frequencyFactor = q.value(3).toInt();
        r.frequencyHz = q.value(4).toDouble();
        r.impedanceReal = q.value(5).toDouble();
        r.impedanceImag = q.value(6).toDouble();
        r.impedanceMagnitude = q.value(7).toDouble();
        r.impedancePhaseDeg = q.value(8).toDouble();
        r.normImpedanceReal = q.value(9).toDouble();
        r.normImpedanceImag = q.value(10).toDouble();
        r.voltageMag = q.value(11).toDouble();
        r.currentMag = q.value(12).toDouble();
        r.valid = q.value(13).toBool();
        rows.append(r);
    }
    return rows;
}

QVector<SqlHistoryQuery::MultiFreqEnvelopeBucket> SqlHistoryQuery::queryMultiFreqOverviewEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs)
{
    QVector<MultiFreqEnvelopeBucket> result;
    if (!m_isOpen || bucketMs <= 0) return result;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT (timestamp_unix_ms / :bucket) * :bucket AS bucket_start, "
        "frequency_factor, "
        "MIN(impedance_real), MAX(impedance_real), "
        "MIN(impedance_imag), MAX(impedance_imag) "
        "FROM multifreq_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "GROUP BY bucket_start, frequency_factor "
        "ORDER BY bucket_start ASC, frequency_factor ASC"));
    q.bindValue(":bucket", bucketMs);
    q.bindValue(":start", startMs);
    q.bindValue(":end", endMs);

    if (!q.exec()) return result;

    while (q.next()) {
        MultiFreqEnvelopeBucket b;
        b.bucketStartMs = q.value(0).toLongLong();
        b.frequencyFactor = q.value(1).toInt();
        b.minImpedanceReal = q.value(2).toDouble();
        b.maxImpedanceReal = q.value(3).toDouble();
        b.minImpedanceImag = q.value(4).toDouble();
        b.maxImpedanceImag = q.value(5).toDouble();
        result.append(b);
    }
    return result;
}

qint64 SqlHistoryQuery::estimateMultiFreqRowCount(qint64 startMs, qint64 endMs)
{
    if (!m_isOpen) return 0;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isValid() || !db.isOpen()) return 0;

    const qint64 spanSec = (endMs - startMs) / 1000;
    if (spanSec <= 3600) {
        QSqlQuery q(db);
        q.prepare("SELECT COUNT(*) FROM multifreq_frames WHERE timestamp_unix_ms BETWEEN :start AND :end");
        q.bindValue(":start", startMs);
        q.bindValue(":end", endMs);
        if (q.exec() && q.next()) return q.value(0).toLongLong();
    }
    return spanSec * 40; // ~10fps * 4 freqs = 40 rows/sec estimate
}
