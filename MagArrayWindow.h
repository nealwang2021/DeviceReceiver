#ifndef MAGARRAYWINDOW_H
#define MAGARRAYWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>

class QCustomPlot;
class QCPColorMap;

/**
 * @brief 漏磁检测数据窗口：以热力图 / 时序图形式展示 20 个传感器 XYZ 数据。
 *
 * 支持两种视图：
 *   - Heatmap 视图：以 ColorMap 展示各轴（X/Y/Z）传感器幅值的空间分布
 *   - TimeSeries 视图：按传感器分组展示 XYZ 均值/最新值的时序变化
 */
class MagArrayWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit MagArrayWindow(QWidget* parent = nullptr);
    ~MagArrayWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

private:
    void setupUi();

    QCustomPlot* m_heatmapPlot = nullptr;
    QCustomPlot* m_timeSeriesPlot = nullptr;
};

#endif // MAGARRAYWINDOW_H
