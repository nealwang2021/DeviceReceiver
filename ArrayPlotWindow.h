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
class BusyOverlay;

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

private slots:
    void onSelectionChanged(qint64 startMs, qint64 endMs, int mode);

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

    /// 回放模式下按 SelectionState 范围从 DB 重建 40 通道数据。
    /// 实际的 SQL 查询会被派发到 worker 线程，结果回到主线程后才更新 graph。
    void renderReviewRange();

    /// review 查询单通道结果（在 worker 线程组装完毕后回到主线程使用）。
    struct ReviewChannelResult
    {
        int channelIndex{-1};
        QVector<double> keys;   // 时间轴（毫秒，min/max 各占一个点）
        QVector<double> values; // 与 keys 同长，min/max 交替
        double dataMin{0.0};
        double dataMax{0.0};
        bool hasMin{false};
        bool hasMax{false};
    };
    struct ReviewQueryResult
    {
        bool ok{false};
        qint64 startMs{0};
        qint64 endMs{0};
        QVector<ReviewChannelResult> channels;
    };

    /// worker 线程结果回主线程的入口；epoch 不一致直接丢弃。
    void onReviewQueryFinished(quint64 epoch, ReviewQueryResult result);

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

    /// true：由 SelectionState 指示处于 Review（历史回放）模式，实时快照不覆盖 X 轴
    bool m_reviewMode{false};
    qint64 m_reviewStartMs{0};
    qint64 m_reviewEndMs{0};

    /// 单调递增的 review 加载世代号；worker 完成时与当前 epoch 比对，旧任务结果丢弃。
    quint64 m_reviewEpoch{0};

    /// 加载蒙版：与 ReviewLoadCoordinator 协同显示/隐藏。覆盖在 m_plot 之上。
    BusyOverlay* m_busyOverlay{nullptr};
};

#endif // ARRAYPLOTWINDOW_H
