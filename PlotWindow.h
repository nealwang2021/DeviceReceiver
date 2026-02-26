#ifndef PLOTWINDOW_H
#define PLOTWINDOW_H

#include <QTimer>
#include <QComboBox>
#include "qcustomplot.h"
#include "FrameData.h"
#include "PlotWindowBase.h"

// QCustomPlot实时绘图窗口（由PlotWindowManager统一管理）
class PlotWindow : public PlotWindowBase
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
    void onDataUpdated(const QVector<FrameData>& frames) override;
    
    /**
     * @brief 处理报警帧
     * @param frame 报警帧数据
     */
    void onCriticalFrame(const FrameData& frame) override;

private:
    void initPlot();                // 初始化绘图配置
    void updatePlotData(const QVector<FrameData>& frames); // 更新绘图数据

    // 辅助布局
    void setupComplexLayout(int channelCount);

private slots:
    void onViewTypeChanged(int index);
    void onLegendClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event);
    void onLegendDoubleClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event);

private:
    QCustomPlot* m_plot;
    QTimer* m_refreshTimer;         // 保留定时器用于平滑动画（可选）
    
    // 本地绘图缓存
    QVector<double> m_xTime;        // 时间轴
    // 兼容 legacy（温度/湿度）或 多通道数据缓存
    QVector<double> m_yTemp;        // 温度数据（legacy）
    QVector<double> m_yHumidity;    // 湿度数据（legacy）
    QVector<QVector<double>> m_channelData; // 每通道的 y 数据（多通道模式）
    QVector<QVector<double>> m_channelData2; // 复杂模式底部图的数据
    int m_currentChannelCount = 0;
    const int MAX_PLOT_POINTS = 1000;// 最大绘图点数（避免卡顿）

    // 复数视图类型
    enum ComplexViewType { RealImag = 0, MagPhase = 1 };
    ComplexViewType m_complexViewType = RealImag;

    // UI 控件
    QComboBox* m_viewTypeCombo;      // 仅在 complex 模式显示

    // 复杂模式时使用的轴矩形（top/bottom）
    QVector<QCPAxisRect*> m_axisRects;

    // 上一个检测模式，用于重建布局
    FrameData::DetectionMode m_lastMode = FrameData::Legacy;
};

#endif // PLOTWINDOW_H
