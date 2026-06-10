#ifndef MULTIFREQSTAGEHEATMAPWINDOW_H
#define MULTIFREQSTAGEHEATMAPWINDOW_H

#include "PlotWindowBase.h"
#include "FrameData.h"
#include <QVector>
#include <QColor>

class QCustomPlot;
class QCPItemPixmap;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;

/**
 * @brief 多频涡流台位热力图窗口（QCPColorMap 固定网格）
 *
 * 三轴台 X-Y 位置映射到固定分辨率网格，每个扫查位置对应一个格子。
 * 颜色映射与阵列热力图一致：colorFromAmpPhase(阻抗幅值, 相位°)。
 */
class MultiFreqStageHeatmapWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit MultiFreqStageHeatmapWindow(QWidget* parent = nullptr);
    ~MultiFreqStageHeatmapWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;
    void onCriticalFrame(const FrameData& frame) override;

private slots:
    void onExportClicked();
    void onAmpRangeChanged();

private:
    void initPlot();
    void onThemeChanged() override;
    void rebuildHeatmap();
    QColor colorFromAmpPhase(double amp, double phaseDeg) const;
    static double clamp01(double v);

    QCustomPlot* m_plot = nullptr;
    QCPItemPixmap* m_pixmapItem = nullptr;
    QComboBox* m_freqCombo = nullptr;
    QDoubleSpinBox* m_ampMinSpin = nullptr;
    QDoubleSpinBox* m_ampMaxSpin = nullptr;
    QPushButton* m_exportBtn = nullptr;
    QLabel* m_pointCountLabel = nullptr;

    int m_freqPointCount = 0;
    QVector<QImage> m_freqImages;  // 每个频点独立一张热力图
    double m_ampMin = 0.0001;
    double m_ampMax = 1.0;
    double m_xMin = 0, m_xMax = 100, m_yMin = 0, m_yMax = 100;
    double m_step = 1.0;          // 网格步距 (mm)

    static constexpr double kVerySmallValue = 1e-12;
};

#endif
