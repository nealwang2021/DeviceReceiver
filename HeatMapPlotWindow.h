#ifndef HEATMAPPLOTWINDOW_H
#define HEATMAPPLOTWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QLabel>
#include <QTimer>
#include <QSpinBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QElapsedTimer>

class QCustomPlot;
class QCPColorMap;
class QCPColorScale;

class HeatMapPlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit HeatMapPlotWindow(QWidget *parent = nullptr);
    ~HeatMapPlotWindow() override;

    // 公有接口：与 FrameData 的连接点（台位 + 通道标量 → 热力图格点）
    void setGridSize(int width, int height);
    void applyGrid(const QVector<double>& values, int width, int height);
    void updateFromFrame(const FrameData& frame);
    void exportImage(const QString& filename);

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

    // 用户交互槽
    void onGridWidthChanged(int width);
    void onGridHeightChanged(int height);
    void onDataMinChanged(double min);
    void onDataMaxChanged(double max);
    void onStartMockDataClicked();
    void onStopMockDataClicked();
    void onExportClicked();
    void onAutoStageRangeToggled(bool checked);
    void onStageRangeSpinChanged();
    void onClearHeatMapClicked();

private:
    void initUI();
    void initHeatMap();
    void updateHeatMapDisplay();
    void generateMockData();
    void setColorGradient();
    // 仅使用第 0 通道：实数取 comp0[0]，复数取 hypot(comp0[0], comp1[0])
    bool tryScalarValueForHeatmap(const FrameData& frame, double* out) const;
    void updateStageBoundsFromPoint(double vx, double vy);
    void mapMmToCell(double vx, double vy, int* ix, int* iy) const;
    void applyStageAxisRange();
    void refreshLiveHeatMapPlot();
    void scheduleThrottledReplot();
    void clearHeatMapData();
    void onThemeChanged() override;

private:
    QCustomPlot* m_plot{nullptr};
    QCPColorMap* m_colorMap{nullptr};
    QCPColorScale* m_colorScale{nullptr};

    int m_gridWidth{101};
    int m_gridHeight{101};
    QVector<double> m_gridData;  // 线性存储：row-major 顺序
    
    double m_dataMin{0.0};
    double m_dataMax{100.0};
    double m_displayMin{0.55};
    double m_displayMax{1.45};

    // UI 控件
    QSpinBox* m_widthSpinBox{nullptr};
    QSpinBox* m_heightSpinBox{nullptr};
    QDoubleSpinBox* m_minSpinBox{nullptr};
    QDoubleSpinBox* m_maxSpinBox{nullptr};
    QPushButton* m_startButton{nullptr};
    QPushButton* m_stopButton{nullptr};
    QPushButton* m_exportButton{nullptr};
    QPushButton* m_clearHeatMapButton{nullptr};
    QLabel* m_statsLabel{nullptr};

    QCheckBox* m_autoStageRangeCheck{nullptr};
    QDoubleSpinBox* m_stageXMinSpin{nullptr};
    QDoubleSpinBox* m_stageXMaxSpin{nullptr};
    QDoubleSpinBox* m_stageYMinSpin{nullptr};
    QDoubleSpinBox* m_stageYMaxSpin{nullptr};

    QTimer* m_mockDataTimer{nullptr};
    bool m_useMockData{false};
    qint64 m_frameCount{0};
    qint64 m_liveFrameCount{0};

    bool m_autoStageRangeMm{false};
    bool m_stageBoundsValid{false};
    double m_stageXMinMm{0.0};
    double m_stageXMaxMm{0.0};
    double m_stageYMinMm{0.0};
    double m_stageYMaxMm{0.0};
    double m_liveScalarMin{0.0};
    double m_liveScalarMax{0.0};
    bool m_liveScalarRangeValid{false};

    QElapsedTimer m_replotThrottle;
    QTimer* m_replotCoalesceTimer{nullptr};
    int m_replotMinIntervalMs{25};
};

#endif // HEATMAPPLOTWINDOW_H