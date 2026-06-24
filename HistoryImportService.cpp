#include "HistoryImportService.h"

#include "RealtimeSqlRecorder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QByteArray>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTextStream>
#include <QUuid>
#include <QVariant>

#include <cmath>
#include <limits>

#ifdef HAS_HDF5
#include <hdf5.h>
#endif

namespace {
constexpr int kChannelCount = RealtimeSqlRecorder::kSqlAlignedChannelCount;
constexpr int kAlignedValueCount = kChannelCount * 5; // amp/phase/x/y/source_channel

QString buildUniqueConnectionName()
{
    return QStringLiteral("history_import_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

class ScopedSqlConnectionCleanup final
{
public:
    explicit ScopedSqlConnectionCleanup(const QString& connectionName)
        : m_connectionName(connectionName)
    {
    }

    ~ScopedSqlConnectionCleanup()
    {
        if (m_connectionName.isEmpty()) {
            return;
        }
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isValid() && db.isOpen()) {
            db.close();
        }
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }

private:
    QString m_connectionName;
};

bool parseCsvLine(const QString& line, QStringList* outFields)
{
    if (!outFields) {
        return false;
    }
    outFields->clear();

    QString current;
    current.reserve(line.size());
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (inQuotes && (i + 1) < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                current.append(QLatin1Char('"'));
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
            continue;
        }
        if (!inQuotes && ch == QLatin1Char(',')) {
            outFields->append(current);
            current.clear();
            continue;
        }
        current.append(ch);
    }
    outFields->append(current);
    return !inQuotes;
}

QVariant parseDoubleOrNull(const QString& token, bool* ok = nullptr)
{
    const QString t = token.trimmed();
    if (t.isEmpty()) {
        if (ok) *ok = true;
        return QVariant();
    }
    bool localOk = false;
    const double v = t.toDouble(&localOk);
    if (!localOk || !std::isfinite(v)) {
        if (ok) *ok = false;
        return QVariant();
    }
    if (ok) *ok = true;
    return QVariant(v);
}

QVariant parseIntOrNull(const QString& token, bool* ok = nullptr)
{
    const QString t = token.trimmed();
    if (t.isEmpty()) {
        if (ok) *ok = true;
        return QVariant();
    }
    bool localOk = false;
    const int v = t.toInt(&localOk);
    if (!localOk) {
        if (ok) *ok = false;
        return QVariant();
    }
    if (ok) *ok = true;
    return QVariant(v);
}
} // namespace

HistoryImportService::HistoryImportService(QObject* parent)
    : QObject(parent)
{
}

HistoryImportService::~HistoryImportService() = default;

void HistoryImportService::cancel()
{
    m_canceled.storeRelaxed(1);
}

void HistoryImportService::run()
{
    m_canceled.storeRelaxed(0);

    // ---- HDF5 自动检测：探测文件包含哪个设备类型的 group ----
    auto detectHdf5Type = [&]() -> int {
        // 0=Standard, 1=MultiFreq, 2=MagArray, 3=PulseEddy
#ifdef HAS_HDF5
        hid_t probeId = H5Fopen(m_request.sourcePath.toLocal8Bit().constData(),
                                H5F_ACC_RDONLY, H5P_DEFAULT);
        if (probeId < 0) return 0;
        struct ProbeCleanup { hid_t id; ~ProbeCleanup() { if (id>=0) H5Fclose(id); } };
        ProbeCleanup pc{probeId};
        if (H5Lexists(probeId, "/pulse", H5P_DEFAULT) > 0)     return 3;
        if (H5Lexists(probeId, "/magarray", H5P_DEFAULT) > 0)  return 2;
        if (H5Lexists(probeId, "/impedance", H5P_DEFAULT) > 0) return 1;
        return 0;
#else
        return 0;
#endif
    };

    // ---- CSV 自动检测：读取文件头部 # source= 注释 ----
    auto detectCsvType = [&]() -> int {
        // 0=Standard, 1=MultiFreq, 2=MagArray, 3=PulseEddy
        QFile f(m_request.sourcePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
        QTextStream s(&f);
        s.setCodec("UTF-8");
        QString line;
        while (s.readLineInto(&line)) {
            if (line.startsWith(QStringLiteral("# source="))) {
                f.close();
                QString src = line.mid(9).trimmed();
                if (src == QStringLiteral("multifreq_eddy")) return 1;
                if (src == QStringLiteral("mag_array"))      return 2;
                if (src == QStringLiteral("pulse_eddy"))     return 3;
                return 0;
            }
            if (!line.startsWith('#')) break;
        }
        f.close();
        return 0;
    };

    QString error;
    bool ok = false;
    switch (m_request.format) {
    case Format::Csv: {
        const int csvType = detectCsvType();
        switch (csvType) {
        case 1: // MultiFreq
            ok = importMultiFreqCsv(m_request.sourcePath, m_request.targetDbPath);
            if (!ok) error = QStringLiteral("多频涡流 CSV 导入失败");
            break;
        case 2: // MagArray
            ok = importMagArrayCsv(m_request.sourcePath, m_request.targetDbPath);
            if (!ok) error = QStringLiteral("漏磁检测 CSV 导入失败");
            break;
        case 3: // PulseEddy
            ok = importPulseEddyCsv(m_request.sourcePath, m_request.targetDbPath);
            if (!ok) error = QStringLiteral("脉冲涡流 CSV 导入失败");
            break;
        default:
            ok = importCsv(&error);
            break;
        }
        break;
    }
    case Format::Hdf5:
#ifdef HAS_HDF5
    {
        const int h5type = detectHdf5Type();
        switch (h5type) {
        case 1: // MultiFreq
            ok = importMultiFreqHdf5(m_request.sourcePath, m_request.targetDbPath);
            if (!ok) error = QStringLiteral("多频涡流 HDF5 导入失败");
            break;
        case 2: // MagArray
            ok = importMagArrayHdf5(m_request.sourcePath, m_request.targetDbPath);
            if (!ok) error = QStringLiteral("漏磁检测 HDF5 导入失败");
            break;
        case 3: // PulseEddy
            ok = importPulseEddyHdf5(m_request.sourcePath, m_request.targetDbPath);
            if (!ok) error = QStringLiteral("脉冲涡流 HDF5 导入失败");
            break;
        default:
            ok = importHdf5(&error);
            break;
        }
    }
#else
        error = QStringLiteral("当前构建未启用 HDF5 支持，请使用 CSV 导入");
        ok = false;
#endif
        break;
    }

    if (!ok && m_canceled.loadRelaxed() == 1) {
        emit finished(false, QStringLiteral("已取消"));
        return;
    }
    emit finished(ok, error);
}

bool HistoryImportService::initTargetDb(QSqlDatabase& db, QString* errorMessage) const
{
    QFileInfo info(m_request.targetDbPath);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建目标目录: %1").arg(dir.absolutePath());
        }
        return false;
    }

    db.setDatabaseName(m_request.targetDbPath);
    if (!db.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("打开目标数据库失败: %1").arg(db.lastError().text());
        }
        return false;
    }

    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=OFF"));
    pragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA cache_size=-65536"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    if (!RealtimeSqlRecorder::ensureAlignedFramesSchema(db, errorMessage)) {
        return false;
    }
    return true;
}

bool HistoryImportService::insertRow(QSqlQuery& insertQuery,
                                     const RowPayload& payload,
                                     const QVector<QVariant>& values,
                                     QString* errorMessage) const
{
    if (values.size() != kAlignedValueCount) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("内部错误：列数量不匹配");
        }
        return false;
    }

    insertQuery.bindValue(0, static_cast<qulonglong>(payload.frameSequence));
    insertQuery.bindValue(1, payload.timestampMs);
    insertQuery.bindValue(2, payload.cellCount);
    insertQuery.bindValue(3, payload.detectMode);
    insertQuery.bindValue(4, payload.sourceTag);
    insertQuery.bindValue(5, QDateTime::currentMSecsSinceEpoch());

    for (int i = 0; i < values.size(); ++i) {
        insertQuery.bindValue(6 + i, values.at(i));
    }

    if (!insertQuery.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("写入失败: %1").arg(insertQuery.lastError().text());
        }
        return false;
    }
    return true;
}

