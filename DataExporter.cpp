#include "DataExporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <limits>

#ifdef HAS_HDF5
#include <hdf5.h>
#endif

namespace {

int maxChannelCount(const QVector<FrameData>& frames)
{
    int maxCount = 0;
    for (const auto& frame : frames) {
        maxCount = qMax(maxCount, static_cast<int>(frame.channelCount));
        maxCount = qMax(maxCount, frame.channels_comp0.size());
        maxCount = qMax(maxCount, frame.channels_comp1.size());
    }
    return maxCount;
}

bool exportCsv(const QVector<FrameData>& frames,
               const DataExporter::ExportOptions& options,
               QString* errorMessage)
{
    QFileInfo fileInfo(options.filePath);
    QDir().mkpath(fileInfo.absolutePath());

    QFile file(options.filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("无法写入CSV文件: %1").arg(file.errorString());
        }
        return false;
    }

    QTextStream stream(&file);

    const int channels = maxChannelCount(frames);
    const qint64 exportAt = QDateTime::currentMSecsSinceEpoch();
    stream << "# schema_version=3\n";
    stream << "# exported_at_ms=" << exportAt << "\n";
    stream << "# source=" << options.sourceTag << "\n";
    stream << "# range_start_ms=" << options.startTimeMs << "\n";
    stream << "# range_end_ms=" << options.endTimeMs << "\n";
    stream << "# detect_mode_semantics=1:comp0=amplitude,comp1=phase;2:comp0=real,comp1=imag\n";
    stream << "# stage_pose=last_known_at_DUT_row; stage_*_mm=毫米; stage_*_pulse=下位机位置计数(非mm,见stage.proto AxisValue)\n";

    stream << "timestamp_ms_utc,frame_id,detect_mode,channel_count";
    for (int index = 0; index < channels; ++index) {
        stream << ",ch" << index << "_comp0";
    }
    for (int index = 0; index < channels; ++index) {
        stream << ",ch" << index << "_comp1";
    }
    stream << ",has_stage,stage_ts_ms,stage_x_mm,stage_y_mm,stage_z_mm,stage_x_pulse,stage_y_pulse,stage_z_pulse";
    stream << "\n";

    for (const auto& frame : frames) {
        stream << frame.timestamp << ","
               << frame.frameId << ","
               << static_cast<int>(frame.detectMode) << ","
               << static_cast<int>(frame.channelCount);

        for (int index = 0; index < channels; ++index) {
            if (index < frame.channels_comp0.size()) {
                stream << "," << QString::number(frame.channels_comp0[index], 'g', 15);
            } else {
                stream << ",";
            }
        }

        for (int index = 0; index < channels; ++index) {
            if (index < frame.channels_comp1.size()) {
                stream << "," << QString::number(frame.channels_comp1[index], 'g', 15);
            } else {
                stream << ",";
            }
        }

        if (frame.hasStagePose) {
            stream << ",1," << frame.stageTimestampMs << ","
                   << QString::number(frame.stageXMm, 'g', 15) << ","
                   << QString::number(frame.stageYMm, 'g', 15) << ","
                   << QString::number(frame.stageZMm, 'g', 15) << ","
                   << frame.stageXPulse << "," << frame.stageYPulse << "," << frame.stageZPulse;
        } else {
            stream << ",0,0,0,0,0,0,0,0";
        }

        stream << "\n";
    }

    file.close();
    return true;
}

