#ifndef PULSEEDDYPLOTWINDOW_H
#define PULSEEDDYPLOTWINDOW_H

#include "PlotWindowBase.h"
#include "FrameData.h"

class QPushButton;
class QLabel;
class QCustomPlot;
class QCPGraph;
class QRadioButton;
class QDoubleSpinBox;
class QButtonGroup;

/**
 * @brief 脉冲涡流实时曲线窗口
 *
 * 单 QCustomPlot，两条曲线：
 *   - 脉冲曲线 (raw_values, 蓝色实线)
 *   - 参考线   (reference_values, 红色实线, has_reference=true 时显示)
 *
 * 顶栏：[开始参考线] [清除参考线]  参考线状态  帧号
 * 参考线指令通过 parentWidget()→appController()->sendCommand() 发送。
 */
class PulseEddyPlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit PulseEddyPlotWindow(QWidget* parent = nullptr);
    ~PulseEddyPlotWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;
    void onCriticalFrame(const FrameData& frame) override;

private slots:
    void onSelectionChanged(qint64 startMs, qint64 endMs, int mode);

private slots:
    void onStartReferenceClicked();
    void onClearReferenceClicked();

private:
    void loadReviewFromDb();
    void initPlot();
    void onThemeChanged() override;
    void updateReferenceStatus();
    void applyYAxisMode();

    QCustomPlot* m_plot = nullptr;
    QPushButton* m_startRefBtn = nullptr;
    QPushButton* m_clearRefBtn = nullptr;
    QLabel*      m_refStatusLabel = nullptr;
    QLabel*      m_frameLabel = nullptr;
    QRadioButton*    m_yAutoRadio = nullptr;
    QRadioButton*    m_yManualRadio = nullptr;
    QDoubleSpinBox*  m_yMinSpin = nullptr;
    QDoubleSpinBox*  m_yMaxSpin = nullptr;

    quint64 m_lastFrameId = 0;
    bool    m_hasReference = false;
    int     m_refFrameCount = 0;

    // --- Review mode ---
    qint64 m_reviewStartMs = 0;
    qint64 m_reviewEndMs = 0;
    bool m_reviewMode = false;
    quint64 m_reviewEpoch = 0;
    QAtomicInt m_reviewLoadCanceled; // 异步加载取消标志，跨线程安全
};

#endif // PULSEEDDYPLOTWINDOW_H
