#include "HistoryExportService.h"

#include "SqlHistoryQuery.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QVariant>
#include <QVector>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef HAS_HDF5
#include <hdf5.h>
#endif

namespace {

constexpr int kChannelCount = SqlHistoryQuery::kAlignedChannelCount;
constexpr int kSchemaVersion = 3;

#ifdef HAS_HDF5
constexpr hsize_t kSourceTagFixedLen = 16; // "device" / "mock" 足够

hid_t createChunkedDataset1D(hid_t parent,
                             const char* name,
                             hid_t type,
                             hsize_t chunkRows)
{
    const hsize_t dims[1] = { 0 };
    const hsize_t maxDims[1] = { H5S_UNLIMITED };
    hid_t space = H5Screate_simple(1, dims, maxDims);
    if (space < 0) return -1;

    hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
    const hsize_t chunk[1] = { chunkRows };
    H5Pset_chunk(plist, 1, chunk);

    hid_t ds = H5Dcreate2(parent, name, type, space, H5P_DEFAULT, plist, H5P_DEFAULT);
    H5Pclose(plist);
    H5Sclose(space);
    return ds;
}

hid_t createChunkedDataset2D(hid_t parent,
                             const char* name,
                             hid_t type,
                             hsize_t chunkRows,
                             hsize_t cols)
{
    const hsize_t dims[2] = { 0, cols };
    const hsize_t maxDims[2] = { H5S_UNLIMITED, cols };
    hid_t space = H5Screate_simple(2, dims, maxDims);
    if (space < 0) return -1;

    hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
    const hsize_t chunk[2] = { chunkRows, cols };
    H5Pset_chunk(plist, 2, chunk);

    hid_t ds = H5Dcreate2(parent, name, type, space, H5P_DEFAULT, plist, H5P_DEFAULT);
    H5Pclose(plist);
    H5Sclose(space);
    return ds;
}

bool appendDataset1D(hid_t ds,
                     hid_t memType,
                     qint64 offsetRows,
                     qint64 count,
                     const void* buf)
{
    if (count <= 0) return true;
    const hsize_t newDims[1] = { static_cast<hsize_t>(offsetRows + count) };
    if (H5Dset_extent(ds, newDims) < 0) return false;

    hid_t fileSpace = H5Dget_space(ds);
    const hsize_t start[1] = { static_cast<hsize_t>(offsetRows) };
    const hsize_t cnt[1]   = { static_cast<hsize_t>(count) };
    if (H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, cnt, nullptr) < 0) {
        H5Sclose(fileSpace);
        return false;
    }

    const hsize_t memDims[1] = { static_cast<hsize_t>(count) };
    hid_t memSpace = H5Screate_simple(1, memDims, nullptr);
    const herr_t status = H5Dwrite(ds, memType, memSpace, fileSpace, H5P_DEFAULT, buf);
    H5Sclose(memSpace);
    H5Sclose(fileSpace);
    return status >= 0;
}

bool appendDataset2D(hid_t ds,
                     hid_t memType,
                     qint64 offsetRows,
                     qint64 count,
                     qint64 cols,
                     const void* buf)
{
    if (count <= 0) return true;
    const hsize_t newDims[2] = { static_cast<hsize_t>(offsetRows + count), static_cast<hsize_t>(cols) };
    if (H5Dset_extent(ds, newDims) < 0) return false;

    hid_t fileSpace = H5Dget_space(ds);
    const hsize_t start[2] = { static_cast<hsize_t>(offsetRows), 0 };
    const hsize_t cnt[2]   = { static_cast<hsize_t>(count), static_cast<hsize_t>(cols) };
    if (H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, cnt, nullptr) < 0) {
        H5Sclose(fileSpace);
        return false;
    }

    const hsize_t memDims[2] = { static_cast<hsize_t>(count), static_cast<hsize_t>(cols) };
    hid_t memSpace = H5Screate_simple(2, memDims, nullptr);
    const herr_t status = H5Dwrite(ds, memType, memSpace, fileSpace, H5P_DEFAULT, buf);
    H5Sclose(memSpace);
    H5Sclose(fileSpace);
    return status >= 0;
}

