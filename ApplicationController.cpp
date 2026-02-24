#include "ApplicationController.h"
#include "DataCacheManager.h"
#include "SerialReceiver.h"
#include "PlotWindow.h"
#include "PlotWindowManager.h"
#include "DataProcessor.h"
#include "AppConfig.h"
#include <QThread>
#include <QMetaObject>
#include <QApplication>
#include <QDebug>

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent)
    , m_cacheManager(nullptr)
    , m_plotWindowManager(nullptr)
{
    qInfo() << "应用控制器已创建";
    
    // 加载配置
    AppConfig* config = AppConfig::instance();
    if (config) {
        m_config.maxCacheSize = config->maxCacheSize();
        m_config.expireTimeMs = config->expireTimeMs();
        m_config.serialPort = config->serialPort();
        m_config.baudRate = config->baudRate();
        m_config.useMockData = config->useMockData();
        m_config.mockDataIntervalMs = config->mockDataIntervalMs();
    }
}

ApplicationController::~ApplicationController()
{
    stop();
    cleanup();
    qInfo() << "应用控制器已销毁";
}

bool ApplicationController::initialize()
{
    qInfo() << "开始初始化应用模块...";
    
    // 按照依赖顺序初始化各模块
    if (!initCacheManager()) {
        qCritical() << "初始化数据缓存管理器失败";
        return false;
    }
    
    if (!initSerialReceiver()) {
        qCritical() << "初始化串口接收模块失败";
        return false;
    }
    
    if (!initDataProcessor()) {
        qCritical() << "初始化数据处理模块失败";
        return false;
    }
    
    // 先初始化绘图窗口管理器
    if (!initPlotWindowManager()) {
        qCritical() << "初始化绘图窗口管理器失败";
        return false;
    }
    
    // 再初始化默认绘图窗口（向后兼容）
    if (!initDefaultPlotWindow()) {
        qCritical() << "初始化默认绘图窗口失败";
        return false;
    }
    
    qInfo() << "所有应用模块初始化完成";
    return true;
}

void ApplicationController::start()
{
    if (m_isRunning) {
        qWarning() << "应用已在运行状态";
        return;
    }
    
    // 显示绘图窗口
    if (m_plotWindow) {
        m_plotWindow->show();
    }
    
    // 启动数据接收
    if (m_serialReceiver) {
        if (m_config.useMockData) {
            // 使用模拟数据
            QMetaObject::invokeMethod(m_serialReceiver.get(), "startMockData", 
                                      Qt::QueuedConnection, 
                                      Q_ARG(int, m_config.mockDataIntervalMs));
            qInfo() << "已启动模拟数据，间隔" << m_config.mockDataIntervalMs << "ms";
        } else {
            // 使用真实串口
            QMetaObject::invokeMethod(m_serialReceiver.get(), "openSerial", 
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, m_config.serialPort), 
                                      Q_ARG(int, m_config.baudRate));
            qInfo() << "已打开串口" << m_config.serialPort << "，波特率" << m_config.baudRate;
        }
    }
    
    // 启动绘图窗口管理器的数据更新
    if (m_plotWindowManager) {
        m_plotWindowManager->startUpdates();
        qInfo() << "已启动绘图窗口管理器数据更新";
    }
    
    m_isRunning = true;
    qInfo() << "应用已启动";
}

void ApplicationController::stop()
{
    if (!m_isRunning) {
        return;
    }
    
    qInfo() << "正在停止应用...";
    
    // 停止数据接收
    if (m_serialReceiver) {
        m_serialReceiver->closeSerial();
    }
    
    // 停止串口线程
    if (m_serialThread) {
        m_serialThread->quit();
        if (!m_serialThread->wait(1000)) {
            qWarning() << "串口线程等待超时，强制终止";
            m_serialThread->terminate();
            m_serialThread->wait();
        }
    }
    
    // 隐藏窗口
    if (m_plotWindow) {
        m_plotWindow->hide();
    }
    
    m_isRunning = false;
    qInfo() << "应用已停止";
}

