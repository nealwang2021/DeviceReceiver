#include "PlotWindowManager.h"
#include "PlotWindowBase.h"
#include "PlotWindow.h"
#include "HeatMapPlotWindow.h"
#include "ArrayPlotWindow.h"
#include "ArrayRgbHeatmapWindow.h"
#include "PulsedDecayPlotWindow.h"
#include "InspectionPlotWindow.h"
#include "DataCacheManager.h"
#include "AppConfig.h"
#include "PlotDataHub.h"
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QDebug>
#include <QDateTime>

PlotWindowManager* PlotWindowManager::m_instance = nullptr;

PlotWindowManager::PlotWindowManager(QObject *parent)
    : QObject(parent)
    , m_updateTimer(nullptr)
    , m_isInitialized(false)
{
    initialize();
}

PlotWindowManager::~PlotWindowManager()
{
    cleanup();
}

PlotWindowManager* PlotWindowManager::instance()
{
    if (!m_instance) {
        m_instance = new PlotWindowManager();
    }
    return m_instance;
}

void PlotWindowManager::destroy()
{
    if (m_instance) {
        delete m_instance;
        m_instance = nullptr;
    }
}

void PlotWindowManager::initialize()
{
    if (m_isInitialized) {
        return;
    }

    m_updateTimer = new QTimer(this);
    int intervalMs = 50;
    if (AppConfig* config = AppConfig::instance()) {
        intervalMs = qBound(10, config->plotRefreshIntervalMs(), 1000);
    }
    m_baseUpdateIntervalMs = intervalMs;
    m_updateTimer->setInterval(intervalMs);
    connect(m_updateTimer, &QTimer::timeout, this, &PlotWindowManager::onUpdateTimer);
    if (AppConfig* config = AppConfig::instance()) {
        PlotDataHub::instance()->setMaxPoints(config->maxPlotPoints());
    }

    DataCacheManager* cacheManager = DataCacheManager::instance();
    if (cacheManager) {
        connect(cacheManager, &DataCacheManager::criticalFrameReceived,
                this, &PlotWindowManager::criticalFrameReceived);
    }

    m_isInitialized = true;
    qInfo() << "PlotWindowManager 初始化完成";
}

void PlotWindowManager::cleanup()
{
    stopUpdates();
    m_windows.clear();

    if (m_updateTimer) {
        m_updateTimer->deleteLater();
        m_updateTimer = nullptr;
    }

    m_hasLastDispatchedKey = false;
    m_lastDispatchedSequence = 0;
    m_lastDispatchedTimestamp = 0;
    m_lastDispatchedFrameId = 0;
    m_lastManagerTickMs = 0;
    m_lastPolledTotalFrames = -1;
    m_unconsumedFramesTotal = 0;
    PlotDataHub::instance()->reset();
    m_isInitialized = false;
}

PlotWindowBase* PlotWindowManager::createWindow(PlotType type, QWidget* parent)
{
    PlotWindowBase* window = nullptr;
    QString title;

    switch (type) {
    case CombinedPlot:
        window = new PlotWindow(parent);
        title = QStringLiteral("组合监控");
        break;
    case HeatmapPlot:
        window = new HeatMapPlotWindow(parent);
        title = QStringLiteral("热力图");
        break;
    case ArrayPlot:
        window = new ArrayPlotWindow(parent);
        title = QStringLiteral("阵列图");
        break;
    case PulsedDecayPlot:
        window = new PulsedDecayPlotWindow(parent);
        title = QStringLiteral("脉冲衰减");
        break;
    case InspectionPlot:
        window = new InspectionPlotWindow(parent);
        title = QStringLiteral("检测分析");
        break;
    case ArrayHeatmapPlot:
        window = new ArrayRgbHeatmapWindow(parent);
        title = QStringLiteral("阵列热力图");
        break;
    default:
        qWarning() << "PlotWindowManager::createWindow 未知类型，回退组合图, type=" << static_cast<int>(type);
        window = new PlotWindow(parent);
        title = QStringLiteral("组合监控");
        break;
    }

    if (!window) {
        qCritical() << "创建绘图窗口失败";
        return nullptr;
    }

    window->setWindowTitle(title);
    registerWindow(window);
    return window;
}

