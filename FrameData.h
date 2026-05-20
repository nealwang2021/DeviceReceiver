#ifndef FRAMEDATA_H
#define FRAMEDATA_H

#include <cstdint>
#include <QVector>
#include <QMetaType>

/// 多频涡流单频点检测结果（对应 multifreq_eddy.proto FrequencyPointResult）
struct MultiFreqPointResult
{
    int     frequencyFactor = 0;
    double  frequencyHz = 0.0;
    double  voltageReal = 0.0;
    double  voltageImag = 0.0;
    double  currentReal = 0.0;
    double  currentImag = 0.0;
    double  impedanceReal_raw = 0.0;
    double  impedanceImag_raw = 0.0;
    double  impedanceMagnitude = 0.0;
    double  impedancePhaseDeg = 0.0;
    double  normalizedImpedanceReal = 0.0;
    double  normalizedImpedanceImag = 0.0;
    double  voltageMagnitude = 0.0;
    double  currentMagnitude = 0.0;
    bool    valid = false;
};

// 实时数据帧结构体（与硬件协议对应）
struct FrameData
{
    // 基本字段
    int64_t timestamp;     // 时间戳（毫秒）
    uint64_t sequence;     // 原始序列号（device.proto: sequence）
    uint64_t frameId;      // 帧序号（与 sequence 对齐；用于 UI/导出/调试展示的全量 uint64 标识）

    // 检测模式：用于区分 legacy / 多通道实数 / 多通道复数
    enum DetectionMode : uint8_t {
        Legacy = 0,
        MultiChannelReal = 1,    // 多通道实数（如漏磁模式）
        MultiChannelComplex = 2, // 多通道复数（如涡流模式）
        MultiFreqEddy = 3        // 多频涡流（频点阻抗）
    } detectMode;

    // 通道数
    uint8_t channelCount;

    // 通道数据：两个分量 comp0/comp1
    // Real 模式：comp0=幅值, comp1=相位
    // Complex 模式：comp0=实部, comp1=虚部
    QVector<double> channels_comp0;
    QVector<double> channels_comp1;

    // 新协议四值通道数据（device.proto: ChannelSampleReply）
    QVector<double> channels_amp;
    QVector<double> channels_phase;
    QVector<double> channels_x;
    QVector<double> channels_y;
    QVector<int> channels_display_index;
    QVector<int> channels_source_channel;

    // 三轴台位（与 stage.proto PositionsReply / StageService 一致）；由 StagePoseLatch 在写入缓存前
    // 按「最近台位 last-known」附加到每条被测设备帧；未连接台或未收到过台位时 hasStagePose=false。
    // mm：物理位移（毫米），由下位机按标定从 pulse 换算，便于读数与 MoveAbs(mm) 对齐。
    // pulse：运动控制原始计数（步进脉冲/编码器计数等，依下位机定义），与 SetSpeed 的 pulse/s 同一套单位。
    bool hasStagePose = false;
    qint64 stageTimestampMs = 0;   // 台位样本 UTC 毫秒（与 PositionsReply.unixms 一致）
    double stageXMm = 0.0;         // X 轴位置（mm）
    double stageYMm = 0.0;         // Y 轴位置（mm）
    double stageZMm = 0.0;         // Z 轴位置（mm）
    int stageXPulse = 0;           // X 轴 pulse（整数计数，非毫米）
    int stageYPulse = 0;           // Y 轴 pulse
    int stageZPulse = 0;           // Z 轴 pulse

    // 多频涡流：每帧 N 个频点的阻抗检测结果
    QVector<MultiFreqPointResult> mfFreqPoints;

    FrameData() :
        timestamp(0),
        sequence(0),
        frameId(0),
        detectMode(Legacy),
        channelCount(0),
        channels_comp0(),
        channels_comp1(),
        channels_amp(),
        channels_phase(),
        channels_x(),
        channels_y(),
        channels_display_index(),
        channels_source_channel(),
        hasStagePose(false),
        stageTimestampMs(0),
        stageXMm(0.0),
        stageYMm(0.0),
        stageZMm(0.0),
        stageXPulse(0),
        stageYPulse(0),
        stageZPulse(0),
        mfFreqPoints()
    {
    }
};

Q_DECLARE_METATYPE(FrameData)

#endif // FRAMEDATA_H