void writeStringAttr(hid_t target, const char* name, const QString& value)
{
    const QByteArray ba = value.toUtf8();
    hid_t type = H5Tcopy(H5T_C_S1);
    H5Tset_size(type, static_cast<size_t>(ba.size() + 1));
    H5Tset_cset(type, H5T_CSET_UTF8);
    H5Tset_strpad(type, H5T_STR_NULLTERM);

    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(target, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr >= 0) {
        H5Awrite(attr, type, ba.constData());
        H5Aclose(attr);
    }
    H5Sclose(space);
    H5Tclose(type);
}

void writeInt64Attr(hid_t target, const char* name, qint64 value)
{
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(target, name, H5T_STD_I64LE, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr >= 0) {
        H5Awrite(attr, H5T_NATIVE_INT64, &value);
        H5Aclose(attr);
    }
    H5Sclose(space);
}

void writeInt32Attr(hid_t target, const char* name, qint32 value)
{
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(target, name, H5T_STD_I32LE, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr >= 0) {
        H5Awrite(attr, H5T_NATIVE_INT32, &value);
        H5Aclose(attr);
    }
    H5Sclose(space);
}
#endif // HAS_HDF5

/// QVariant -> double（NULL -> NaN）
inline double variantToDouble(const QVariant& v)
{
    if (v.isNull() || !v.isValid()) return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok) return std::numeric_limits<double>::quiet_NaN();
    return d;
}

/// QVariant -> int32（NULL -> sentinel -1）
inline qint32 variantToInt32(const QVariant& v)
{
    if (v.isNull() || !v.isValid()) return -1;
    bool ok = false;
    const int i = v.toInt(&ok);
    if (!ok) return -1;
    return static_cast<qint32>(i);
}

} // namespace

HistoryExportService::HistoryExportService(QObject* parent)
    : QObject(parent)
{
}

HistoryExportService::~HistoryExportService() = default;

void HistoryExportService::cancel()
{
    m_canceled.storeRelaxed(1);
}

void HistoryExportService::run()
{
    m_canceled.storeRelaxed(0);

    if (m_request.dbPath.isEmpty() || m_request.outputPath.isEmpty()) {
        emit finished(false, QStringLiteral("导出请求参数不完整"));
        return;
    }
    if (m_request.endMs < m_request.startMs) {
        emit finished(false, QStringLiteral("结束时间不能早于开始时间"));
        return;
    }

    QString errorMessage;
    bool ok = false;
    switch (m_request.format) {
    case Format::Csv:
        ok = exportCsv(&errorMessage);
        break;
    case Format::Hdf5:
#ifdef HAS_HDF5
        ok = exportHdf5(&errorMessage);
#else
        errorMessage = QStringLiteral("当前构建未启用 HDF5 支持，请选择 CSV 格式");
        ok = false;
#endif
        break;
    }

    if (!ok && m_canceled.loadRelaxed() == 1) {
        // 取消路径：明确返回 cancel 消息；ok=false + message="已取消"
        emit finished(false, QStringLiteral("已取消"));
        return;
    }
    emit finished(ok, errorMessage);
}

