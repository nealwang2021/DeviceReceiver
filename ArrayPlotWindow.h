#ifndef ARRAYPLOTWINDOW_H
#define ARRAYPLOTWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QLabel>
#include <QTimer>
#include <QElapsedTimer>

class QCustomPlot;
class QCPAxisRect;
class QComboBox;
class QCheckBox;
class QPushButton;
class QScrollArea;
class QWidget;

class ArrayPlotWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit ArrayPlotWindow(QWidget *parent = nullptr);
    ~ArrayPlotWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

private:
    enum class ArrayComponent {
        Amplitude,
        Phase,
        Real,
        Imag,
    };

    enum class RowLabelMode {
        ChannelIndex,
        DisplayIndex,
        SourceChannel,
    };

    enum class LayoutDensity {
        Compact,
        Standard,
        Comfortable,
    };

    enum class YAxisMode {
        Auto,
        Fixed,
    };

    void initArrayPlot();
    void updateArrayData();
    void generateMockData();
    void rebuildChannelSelector();
    void applyChannelVisibility();
    void updateUnifiedYAxisRange();
    void exportImage(const QString& filename);
    void onExportClicked();
    void renderSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot, bool forceRefresh);
    const QVector<QVector<double>>* resolveSnapshotSource(const QSharedPointer<const PlotSnapshot>& snapshot) const;
    int perRowHeightForDensity() const;
    void onThemeChanged() override;

private:
    QCustomPlot* m_plot{nullptr};
    QScrollArea* m_plotScrollArea{nullptr};
    QVector<QCPAxisRect*> m_channelAxisRects;  // 通道轴矩形
    QComboBox* m_componentCombo{nullptr};
    QComboBox* m_rowLabelCombo{nullptr};
    QComboBox* m_densityCombo{nullptr};
    QComboBox* m_yAxisCombo{nullptr};
    QPushButton* m_exportButton{nullptr};
    QScrollArea* m_channelScrollArea{nullptr};
    QWidget* m_channelSelectorWidget{nullptr};
    QVector<QCheckBox*> m_channelChecks;
    QLabel* m_statsLabel{nullptr};
    QTimer* m_mockDataTimer{nullptr};
    
    QVector<double> m_timeAxis;           // 共享时间轴
    QVector<QVector<double>> m_channelValues; // 每通道数据
    QVector<QVector<double>> m_channelValues2; // 复杂模式的第二分量
    QVector<bool> m_channelVisible;
    
    int m_maxDataPoints{1000};
    bool m_useMockData{false};
    qint64 m_frameCount{0};
    int m_currentChannelCount{0};
    FrameData::DetectionMode m_lastMode{FrameData::Legacy};
    double m_latestTime{0.0};
    int m_axisUpdateCounter{0};
    int m_axisUpdateStride{10};
    quint64 m_lastSnapshotVersion{0};
    ArrayComponent m_componentMode{ArrayComponent::Amplitude};
    RowLabelMode m_rowLabelMode{RowLabelMode::ChannelIndex};
    LayoutDensity m_layoutDensity{LayoutDensity::Compact};
    YAxisMode m_yAxisMode{YAxisMode::Auto};
    bool m_yAxisRangeValid{false};
    double m_yAxisLower{0.0};
    double m_yAxisUpper{0.0};
    QSharedPointer<const PlotSnapshot> m_cachedSnapshot;
    QVector<double> m_channelDataMin;
    QVector<double> m_channelDataMax;
    QElapsedTimer m_perfLogTimer;
    qint64 m_perfRenderCount{0};
    qint64 m_perfRenderCostMs{0};
};

#endif // ARRAYPLOTWINDOW_H
