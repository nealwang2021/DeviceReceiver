#ifndef MAGARRAYWINDOW_H
#define MAGARRAYWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QScrollArea>
#include <QLabel>
#include <QRadioButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QElapsedTimer>

class QCustomPlot;
class QCPColorMap;
class QCPColorScale;
class QCPAxisRect;
class QSplitter;

class MagArrayWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit MagArrayWindow(QWidget* parent = nullptr);
    ~MagArrayWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;
    void onCriticalFrame(const FrameData& frame) override;

private:
    void buildUi();
    void rebuildWaveformGraphs(int channelCount);
    void updateWaveformFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot);
    void updateHeatmapFromFrame(const FrameData& frame);
    void onThemeChanged() override;
    void onColorRangeChanged();
    void onHeatmapAxisModeChanged();

    // Layout
    QSplitter* m_splitter = nullptr;

    // --- Waveform (left) ---
    QCustomPlot* m_waveformPlot = nullptr;
    QScrollArea* m_waveformScrollArea = nullptr;
    QVector<QCPAxisRect*> m_waveformAxisRects;
    int m_waveformChannelCount = 60;

    // --- Heatmap (right) ---
    QCustomPlot* m_heatmapPlot = nullptr;
    QScrollArea* m_heatmapScrollArea = nullptr;
    // 3 stacked QCPAxisRects, each containing one QCPColorMap (X / Y / Z)
    QCPAxisRect* m_heatmapAxisRects[3] = {nullptr, nullptr, nullptr};
    QCPColorMap* m_heatmapColorMaps[3] = {nullptr, nullptr, nullptr};
    QCPColorScale* m_heatmapColorScales[3] = {nullptr, nullptr, nullptr};

    static constexpr int kHeatmapCols = 2000;  // 最大显示帧数
    static constexpr int kHeatmapRows = 20;

    // heatmap data ring buffers: [axis][row * kHeatmapCols + col]
    QVector<double> m_heatmapData[3];
    // timestamp ring buffer for the X axis
    QVector<double> m_heatmapTimeCol;
    // position buffer for pos mode
    QVector<double> m_heatmapPosCol;
    int m_heatmapWriteCol = 0;  // next column to write (wraps, 0..kHeatmapCols-1)

    // --- Controls ---
    QSpinBox* m_maxFramesSpin = nullptr;
    QCheckBox* m_axisChecks[3] = {nullptr, nullptr, nullptr};
    QRadioButton* m_posModeBtn = nullptr;
    QRadioButton* m_timeModeBtn = nullptr;
    QDoubleSpinBox* m_colorMinSpin = nullptr;
    QDoubleSpinBox* m_colorMaxSpin = nullptr;
    QLabel* m_stageStatusLabel = nullptr;
    QLabel* m_statsLabel = nullptr;

    // --- State ---
    int m_liveFrameCount = 0;
    QElapsedTimer m_replotThrottle;
    int m_replotMinMs = 33;
    FrameData::DetectionMode m_lastMode = FrameData::Legacy;
    int m_lastSnapshotChannelCount = 0;
    quint64 m_lastFrameId = 0;
    bool m_usePosMode = false;  // false = time mode (default)
    double m_colorRangeMin = 0.0;
    double m_colorRangeMax = 100.0;
};

#endif // MAGARRAYWINDOW_H