#ifdef HAS_HDF5
template <typename T>
bool writeDataset1D(hid_t fileId,
                    const char* name,
                    hid_t type,
                    const QVector<T>& values,
                    QString* errorMessage)
{
    const hsize_t dims[1] = { static_cast<hsize_t>(values.size()) };
    hid_t spaceId = H5Screate_simple(1, dims, nullptr);
    if (spaceId < 0) {
        if (errorMessage) {
            *errorMessage = QString("创建HDF5数据空间失败: %1").arg(name);
        }
        return false;
    }

    hid_t dataId = H5Dcreate2(fileId, name, type, spaceId, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (dataId < 0) {
        H5Sclose(spaceId);
        if (errorMessage) {
            *errorMessage = QString("创建HDF5数据集失败: %1").arg(name);
        }
        return false;
    }

    herr_t writeStatus = H5Dwrite(dataId, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.constData());
    H5Dclose(dataId);
    H5Sclose(spaceId);
    if (writeStatus < 0) {
        if (errorMessage) {
            *errorMessage = QString("写入HDF5数据集失败: %1").arg(name);
        }
        return false;
    }

    return true;
}

bool writeDataset1DDouble(hid_t fileId,
                          const char* name,
                          const QVector<double>& values,
                          QString* errorMessage)
{
    const hsize_t dims[1] = { static_cast<hsize_t>(values.size()) };
    hid_t spaceId = H5Screate_simple(1, dims, nullptr);
    if (spaceId < 0) {
        if (errorMessage) {
            *errorMessage = QString("创建HDF5数据空间失败: %1").arg(name);
        }
        return false;
    }

    hid_t dataId = H5Dcreate2(fileId, name, H5T_IEEE_F64LE, spaceId, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (dataId < 0) {
        H5Sclose(spaceId);
        if (errorMessage) {
            *errorMessage = QString("创建HDF5数据集失败: %1").arg(name);
        }
        return false;
    }

    herr_t writeStatus = H5Dwrite(dataId, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.constData());
    H5Dclose(dataId);
    H5Sclose(spaceId);
    if (writeStatus < 0) {
        if (errorMessage) {
            *errorMessage = QString("写入HDF5数据集失败: %1").arg(name);
        }
        return false;
    }

    return true;
}

bool writeDataset2D(hid_t fileId,
                    const char* name,
                    const QVector<double>& values,
                    int rows,
                    int cols,
                    QString* errorMessage)
{
    const hsize_t dims[2] = {
        static_cast<hsize_t>(rows),
        static_cast<hsize_t>(cols)
    };

    hid_t spaceId = H5Screate_simple(2, dims, nullptr);
    if (spaceId < 0) {
        if (errorMessage) {
            *errorMessage = QString("创建HDF5二维数据空间失败: %1").arg(name);
        }
        return false;
    }

    hid_t dataId = H5Dcreate2(fileId, name, H5T_IEEE_F64LE, spaceId, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (dataId < 0) {
        H5Sclose(spaceId);
        if (errorMessage) {
            *errorMessage = QString("创建HDF5二维数据集失败: %1").arg(name);
        }
        return false;
    }

    herr_t writeStatus = H5Dwrite(dataId, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.constData());
    H5Dclose(dataId);
    H5Sclose(spaceId);
    if (writeStatus < 0) {
        if (errorMessage) {
            *errorMessage = QString("写入HDF5二维数据集失败: %1").arg(name);
        }
        return false;
    }

    return true;
}

bool exportHdf5(const QVector<FrameData>& frames,
                const DataExporter::ExportOptions& options,
                QString* errorMessage)
{
    QFileInfo fileInfo(options.filePath);
    QDir().mkpath(fileInfo.absolutePath());

    hid_t fileId = H5Fcreate(options.filePath.toLocal8Bit().constData(),
                             H5F_ACC_TRUNC,
                             H5P_DEFAULT,
                             H5P_DEFAULT);
    if (fileId < 0) {
        if (errorMessage) {
            *errorMessage = "创建HDF5文件失败";
        }
        return false;
    }

    QVector<qint64> timestamps;
    QVector<quint64> frameIds;
    QVector<quint8> detectModes;
    QVector<quint8> channelCounts;
    QVector<quint8> stageHas;
    QVector<qint64> stageTimestamps;
    QVector<double> stageXMm;
    QVector<double> stageYMm;
    QVector<double> stageZMm;
    QVector<qint64> stageXPulse;
    QVector<qint64> stageYPulse;
    QVector<qint64> stageZPulse;
    timestamps.reserve(frames.size());
    frameIds.reserve(frames.size());
    detectModes.reserve(frames.size());
    channelCounts.reserve(frames.size());
    stageHas.reserve(frames.size());
    stageTimestamps.reserve(frames.size());
    stageXMm.reserve(frames.size());
    stageYMm.reserve(frames.size());
    stageZMm.reserve(frames.size());
    stageXPulse.reserve(frames.size());
    stageYPulse.reserve(frames.size());
    stageZPulse.reserve(frames.size());

    const int channelMax = qMax(1, maxChannelCount(frames));
    QVector<double> comp0;
    QVector<double> comp1;
    comp0.resize(frames.size() * channelMax);
    comp1.resize(frames.size() * channelMax);

    const double nanValue = std::numeric_limits<double>::quiet_NaN();
    comp0.fill(nanValue);
    comp1.fill(nanValue);

    for (int row = 0; row < frames.size(); ++row) {
        const auto& frame = frames[row];
        timestamps.append(static_cast<qint64>(frame.timestamp));
        frameIds.append(frame.frameId);
        detectModes.append(static_cast<quint8>(frame.detectMode));
        channelCounts.append(frame.channelCount);

        for (int col = 0; col < frame.channels_comp0.size() && col < channelMax; ++col) {
            comp0[row * channelMax + col] = frame.channels_comp0[col];
        }
        for (int col = 0; col < frame.channels_comp1.size() && col < channelMax; ++col) {
            comp1[row * channelMax + col] = frame.channels_comp1[col];
        }

        stageHas.append(frame.hasStagePose ? static_cast<quint8>(1) : static_cast<quint8>(0));
        if (frame.hasStagePose) {
            stageTimestamps.append(frame.stageTimestampMs);
            stageXMm.append(frame.stageXMm);
            stageYMm.append(frame.stageYMm);
            stageZMm.append(frame.stageZMm);
            stageXPulse.append(frame.stageXPulse);
            stageYPulse.append(frame.stageYPulse);
            stageZPulse.append(frame.stageZPulse);
        } else {
            stageTimestamps.append(0);
            stageXMm.append(0.0);
            stageYMm.append(0.0);
            stageZMm.append(0.0);
            stageXPulse.append(0);
            stageYPulse.append(0);
            stageZPulse.append(0);
        }
    }

    hid_t framesGroup = H5Gcreate2(fileId, "/frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    hid_t channelsGroup = H5Gcreate2(fileId, "/channels", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (framesGroup < 0 || channelsGroup < 0) {
        if (framesGroup >= 0) {
            H5Gclose(framesGroup);
        }
        if (channelsGroup >= 0) {
            H5Gclose(channelsGroup);
        }
        H5Fclose(fileId);
        if (errorMessage) {
            *errorMessage = "创建HDF5分组失败";
        }
        return false;
    }
    H5Gclose(framesGroup);
    H5Gclose(channelsGroup);

    hid_t stagePoseGroup = H5Gcreate2(fileId, "/stage_pose", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (stagePoseGroup < 0) {
        H5Fclose(fileId);
        if (errorMessage) {
            *errorMessage = QStringLiteral("创建HDF5分组失败: /stage_pose");
        }
        return false;
    }
    H5Gclose(stagePoseGroup);

    if (!writeDataset1D(fileId, "/frames/timestamp_ms_utc", H5T_STD_I64LE, timestamps, errorMessage) ||
        !writeDataset1D(fileId, "/frames/frame_id", H5T_STD_U64LE, frameIds, errorMessage) ||
        !writeDataset1D(fileId, "/frames/detect_mode", H5T_STD_U8LE, detectModes, errorMessage) ||
        !writeDataset1D(fileId, "/frames/channel_count", H5T_STD_U8LE, channelCounts, errorMessage) ||
        !writeDataset2D(fileId, "/channels/comp0", comp0, frames.size(), channelMax, errorMessage) ||
        !writeDataset2D(fileId, "/channels/comp1", comp1, frames.size(), channelMax, errorMessage) ||
        !writeDataset1D(fileId, "/stage_pose/has_stage", H5T_STD_U8LE, stageHas, errorMessage) ||
        !writeDataset1D(fileId, "/stage_pose/stage_timestamp_ms_utc", H5T_STD_I64LE, stageTimestamps, errorMessage) ||
        !writeDataset1DDouble(fileId, "/stage_pose/x_mm", stageXMm, errorMessage) ||
        !writeDataset1DDouble(fileId, "/stage_pose/y_mm", stageYMm, errorMessage) ||
        !writeDataset1DDouble(fileId, "/stage_pose/z_mm", stageZMm, errorMessage) ||
        !writeDataset1D(fileId, "/stage_pose/x_pulse", H5T_STD_I64LE, stageXPulse, errorMessage) ||
        !writeDataset1D(fileId, "/stage_pose/y_pulse", H5T_STD_I64LE, stageYPulse, errorMessage) ||
        !writeDataset1D(fileId, "/stage_pose/z_pulse", H5T_STD_I64LE, stageZPulse, errorMessage)) {
        H5Fclose(fileId);
        return false;
    }

    H5Fclose(fileId);
    return true;
}
#endif

}

bool DataExporter::exportFrames(const QVector<FrameData>& frames,
                                DataExporter::Format format,
                                const DataExporter::ExportOptions& options,
                                QString* errorMessage)
{
    if (frames.isEmpty()) {
        if (errorMessage) {
            *errorMessage = "导出失败：没有可导出的缓存数据";
        }
        return false;
    }

    switch (format) {
    case Format::Csv:
        return exportCsv(frames, options, errorMessage);
    case Format::Hdf5:
#ifdef HAS_HDF5
        return exportHdf5(frames, options, errorMessage);
#else
        if (errorMessage) {
            *errorMessage = "当前构建未启用HDF5支持（请在qmake中配置HAS_HDF5与库路径）";
        }
        return false;
#endif
    }

    if (errorMessage) {
        *errorMessage = "未知导出格式";
    }
    return false;
}