bool HistoryImportService::importCsv(QString* errorMessage)
{
    QFile srcFile(m_request.sourcePath);
    if (!srcFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法读取 CSV 文件: %1").arg(srcFile.errorString());
        }
        return false;
    }

    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    if (!initTargetDb(db, errorMessage)) {
        srcFile.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    QSqlQuery clearQuery(db);
    if (!clearQuery.exec(QStringLiteral("DELETE FROM aligned_frames"))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("清空目标数据库失败: %1").arg(clearQuery.lastError().text());
        }
        srcFile.close();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(RealtimeSqlRecorder::alignedFramesInsertSql())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("准备 INSERT 失败: %1").arg(insertQuery.lastError().text());
        }
        srcFile.close();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    if (!db.transaction()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("开启事务失败: %1").arg(db.lastError().text());
        }
        srcFile.close();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    QTextStream stream(&srcFile);
    stream.setCodec("UTF-8");

    QHash<QString, int> colIndex;
    QStringList fields;
    qint64 lineNo = 0;
    qint64 writtenRows = 0;
    emit progress(0, 0); // CSV 总量未知，UI 走 busy

    auto col = [&](const QString& name) -> int { return colIndex.value(name, -1); };

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        ++lineNo;
        if (line.trimmed().isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        if (colIndex.isEmpty()) {
            if (!parseCsvLine(line, &fields)) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("CSV 表头解析失败（引号未闭合）");
                }
                db.rollback();
                srcFile.close();
                db.close();
                db = QSqlDatabase();
                // defer removeDatabase: avoid removing while query handles may still be alive
                QFile::remove(m_request.targetDbPath);
                return false;
            }
            for (int i = 0; i < fields.size(); ++i) {
                colIndex.insert(fields.at(i).trimmed(), i);
            }
            const QStringList required {
                QStringLiteral("timestamp_ms_utc"),
                QStringLiteral("frame_sequence"),
                QStringLiteral("detect_mode"),
                QStringLiteral("cell_count"),
            };
            for (const QString& name : required) {
                if (!colIndex.contains(name)) {
                    if (errorMessage) {
                        *errorMessage = QStringLiteral("CSV 缺少必需列: %1").arg(name);
                    }
                    db.rollback();
                    srcFile.close();
                    db.close();
                    db = QSqlDatabase();
                    // defer removeDatabase: avoid removing while query handles may still be alive
                    QFile::remove(m_request.targetDbPath);
                    return false;
                }
            }
            // 必需 40 槽位列
            for (int ch = 0; ch < kChannelCount; ++ch) {
                const QString prefix = QStringLiteral("pos%1").arg(ch, 2, 10, QLatin1Char('0'));
                const QStringList cols {
                    prefix + QStringLiteral("_amp"),
                    prefix + QStringLiteral("_phase"),
                    prefix + QStringLiteral("_x"),
                    prefix + QStringLiteral("_y"),
                    prefix + QStringLiteral("_source_channel"),
                };
                for (const QString& c : cols) {
                    if (!colIndex.contains(c)) {
                        if (errorMessage) {
                            *errorMessage = QStringLiteral("CSV 缺少必需列: %1").arg(c);
                        }
                        db.rollback();
                        srcFile.close();
                        db.close();
                        db = QSqlDatabase();
                        // defer removeDatabase: avoid removing while query handles may still be alive
                        QFile::remove(m_request.targetDbPath);
                        return false;
                    }
                }
            }
            continue;
        }

        if (m_canceled.loadRelaxed() == 1) {
            db.rollback();
            srcFile.close();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }

        if (!parseCsvLine(line, &fields)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("CSV 第 %1 行解析失败（引号未闭合）").arg(lineNo);
            }
            db.rollback();
            srcFile.close();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }
        if (fields.size() < colIndex.size()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("CSV 第 %1 行列数不足").arg(lineNo);
            }
            db.rollback();
            srcFile.close();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }

        auto token = [&](const QString& name) -> QString {
            const int i = col(name);
            return (i >= 0 && i < fields.size()) ? fields.at(i) : QString();
        };

        bool okTs = false;
        const qint64 ts = token(QStringLiteral("timestamp_ms_utc")).trimmed().toLongLong(&okTs);
        bool okSeq = false;
        const qint64 seq = token(QStringLiteral("frame_sequence")).trimmed().toLongLong(&okSeq);
        bool okMode = false;
        const int mode = token(QStringLiteral("detect_mode")).trimmed().toInt(&okMode);
        bool okCount = false;
        const int cellCount = token(QStringLiteral("cell_count")).trimmed().toInt(&okCount);
        if (!okTs || !okSeq || !okMode || !okCount) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("CSV 第 %1 行基础字段解析失败").arg(lineNo);
            }
            db.rollback();
            srcFile.close();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }

        RowPayload row;
        row.timestampMs = ts;
        row.frameSequence = seq;
        row.detectMode = mode;
        row.cellCount = cellCount;
        row.sourceTag = token(QStringLiteral("source_tag"));

        QVector<QVariant> values(kAlignedValueCount);
        for (int ch = 0; ch < kChannelCount; ++ch) {
            const QString prefix = QStringLiteral("pos%1").arg(ch, 2, 10, QLatin1Char('0'));
            bool okField = false;
            const QVariant amp = parseDoubleOrNull(token(prefix + QStringLiteral("_amp")), &okField);
            if (!okField) {
                if (errorMessage) *errorMessage = QStringLiteral("CSV 第 %1 行 %2 解析失败").arg(lineNo).arg(prefix + QStringLiteral("_amp"));
                db.rollback(); srcFile.close(); db.close(); db = QSqlDatabase();
                QFile::remove(m_request.targetDbPath);
                return false;
            }
            const QVariant phase = parseDoubleOrNull(token(prefix + QStringLiteral("_phase")), &okField);
            if (!okField) {
                if (errorMessage) *errorMessage = QStringLiteral("CSV 第 %1 行 %2 解析失败").arg(lineNo).arg(prefix + QStringLiteral("_phase"));
                db.rollback(); srcFile.close(); db.close(); db = QSqlDatabase();
                QFile::remove(m_request.targetDbPath);
                return false;
            }
            const QVariant x = parseDoubleOrNull(token(prefix + QStringLiteral("_x")), &okField);
            if (!okField) {
                if (errorMessage) *errorMessage = QStringLiteral("CSV 第 %1 行 %2 解析失败").arg(lineNo).arg(prefix + QStringLiteral("_x"));
                db.rollback(); srcFile.close(); db.close(); db = QSqlDatabase();
                QFile::remove(m_request.targetDbPath);
                return false;
            }
            const QVariant y = parseDoubleOrNull(token(prefix + QStringLiteral("_y")), &okField);
            if (!okField) {
                if (errorMessage) *errorMessage = QStringLiteral("CSV 第 %1 行 %2 解析失败").arg(lineNo).arg(prefix + QStringLiteral("_y"));
                db.rollback(); srcFile.close(); db.close(); db = QSqlDatabase();
                QFile::remove(m_request.targetDbPath);
                return false;
            }
            const QVariant src = parseIntOrNull(token(prefix + QStringLiteral("_source_channel")), &okField);
            if (!okField) {
                if (errorMessage) *errorMessage = QStringLiteral("CSV 第 %1 行 %2 解析失败").arg(lineNo).arg(prefix + QStringLiteral("_source_channel"));
                db.rollback(); srcFile.close(); db.close(); db = QSqlDatabase();
                QFile::remove(m_request.targetDbPath);
                return false;
            }

            const int base = ch * 5;
            values[base + 0] = amp;
            values[base + 1] = phase;
            values[base + 2] = x;
            values[base + 3] = y;
            values[base + 4] = src;
        }

        QString insertError;
        if (!insertRow(insertQuery, row, values, &insertError)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("CSV 第 %1 行写入失败: %2")
                                    .arg(lineNo)
                                    .arg(insertError);
            }
            db.rollback();
            srcFile.close();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }

        ++writtenRows;
        if (writtenRows % qMax(200, m_request.chunkSize) == 0) {
            emit progress(writtenRows, 0);
        }
    }

    if (!db.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("提交事务失败: %1").arg(db.lastError().text());
        }
        db.rollback();
        srcFile.close();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    srcFile.close();
    db.close();
    db = QSqlDatabase();
    // defer removeDatabase: avoid removing while query handles may still be alive
    emit progress(writtenRows, writtenRows);
    return true;
}

