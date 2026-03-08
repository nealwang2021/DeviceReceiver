#include "DataProcessor.h"
#include "DataCacheManager.h"
#include <QDateTime>
#include <QDebug>
#include <algorithm>
#include <cmath>
DataProcessor::DataProcessor(QObject *parent) : QObject(parent)
{
    // 1Hz统计定时器
    m_statTimer = new QTimer(this);
    m_statTimer->setInterval(1000);
    connect(m_statTimer, &QTimer::timeout, this, &DataProcessor::calcStats);
    m_statTimer->start();
}

void DataProcessor::calcStats()
{
    // 获取过去1秒内的所有帧
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto frames = DataCacheManager::instance()->getFramesInTimeRange(now - 1000, now);
    
    if (frames.isEmpty()) {
        qInfo() << "【统计】过去1秒无数据";
        return;
    }

    // 计算统计值
    int frameCount = frames.size();
    double channelSum = 0, channelMax = -1e9, channelMin = 1e9;
    int validChannelCount = 0;

    for (const auto& frame : frames) {
        // 根据模式决定统计数值
        double channelVal = 0.0;
        bool hasValidValue = false;
        if (frame.detectMode == FrameData::Legacy) {
            // Legacy模式不再支持，跳过统计
            continue;
        } else if (frame.detectMode == FrameData::MultiChannelReal) {
            // Real 模式：comp0=幅值，统计幅值首通道
            if (!frame.channels_comp0.isEmpty()) {
                channelVal = frame.channels_comp0.first();
                hasValidValue = std::isfinite(channelVal);
            }
        } else if (frame.detectMode == FrameData::MultiChannelComplex) {
            // Complex 模式：comp0=实部、comp1=虚部，统计首通道幅值
            if (!frame.channels_comp0.isEmpty()) {
                double re = frame.channels_comp0.first();
                double im = frame.channels_comp1.isEmpty() ? 0.0 : frame.channels_comp1.first();
                channelVal = std::hypot(re, im);
                hasValidValue = std::isfinite(channelVal);
            }
        }

        if (!hasValidValue) {
            continue;
        }

        validChannelCount++;

        channelSum += channelVal;
        channelMax = std::max(channelMax, channelVal);
        channelMin = std::min(channelMin, channelVal);
    }

    if (validChannelCount == 0) {
        qInfo() << QString("【1秒统计】帧数：%1 | 无有效通道数据")
                    .arg(frameCount);
        return;
    }

    // 输出统计结果
    qInfo() << QString("【1秒统计】帧数：%1 | 通道值：平均%2(最大%3/最小%4)")
                .arg(frameCount)
                .arg(channelSum/validChannelCount, 0, 'f', 1)
                .arg(channelMax, 0, 'f', 1)
                .arg(channelMin, 0, 'f', 1);
}