PlotWindow* ApplicationController::plotWindow() const
{
    return m_plotWindow.get();
}

bool ApplicationController::initCacheManager()
{
    m_cacheManager = DataCacheManager::instance();
    if (!m_cacheManager) {
        qCritical() << "无法获取缓存管理器实例";
        return false;
    }
    
    // 配置缓存参数
    m_cacheManager->setMaxCacheSize(m_config.maxCacheSize);
    m_cacheManager->setExpireTimeMs(m_config.expireTimeMs);
    
    qInfo() << QString("缓存管理器已初始化：最大缓存%1帧，过期时间%2ms")
               .arg(m_config.maxCacheSize)
               .arg(m_config.expireTimeMs);
    
    return true;
}

bool ApplicationController::initSerialReceiver()
{
    // 创建串口接收对象
    m_serialReceiver.reset(new SerialReceiver);
    if (!m_serialReceiver) {
        qCritical() << "创建串口接收器失败";
        return false;
    }
    
    // 创建并启动串口线程
    m_serialThread.reset(new QThread);
    m_serialReceiver->moveToThread(m_serialThread.get());
    m_serialThread->start();
    
    qInfo() << "串口接收模块已初始化，运行在独立线程";
    return true;
}

bool ApplicationController::initDataProcessor()
{
    m_dataProcessor.reset(new DataProcessor);
    if (!m_dataProcessor) {
        qCritical() << "创建数据处理器失败";
        return false;
    }
    
    qInfo() << "数据处理模块已初始化";
    return true;
}

bool ApplicationController::initPlotWindowManager()
{
    m_plotWindowManager = PlotWindowManager::instance();
    if (!m_plotWindowManager) {
        qCritical() << "无法获取绘图窗口管理器实例";
        return false;
    }
    
    qInfo() << "绘图窗口管理器已初始化";
    return true;
}

bool ApplicationController::initDefaultPlotWindow()
{
    // 创建默认绘图窗口（向后兼容）
    PlotWindowManager::PlotType windowType = static_cast<PlotWindowManager::PlotType>(m_config.initialWindowType);
    m_plotWindow.reset(m_plotWindowManager->createWindow(windowType));
    if (!m_plotWindow) {
        qCritical() << "创建默认绘图窗口失败";
        return false;
    }
    
    qInfo() << "默认绘图窗口已初始化";
    return true;
}

PlotWindowManager* ApplicationController::plotWindowManager() const
{
    return m_plotWindowManager;
}

PlotWindow* ApplicationController::createPlotWindow(PlotType type)
{
    if (!m_plotWindowManager) {
        qWarning() << "绘图窗口管理器未初始化";
        return nullptr;
    }
    
    // 将ApplicationController::PlotType转换为PlotWindowManager::PlotType
    PlotWindowManager::PlotType windowType = static_cast<PlotWindowManager::PlotType>(type);
    PlotWindow* window = m_plotWindowManager->createWindow(windowType);
    if (window) {
        window->show();
        qInfo() << "创建新绘图窗口:" << window->windowTitle();
    }
    
    return window;
}

void ApplicationController::cleanup()
{
    // 清理顺序：先停止数据处理，再销毁窗口，最后清理串口
    
    // 停止并销毁数据处理模块
    m_dataProcessor.reset();
    
    // 销毁绘图窗口
    m_plotWindow.reset();
    
    // 销毁串口接收器
    m_serialReceiver.reset();
    
    // 销毁线程
    m_serialThread.reset();
    
    // 清理绘图窗口管理器（单例，由管理器自身管理销毁）
    if (m_plotWindowManager) {
        PlotWindowManager::destroy();
        m_plotWindowManager = nullptr;
    }
    
    qInfo() << "应用资源已清理";
}