// ---- CSV 导入：各设备类型 ----

bool HistoryImportService::importMultiFreqCsv(const QString& filePath, const QString& targetDbPath)
{
    if (!QFile::exists(filePath)) return false;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QDir().mkpath(QFileInfo(targetDbPath).absolutePath());
    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(targetDbPath);
    if (!db.open()) return false;

    QString schemaError;
    if (!initTargetDb(db, &schemaError)) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    if (!RealtimeSqlRecorder::ensureMultiFreqFramesSchema(db, &schemaError)) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }

    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM multifreq_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(QStringLiteral(
            "INSERT INTO multifreq_frames(timestamp_unix_ms, frame_index, frequency_factor, frequency_hz, "
            "impedance_real, impedance_imag, impedance_magnitude, impedance_phase_deg, "
            "normalized_impedance_real, normalized_impedance_imag, "
            "voltage_magnitude, current_magnitude, valid) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)")))
    {
        db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }
    if (!db.transaction()) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");

    // Skip header comments and column header line
    QString line;
    while (stream.readLineInto(&line)) {
        if (line.startsWith('#')) continue;
        if (line.startsWith(QStringLiteral("timestamp_ms_utc"))) continue;
        break;
    }
    if (line.isEmpty() && stream.atEnd()) { db.commit(); db.close(); db = QSqlDatabase(); emit progress(0,0); return true; }

    qint64 written = 0;
    emit progress(0, 0);
    auto processLine = [&](const QString& l) {
        QStringList fields;
        if (!parseCsvLine(l, &fields)) return;
        if (fields.size() < 12) return;
        insertQuery.bindValue(0,  fields[0].toLongLong());
        insertQuery.bindValue(1,  fields[1].toLongLong());
        insertQuery.bindValue(2,  fields[2].toInt());
        insertQuery.bindValue(3,  fields[3].toDouble());
        insertQuery.bindValue(4,  parseDoubleOrNull(fields[4]));
        insertQuery.bindValue(5,  parseDoubleOrNull(fields[5]));
        insertQuery.bindValue(6,  parseDoubleOrNull(fields[6]));
        insertQuery.bindValue(7,  parseDoubleOrNull(fields[7]));
        insertQuery.bindValue(8,  parseDoubleOrNull(fields[8]));
        insertQuery.bindValue(9,  parseDoubleOrNull(fields[9]));
        insertQuery.bindValue(10, parseDoubleOrNull(fields[10]));
        insertQuery.bindValue(11, parseDoubleOrNull(fields[11]));
        insertQuery.bindValue(12, 1); // valid
        if (insertQuery.exec()) ++written;
    };
    processLine(line);
    while (stream.readLineInto(&line)) {
        if (m_canceled.loadRelaxed() == 1) { db.rollback(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
        processLine(line);
        if ((written % 1000) == 0) emit progress(written, 0);
    }
    if (!db.commit()) { db.rollback(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    db.close(); db = QSqlDatabase();
    emit progress(written, written);
    return true;
}

bool HistoryImportService::importMagArrayCsv(const QString& filePath, const QString& targetDbPath)
{
    if (!QFile::exists(filePath)) return false;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QDir().mkpath(QFileInfo(targetDbPath).absolutePath());
    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(targetDbPath);
    if (!db.open()) return false;

    QString schemaError;
    if (!initTargetDb(db, &schemaError)) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    if (!RealtimeSqlRecorder::ensureMagArrayFramesSchema(db, &schemaError)) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }

    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM mag_array_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(QStringLiteral(
            "INSERT INTO mag_array_frames(timestamp_unix_ms, frame_index, sensor_index, "
            "x_mean, y_mean, z_mean, x_latest, y_latest, z_latest, "
            "magnitude_mean, magnitude_latest, "
            "processed_value_x, processed_value_y, processed_value_z) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)")))
    {
        db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }
    if (!db.transaction()) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");

    QString line;
    while (stream.readLineInto(&line)) {
        if (line.startsWith('#')) continue;
        if (line.startsWith(QStringLiteral("timestamp_ms_utc"))) continue;
        break;
    }
    if (line.isEmpty() && stream.atEnd()) { db.commit(); db.close(); db = QSqlDatabase(); emit progress(0,0); return true; }

    qint64 written = 0;
    emit progress(0, 0);
    auto processLine = [&](const QString& l) {
        QStringList fields;
        if (!parseCsvLine(l, &fields)) return;
        if (fields.size() < 14) return;
        insertQuery.bindValue(0,  fields[0].toLongLong());
        insertQuery.bindValue(1,  fields[1].toLongLong());
        insertQuery.bindValue(2,  fields[2].toInt());
        insertQuery.bindValue(3,  parseDoubleOrNull(fields[3]));
        insertQuery.bindValue(4,  parseDoubleOrNull(fields[4]));
        insertQuery.bindValue(5,  parseDoubleOrNull(fields[5]));
        insertQuery.bindValue(6,  parseDoubleOrNull(fields[6]));
        insertQuery.bindValue(7,  parseDoubleOrNull(fields[7]));
        insertQuery.bindValue(8,  parseDoubleOrNull(fields[8]));
        insertQuery.bindValue(9,  parseDoubleOrNull(fields[9]));
        insertQuery.bindValue(10, parseDoubleOrNull(fields[10]));
        insertQuery.bindValue(11, parseDoubleOrNull(fields[11]));
        insertQuery.bindValue(12, parseDoubleOrNull(fields[12]));
        insertQuery.bindValue(13, parseDoubleOrNull(fields[13]));
        if (insertQuery.exec()) ++written;
    };
    processLine(line);
    while (stream.readLineInto(&line)) {
        if (m_canceled.loadRelaxed() == 1) { db.rollback(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
        processLine(line);
        if ((written % 1000) == 0) emit progress(written, 0);
    }
    if (!db.commit()) { db.rollback(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    db.close(); db = QSqlDatabase();
    emit progress(written, written);
    return true;
}

bool HistoryImportService::importPulseEddyCsv(const QString& filePath, const QString& targetDbPath)
{
    if (!QFile::exists(filePath)) return false;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QDir().mkpath(QFileInfo(targetDbPath).absolutePath());
    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(targetDbPath);
    if (!db.open()) return false;

    QString schemaError;
    if (!initTargetDb(db, &schemaError)) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    if (!RealtimeSqlRecorder::ensurePulseEddyFramesSchema(db, &schemaError)) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }

    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM pulse_eddy_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(QStringLiteral(
            "INSERT INTO pulse_eddy_frames(timestamp_unix_ms, frame_index, "
            "sample_count, sample_rate_hz, raw_values, has_reference, reference_values, min_value, max_value) "
            "VALUES(?,?,?,?,?,?,?,?,?)")))
    {
        db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }
    if (!db.transaction()) { db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");

    QString line;
    while (stream.readLineInto(&line)) {
        if (line.startsWith('#')) continue;
        if (line.startsWith(QStringLiteral("timestamp_ms_utc"))) continue;
        break;
    }
    if (line.isEmpty() && stream.atEnd()) { db.commit(); db.close(); db = QSqlDatabase(); emit progress(0,0); return true; }

    qint64 written = 0;
    emit progress(0, 0);
    auto processLine = [&](const QString& l) {
        QStringList fields;
        if (!parseCsvLine(l, &fields)) return;
        if (fields.size() < 9) return;
        insertQuery.bindValue(0, fields[0].toLongLong());
        insertQuery.bindValue(1, fields[1].toLongLong());
        insertQuery.bindValue(2, fields[2].toInt());
        insertQuery.bindValue(3, parseDoubleOrNull(fields[3]));
        // raw_values: decode from Base64
        insertQuery.bindValue(4, QByteArray::fromBase64(fields[4].toLatin1()));
        insertQuery.bindValue(5, fields[5].toInt());
        // reference_values: decode from Base64
        insertQuery.bindValue(6, fields[6].isEmpty() ? QByteArray() : QByteArray::fromBase64(fields[6].toLatin1()));
        insertQuery.bindValue(7, parseDoubleOrNull(fields[7]));
        insertQuery.bindValue(8, parseDoubleOrNull(fields[8]));
        if (insertQuery.exec()) ++written;
    };
    processLine(line);
    while (stream.readLineInto(&line)) {
        if (m_canceled.loadRelaxed() == 1) { db.rollback(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
        processLine(line);
        if ((written % 100) == 0) emit progress(written, 0);
    }
    if (!db.commit()) { db.rollback(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    db.close(); db = QSqlDatabase();
    emit progress(written, written);
    return true;
}

#ifdef HAS_HDF5
namespace {
bool readHyperslab1D(hid_t ds, hid_t memType, hsize_t offset, hsize_t count, void* out)
{
    hid_t fileSpace = H5Dget_space(ds);
    if (fileSpace < 0) return false;
    const hsize_t start[1] = { offset };
    const hsize_t cnt[1] = { count };
    if (H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, cnt, nullptr) < 0) {
        H5Sclose(fileSpace);
        return false;
    }
    hid_t memSpace = H5Screate_simple(1, cnt, nullptr);
    if (memSpace < 0) {
        H5Sclose(fileSpace);
        return false;
    }
    const herr_t status = H5Dread(ds, memType, memSpace, fileSpace, H5P_DEFAULT, out);
    H5Sclose(memSpace);
    H5Sclose(fileSpace);
    return status >= 0;
}

bool readHyperslab2D(hid_t ds, hid_t memType, hsize_t rowOffset, hsize_t rowCount, hsize_t cols, void* out)
{
    hid_t fileSpace = H5Dget_space(ds);
    if (fileSpace < 0) return false;
    const hsize_t start[2] = { rowOffset, 0 };
    const hsize_t cnt[2] = { rowCount, cols };
    if (H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, cnt, nullptr) < 0) {
        H5Sclose(fileSpace);
        return false;
    }
    hid_t memSpace = H5Screate_simple(2, cnt, nullptr);
    if (memSpace < 0) {
        H5Sclose(fileSpace);
        return false;
    }
    const herr_t status = H5Dread(ds, memType, memSpace, fileSpace, H5P_DEFAULT, out);
    H5Sclose(memSpace);
    H5Sclose(fileSpace);
    return status >= 0;
}
} // namespace

bool HistoryImportService::importHdf5(QString* errorMessage)
{
    hid_t fileId = H5Fopen(m_request.sourcePath.toLocal8Bit().constData(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fileId < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法打开 HDF5 文件");
        }
        return false;
    }

    auto closeH5 = [&]() {
        if (fileId >= 0) {
            H5Fclose(fileId);
            fileId = -1;
        }
    };

    auto openDs = [&](const char* name) -> hid_t {
        if (H5Lexists(fileId, name, H5P_DEFAULT) <= 0) {
            return -1;
        }
        return H5Dopen2(fileId, name, H5P_DEFAULT);
    };

    hid_t dsTs = openDs("/frames/timestamp_ms_utc");
    hid_t dsSeq = openDs("/frames/frame_sequence");
    hid_t dsMode = openDs("/frames/detect_mode");
    hid_t dsCount = openDs("/frames/cell_count");
    if (dsTs < 0 || dsSeq < 0 || dsMode < 0 || dsCount < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("HDF5 缺少必需数据集 /frames/*");
        }
        if (dsTs >= 0) H5Dclose(dsTs);
        if (dsSeq >= 0) H5Dclose(dsSeq);
        if (dsMode >= 0) H5Dclose(dsMode);
        if (dsCount >= 0) H5Dclose(dsCount);
        closeH5();
        return false;
    }

    hid_t tsSpace = H5Dget_space(dsTs);
    hsize_t dims[1] = {0};
    H5Sget_simple_extent_dims(tsSpace, dims, nullptr);
    H5Sclose(tsSpace);
    const qint64 totalRows = static_cast<qint64>(dims[0]);
    emit progress(0, totalRows);

    hid_t dsTag = openDs("/frames/source_tag");
    hid_t dsAmp = openDs("/channels/amp");
    hid_t dsPhase = openDs("/channels/phase");
    hid_t dsX = openDs("/channels/x");
    hid_t dsY = openDs("/channels/y");
    hid_t dsSrc = openDs("/channels/source_channel");

    if (dsAmp < 0 && dsPhase < 0 && dsX < 0 && dsY < 0 && dsSrc < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("HDF5 缺少 /channels 数据集");
        }
        if (dsTag >= 0) H5Dclose(dsTag);
        H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
        closeH5();
        return false;
    }

    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    if (!initTargetDb(db, errorMessage)) {
        if (dsTag >= 0) H5Dclose(dsTag);
        if (dsAmp >= 0) H5Dclose(dsAmp);
        if (dsPhase >= 0) H5Dclose(dsPhase);
        if (dsX >= 0) H5Dclose(dsX);
        if (dsY >= 0) H5Dclose(dsY);
        if (dsSrc >= 0) H5Dclose(dsSrc);
        H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
        closeH5();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM aligned_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(RealtimeSqlRecorder::alignedFramesInsertSql())) {
        if (errorMessage) *errorMessage = insertQuery.lastError().text();
        if (dsTag >= 0) H5Dclose(dsTag);
        if (dsAmp >= 0) H5Dclose(dsAmp);
        if (dsPhase >= 0) H5Dclose(dsPhase);
        if (dsX >= 0) H5Dclose(dsX);
        if (dsY >= 0) H5Dclose(dsY);
        if (dsSrc >= 0) H5Dclose(dsSrc);
        H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
        closeH5();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }
    if (!db.transaction()) {
        if (errorMessage) *errorMessage = db.lastError().text();
        if (dsTag >= 0) H5Dclose(dsTag);
        if (dsAmp >= 0) H5Dclose(dsAmp);
        if (dsPhase >= 0) H5Dclose(dsPhase);
        if (dsX >= 0) H5Dclose(dsX);
        if (dsY >= 0) H5Dclose(dsY);
        if (dsSrc >= 0) H5Dclose(dsSrc);
        H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
        closeH5();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    const qint64 chunkSize = qMax(256, m_request.chunkSize);
    QVector<qint64> tsBuf;
    QVector<qint64> seqBuf;
    QVector<quint8> modeBuf;
    QVector<quint8> countBuf;
    QVector<double> ampBuf;
    QVector<double> phaseBuf;
    QVector<double> xBuf;
    QVector<double> yBuf;
    QVector<qint32> srcBuf;

    QByteArray fixedTagRaw;
    hid_t tagType = -1;
    bool tagVariable = false;
    size_t fixedTagSize = 0;
    if (dsTag >= 0) {
        tagType = H5Dget_type(dsTag);
        if (tagType >= 0) {
            tagVariable = (H5Tis_variable_str(tagType) > 0);
            if (!tagVariable) {
                fixedTagSize = H5Tget_size(tagType);
            }
        }
    }

    qint64 written = 0;
    for (qint64 offset = 0; offset < totalRows; offset += chunkSize) {
        if (m_canceled.loadRelaxed() == 1) {
            db.rollback();
            if (tagType >= 0) H5Tclose(tagType);
            if (dsTag >= 0) H5Dclose(dsTag);
            if (dsAmp >= 0) H5Dclose(dsAmp);
            if (dsPhase >= 0) H5Dclose(dsPhase);
            if (dsX >= 0) H5Dclose(dsX);
            if (dsY >= 0) H5Dclose(dsY);
            if (dsSrc >= 0) H5Dclose(dsSrc);
            H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
            closeH5();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }

        const qint64 n = qMin(chunkSize, totalRows - offset);
        tsBuf.resize(n);
        seqBuf.resize(n);
        modeBuf.resize(n);
        countBuf.resize(n);
        if (!readHyperslab1D(dsTs, H5T_NATIVE_INT64, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), tsBuf.data()) ||
            !readHyperslab1D(dsSeq, H5T_NATIVE_INT64, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), seqBuf.data()) ||
            !readHyperslab1D(dsMode, H5T_NATIVE_UINT8, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), modeBuf.data()) ||
            !readHyperslab1D(dsCount, H5T_NATIVE_UINT8, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), countBuf.data())) {
            if (errorMessage) *errorMessage = QStringLiteral("读取 HDF5 /frames 失败");
            db.rollback();
            if (tagType >= 0) H5Tclose(tagType);
            if (dsTag >= 0) H5Dclose(dsTag);
            if (dsAmp >= 0) H5Dclose(dsAmp);
            if (dsPhase >= 0) H5Dclose(dsPhase);
            if (dsX >= 0) H5Dclose(dsX);
            if (dsY >= 0) H5Dclose(dsY);
            if (dsSrc >= 0) H5Dclose(dsSrc);
            H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
            closeH5();
            db.close();
            db = QSqlDatabase();
            // defer removeDatabase: avoid removing while query handles may still be alive
            QFile::remove(m_request.targetDbPath);
            return false;
        }

        QVector<QString> tagValues(n);
        if (dsTag >= 0 && tagType >= 0) {
            if (tagVariable) {
                QVector<char*> varBuf(n, nullptr);
                hid_t memType = H5Tcopy(H5T_C_S1);
                H5Tset_size(memType, H5T_VARIABLE);
                if (!readHyperslab1D(dsTag, memType, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), varBuf.data())) {
                    H5Tclose(memType);
                    if (errorMessage) *errorMessage = QStringLiteral("读取 HDF5 source_tag 失败");
                    db.rollback();
                    if (tagType >= 0) H5Tclose(tagType);
                    if (dsTag >= 0) H5Dclose(dsTag);
                    if (dsAmp >= 0) H5Dclose(dsAmp);
                    if (dsPhase >= 0) H5Dclose(dsPhase);
                    if (dsX >= 0) H5Dclose(dsX);
                    if (dsY >= 0) H5Dclose(dsY);
                    if (dsSrc >= 0) H5Dclose(dsSrc);
                    H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
                    closeH5();
                    db.close();
                    db = QSqlDatabase();
                    // defer removeDatabase: avoid removing while query handles may still be alive
                    QFile::remove(m_request.targetDbPath);
                    return false;
                }
                for (int i = 0; i < n; ++i) {
                    if (varBuf[i]) {
                        tagValues[i] = QString::fromUtf8(varBuf[i]);
                    }
                }
                const hsize_t memDim[1] = { static_cast<hsize_t>(n) };
                hid_t memSpace = H5Screate_simple(1, memDim, nullptr);
                H5Dvlen_reclaim(memType, memSpace, H5P_DEFAULT, varBuf.data());
                H5Sclose(memSpace);
                H5Tclose(memType);
            } else if (fixedTagSize > 0) {
                fixedTagRaw.fill('\0', static_cast<int>(n * static_cast<qint64>(fixedTagSize)));
                if (!readHyperslab1D(dsTag, tagType, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), fixedTagRaw.data())) {
                    if (errorMessage) *errorMessage = QStringLiteral("读取 HDF5 source_tag 失败");
                    db.rollback();
                    if (tagType >= 0) H5Tclose(tagType);
                    if (dsTag >= 0) H5Dclose(dsTag);
                    if (dsAmp >= 0) H5Dclose(dsAmp);
                    if (dsPhase >= 0) H5Dclose(dsPhase);
                    if (dsX >= 0) H5Dclose(dsX);
                    if (dsY >= 0) H5Dclose(dsY);
                    if (dsSrc >= 0) H5Dclose(dsSrc);
                    H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
                    closeH5();
                    db.close();
                    db = QSqlDatabase();
                    // defer removeDatabase: avoid removing while query handles may still be alive
                    QFile::remove(m_request.targetDbPath);
                    return false;
                }
                for (int i = 0; i < n; ++i) {
                    const char* p = fixedTagRaw.constData() + static_cast<qint64>(i) * static_cast<qint64>(fixedTagSize);
                    tagValues[i] = QString::fromUtf8(p).trimmed();
                }
            }
        }

        auto initDoubleBuf = [&](QVector<double>& buf) {
            buf.resize(n * kChannelCount);
            buf.fill(std::numeric_limits<double>::quiet_NaN());
        };
        auto initSrcBuf = [&](QVector<qint32>& buf) {
            buf.resize(n * kChannelCount);
            buf.fill(-1);
        };
        initDoubleBuf(ampBuf);
        initDoubleBuf(phaseBuf);
        initDoubleBuf(xBuf);
        initDoubleBuf(yBuf);
        initSrcBuf(srcBuf);

        if (dsAmp >= 0) readHyperslab2D(dsAmp, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), kChannelCount, ampBuf.data());
        if (dsPhase >= 0) readHyperslab2D(dsPhase, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), kChannelCount, phaseBuf.data());
        if (dsX >= 0) readHyperslab2D(dsX, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), kChannelCount, xBuf.data());
        if (dsY >= 0) readHyperslab2D(dsY, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), kChannelCount, yBuf.data());
        if (dsSrc >= 0) readHyperslab2D(dsSrc, H5T_NATIVE_INT32, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), kChannelCount, srcBuf.data());

        for (int r = 0; r < n; ++r) {
            RowPayload row;
            row.timestampMs = tsBuf[r];
            row.frameSequence = seqBuf[r];
            row.detectMode = modeBuf[r];
            row.cellCount = countBuf[r];
            row.sourceTag = tagValues.value(r);

            QVector<QVariant> values(kAlignedValueCount);
            for (int ch = 0; ch < kChannelCount; ++ch) {
                const int idx = r * kChannelCount + ch;
                const int base = ch * 5;
                const double amp = ampBuf[idx];
                const double phase = phaseBuf[idx];
                const double vx = xBuf[idx];
                const double vy = yBuf[idx];
                const qint32 src = srcBuf[idx];

                values[base + 0] = std::isfinite(amp) ? QVariant(amp) : QVariant();
                values[base + 1] = std::isfinite(phase) ? QVariant(phase) : QVariant();
                values[base + 2] = std::isfinite(vx) ? QVariant(vx) : QVariant();
                values[base + 3] = std::isfinite(vy) ? QVariant(vy) : QVariant();
                values[base + 4] = (src >= 0) ? QVariant(static_cast<int>(src)) : QVariant();
            }

            QString insertError;
            if (!insertRow(insertQuery, row, values, &insertError)) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("写入 HDF5 行失败: %1").arg(insertError);
                }
                db.rollback();
                if (tagType >= 0) H5Tclose(tagType);
                if (dsTag >= 0) H5Dclose(dsTag);
                if (dsAmp >= 0) H5Dclose(dsAmp);
                if (dsPhase >= 0) H5Dclose(dsPhase);
                if (dsX >= 0) H5Dclose(dsX);
                if (dsY >= 0) H5Dclose(dsY);
                if (dsSrc >= 0) H5Dclose(dsSrc);
                H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
                closeH5();
                db.close();
                db = QSqlDatabase();
                // defer removeDatabase: avoid removing while query handles may still be alive
                QFile::remove(m_request.targetDbPath);
                return false;
            }
            ++written;
        }
        emit progress(written, totalRows);
    }

    if (!db.commit()) {
        if (errorMessage) *errorMessage = QStringLiteral("提交事务失败: %1").arg(db.lastError().text());
        db.rollback();
        if (tagType >= 0) H5Tclose(tagType);
        if (dsTag >= 0) H5Dclose(dsTag);
        if (dsAmp >= 0) H5Dclose(dsAmp);
        if (dsPhase >= 0) H5Dclose(dsPhase);
        if (dsX >= 0) H5Dclose(dsX);
        if (dsY >= 0) H5Dclose(dsY);
        if (dsSrc >= 0) H5Dclose(dsSrc);
        H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
        closeH5();
        db.close();
        db = QSqlDatabase();
        // defer removeDatabase: avoid removing while query handles may still be alive
        QFile::remove(m_request.targetDbPath);
        return false;
    }

    if (tagType >= 0) H5Tclose(tagType);
    if (dsTag >= 0) H5Dclose(dsTag);
    if (dsAmp >= 0) H5Dclose(dsAmp);
    if (dsPhase >= 0) H5Dclose(dsPhase);
    if (dsX >= 0) H5Dclose(dsX);
    if (dsY >= 0) H5Dclose(dsY);
    if (dsSrc >= 0) H5Dclose(dsSrc);
    H5Dclose(dsTs); H5Dclose(dsSeq); H5Dclose(dsMode); H5Dclose(dsCount);
    closeH5();

    db.close();
    db = QSqlDatabase();
    // defer removeDatabase: avoid removing while query handles may still be alive
    emit progress(written, written);
    return true;
}

