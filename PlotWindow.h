#ifndef PLOTWINDOW_H
#define PLOTWINDOW_H

#include <QTimer>
#include <QComboBox>
#include <QVector>
#include "qcustomplot.h"
#include "FrameData.h"
#include "PlotWindowBase.h"

class QSplitter;
class QRadioButton;
class QCheckBox;
class QDoubleSpinBox;
class QScrollArea;
class QCPItemEllipse;

// QCustomPlot实时绘图窗口（由PlotWindowManager统一管理）
class PlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit PlotWindow(QWidget *parent = nullptr);
    ~PlotWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;
    void onCriticalFrame(const FrameData& frame) override;

private:
    void initPlot();
    void updatePlotDataFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot);
    int effectiveMaxPlotPoints() const;
    void onThemeChanged() override;

    // 阵列涡流布局
    void setupComplexLayout(int channelCount);

    // 多频涡流布局
    void setupMultiFreqLayout(int freqPointCount);
    void updateMultiFreqPlots(const QSharedPointer<const PlotSnapshot>& snapshot);
    void rebuildMultiFreqGraphs(int freqPointCount);
    void applyImpedanceAxisMode();
    void updateCircleBoundary();
    void styleMultiFreqPlot(QCustomPlot* p);

    // 时基列工厂（Y轴反转，时间自上而下）
    QWidget* buildTimeBaseColumn(QCustomPlot*& plotOut);

private slots:
    void onViewTypeChanged(int index);
    void onLegendClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event);
    void onLegendDoubleClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event);
    void onMfFreqCheckToggled();
    void onMfCircleToggled();

private:
    // ---- 阵列涡流（MultiChannelComplex）----
    QCustomPlot* m_plot;
    QTimer* m_refreshTimer;

    int m_currentChannelCount = 0;
    int m_baseMaxPlotPoints = 1000;

    enum ComplexViewType { RealImag = 0, MagPhase = 1 };
    ComplexViewType m_complexViewType = RealImag;

    QComboBox* m_viewTypeCombo;
    QVector<QCPAxisRect*> m_axisRects;
    QCPLegend* m_complexTopLegend = nullptr;

    // ---- 多频涡流三列布局 ----
    QSplitter* m_mfSplitter = nullptr;
    QCustomPlot* m_mfTbPlot1 = nullptr;       // 时基图1：幅值-相位
    QCustomPlot* m_mfTbPlot2 = nullptr;       // 时基图2：实部-虚部
    QCustomPlot* m_mfImpedancePlot = nullptr; // 阻抗图

    // 阻抗图控件
    QRadioButton* m_mfAdaptiveRadio = nullptr;
    QRadioButton* m_mfDefaultRadio = nullptr;
    QScrollArea* m_mfFreqCheckArea = nullptr;
    QWidget* m_mfFreqCheckContainer = nullptr;
    QHBoxLayout* m_mfFreqCheckLayout = nullptr;
    QVector<QCheckBox*> m_mfFreqChecks;
    QRadioButton* m_mfRawRadio = nullptr;
    QRadioButton* m_mfNormRadio = nullptr;
    QDoubleSpinBox* m_mfRetentionSpin = nullptr;
    QDoubleSpinBox* m_mfCircleRadiusSpin = nullptr;
    QCheckBox* m_mfCircleShowCheck = nullptr;
    QCPItemEllipse* m_mfCircleItem = nullptr;
    QVector<QCPCurve*> m_mfImpedanceCurves;
    bool m_mfUseNormalized = false;
    double m_mfRetentionSecs = 3.0;

    // 上一个检测模式，用于重建布局
    FrameData::DetectionMode m_lastMode = FrameData::Legacy;
    quint64 m_lastSnapshotVersion = 0;
};

#endif // PLOTWINDOW_H
