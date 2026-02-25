#include "ApplicationController.h"
#include "DataCacheManager.h"
#include "SerialReceiver.h"
#include "PlotWindow.h"
#include "PlotWindowManager.h"
#include "DataProcessor.h"
#include "AppConfig.h"
#include "MainWindow.h"
#include <QThread>
#include <QMetaObject>
#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QPushButton>

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
    
    // 先初始化主界面窗口（以便获取MDI区域）
    if (!initMainWindow()) {
        qCritical() << "初始化主界面窗口失败";
        return false;
    }
    
    qInfo() << "所有应用模块初始化完成（不创建默认MDI子窗口）";
    return true;
}

void ApplicationController::start()
{
    if (m_isRunning) {
        qWarning() << "应用已在运行状态";
        return;
    }
    
    // 确保串口线程在运行状态
    if (m_serialThread && !m_serialThread->isRunning()) {
        qInfo() << "串口线程未运行，重新启动线程";
        m_serialThread->start();
    }
    
    // 显示绘图窗口
    if (m_plotWindow) {
        m_plotWindow->show();
    }
    
    // 启动数据接收，支持重试/切换到模拟数据
    bool startedReceiving = false;
    if (m_serialReceiver) {
        if (m_config.useMockData) {
            QMetaObject::invokeMethod(m_serialReceiver.get(), "startMockData",
                                      Qt::QueuedConnection,
                                      Q_ARG(int, m_config.mockDataIntervalMs));
            qInfo() << "已启动模拟数据，间隔" << m_config.mockDataIntervalMs << "ms";
            startedReceiving = true;
        } else {
            // 尝试打开真实串口，允许用户重试或切换到模拟数据
            while (true) {
                bool opened = false;
                QMetaObject::invokeMethod(m_serialReceiver.get(), "openSerial",
                                          Qt::BlockingQueuedConnection,
                                          Q_RETURN_ARG(bool, opened),
                                          Q_ARG(QString, m_config.serialPort),
                                          Q_ARG(int, m_config.baudRate));

                if (opened) {
                    qInfo() << "已打开串口" << m_config.serialPort << "，波特率" << m_config.baudRate;
                    startedReceiving = true;
                    break;
                }

                // 打开失败，询问用户操作
                QMessageBox msgBox;
                msgBox.setWindowTitle("串口打开失败");
                msgBox.setText(QString("无法打开串口 %1 (波特率 %2)").arg(m_config.serialPort).arg(m_config.baudRate));
                msgBox.setInformativeText("请选择重试、使用模拟数据或取消。");
                QPushButton* retryBtn = msgBox.addButton("重试", QMessageBox::AcceptRole);
                QPushButton* mockBtn = msgBox.addButton("使用模拟数据", QMessageBox::DestructiveRole);
                msgBox.addButton(QMessageBox::Cancel);
                msgBox.exec();

                if (msgBox.clickedButton() == retryBtn) {
                    continue; // 再次尝试打开
                } else if (msgBox.clickedButton() == mockBtn) {
                    // 切换为模拟数据
                    m_config.useMockData = true;
                    QMetaObject::invokeMethod(m_serialReceiver.get(), "startMockData",
                                              Qt::QueuedConnection,
                                              Q_ARG(int, m_config.mockDataIntervalMs));
                    qInfo() << "切换到模拟数据，间隔" << m_config.mockDataIntervalMs << "ms";
                    startedReceiving = true;
                    break;
                } else {
                    // 取消，放弃启动数据接收
                    qInfo() << "用户取消串口打开，放弃启动数据接收";
                    startedReceiving = false;
                    break;
                }
            }
        }
    }

    // 启动绘图窗口管理器的数据更新（仅在已开始接收时）
    if (startedReceiving && m_plotWindowManager) {
        m_plotWindowManager->startUpdates();
        qInfo() << "已启动绘图窗口管理器数据更新";
    }

    m_isRunning = startedReceiving;
    qInfo() << "应用启动完成，接收状态:" << m_isRunning;
    
    // 发射启动信号
    emit started(m_isRunning);
}