bool HistoryExportService::exportCsv(QString* errorMessage)
{
    QFileInfo fileInfo(m_request.outputPath);
    QDir().mkpath(fileInfo.absolutePath());

    QFile file(m_request.outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法写入 CSV 文件: %1").arg(file.errorString());
        return false;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream.setGenerateByteOrderMark(false);

    const qint64 exportAt = QDateTime::currentMSecsSinceEpoch();
    stream << "# schema_version=" << kSchemaVersion << "\n";
    stream << "# exported_at_ms=" << exportAt << "\n";
    stream << "# source=" << m_request.sourceTag << "\n";
    stream << "# source_mode=" << m_request.sourceMode << "\n";
    stream << "# range_start_ms=" << m_request.startMs << "\n";
    stream << "# range_end_ms=" << m_request.endMs << "\n";
    stream << "# detect_mode=0:Legacy,1:MultiChannelReal,2:MultiChannelComplex\n";
    stream << "# stage_pose=reserved_empty(schema_v3_no_db_column, kept for forward compatibility)\n";
    stream << "# frame_id_note=not_stored_in_db\n";

    // 列头
    stream << "timestamp_ms_utc,frame_sequence,detect_mode,cell_count,source_tag";
    for (int i = 0; i < kChannelCount; ++i) {
        const QString p = QStringLiteral("pos%1").arg(i, 2, 10, QLatin1Char('0'));
        stream << "," << p << "_amp"
               << "," << p << "_phase"
               << "," << p << "_x"
               << "," << p << "_y"
               << "," << p << "_source_channel";
    }
    stream << ",has_stage,stage_ts_ms,stage_x_mm,stage_y_mm,stage_z_mm"
              ",stage_x_pulse,stage_y_pulse,stage_z_pulse\n";

    SqlHistoryQuery query;
    if (!query.open(m_request.dbPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("打开数据库失败: %1").arg(m_request.dbPath);
        file.close();
        QFile::remove(m_request.outputPath);
        return false;
    }

    qint64 lastTs = m_request.startMs - 1;
    qint64 lastRowId = std::numeric_limits<qint64>::min();
    qint64 written = 0;
    qint64 reportedTotal = m_request.estimatedTotal;

    emit progress(0, reportedTotal);

    QVector<SqlHistoryQuery::AlignedFrameRow> chunk;
    while (true) {
        if (m_canceled.loadRelaxed() == 1) {
            file.close();
            QFile::remove(m_request.outputPath);
            query.close();
            return false;
        }

        QString err;
        if (!query.fetchRawChunk(m_request.startMs, m_request.endMs,
                                 lastTs, lastRowId, m_request.chunkSize, &chunk, &err)) {
            if (errorMessage) *errorMessage = QStringLiteral("读取数据库失败: %1").arg(err);
            file.close();
            QFile::remove(m_request.outputPath);
            query.close();
            return false;
        }
        if (chunk.isEmpty()) {
            break;
        }

        for (const auto& row : chunk) {
            stream << row.timestampMs << ','
                   << row.frameSequence << ','
                   << row.detectMode << ','
                   << row.cellCount << ',';
            // source_tag：写入时转义双引号与逗号。典型值为 'device' / 'mock'，直接包一层双引号足够。
            const QString tag = row.sourceTag;
            if (tag.contains(',') || tag.contains('"') || tag.contains('\n')) {
                QString escaped = tag;
                escaped.replace('"', QStringLiteral("\"\""));
                stream << '"' << escaped << '"';
            } else {
                stream << tag;
            }

            for (int i = 0; i < kChannelCount; ++i) {
                auto writeDouble = [&](const QVariant& v) {
                    if (v.isNull() || !v.isValid()) {
                        stream << ',';
                        return;
                    }
                    bool ok = false;
                    const double d = v.toDouble(&ok);
                    if (!ok || !std::isfinite(d)) {
                        stream << ',';
                    } else {
                        stream << ',' << QString::number(d, 'g', 15);
                    }
                };
                auto writeInt = [&](const QVariant& v) {
                    if (v.isNull() || !v.isValid()) {
                        stream << ',';
                        return;
                    }
                    stream << ',' << v.toInt();
                };
                writeDouble(row.amp[i]);
                writeDouble(row.phase[i]);
                writeDouble(row.x[i]);
                writeDouble(row.y[i]);
                writeInt(row.sourceChannel[i]);
            }

            // stage_pose 占位恒空
            stream << ",0,0,0,0,0,0,0,0\n";
        }

        written += chunk.size();
        lastTs = chunk.last().timestampMs;
        lastRowId = chunk.last().rowId;

        // 估算可能偏低：若已写超过估算 80%，将 total 向上放宽（至少 written * 1.5）避免进度条卡顶。
        if (reportedTotal > 0 && written > reportedTotal * 8 / 10) {
            reportedTotal = qMax(reportedTotal, written * 3 / 2);
        }
        emit progress(written, reportedTotal);

        if (chunk.size() < m_request.chunkSize) {
            break; // 末尾不满一 chunk，已读完
        }
    }

    stream.flush();
    file.close();
    query.close();

    // 精确结束值
    emit progress(written, written);
    return true;
}

#ifdef HAS_HDF5
bool HistoryExportService::exportHdf5(QString* errorMessage)
{
    QFileInfo fileInfo(m_request.outputPath);
    QDir().mkpath(fileInfo.absolutePath());

    const int chunkRows = qMax(1024, m_request.chunkSize);

    hid_t fileId = H5Fcreate(m_request.outputPath.toLocal8Bit().constData(),
                             H5F_ACC_TRUNC,
                             H5P_DEFAULT,
                             H5P_DEFAULT);
    if (fileId < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("创建 HDF5 文件失败");
        return false;
    }

    auto cleanupAndRemove = [&]() {
        if (fileId >= 0) H5Fclose(fileId);
        QFile::remove(m_request.outputPath);
    };

    // 组
    hid_t framesGroup   = H5Gcreate2(fileId, "/frames",     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    hid_t channelsGroup = H5Gcreate2(fileId, "/channels",   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    hid_t stageGroup    = H5Gcreate2(fileId, "/stage_pose", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (framesGroup < 0 || channelsGroup < 0 || stageGroup < 0) {
        if (errorMessage) *errorMessage = QStringLiteral("创建 HDF5 分组失败");
        if (framesGroup   >= 0) H5Gclose(framesGroup);
        if (channelsGroup >= 0) H5Gclose(channelsGroup);
        if (stageGroup    >= 0) H5Gclose(stageGroup);
        cleanupAndRemove();
        return false;
    }

    // source_tag 固定 16B 字符串类型
    hid_t tagType = H5Tcopy(H5T_C_S1);
    H5Tset_size(tagType, kSourceTagFixedLen);
    H5Tset_strpad(tagType, H5T_STR_NULLPAD);
    H5Tset_cset(tagType, H5T_CSET_UTF8);

    // 1D 数据集（/frames）
    hid_t dsTs    = createChunkedDataset1D(framesGroup, "timestamp_ms_utc", H5T_STD_I64LE, chunkRows);
    hid_t dsSeq   = createChunkedDataset1D(framesGroup, "frame_sequence",   H5T_STD_I64LE, chunkRows);
    hid_t dsMode  = createChunkedDataset1D(framesGroup, "detect_mode",      H5T_STD_U8LE,  chunkRows);
    hid_t dsCount = createChunkedDataset1D(framesGroup, "cell_count",       H5T_STD_U8LE,  chunkRows);
    hid_t dsTag   = createChunkedDataset1D(framesGroup, "source_tag",       tagType,       chunkRows);

    // 2D 数据集（/channels）
    hid_t dsAmp   = createChunkedDataset2D(channelsGroup, "amp",            H5T_IEEE_F64LE, chunkRows, kChannelCount);
    hid_t dsPhase = createChunkedDataset2D(channelsGroup, "phase",          H5T_IEEE_F64LE, chunkRows, kChannelCount);
    hid_t dsX     = createChunkedDataset2D(channelsGroup, "x",              H5T_IEEE_F64LE, chunkRows, kChannelCount);
    hid_t dsY     = createChunkedDataset2D(channelsGroup, "y",              H5T_IEEE_F64LE, chunkRows, kChannelCount);
    hid_t dsSrc   = createChunkedDataset2D(channelsGroup, "source_channel", H5T_STD_I32LE,  chunkRows, kChannelCount);

    // stage_pose 占位（全 0）
    hid_t dsStageHas   = createChunkedDataset1D(stageGroup, "has_stage",            H5T_STD_U8LE,  chunkRows);
    hid_t dsStageTs    = createChunkedDataset1D(stageGroup, "stage_timestamp_ms_utc", H5T_STD_I64LE, chunkRows);
    hid_t dsStageXMm   = createChunkedDataset1D(stageGroup, "x_mm",                  H5T_IEEE_F64LE, chunkRows);
    hid_t dsStageYMm   = createChunkedDataset1D(stageGroup, "y_mm",                  H5T_IEEE_F64LE, chunkRows);
    hid_t dsStageZMm   = createChunkedDataset1D(stageGroup, "z_mm",                  H5T_IEEE_F64LE, chunkRows);
    hid_t dsStageXPls  = createChunkedDataset1D(stageGroup, "x_pulse",               H5T_STD_I64LE, chunkRows);
    hid_t dsStageYPls  = createChunkedDataset1D(stageGroup, "y_pulse",               H5T_STD_I64LE, chunkRows);
    hid_t dsStageZPls  = createChunkedDataset1D(stageGroup, "z_pulse",               H5T_STD_I64LE, chunkRows);

    H5Gclose(framesGroup);
    H5Gclose(channelsGroup);
    H5Gclose(stageGroup);

    const hid_t datasets[] = { dsTs, dsSeq, dsMode, dsCount, dsTag,
                               dsAmp, dsPhase, dsX, dsY, dsSrc,
                               dsStageHas, dsStageTs,
                               dsStageXMm, dsStageYMm, dsStageZMm,
                               dsStageXPls, dsStageYPls, dsStageZPls };
    for (hid_t d : datasets) {
        if (d < 0) {
            if (errorMessage) *errorMessage = QStringLiteral("创建 HDF5 数据集失败");
            for (hid_t dd : datasets) if (dd >= 0) H5Dclose(dd);
            H5Tclose(tagType);
            cleanupAndRemove();
            return false;
        }
    }

    // 文件级属性
    writeInt32Attr(fileId, "schema_version", kSchemaVersion);
    writeInt64Attr(fileId, "exported_at_ms", QDateTime::currentMSecsSinceEpoch());
    writeInt64Attr(fileId, "range_start_ms", m_request.startMs);
    writeInt64Attr(fileId, "range_end_ms",   m_request.endMs);
    writeStringAttr(fileId, "source",        m_request.sourceTag);
    writeStringAttr(fileId, "source_mode",   m_request.sourceMode);
    writeStringAttr(fileId, "stage_pose_note",
                    QStringLiteral("reserved_empty(schema_v3_no_db_column, kept for forward compatibility)"));
    writeStringAttr(fileId, "frame_id_note", QStringLiteral("not_stored_in_db"));

    // 读取 + 批量写入
    SqlHistoryQuery query;
    if (!query.open(m_request.dbPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("打开数据库失败: %1").arg(m_request.dbPath);
        for (hid_t d : datasets) H5Dclose(d);
        H5Tclose(tagType);
        cleanupAndRemove();
        return false;
    }

    qint64 lastTs = m_request.startMs - 1;
    qint64 lastRowId = std::numeric_limits<qint64>::min();
    qint64 offset = 0;
    qint64 reportedTotal = m_request.estimatedTotal;

    emit progress(0, reportedTotal);

    QVector<SqlHistoryQuery::AlignedFrameRow> chunk;
    // 持久 buffer：每轮 resize 到当前 chunk 大小
    QVector<qint64>  bufTs;
    QVector<qint64>  bufSeq;
    QVector<quint8>  bufMode;
    QVector<quint8>  bufCount;
    QByteArray       bufTag;
    QVector<double>  bufAmp;
    QVector<double>  bufPhase;
    QVector<double>  bufX;
    QVector<double>  bufY;
    QVector<qint32>  bufSrc;
    QVector<quint8>  bufStageHas;
    QVector<qint64>  bufStageTs;
    QVector<double>  bufStageZero;
    QVector<qint64>  bufStageZeroI;

    bool success = true;
    QString err;

    while (true) {
        if (m_canceled.loadRelaxed() == 1) {
            success = false;
            break;
        }

        if (!query.fetchRawChunk(m_request.startMs, m_request.endMs,
                                 lastTs, lastRowId, m_request.chunkSize, &chunk, &err)) {
            if (errorMessage) *errorMessage = QStringLiteral("读取数据库失败: %1").arg(err);
            success = false;
            break;
        }
        if (chunk.isEmpty()) {
            break;
        }

        const qint64 n = chunk.size();
        bufTs.resize(n);
        bufSeq.resize(n);
        bufMode.resize(n);
        bufCount.resize(n);
        bufTag.fill('\0', static_cast<int>(n * kSourceTagFixedLen));
        bufAmp.resize(n * kChannelCount);
        bufPhase.resize(n * kChannelCount);
        bufX.resize(n * kChannelCount);
        bufY.resize(n * kChannelCount);
        bufSrc.resize(n * kChannelCount);
        bufStageHas.fill(0, n);
        bufStageTs.fill(0, n);
        bufStageZero.fill(0.0, n);
        bufStageZeroI.fill(0, n);

        for (int r = 0; r < n; ++r) {
            const auto& row = chunk[r];
            bufTs[r]    = row.timestampMs;
            bufSeq[r]   = row.frameSequence;
            bufMode[r]  = static_cast<quint8>(row.detectMode & 0xFF);
            bufCount[r] = static_cast<quint8>(row.cellCount & 0xFF);

            const QByteArray tagBytes = row.sourceTag.toUtf8();
            const int copyLen = qMin<int>(tagBytes.size(), static_cast<int>(kSourceTagFixedLen));
            if (copyLen > 0) {
                std::memcpy(bufTag.data() + r * kSourceTagFixedLen, tagBytes.constData(), copyLen);
            }

            for (int i = 0; i < kChannelCount; ++i) {
                bufAmp  [r * kChannelCount + i] = variantToDouble(row.amp[i]);
                bufPhase[r * kChannelCount + i] = variantToDouble(row.phase[i]);
                bufX    [r * kChannelCount + i] = variantToDouble(row.x[i]);
                bufY    [r * kChannelCount + i] = variantToDouble(row.y[i]);
                bufSrc  [r * kChannelCount + i] = variantToInt32 (row.sourceChannel[i]);
            }
        }

        if (!appendDataset1D(dsTs,    H5T_NATIVE_INT64,  offset, n, bufTs.constData()) ||
            !appendDataset1D(dsSeq,   H5T_NATIVE_INT64,  offset, n, bufSeq.constData()) ||
            !appendDataset1D(dsMode,  H5T_NATIVE_UINT8,  offset, n, bufMode.constData()) ||
            !appendDataset1D(dsCount, H5T_NATIVE_UINT8,  offset, n, bufCount.constData()) ||
            !appendDataset1D(dsTag,   tagType,           offset, n, bufTag.constData()) ||
            !appendDataset2D(dsAmp,   H5T_NATIVE_DOUBLE, offset, n, kChannelCount, bufAmp.constData()) ||
            !appendDataset2D(dsPhase, H5T_NATIVE_DOUBLE, offset, n, kChannelCount, bufPhase.constData()) ||
            !appendDataset2D(dsX,     H5T_NATIVE_DOUBLE, offset, n, kChannelCount, bufX.constData()) ||
            !appendDataset2D(dsY,     H5T_NATIVE_DOUBLE, offset, n, kChannelCount, bufY.constData()) ||
            !appendDataset2D(dsSrc,   H5T_NATIVE_INT32,  offset, n, kChannelCount, bufSrc.constData()) ||
            !appendDataset1D(dsStageHas,  H5T_NATIVE_UINT8,  offset, n, bufStageHas.constData()) ||
            !appendDataset1D(dsStageTs,   H5T_NATIVE_INT64,  offset, n, bufStageTs.constData()) ||
            !appendDataset1D(dsStageXMm,  H5T_NATIVE_DOUBLE, offset, n, bufStageZero.constData()) ||
            !appendDataset1D(dsStageYMm,  H5T_NATIVE_DOUBLE, offset, n, bufStageZero.constData()) ||
            !appendDataset1D(dsStageZMm,  H5T_NATIVE_DOUBLE, offset, n, bufStageZero.constData()) ||
            !appendDataset1D(dsStageXPls, H5T_NATIVE_INT64,  offset, n, bufStageZeroI.constData()) ||
            !appendDataset1D(dsStageYPls, H5T_NATIVE_INT64,  offset, n, bufStageZeroI.constData()) ||
            !appendDataset1D(dsStageZPls, H5T_NATIVE_INT64,  offset, n, bufStageZeroI.constData()))
        {
            if (errorMessage) *errorMessage = QStringLiteral("写入 HDF5 数据集失败");
            success = false;
            break;
        }

        offset += n;
        lastTs = chunk.last().timestampMs;
        lastRowId = chunk.last().rowId;

        if (reportedTotal > 0 && offset > reportedTotal * 8 / 10) {
            reportedTotal = qMax(reportedTotal, offset * 3 / 2);
        }
        emit progress(offset, reportedTotal);

        if (n < m_request.chunkSize) {
            break;
        }
    }

    for (hid_t d : datasets) H5Dclose(d);
    H5Tclose(tagType);
    query.close();
    H5Fclose(fileId);
    fileId = -1;

    if (!success) {
        QFile::remove(m_request.outputPath);
        return false;
    }

    emit progress(offset, offset);
    return true;
}
#else
bool HistoryExportService::exportHdf5(QString* errorMessage)
{
    if (errorMessage) *errorMessage = QStringLiteral("当前构建未启用 HDF5 支持");
    return false;
}
#endif // HAS_HDF5
