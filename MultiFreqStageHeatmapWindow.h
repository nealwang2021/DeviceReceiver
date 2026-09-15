#ifndef MULTIFREQSTAGEHEATMAPWINDOW_H
#define MULTIFREQSTAGEHEATMAPWINDOW_H

#include "PlotWindowBase.h"
#include "FrameData.h"
#include <QVector>
#include <QColor>
#include <QSet>
#include <QPair>

class QCustomPlot;
class QCPItemPixmap;
class QCPColorScale;
class QCPColorGradient;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;

/**
 * @brief 多频涡流台位热力图窗口（QCPItemPixmap 固定网格 + QCPColorScale 色标）
 *
 * 三轴台 X-Y 位置映射到自适应分辨率网格，每个扫查位置对应一个格子。
 * 颜色映射：相位决定色相，阻抗幅值（log10归一化）决定亮度。
 * 网格范围根据实际台位数据自动扩展。
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
    void rebuildGridAndImages(double xMin, double xMax, double yMin, double yMax);
    void updateColorScale();
    QColor colorFromAmpPhase(double amp, double phaseDeg) const;
    static double clamp01(double v);

    QCustomPlot* m_plot = nullptr;
    QCPItemPixmap* m_pixmapItem = nullptr;
    QCPColorScale* m_colorScale = nullptr;
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
    bool m_hasData = false;       // 是否收到过有效台位数据

    // 增量统计：记录已扫 (ix,iy) 坐标，避免全图扫描
    QSet<QPair<int,int>> m_filledSet;
    int m_filledCount = 0;

    static constexpr double kVerySmallValue = 1e-12;
};

#endif
