#ifndef PULSEDDECAYPLOTWINDOW_H
#define PULSEDDECAYPLOTWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QColor>
#include <QElapsedTimer>

class QCustomPlot;
class QCPGraph;
class QListWidget;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QTimer;

/**
 * 脉冲涡流衰减曲线：每条曲线对应一次脉冲激励后的瞬态序列。
 *
 * 脉冲涡流检测（EddyCurrentPulse）常见含义：
 * - 通过脉冲激励线圈/激励电路向被测体发射短暂磁场；
 * - 被测体感应涡流后，探头/传感器输出会随时间逐渐衰减（即“瞬态衰减”）；
 * - 因此曲线横轴使用“相对本脉冲起点的时间”，纵轴使用“探头信号幅值/模长”随时间的衰减过程。
 *
 * 信号取值约定（与当前系统 detect_mode 对齐）：
 * - Real（MultiChannelReal）：纵轴值取 `channels_comp0[k]`；
 * - Complex（MultiChannelComplex）：纵轴值取 `hypot(comp0[k], comp1[k])`（模长），等价于把实部/虚部分量折算为幅值；
 * - 本窗口默认取用户选定通道索引 `k`。
 *
 * 脉冲界定（首版，因 FrameData / device_data.proto 尚无 pulse_seq 等字段）：
 * - 手动「下一段脉冲」：下一帧起新开一条曲线；
 * - 可选自动：相邻帧 timestamp 间隔超过阈值视为新脉冲起点。
 * 后续可在 proto / FrameData 增加 pulse_seq、sample_in_pulse，由设备显式标定，再优先使用协议字段。
 */
class PulsedDecayPlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit PulsedDecayPlotWindow(QWidget* parent = nullptr);
    ~PulsedDecayPlotWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

private slots:
    void onNextPulseClicked();
    void onDeleteSelectedClicked();
    void onBuildReferenceClicked();
    void onClearReferenceClicked();
    void onListSelectionChanged();
    void onMockToggled(bool on);
    void onMockTick();
    void onMaxCurvesChanged(int v);
    void onAutoGapToggled(bool on);

private:
    struct DecayCurveEntry {
        int id = 0;
        qint64 pulseStartMs = 0;
        QVector<double> tRelMs;
        QVector<double> y;
        QCPGraph* graph = nullptr;
    };

    void rebuildPlotUi();
    bool scalarFromFrame(const FrameData& frame, int ch, double* out) const;
    void processFrame(const FrameData& frame);
    void startNewPulseAt(qint64 pulseStartMs);
    void enforceMaxCurves();
    void removeCurveAt(int index);
    void syncListWidget();
    void updateCurveStyles();
    void applyCurveDataToGraph(DecayCurveEntry& c);
    void scheduleReplot();
    void bumpRefGraphToFront();
    void buildReferenceFromSelection();
    static double interpY(const QVector<double>& t, const QVector<double>& y, double tt);

    QCustomPlot* m_decayPlot = nullptr;
    QListWidget* m_curveList = nullptr;
    QSpinBox* m_channelSpin = nullptr;
    QSpinBox* m_maxCurvesSpin = nullptr;
    QCheckBox* m_autoGapCheck = nullptr;
    QDoubleSpinBox* m_gapMsSpin = nullptr;
    QPushButton* m_nextPulseBtn = nullptr;
    QPushButton* m_delBtn = nullptr;
    QPushButton* m_refBtn = nullptr;
    QPushButton* m_clearRefBtn = nullptr;
    QCheckBox* m_mockCheck = nullptr;
    QLabel* m_statusLabel = nullptr;

    QVector<DecayCurveEntry> m_curves;
    int m_nextCurveId = 1;
    qint64 m_lastFrameTs = -1;
    bool m_startNewPulseNextFrame = false;

    QCPGraph* m_refGraph = nullptr;
    QVector<double> m_refT;
    QVector<double> m_refY;

    QTimer* m_mockTimer = nullptr;
    bool m_useMockData = false;
    double m_mockT = 0.0;

    QElapsedTimer m_replotThrottle;
    int m_replotMinMs = 33;
};

#endif // PULSEDDECAYPLOTWINDOW_H