void ApplicationController::stop()
{
    if (!m_isRunning) {
        return;
    }
    
    qInfo() << "正在停止应用...";
    
    // 停止数据接收，但保持线程运行以便重新连接
    if (m_serialReceiver) {
        QMetaObject::invokeMethod(m_serialReceiver.get(), "closeSerial",
                                  Qt::BlockingQueuedConnection);
        qInfo() << "串口已关闭，串口接收器线程保持运行";
    }
    
    // 不再停止串口线程，保持运行以便重新连接
    // 但需要确保线程仍在运行，如果线程意外停止，则重新启动
    if (m_serialThread && !m_serialThread->isRunning()) {
        qInfo() << "串口线程未运行，重新启动线程";
        m_serialThread->start();
    }
    
    // 停止绘图窗口管理器的数据更新
    if (m_plotWindowManager) {
        m_plotWindowManager->stopUpdates();
        qInfo() << "绘图窗口管理器数据更新已停止";
    }
    
    // 隐藏窗口
    if (m_plotWindow) {
        m_plotWindow->hide();
    }
    
    m_isRunning = false;
    qInfo() << "应用已停止";
    
    // 发射停止信号
    emit stopped();
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
    if (!m_mainWindow || !m_plotWindowManager) {
        qCritical() << "无法创建默认绘图窗口：主窗口或窗口管理器未初始化";
        return false;
    }
    
    // 在主窗口的MDI区域中创建默认绘图窗口
    PlotWindowManager::PlotType windowType = static_cast<PlotWindowManager::PlotType>(m_config.initialWindowType);
    
    // 获取MainWindow的MDI区域（需要添加访问方法）
    // 暂时使用一个变通方法：通过查找子对象获取QMdiArea
    QMdiArea* mdiArea = m_mainWindow->findChild<QMdiArea*>();
    if (!mdiArea) {
        qCritical() << "无法获取主窗口的MDI区域";
        // 回退：创建独立窗口
        m_plotWindow.reset(m_plotWindowManager->createWindow(windowType));
        if (!m_plotWindow) {
            qCritical() << "创建默认绘图窗口失败";
            return false;
        }
        qInfo() << "创建独立默认绘图窗口";
        return true;
    }
    
    // 在MDI区域中创建窗口
    PlotWindow* plotWindow = m_plotWindowManager->createWindowInMdiArea(mdiArea, windowType);
    if (!plotWindow) {
        qCritical() << "在MDI区域中创建默认绘图窗口失败";
        return false;
    }
    
    // 存储引用（但不接管所有权，因为MDI区域会管理窗口）
    // 注意：QScopedPointer需要释放所有权，因为MDI区域已经管理窗口
    m_plotWindow.take(); // 释放之前可能持有的指针
    m_plotWindow.reset(plotWindow); // 存储引用，但后续不会删除，因为MDI区域管理
    
    // 强制立即显示窗口
    if (mdiArea) {
        QList<QMdiSubWindow*> subWindows = mdiArea->subWindowList();
        if (!subWindows.isEmpty()) {
            subWindows.last()->showNormal();
            subWindows.last()->raise();
            qInfo() << "强制显示并置顶MDI子窗口";
        }
    }
    
    qInfo() << "默认绘图窗口已在MDI区域中初始化";
    return true;
}

bool ApplicationController::initMainWindow()
{
    // 创建主界面窗口
    m_mainWindow.reset(new MainWindow(this));
    if (!m_mainWindow) {
        qCritical() << "创建主界面窗口失败";
        return false;
    }
    
    // 设置窗口标题
    m_mainWindow->setWindowTitle(QString("实时数据监控 v1.0"));
    
    // 初始化主窗口界面
    m_mainWindow->initialize();
    
    // 将串口模块的信号连接到主窗口用于显示与错误提示（跨线程使用QueuedConnection）
    if (m_serialReceiver && m_mainWindow) {
        QObject::connect(m_serialReceiver.get(), &SerialReceiver::dataReceived,
                         m_mainWindow.get(), &MainWindow::onDataReceived, Qt::QueuedConnection);
        QObject::connect(m_serialReceiver.get(), &SerialReceiver::commandSent,
                         m_mainWindow.get(), &MainWindow::onCommandSent, Qt::QueuedConnection);
        QObject::connect(m_serialReceiver.get(), &SerialReceiver::commandError,
                         m_mainWindow.get(), &MainWindow::onCommandError, Qt::QueuedConnection);
    }
    
    // 连接应用控制器信号到主窗口的更新连接状态
    if (m_mainWindow) {
        QObject::connect(this, &ApplicationController::started,
                         m_mainWindow.get(), &MainWindow::updateConnectionStatus, Qt::QueuedConnection);
        QObject::connect(this, &ApplicationController::stopped,
                         m_mainWindow.get(), [this]() { m_mainWindow->updateConnectionStatus(false); }, Qt::QueuedConnection);
    }
    
    // 显示主窗口
    m_mainWindow->show();
    
    qInfo() << "主界面窗口已初始化并显示";
    return true;
}

void ApplicationController::sendCommand(const QString& command, bool isHex)
{
    if (!m_serialReceiver) {
        qWarning() << "发送指令失败：串口模块未初始化";
        return;
    }

    // 使用 QueuedConnection 调用，确保跨线程安全
    QMetaObject::invokeMethod(m_serialReceiver.get(), "sendCommand",
                              Qt::QueuedConnection,
                              Q_ARG(QString, command),
                              Q_ARG(bool, isHex));
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
    
    // 销毁主界面窗口
    m_mainWindow.reset();
    
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
