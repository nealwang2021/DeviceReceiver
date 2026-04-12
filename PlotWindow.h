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
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;
    
    /**
     * @brief 处理报警帧
     * @param frame 报警帧数据
     */
    void onCriticalFrame(const FrameData& frame) override;

private:
    void initPlot();                // 初始化绘图配置
    void updatePlotDataFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot);
    int effectiveMaxPlotPoints() const;
    void onThemeChanged() override;

    // 辅助布局
    void setupComplexLayout(int channelCount);

private slots:
    void onViewTypeChanged(int index);
    void onLegendClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event);
    void onLegendDoubleClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event);

private:
    QCustomPlot* m_plot;
    QTimer* m_refreshTimer;         // 保留定时器用于平滑动画（可选）
    
    int m_currentChannelCount = 0;
    int m_baseMaxPlotPoints = 1000;  // 基础最大绘图点数

    // 复数视图类型
    enum ComplexViewType { RealImag = 0, MagPhase = 1 };
    ComplexViewType m_complexViewType = RealImag;

    // UI 控件
    QComboBox* m_viewTypeCombo;      // 仅在 complex 模式显示

    // 复杂模式时使用的轴矩形（top/bottom）
    QVector<QCPAxisRect*> m_axisRects;

    // 复杂模式下 topRect 使用的独立图例（m_plot->legend 在 clear() 后失效，不可用）
    QCPLegend* m_complexTopLegend = nullptr;

    // 上一个检测模式，用于重建布局
    FrameData::DetectionMode m_lastMode = FrameData::Legacy;
    quint64 m_lastSnapshotVersion = 0;
};

#endif // PLOTWINDOW_H
