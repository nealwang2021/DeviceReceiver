#include "PlotWindowManager.h"
#include "PlotWindow.h"
#include "DataCacheManager.h"
#include <QDebug>

// 静态成员初始化
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

    // 创建更新定时器
    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(50); // 默认50ms（20Hz）
    connect(m_updateTimer, &QTimer::timeout, this, &PlotWindowManager::onUpdateTimer);

    // 连接DataCacheManager的报警信号
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
    
    // 注意：这里不删除窗口，窗口由其父对象管理
    m_windows.clear();
    
    if (m_updateTimer) {
        m_updateTimer->deleteLater();
        m_updateTimer = nullptr;
    }
    
    m_isInitialized = false;
}

PlotWindow* PlotWindowManager::createWindow(PlotType type, QWidget* parent)
{
    // 创建PlotWindow实例
    PlotWindow* window = new PlotWindow(parent);
    if (!window) {
        qCritical() << "创建PlotWindow失败";
        return nullptr;
    }

    // 根据类型配置窗口
    QString title;
    switch (type) {
    case CombinedPlot:
        title = "温湿度监控";
        // 组合图是默认配置，不需要特殊设置
        break;
    case TemperaturePlot:
        title = "温度监控";
        // TODO: 可配置为只显示温度
        break;
    case HumidityPlot:
        title = "湿度监控";
        // TODO: 可配置为只显示湿度
        break;
    case VoltagePlot:
        title = "电压监控";
        // TODO: 可配置为显示电压
        break;
    case HistoryPlot:
        title = "历史数据";
        // TODO: 可配置为显示更长时间范围
        break;
    }

    window->setWindowTitle(title);
    
    // 注册窗口
    registerWindow(window);
    
    return window;
}

void PlotWindowManager::registerWindow(PlotWindow* window)
{
    if (!window || m_windows.contains(window)) {
        return;
    }

    m_windows.append(window);
    
    // 连接数据更新信号
    connect(this, &PlotWindowManager::dataUpdated,
            window, &PlotWindow::onDataUpdated);
    
    // 连接报警信号
    connect(this, &PlotWindowManager::criticalFrameReceived,
            window, &PlotWindow::onCriticalFrame);

    qInfo() << "注册PlotWindow:" << window->windowTitle() 
            << "，当前窗口数:" << m_windows.size();
    
    emit windowAdded(window);
}

void PlotWindowManager::unregisterWindow(PlotWindow* window)
{
    if (!window || !m_windows.contains(window)) {
        return;
    }

    // 断开信号连接
    disconnect(this, &PlotWindowManager::dataUpdated,
               window, &PlotWindow::onDataUpdated);
    disconnect(this, &PlotWindowManager::criticalFrameReceived,
               window, &PlotWindow::onCriticalFrame);

    m_windows.removeOne(window);
    
    qInfo() << "注销PlotWindow:" << window->windowTitle()
            << "，剩余窗口数:" << m_windows.size();
    
    emit windowRemoved(window);
}

void PlotWindowManager::updateAllWindows()
{
    QVector<FrameData> frames = getRecentFrames(5);
    if (!frames.isEmpty()) {
        emit dataUpdated(frames);
    }
}

void PlotWindowManager::setUpdateInterval(int intervalMs)
{
    if (intervalMs < 10) {
        intervalMs = 10; // 最小10ms（100Hz）
    } else if (intervalMs > 1000) {
        intervalMs = 1000; // 最大1000ms（1Hz）
    }
    
    if (m_updateTimer) {
        m_updateTimer->setInterval(intervalMs);
        qInfo() << "更新间隔设置为:" << intervalMs << "ms";
    }
}

void PlotWindowManager::startUpdates()
{
    if (m_updateTimer && !m_updateTimer->isActive()) {
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
    // 批量拉取最近5帧数据
    QVector<FrameData> frames = getRecentFrames(5);
    if (frames.isEmpty()) {
        return;
    }
    
    // 分发数据给所有注册的窗口
    emit dataUpdated(frames);
    
    // 性能监控：如果窗口过多，可以适当降低更新频率
    if (m_windows.size() > 5) {
        // 每5个窗口增加10ms延迟
        int recommendedInterval = 50 + (m_windows.size() / 5) * 10;
        if (m_updateTimer->interval() != recommendedInterval) {
            setUpdateInterval(recommendedInterval);
        }
    }
}