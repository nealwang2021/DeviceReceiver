#ifndef ARRAYPLOTWINDOW_H
#define ARRAYPLOTWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QLabel>
#include <QTimer>

class QCustomPlot;
class QCPAxisRect;

class ArrayPlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit ArrayPlotWindow(QWidget *parent = nullptr);
    ~ArrayPlotWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

private:
    void initArrayPlot();
    void updateArrayData();
    void generateMockData();

private:
    QCustomPlot* m_plot{nullptr};
    QVector<QCPAxisRect*> m_channelAxisRects;  // 通道轴矩形
    QLabel* m_statsLabel{nullptr};
    QTimer* m_mockDataTimer{nullptr};
    
    QVector<double> m_timeAxis;           // 共享时间轴
    QVector<QVector<double>> m_channelValues; // 每通道数据
    QVector<QVector<double>> m_channelValues2; // 复杂模式的第二分量
    
    int m_maxDataPoints{1000};
    bool m_useMockData{false};
    qint64 m_frameCount{0};
    int m_currentChannelCount{0};
    FrameData::DetectionMode m_lastMode{FrameData::Legacy};
    double m_latestTime{0.0};
    int m_axisUpdateCounter{0};
    int m_axisUpdateStride{10};
    quint64 m_lastSnapshotVersion{0};
};

#endif // ARRAYPLOTWINDOW_H