bool HistoryImportService::importMultiFreqHdf5(const QString& filePath, const QString& targetDbPath)
{
    if (!QFile::exists(filePath)) {
        return false;
    }

    hid_t fileId = H5Fopen(filePath.toLocal8Bit().constData(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fileId < 0) {
        return false;
    }

    auto closeH5 = [&]() {
        if (fileId >= 0) {
            H5Fclose(fileId);
            fileId = -1;
        }
    };

    auto openDs = [&](const char* name) -> hid_t {
        if (H5Lexists(fileId, name, H5P_DEFAULT) <= 0) {
            return -1;
        }
        return H5Dopen2(fileId, name, H5P_DEFAULT);
    };

    // Open all 12 datasets
    hid_t dsTs       = openDs("/frames/timestamp_ms_utc");
    hid_t dsFrameIdx = openDs("/frames/frame_index");
    hid_t dsFreqFac  = openDs("/frames/frequency_factor");
    hid_t dsFreqHz   = openDs("/frames/frequency_hz");
    hid_t dsImpReal  = openDs("/impedance/real");
    hid_t dsImpImag  = openDs("/impedance/imag");
    hid_t dsImpMag   = openDs("/impedance/magnitude");
    hid_t dsImpPhase = openDs("/impedance/phase_deg");
    hid_t dsNormReal = openDs("/impedance/norm_real");
    hid_t dsNormImag = openDs("/impedance/norm_imag");
    hid_t dsVoltMag  = openDs("/voltage/magnitude");
    hid_t dsCurrMag  = openDs("/current/magnitude");

    const hid_t allDs[] = { dsTs, dsFrameIdx, dsFreqFac, dsFreqHz,
                            dsImpReal, dsImpImag, dsImpMag, dsImpPhase,
                            dsNormReal, dsNormImag, dsVoltMag, dsCurrMag };
    for (hid_t d : allDs) {
        if (d < 0) {
            for (hid_t dd : allDs) if (dd >= 0) H5Dclose(dd);
            closeH5();
            return false;
        }
    }

    // Get row count
    hid_t tsSpace = H5Dget_space(dsTs);
    hsize_t dims[1] = {0};
    H5Sget_simple_extent_dims(tsSpace, dims, nullptr);
    H5Sclose(tsSpace);
    const qint64 totalRows = static_cast<qint64>(dims[0]);
    emit progress(0, totalRows);

    // Setup target database
    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(targetDbPath);
    if (!db.open()) {
        for (hid_t d : allDs) H5Dclose(d);
        closeH5();
        return false;
    }

    // Ensure multifreq_frames schema
    QString schemaError;
    if (!RealtimeSqlRecorder::ensureMultiFreqFramesSchema(db, &schemaError)) {
        for (hid_t d : allDs) H5Dclose(d);
        closeH5();
        db.close();
        db = QSqlDatabase();
        QFile::remove(targetDbPath);
        return false;
    }

    // Prepare insert
    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM multifreq_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(QStringLiteral(
            "INSERT INTO multifreq_frames(timestamp_unix_ms, frame_index, frequency_factor, frequency_hz, "
            "impedance_real, impedance_imag, impedance_magnitude, impedance_phase_deg, "
            "normalized_impedance_real, normalized_impedance_imag, "
            "voltage_magnitude, current_magnitude, valid) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)")))
    {
        for (hid_t d : allDs) H5Dclose(d);
        closeH5();
        db.close();
        db = QSqlDatabase();
        QFile::remove(targetDbPath);
        return false;
    }
    if (!db.transaction()) {
        for (hid_t d : allDs) H5Dclose(d);
        closeH5();
        db.close();
        db = QSqlDatabase();
        QFile::remove(targetDbPath);
        return false;
    }

    // Chunked reading loop
    const qint64 chunkSize = qMax(256, m_request.chunkSize);
    QVector<qint64>  bufTs;
    QVector<qint64>  bufFrameIdx;
    QVector<qint32>  bufFreqFac;
    QVector<double>  bufFreqHz;
    QVector<double>  bufImpReal;
    QVector<double>  bufImpImag;
    QVector<double>  bufImpMag;
    QVector<double>  bufImpPhase;
    QVector<double>  bufNormReal;
    QVector<double>  bufNormImag;
    QVector<double>  bufVoltMag;
    QVector<double>  bufCurrMag;

    qint64 written = 0;
    for (qint64 offset = 0; offset < totalRows; offset += chunkSize) {
        if (m_canceled.loadRelaxed() == 1) {
            db.rollback();
            for (hid_t d : allDs) H5Dclose(d);
            closeH5();
            db.close();
            db = QSqlDatabase();
            QFile::remove(targetDbPath);
            return false;
        }

        const qint64 n = qMin(chunkSize, totalRows - offset);
        bufTs.resize(n);
        bufFrameIdx.resize(n);
        bufFreqFac.resize(n);
        bufFreqHz.resize(n);
        bufImpReal.resize(n);
        bufImpImag.resize(n);
        bufImpMag.resize(n);
        bufImpPhase.resize(n);
        bufNormReal.resize(n);
        bufNormImag.resize(n);
        bufVoltMag.resize(n);
        bufCurrMag.resize(n);

        if (!readHyperslab1D(dsTs,       H5T_NATIVE_INT64,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufTs.data()) ||
            !readHyperslab1D(dsFrameIdx, H5T_NATIVE_INT64,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufFrameIdx.data()) ||
            !readHyperslab1D(dsFreqFac,  H5T_NATIVE_INT32,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufFreqFac.data()) ||
            !readHyperslab1D(dsFreqHz,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufFreqHz.data()) ||
            !readHyperslab1D(dsImpReal,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufImpReal.data()) ||
            !readHyperslab1D(dsImpImag,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufImpImag.data()) ||
            !readHyperslab1D(dsImpMag,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufImpMag.data()) ||
            !readHyperslab1D(dsImpPhase, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufImpPhase.data()) ||
            !readHyperslab1D(dsNormReal, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufNormReal.data()) ||
            !readHyperslab1D(dsNormImag, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufNormImag.data()) ||
            !readHyperslab1D(dsVoltMag,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufVoltMag.data()) ||
            !readHyperslab1D(dsCurrMag,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bufCurrMag.data()))
        {
            db.rollback();
            for (hid_t d : allDs) H5Dclose(d);
            closeH5();
            db.close();
            db = QSqlDatabase();
            QFile::remove(targetDbPath);
            return false;
        }

        for (qint64 r = 0; r < n; ++r) {
            insertQuery.bindValue(0,  bufTs[r]);
            insertQuery.bindValue(1,  bufFrameIdx[r]);
            insertQuery.bindValue(2,  static_cast<int>(bufFreqFac[r]));
            insertQuery.bindValue(3,  bufFreqHz[r]);
            insertQuery.bindValue(4,  std::isfinite(bufImpReal[r])  ? QVariant(bufImpReal[r])  : QVariant());
            insertQuery.bindValue(5,  std::isfinite(bufImpImag[r])  ? QVariant(bufImpImag[r])  : QVariant());
            insertQuery.bindValue(6,  std::isfinite(bufImpMag[r])   ? QVariant(bufImpMag[r])   : QVariant());
            insertQuery.bindValue(7,  std::isfinite(bufImpPhase[r]) ? QVariant(bufImpPhase[r]) : QVariant());
            insertQuery.bindValue(8,  std::isfinite(bufNormReal[r]) ? QVariant(bufNormReal[r]) : QVariant());
            insertQuery.bindValue(9,  std::isfinite(bufNormImag[r]) ? QVariant(bufNormImag[r]) : QVariant());
            insertQuery.bindValue(10, std::isfinite(bufVoltMag[r])  ? QVariant(bufVoltMag[r])  : QVariant());
            insertQuery.bindValue(11, std::isfinite(bufCurrMag[r])  ? QVariant(bufCurrMag[r])  : QVariant());
            insertQuery.bindValue(12, 1); // valid

            if (!insertQuery.exec()) {
                db.rollback();
                for (hid_t d : allDs) H5Dclose(d);
                closeH5();
                db.close();
                db = QSqlDatabase();
                QFile::remove(targetDbPath);
                return false;
            }
            ++written;
        }
        emit progress(written, totalRows);
    }

    if (!db.commit()) {
        db.rollback();
        for (hid_t d : allDs) H5Dclose(d);
        closeH5();
        db.close();
        db = QSqlDatabase();
        QFile::remove(targetDbPath);
        return false;
    }

    for (hid_t d : allDs) H5Dclose(d);
    closeH5();
    db.close();
    db = QSqlDatabase();
    emit progress(written, written);
    return true;
}

