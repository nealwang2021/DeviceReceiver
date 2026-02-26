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
    double tempSum = 0, tempMax = 0, tempMin = 100;
    double humiSum = 0, humiMax = 0, humiMin = 100;

    for (const auto& frame : frames) {
        // 根据模式决定统计数值
        double tempVal = 0.0;
        double humVal = 0.0;
        if (frame.detectMode == FrameData::Legacy) {
            tempVal = frame.temperature;
            humVal = frame.humidity;
        } else if (frame.detectMode == FrameData::MultiChannelReal) {
            // 使用第一个通道值作为温度统计，湿度留空
            if (!frame.channels_comp0.isEmpty()) tempVal = frame.channels_comp0.first();
        } else if (frame.detectMode == FrameData::MultiChannelComplex) {
            // 复数模式用第一个通道幅值
            if (!frame.channels_comp0.isEmpty()) {
                double re = frame.channels_comp0.first();
                double im = frame.channels_comp1.isEmpty() ? 0.0 : frame.channels_comp1.first();
                tempVal = std::hypot(re, im);
            }
        }

        tempSum += tempVal;
        tempMax = std::max(tempMax, tempVal);
        tempMin = std::min(tempMin, tempVal);

        humiSum += humVal;
        humiMax = std::max(humiMax, humVal);
        humiMin = std::min(humiMin, humVal);
    }

    // 输出统计结果
    qInfo() << QString("【1秒统计】帧数：%1 | 温度：平均%2(最大%3/最小%4) | 湿度：平均%5(最大%6/最小%7)")
                .arg(frameCount)
                .arg(tempSum/frameCount, 0, 'f', 1)
                .arg(tempMax, 0, 'f', 1)
                .arg(tempMin, 0, 'f', 1)
                .arg(humiSum/frameCount, 0, 'f', 1)
                .arg(humiMax, 0, 'f', 1)
                .arg(humiMin, 0, 'f', 1);
}
