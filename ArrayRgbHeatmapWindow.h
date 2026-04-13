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

private:
    QCustomPlot* m_ampPhasePlot{nullptr};
    QCustomPlot* m_realImagPlot{nullptr};
    QCPItemPixmap* m_ampPhasePixmap{nullptr};
    QCPItemPixmap* m_realImagPixmap{nullptr};

    QComboBox* m_xAxisModeCombo{nullptr};
    QPushButton* m_clearButton{nullptr};
    QPushButton* m_exportButton{nullptr};
    QLabel* m_statusLabel{nullptr};

    QVector<FrameRecord> m_frames;
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
};

#endif // ARRAYRGBHEATMAPWINDOW_H