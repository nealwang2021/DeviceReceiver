#ifndef PLOTWINDOW_H
#define PLOTWINDOW_H

#include <QWidget>
#include <QTimer>
#include "qcustomplot.h"
#include "FrameData.h"

// QCustomPlot实时绘图窗口（由PlotWindowManager统一管理）
class PlotWindow : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWindow(QWidget *parent = nullptr);
    ~PlotWindow() override;

public slots:
    /**
     * @brief 处理来自PlotWindowManager的数据更新
     * @param frames 最新的数据帧
     */
    void onDataUpdated(const QVector<FrameData>& frames);
    
    /**
     * @brief 处理报警帧
     * @param frame 报警帧数据
     */
    void onCriticalFrame(const FrameData& frame);

private:
    void initPlot();                // 初始化绘图配置
    void updatePlotData(const QVector<FrameData>& frames); // 更新绘图数据

private:
    QCustomPlot* m_plot;
    QTimer* m_refreshTimer;         // 保留定时器用于平滑动画（可选）
    
    // 本地绘图缓存
    QVector<double> m_xTime;        // 时间轴
    QVector<double> m_yTemp;        // 温度数据
    QVector<double> m_yHumidity;    // 湿度数据
    const int MAX_PLOT_POINTS = 200;// 最大绘图点数（避免卡顿）
};

#endif // PLOTWINDOW_H
