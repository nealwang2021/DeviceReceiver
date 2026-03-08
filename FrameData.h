#ifndef FRAMEDATA_H
#define FRAMEDATA_H

#include <cstdint>
#include <QVector>
#include <QMetaType>

// 实时数据帧结构体（与硬件协议对应）
struct FrameData
{
    // 基本字段
    int64_t timestamp;     // 时间戳（毫秒）
    uint16_t frameId;      // 帧ID（0-65535）

    // 检测模式：用于区分 legacy / 多通道实数 / 多通道复数
    enum DetectionMode : uint8_t {
        Legacy = 0,
        MultiChannelReal = 1,    // 多通道实数（如漏磁模式）
        MultiChannelComplex = 2  // 多通道复数（如涡流模式）
    } detectMode;

    // 通道数
    uint8_t channelCount;

    // 通道数据：两个分量 comp0/comp1
    // Real 模式：comp0=幅值, comp1=相位
    // Complex 模式：comp0=实部, comp1=虚部
    QVector<double> channels_comp0;
    QVector<double> channels_comp1;

    FrameData() :
        timestamp(0),
        frameId(0),
        detectMode(Legacy),
        channelCount(0),
        channels_comp0(),
        channels_comp1()
    {
    }
};

Q_DECLARE_METATYPE(FrameData)

#endif // FRAMEDATA_H
