#ifndef ARRAYRGBHEATMAPWINDOW_H
#define ARRAYRGBHEATMAPWINDOW_H

#include "PlotWindowBase.h"
#include <QVector>
#include <QLabel>
#include <QTimer>
#include <QImage>
#include <QColor>
#include <QElapsedTimer>

class QComboBox;
class QPushButton;
class QCustomPlot;
class QCPItemPixmap;
class BusyOverlay;

class ArrayRgbHeatmapWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit ArrayRgbHeatmapWindow(QWidget* parent = nullptr);
    ~ArrayRgbHeatmapWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onCriticalFrame(const FrameData& frame) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;

private slots:
    void onXAxisModeChanged(int index);
    void onClearClicked();
    void onExportClicked();
    void onSelectionChanged(qint64 startMs, qint64 endMs, int mode);

private:
    enum class XAxisMode {
        FrameNumber,
        TimeSeconds,
    };

    struct FrameRecord
    {
        qint64 sequence{0};
        qint64 timestampMs{0};
        QVector<double> amp;
        QVector<double> phase;
        QVector<double> x;
        QVector<double> y;
    };

    void initUi();
    void loadSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot);
    bool appendFrame(const FrameData& frame);
    bool isNewFrame(const FrameData& frame) const;
    void clearFrames();
    void rebuildPlots();
    QImage buildHeatmapImage(bool useAmpPhase) const;
    QColor colorFromAmpPhase(double amp, double phaseDeg) const;
    void configurePlot(QCustomPlot* plot,
                       QCPItemPixmap* item,
                       const QImage& image,
                       const QString& xAxisLabel) const;
    QString currentXAxisLabel() const;
    double currentXAxisMax() const;
    int displayChannelCount() const;
    int maximumBufferedFrames() const;
    void scheduleRebuild();

    /// 返回当前用于渲染的帧索引列表：Live 下为全部；Review 下为已加载 review 帧的全部。
    QVector<int> collectVisibleFrameIndices() const;

    /// 渲染数据源：Review 模式下使用 m_reviewFrames，否则使用 m_frames。
    const QVector<FrameRecord>& sourceFramesForRender() const;

    /// 在 Review 模式下从 HistoryDataProvider 拉取时间段内的原始 40 通道数据，
    /// 实际查询走 worker 线程，结果回主线程后再触发重绘。
    void loadReviewFromDb();

    /// worker 线程 review 加载结果，主线程入口。
    struct ReviewQueryResult
    {
        bool ok{false};
        qint64 startMs{0};
        qint64 endMs{0};
        int channelCount{40};
        int stride{1};
        QVector<FrameRecord> frames;
    };
    void onReviewQueryFinished(quint64 epoch, ReviewQueryResult result);

    void trimFramesToRealtimeLiveWindow();

private:
    QCustomPlot* m_ampPhasePlot{nullptr};
    QCustomPlot* m_realImagPlot{nullptr};
    QCPItemPixmap* m_ampPhasePixmap{nullptr};
    QCPItemPixmap* m_realImagPixmap{nullptr};

    QComboBox* m_xAxisModeCombo{nullptr};
    QPushButton* m_clearButton{nullptr};
    QPushButton* m_exportButton{nullptr};
    QLabel* m_statusLabel{nullptr};

    QVector<FrameRecord> m_frames;        // Live 模式缓冲（来自 onDataUpdated/onCriticalFrame/onPlotSnapshotUpdated）
    QVector<FrameRecord> m_reviewFrames;  // Review 模式缓冲（来自 HistoryDataProvider::fetchRawChunk）
    XAxisMode m_xAxisMode{XAxisMode::FrameNumber};
    qint64 m_lastSequence{-1};
    qint64 m_lastTimestamp{-1};
    int m_channelCount{40};
    QTimer* m_rebuildTimer{nullptr};
    int m_rebuildMinIntervalMs{80};
    QElapsedTimer m_rebuildThrottle;
    QElapsedTimer m_perfLogTimer;
    qint64 m_perfRebuildCount{0};
    qint64 m_perfRebuildCostMs{0};
    bool m_rebuildPending{false};

    bool m_reviewMode{false};
    qint64 m_reviewStartMs{0};
    qint64 m_reviewEndMs{0};
    quint64 m_reviewEpoch{0};

    BusyOverlay* m_busyOverlay{nullptr};
};

#endif // ARRAYRGBHEATMAPWINDOW_H