PlotWindowBase* PlotWindowManager::createWindowInMdiArea(QMdiArea* mdiArea, PlotType type)
{
    qDebug() << "[PlotWindowManager] createWindowInMdiArea enter, type" << type << "mdiArea" << mdiArea;
    if (!mdiArea) {
        qCritical() << "MDI区域无效";
        return nullptr;
    }

    PlotWindowBase* plotWindow = createWindow(type);
    qDebug() << "[PlotWindowManager] createWindow returned" << plotWindow;
    if (!plotWindow) {
        return nullptr;
    }

    qDebug() << "[PlotWindowManager] adding subwindow for" << plotWindow;
    QMdiSubWindow* subWindow = mdiArea->addSubWindow(plotWindow);
    qDebug() << "[PlotWindowManager] subwindow result" << subWindow;
    if (!subWindow) {
        qCritical() << "创建MDI子窗口失败";
        delete plotWindow;
        return nullptr;
    }

    subWindow->setAttribute(Qt::WA_DeleteOnClose);
    subWindow->setWindowTitle(plotWindow->windowTitle());
    subWindow->resize(600, 400);
    subWindow->show();

    connect(subWindow, &QMdiSubWindow::destroyed, this, [this, plotWindow]() {
        qDebug() << "[PlotWindowManager] subWindow destroyed, unregistering" << plotWindow;
        unregisterWindow(plotWindow);
    });

    qInfo() << "在MDI区域创建窗口:" << plotWindow->windowTitle();
    qDebug() << "[PlotWindowManager] createWindowInMdiArea exit";

    return plotWindow;
}

void PlotWindowManager::registerWindow(PlotWindowBase* window)
{
    if (!window || m_windows.contains(window)) {
        qDebug() << "registerWindow skipped for" << window;
        return;
    }

    qDebug() << "registering window" << window << "title" << window->windowTitle();
    m_windows.append(window);

    connect(window, &QObject::destroyed, this, [this, window]() {
        unregisterWindow(window);
    });

    connect(this, &PlotWindowManager::dataUpdated,
            window, &PlotWindowBase::onDataUpdated);
    connect(this, &PlotWindowManager::plotSnapshotUpdated,
            window, &PlotWindowBase::onPlotSnapshotUpdated);
    connect(this, &PlotWindowManager::criticalFrameReceived,
            window, &PlotWindowBase::onCriticalFrame);

    qInfo() << "注册绘图窗口:" << window->windowTitle()
            << "，当前窗口数:" << m_windows.size();

    if (auto snapshot = PlotDataHub::instance()->snapshot()) {
        emit plotSnapshotUpdated(snapshot);
    }

    emit windowAdded(window);
}

void PlotWindowManager::unregisterWindow(PlotWindowBase* window)
{
    if (!window || !m_windows.contains(window)) {
        return;
    }

    disconnect(this, &PlotWindowManager::dataUpdated,
               window, &PlotWindowBase::onDataUpdated);
    disconnect(this, &PlotWindowManager::plotSnapshotUpdated,
               window, &PlotWindowBase::onPlotSnapshotUpdated);
    disconnect(this, &PlotWindowManager::criticalFrameReceived,
               window, &PlotWindowBase::onCriticalFrame);

    m_windows.removeOne(window);

    qInfo() << "注销绘图窗口:" << window->windowTitle()
            << "，剩余窗口数:" << m_windows.size();

    emit windowRemoved(window);
}

void PlotWindowManager::updateAllWindows()
{
    QVector<FrameData> frames = getRecentFrames(5);
    if (!frames.isEmpty()) {
        emit dataUpdated(frames);
        emit plotSnapshotUpdated(PlotDataHub::instance()->appendFrames(frames));
    }
}

void PlotWindowManager::setUpdateInterval(int intervalMs)
{
    if (intervalMs < 10) {
        intervalMs = 10;
    } else if (intervalMs > 1000) {
        intervalMs = 1000;
    }

    if (m_updateTimer) {
        m_updateTimer->setInterval(intervalMs);
        qInfo() << "更新间隔设置为:" << intervalMs << "ms";
    }
}

void PlotWindowManager::startUpdates()
{
    if (m_updateTimer && !m_updateTimer->isActive()) {
        m_lastManagerTickMs = 0;
        m_lastPolledTotalFrames = -1;
        m_unconsumedFramesTotal = 0;
        m_updateTimer->start();
        qInfo() << "PlotWindowManager 开始数据更新";
    }
}