bool HistoryImportService::importPulseEddyHdf5(const QString& filePath, const QString& targetDbPath)
{
    if (!QFile::exists(filePath)) return false;

    hid_t fileId = H5Fopen(filePath.toLocal8Bit().constData(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fileId < 0) return false;
    auto closeH5 = [&]() { if (fileId >= 0) { H5Fclose(fileId); fileId = -1; } };

    auto openDs = [&](const char* name) -> hid_t {
        if (H5Lexists(fileId, name, H5P_DEFAULT) <= 0) return -1;
        return H5Dopen2(fileId, name, H5P_DEFAULT);
    };

    hid_t dsTs   = openDs("/frames/timestamp_ms_utc");
    hid_t dsFi   = openDs("/frames/frame_index");
    hid_t dsSc   = openDs("/frames/sample_count");
    hid_t dsSr   = openDs("/frames/sample_rate_hz");
    hid_t dsMin  = openDs("/frames/min_value");
    hid_t dsMax  = openDs("/frames/max_value");
    hid_t dsRef  = openDs("/frames/has_reference");
    hid_t dsRaw  = openDs("/pulse/raw_values");
    hid_t dsRefV = openDs("/pulse/reference_values");
    const hid_t allDs[] = { dsTs, dsFi, dsSc, dsSr, dsMin, dsMax, dsRef, dsRaw, dsRefV };
    constexpr int nAllDs = sizeof(allDs) / sizeof(allDs[0]);
    for (int i = 0; i < nAllDs; ++i) {
        if (allDs[i] < 0) {
            for (int j = 0; j < nAllDs; ++j) if (allDs[j] >= 0) H5Dclose(allDs[j]);
            closeH5(); return false;
        }
    }

    hid_t tsSpace = H5Dget_space(dsTs);
    hsize_t dims[1] = {0};
    H5Sget_simple_extent_dims(tsSpace, dims, nullptr);
    H5Sclose(tsSpace);
    const qint64 totalRows = static_cast<qint64>(dims[0]);

    hid_t rawSpace = H5Dget_space(dsRaw);
    hsize_t rawDims[2] = {0, 0};
    H5Sget_simple_extent_dims(rawSpace, rawDims, nullptr);
    H5Sclose(rawSpace);
    const int nSamp = static_cast<int>(rawDims[1]);

    emit progress(0, totalRows);

    QDir().mkpath(QFileInfo(targetDbPath).absolutePath());
    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(targetDbPath);
    if (!db.open()) { for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); return false; }

    QString schemaError;
    if (!initTargetDb(db, &schemaError)) { for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    if (!RealtimeSqlRecorder::ensurePulseEddyFramesSchema(db, &schemaError)) {
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }

    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM pulse_eddy_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(QStringLiteral(
            "INSERT INTO pulse_eddy_frames(timestamp_unix_ms, frame_index, "
            "sample_count, sample_rate_hz, raw_values, has_reference, reference_values, min_value, max_value) "
            "VALUES(?,?,?,?,?,?,?,?,?)")))
    {
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }
    if (!db.transaction()) {
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }

    const qint64 chunkSize = qMax(64, m_request.chunkSize);
    QVector<qint64>  bTs, bFi;
    QVector<qint32>  bSc;
    QVector<double>  bSr, bMn, bMx;
    QVector<quint8>  bRef;
    QVector<double>  bRaw, bRefV;

    qint64 written = 0;
    for (qint64 offset = 0; offset < totalRows; offset += chunkSize) {
        if (m_canceled.loadRelaxed() == 1) {
            db.rollback();
            for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
        }
        const qint64 n = qMin(chunkSize, totalRows - offset);
        bTs.resize(n); bFi.resize(n); bSc.resize(n); bSr.resize(n); bMn.resize(n); bMx.resize(n); bRef.resize(n);
        bRaw.resize(n * nSamp); bRefV.resize(n * nSamp);

        if (!readHyperslab1D(dsTs,  H5T_NATIVE_INT64,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bTs.data()) ||
            !readHyperslab1D(dsFi,  H5T_NATIVE_INT64,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bFi.data()) ||
            !readHyperslab1D(dsSc,  H5T_NATIVE_INT32,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bSc.data()) ||
            !readHyperslab1D(dsSr,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bSr.data()) ||
            !readHyperslab1D(dsMin, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bMn.data()) ||
            !readHyperslab1D(dsMax, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bMx.data()) ||
            !readHyperslab1D(dsRef, H5T_NATIVE_UINT8,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bRef.data()) ||
            !readHyperslab2D(dsRaw,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), static_cast<hsize_t>(nSamp), bRaw.data()) ||
            !readHyperslab2D(dsRefV, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), static_cast<hsize_t>(nSamp), bRefV.data()))
        {
            db.rollback();
            for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
        }

        for (qint64 r = 0; r < n; ++r) {
            QByteArray rawBa(reinterpret_cast<const char*>(bRaw.data() + r * nSamp), nSamp * static_cast<int>(sizeof(double)));
            QByteArray refBa(reinterpret_cast<const char*>(bRefV.data() + r * nSamp), nSamp * static_cast<int>(sizeof(double)));
            insertQuery.bindValue(0, bTs[r]);
            insertQuery.bindValue(1, bFi[r]);
            insertQuery.bindValue(2, static_cast<int>(bSc[r]));
            insertQuery.bindValue(3, bSr[r]);
            insertQuery.bindValue(4, rawBa);
            insertQuery.bindValue(5, static_cast<int>(bRef[r]));
            insertQuery.bindValue(6, static_cast<int>(bRef[r]) ? QVariant(refBa) : QVariant(QByteArray()));
            insertQuery.bindValue(7, std::isfinite(bMn[r]) ? QVariant(bMn[r]) : QVariant());
            insertQuery.bindValue(8, std::isfinite(bMx[r]) ? QVariant(bMx[r]) : QVariant());
            if (!insertQuery.exec()) {
                db.rollback();
                for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
            }
            ++written;
        }
        emit progress(written, totalRows);
    }

    if (!db.commit()) {
        db.rollback();
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }

    for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]);
    closeH5();
    db.close();
    db = QSqlDatabase();
    emit progress(written, written);
    return true;
}

