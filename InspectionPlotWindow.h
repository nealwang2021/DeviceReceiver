#ifndef INSPECTIONPLOTWINDOW_H
#define INSPECTIONPLOTWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QElapsedTimer>

class QShowEvent;
class QCustomPlot;
class QCPCurve;
class QCPGraph;
class QCPItemEllipse;
class QSplitter;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QRadioButton;
class QLabel;
class QVBoxLayout;
class QHBoxLayout;
class QWidget;
class QScrollArea;

/**
 * 检测分析窗口：顶栏（组/通道）+ 横向三列（时基1 | 时基2 | 阻抗）。
 *
 * 复数模式：时基1 幅值(实线)/相位(虚线)，时基2 实部(实线)/虚部(虚线)；
 *   每图顶栏有分量全局勾选；组内通道勾选仍有效。
 * 实数模式：仅第一列时基（幅值），第二列隐藏；列1:列3 约 1:2。
 */
class InspectionPlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit InspectionPlotWindow(QWidget* parent = nullptr);
    ~InspectionPlotWindow() override;

protected:
    void showEvent(QShowEvent* event) override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

private slots:
    void onGroupComboChanged(int index);
    void onChannelsPerGroupChanged(int value);
    void onChannelCheckToggled();
    void onComponentCheckToggled();
    void onFreqCheckToggled();
    void onImpedanceModeChanged();
    void onCircleRadiusChanged(double radius);
    void onCircleShowToggled(bool show);

private:
    void rebuildUi();
    void applyTimeBaseModeLayout();

    void rebuildGroupCombo(int totalChannels);
    void rebuildChannelChecks();
    void rebuildTimeBaseGraphs();
    void updateTimeBasePlots(const QSharedPointer<const PlotSnapshot>& snap);

    void rebuildFreqChecks(int totalChannels);
    void rebuildImpedanceCurves(int totalChannels);
    void updateImpedancePlane(const QSharedPointer<const PlotSnapshot>& snap);
    void updateCircleBoundary();
    void applyImpedanceAxisMode();

    void scheduleReplot();

    static QColor colorForChannel(int ch);

    QWidget* m_topBar = nullptr;
    QSplitter* m_plotSplitter = nullptr;

    QWidget* m_timeCol1 = nullptr;
    QWidget* m_tb1ComponentRow = nullptr;
    QCheckBox* m_showMagCheck = nullptr;
    QCheckBox* m_showPhaseCheck = nullptr;

    QWidget* m_timeCol2 = nullptr;
    QWidget* m_tb2ComponentRow = nullptr;
    QCheckBox* m_showRealCheck = nullptr;
    QCheckBox* m_showImagCheck = nullptr;

    QWidget* m_impedanceCol = nullptr;

    QComboBox* m_groupCombo = nullptr;
    QSpinBox* m_chPerGroupSpin = nullptr;
    QScrollArea* m_channelCheckArea = nullptr;
    QWidget* m_channelCheckContainer = nullptr;
    QHBoxLayout* m_channelCheckLayout = nullptr;
    QVector<QCheckBox*> m_channelChecks;

    QCustomPlot* m_tbPlot1 = nullptr;
    QCustomPlot* m_tbPlot2 = nullptr;

    QCustomPlot* m_impedancePlot = nullptr;
    QRadioButton* m_adaptiveRadio = nullptr;
    QRadioButton* m_defaultRadio = nullptr;
    QScrollArea* m_freqCheckArea = nullptr;
    QWidget* m_freqCheckContainer = nullptr;
    QHBoxLayout* m_freqCheckLayout = nullptr;
    QVector<QCheckBox*> m_freqChecks;
    QDoubleSpinBox* m_circleRadiusSpin = nullptr;
    QCheckBox* m_circleShowCheck = nullptr;
    QCPItemEllipse* m_circleItem = nullptr;
    QLabel* m_statusLabel = nullptr;

    QVector<QCPCurve*> m_impedanceCurves;

    FrameData::DetectionMode m_lastMode = FrameData::Legacy;
    int m_lastChannelCount = 0;
    quint64 m_lastSnapshotVersion = 0;
    int m_channelsPerGroup = 8;

    QElapsedTimer m_replotThrottle;
    int m_replotMinMs = 33;
};

#endif // INSPECTIONPLOTWINDOW_H