void PlotWindowManager::stopUpdates()
{
    if (m_updateTimer && m_updateTimer->isActive()) {
        m_updateTimer->stop();
        qInfo() << "PlotWindowManager 停止数据更新";
    }
}

QVector<FrameData> PlotWindowManager::getRecentFrames(int count)
{
    DataCacheManager* cacheManager = DataCacheManager::instance();
    if (cacheManager) {
        return cacheManager->getLastNFrames(count);
    }
    return QVector<FrameData>();
}

void PlotWindowManager::onUpdateTimer()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int actualIntervalMs = (m_lastManagerTickMs > 0)
        ? static_cast<int>(qBound<qint64>(0, nowMs - m_lastManagerTickMs, 60000))
        : 0;
    m_lastManagerTickMs = nowMs;

    int fetchCount = 5;
    if (m_windows.size() > 8) {
        fetchCount = 2;
    } else if (m_windows.size() > 4) {
        fetchCount = 3;
    }

    qint64 receivedFramesSinceLast = 0;
    if (DataCacheManager* cacheManager = DataCacheManager::instance()) {
        const qint64 totalFrames = cacheManager->getTotalFrameCount();
        if (m_lastPolledTotalFrames >= 0) {
            receivedFramesSinceLast = qMax<qint64>(0, totalFrames - m_lastPolledTotalFrames);
        }
        m_lastPolledTotalFrames = totalFrames;
    }

    const qint64 unconsumedNow = qMax<qint64>(0, receivedFramesSinceLast - fetchCount);
    m_unconsumedFramesTotal += unconsumedNow;

    QVector<FrameData> frames = getRecentFrames(fetchCount);
    int dispatchedFrames = 0;
    if (frames.isEmpty()) {
        emit telemetryUpdated(m_updateTimer ? m_updateTimer->interval() : m_baseUpdateIntervalMs,
                              actualIntervalMs,
                              fetchCount,
                              receivedFramesSinceLast,
                              dispatchedFrames,
                              m_unconsumedFramesTotal);
        return;
    }

    QVector<FrameData> incrementalFrames;
    incrementalFrames.reserve(frames.size());
    int startIndex = 0;
    if (m_hasLastDispatchedKey) {
        int lastSeenIndex = -1;
        for (int i = frames.size() - 1; i >= 0; --i) {
            const FrameData& frame = frames.at(i);
            if (frame.sequence == m_lastDispatchedSequence
                && frame.timestamp == m_lastDispatchedTimestamp
                && frame.frameId == m_lastDispatchedFrameId) {
                lastSeenIndex = i;
                break;
            }
        }
        startIndex = (lastSeenIndex >= 0) ? (lastSeenIndex + 1) : 0;
    }

    for (int i = startIndex; i < frames.size(); ++i) {
        incrementalFrames.append(frames.at(i));
    }

    if (incrementalFrames.isEmpty()) {
        emit telemetryUpdated(m_updateTimer ? m_updateTimer->interval() : m_baseUpdateIntervalMs,
                              actualIntervalMs,
                              fetchCount,
                              receivedFramesSinceLast,
                              dispatchedFrames,
                              m_unconsumedFramesTotal);
        return;
    }

    const FrameData& lastFrame = incrementalFrames.last();
    m_hasLastDispatchedKey = true;
    m_lastDispatchedSequence = lastFrame.sequence;
    m_lastDispatchedTimestamp = lastFrame.timestamp;
    m_lastDispatchedFrameId = lastFrame.frameId;

    emit dataUpdated(incrementalFrames);
    emit plotSnapshotUpdated(PlotDataHub::instance()->appendFrames(incrementalFrames));
    dispatchedFrames = incrementalFrames.size();

    if (m_windows.size() > 5) {
        int recommendedInterval = m_baseUpdateIntervalMs + (m_windows.size() / 5) * 10;
        if (m_updateTimer->interval() != recommendedInterval) {
            setUpdateInterval(recommendedInterval);
        }
    } else if (m_updateTimer->interval() != m_baseUpdateIntervalMs) {
        setUpdateInterval(m_baseUpdateIntervalMs);
    }

    emit telemetryUpdated(m_updateTimer ? m_updateTimer->interval() : m_baseUpdateIntervalMs,
                          actualIntervalMs,
                          fetchCount,
                          receivedFramesSinceLast,
                          dispatchedFrames,
                          m_unconsumedFramesTotal);
}