bool HistoryImportService::importMagArrayHdf5(const QString& filePath, const QString& targetDbPath)
{
    if (!QFile::exists(filePath)) return false;

    hid_t fileId = H5Fopen(filePath.toLocal8Bit().constData(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fileId < 0) return false;
    auto closeH5 = [&]() { if (fileId >= 0) { H5Fclose(fileId); fileId = -1; } };

    auto openDs = [&](const char* name) -> hid_t {
        if (H5Lexists(fileId, name, H5P_DEFAULT) <= 0) return -1;
        return H5Dopen2(fileId, name, H5P_DEFAULT);
    };

    hid_t dsTs   = openDs("/frames/timestamp_ms_utc");
    hid_t dsFi   = openDs("/frames/frame_index");
    hid_t dsSi   = openDs("/magarray/sensor_index");
    hid_t dsXM   = openDs("/magarray/x_mean");
    hid_t dsYM   = openDs("/magarray/y_mean");
    hid_t dsZM   = openDs("/magarray/z_mean");
    hid_t dsXL   = openDs("/magarray/x_latest");
    hid_t dsYL   = openDs("/magarray/y_latest");
    hid_t dsZL   = openDs("/magarray/z_latest");
    hid_t dsMagM = openDs("/magarray/magnitude_mean");
    hid_t dsMagL = openDs("/magarray/magnitude_latest");
    hid_t dsPvX  = openDs("/magarray/processed_value_x");
    hid_t dsPvY  = openDs("/magarray/processed_value_y");
    hid_t dsPvZ  = openDs("/magarray/processed_value_z");
    const hid_t allDs[] = { dsTs, dsFi, dsSi, dsXM, dsYM, dsZM, dsXL, dsYL, dsZL,
                            dsMagM, dsMagL, dsPvX, dsPvY, dsPvZ };
    constexpr int nAllDs = sizeof(allDs) / sizeof(allDs[0]);
    for (int i = 0; i < nAllDs; ++i) {
        if (allDs[i] < 0) {
            for (int j = 0; j < nAllDs; ++j) if (allDs[j] >= 0) H5Dclose(allDs[j]);
            closeH5(); return false;
        }
    }

    hid_t tsSpace = H5Dget_space(dsTs);
    hsize_t dims[1] = {0};
    H5Sget_simple_extent_dims(tsSpace, dims, nullptr);
    H5Sclose(tsSpace);
    const qint64 totalRows = static_cast<qint64>(dims[0]);
    emit progress(0, totalRows);

    QDir().mkpath(QFileInfo(targetDbPath).absolutePath());
    const QString connectionName = buildUniqueConnectionName();
    ScopedSqlConnectionCleanup connectionCleanup(connectionName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(targetDbPath);
    if (!db.open()) { for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); return false; }

    QString schemaError;
    if (!initTargetDb(db, &schemaError)) { for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false; }
    if (!RealtimeSqlRecorder::ensureMagArrayFramesSchema(db, &schemaError)) {
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }

    QSqlQuery clearQuery(db);
    clearQuery.exec(QStringLiteral("DELETE FROM mag_array_frames"));
    QSqlQuery insertQuery(db);
    if (!insertQuery.prepare(QStringLiteral(
            "INSERT INTO mag_array_frames(timestamp_unix_ms, frame_index, sensor_index, "
            "x_mean, y_mean, z_mean, x_latest, y_latest, z_latest, "
            "magnitude_mean, magnitude_latest, "
            "processed_value_x, processed_value_y, processed_value_z) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)")))
    {
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }
    if (!db.transaction()) {
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }

    const qint64 chunkSize = qMax(256, m_request.chunkSize);
    QVector<qint64>  bTs, bFi;
    QVector<qint32>  bSi;
    QVector<double>  bXM, bYM, bZM, bXL, bYL, bZL, bMM, bML, bPX, bPY, bPZ;

    qint64 written = 0;
    for (qint64 offset = 0; offset < totalRows; offset += chunkSize) {
        if (m_canceled.loadRelaxed() == 1) {
            db.rollback();
            for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
        }
        const qint64 n = qMin(chunkSize, totalRows - offset);
        bTs.resize(n); bFi.resize(n); bSi.resize(n);
        bXM.resize(n); bYM.resize(n); bZM.resize(n); bXL.resize(n); bYL.resize(n); bZL.resize(n);
        bMM.resize(n); bML.resize(n); bPX.resize(n); bPY.resize(n); bPZ.resize(n);

        if (!readHyperslab1D(dsTs,   H5T_NATIVE_INT64,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bTs.data()) ||
            !readHyperslab1D(dsFi,   H5T_NATIVE_INT64,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bFi.data()) ||
            !readHyperslab1D(dsSi,   H5T_NATIVE_INT32,  static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bSi.data()) ||
            !readHyperslab1D(dsXM,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bXM.data()) ||
            !readHyperslab1D(dsYM,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bYM.data()) ||
            !readHyperslab1D(dsZM,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bZM.data()) ||
            !readHyperslab1D(dsXL,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bXL.data()) ||
            !readHyperslab1D(dsYL,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bYL.data()) ||
            !readHyperslab1D(dsZL,   H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bZL.data()) ||
            !readHyperslab1D(dsMagM, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bMM.data()) ||
            !readHyperslab1D(dsMagL, H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bML.data()) ||
            !readHyperslab1D(dsPvX,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bPX.data()) ||
            !readHyperslab1D(dsPvY,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bPY.data()) ||
            !readHyperslab1D(dsPvZ,  H5T_NATIVE_DOUBLE, static_cast<hsize_t>(offset), static_cast<hsize_t>(n), bPZ.data()))
        {
            db.rollback();
            for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
        }

        for (qint64 r = 0; r < n; ++r) {
            insertQuery.bindValue(0,  bTs[r]);
            insertQuery.bindValue(1,  bFi[r]);
            insertQuery.bindValue(2,  static_cast<int>(bSi[r]));
            insertQuery.bindValue(3,  std::isfinite(bXM[r])   ? QVariant(bXM[r])   : QVariant());
            insertQuery.bindValue(4,  std::isfinite(bYM[r])   ? QVariant(bYM[r])   : QVariant());
            insertQuery.bindValue(5,  std::isfinite(bZM[r])   ? QVariant(bZM[r])   : QVariant());
            insertQuery.bindValue(6,  std::isfinite(bXL[r])   ? QVariant(bXL[r])   : QVariant());
            insertQuery.bindValue(7,  std::isfinite(bYL[r])   ? QVariant(bYL[r])   : QVariant());
            insertQuery.bindValue(8,  std::isfinite(bZL[r])   ? QVariant(bZL[r])   : QVariant());
            insertQuery.bindValue(9,  std::isfinite(bMM[r])   ? QVariant(bMM[r])   : QVariant());
            insertQuery.bindValue(10, std::isfinite(bML[r])   ? QVariant(bML[r])   : QVariant());
            insertQuery.bindValue(11, std::isfinite(bPX[r])   ? QVariant(bPX[r])   : QVariant());
            insertQuery.bindValue(12, std::isfinite(bPY[r])   ? QVariant(bPY[r])   : QVariant());
            insertQuery.bindValue(13, std::isfinite(bPZ[r])   ? QVariant(bPZ[r])   : QVariant());
            if (!insertQuery.exec()) {
                db.rollback();
                for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
            }
            ++written;
        }
        emit progress(written, totalRows);
    }

    if (!db.commit()) {
        db.rollback();
        for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]); closeH5(); db.close(); db = QSqlDatabase(); QFile::remove(targetDbPath); return false;
    }

    for (int j=0;j<nAllDs;++j) H5Dclose(allDs[j]);
    closeH5();
    db.close();
    db = QSqlDatabase();
    emit progress(written, written);
    return true;
}
#else
bool HistoryImportService::importHdf5(QString* errorMessage)
{
    if (errorMessage) {
        *errorMessage = QStringLiteral("当前构建未启用 HDF5 支持");
    }
    return false;
}

bool HistoryImportService::importMultiFreqHdf5(const QString& filePath, const QString& targetDbPath)
{
    Q_UNUSED(filePath); Q_UNUSED(targetDbPath);
    return false;
}

bool HistoryImportService::importPulseEddyHdf5(const QString& filePath, const QString& targetDbPath)
{
    Q_UNUSED(filePath); Q_UNUSED(targetDbPath);
    return false;
}

bool HistoryImportService::importMagArrayHdf5(const QString& filePath, const QString& targetDbPath)
{
    Q_UNUSED(filePath); Q_UNUSED(targetDbPath);
    return false;
}
#endif

