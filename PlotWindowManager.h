#ifndef PLOTWINDOWMANAGER_H
#define PLOTWINDOWMANAGER_H

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QList>
#include "FrameData.h"
#include "PlotDataHub.h"

class PlotWindowBase;
class QMdiArea;

/**
 * @brief 绘图窗口管理器（单例模式）
 *
 * 负责集中管理所有 PlotWindowBase 派生窗口，统一分发数据更新。
 */
class PlotWindowManager : public QObject
{
    Q_OBJECT
public:
    enum PlotType {
        CombinedPlot,
        HeatmapPlot,
        ArrayPlot,
        PulsedDecayPlot,
        InspectionPlot
    };

    static PlotWindowManager* instance();
    static void destroy();

    PlotWindowBase* createWindow(PlotType type = CombinedPlot, QWidget* parent = nullptr);
    PlotWindowBase* createWindowInMdiArea(QMdiArea* mdiArea, PlotType type = CombinedPlot);

    void registerWindow(PlotWindowBase* window);
    void unregisterWindow(PlotWindowBase* window);

    QList<PlotWindowBase*> windows() const { return m_windows; }
    int windowCount() const { return m_windows.size(); }

    void updateAllWindows();
    void setUpdateInterval(int intervalMs);
    int updateInterval() const { return m_updateTimer->interval(); }
    void startUpdates();
    void stopUpdates();
    QVector<FrameData> getRecentFrames(int count = 5);

signals:
    void dataUpdated(const QVector<FrameData>& frames);
    void plotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot);
    void windowAdded(PlotWindowBase* window);
    void windowRemoved(PlotWindowBase* window);
    void criticalFrameReceived(const FrameData& frame);

private:
    explicit PlotWindowManager(QObject *parent = nullptr);
    ~PlotWindowManager();
    PlotWindowManager(const PlotWindowManager&) = delete;
    PlotWindowManager& operator=(const PlotWindowManager&) = delete;
    void initialize();
    void cleanup();

private slots:
    void onUpdateTimer();

private:
    static PlotWindowManager* m_instance;
    QList<PlotWindowBase*> m_windows;
    QTimer* m_updateTimer;
    bool m_isInitialized;
    qint64 m_lastDispatchedTimestamp{0};
    int m_baseUpdateIntervalMs{50};
};

#endif // PLOTWINDOWMANAGER_H
