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

    QString error;
    bool ok = false;
    switch (m_request.format) {
    case Format::Csv:
        ok = importCsv(&error);
        break;
    case Format::Hdf5:
#ifdef HAS_HDF5
        ok = importHdf5(&error);
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
#else
bool HistoryImportService::importHdf5(QString* errorMessage)
{
    if (errorMessage) {
        *errorMessage = QStringLiteral("当前构建未启用 HDF5 支持");
    }
    return false;
}
#endif

