#include "ApplicationController.h"
#include "DataCacheManager.h"
#include "DataExporter.h"
#include "IReceiverBackend.h"
#include "SerialReceiver.h"
#include "GrpcReceiverBackend.h"
#include "StageReceiverBackend.h"
#include "PlotWindowBase.h"
#include "PlotWindow.h"
#include "PlotWindowManager.h"
#include "DataProcessor.h"
#include "AppConfig.h"
#include "MainWindow.h"
#include "RealtimeSqlRecorder.h"
#include "HistoryDataProvider.h"
#include <QThread>
#include <QMetaObject>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QMessageBox>
#include <QPushButton>
#include <QDir>
#include <QDateTime>

namespace {

PlotWindowManager::PlotType normalizeStoredPlotType(int v)
{
    switch (v) {
    case 5: return PlotWindowManager::HeatmapPlot;
    case 6: return PlotWindowManager::ArrayPlot;
    case 7: return PlotWindowManager::PulsedDecayPlot;
    case 8: return PlotWindowManager::InspectionPlot;
    case 9: return PlotWindowManager::ArrayHeatmapPlot;
    default: break;
    }
    if (v >= 0 && v <= static_cast<int>(PlotWindowManager::ArrayHeatmapPlot)) {
        return static_cast<PlotWindowManager::PlotType>(v);
    }
    qWarning() << "ApplicationController: 无效的绘图类型序号" << v << "，回退为组合图";
    return PlotWindowManager::CombinedPlot;
}

} // namespace

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent)
    , m_cacheManager(nullptr)
    , m_plotWindowManager(nullptr)
{
    try {
        qDebug() << "[ApplicationController] 构造函数开始";

        reloadRuntimeConfig();
        qDebug() << "[ApplicationController] 构造函数结束";
    } catch (const std::exception& e) {
        qCritical() << "[ApplicationController] 构造函数异常:" << e.what();
        throw;
    } catch (...) {
        qCritical() << "[ApplicationController] 构造函数未知异常";
        throw;
    }
}

void ApplicationController::applyReceiverBackendFromConfig()
{
    reloadRuntimeConfig();
    if (m_config.backendType.compare(m_activeBackendType, Qt::CaseInsensitive) == 0) {
        return;
    }

    qInfo() << "接收后端类型变更，重建实现:" << m_activeBackendType << "->" << m_config.backendType;
    if (m_isRunning) {
        stop();
    }
    if (!initReceiverBackend()) {
        qCritical() << "applyReceiverBackendFromConfig: 重建接收后端失败";
        return;
    }
    connectReceiverToMainWindow();
}

void ApplicationController::reloadRuntimeConfig()
{
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }

    m_config.maxCacheSize = config->maxCacheSize();
    m_config.expireTimeMs = config->expireTimeMs();
    m_config.serialPort = config->serialPort();
    m_config.baudRate = config->baudRate();
    m_config.backendType = config->receiverBackendType().trimmed().toLower();
    if (m_config.backendType.isEmpty()) {
        m_config.backendType = "grpc";
    }
    // 三轴台不再作为「主采集源」；旧配置 stage 迁移为 grpc（被测设备）
    if (m_config.backendType.compare(QStringLiteral("stage"), Qt::CaseInsensitive) == 0) {
        m_config.backendType = QStringLiteral("grpc");
    }
    m_config.grpcEndpoint = config->grpcEndpoint();
    m_config.useMockData = config->useMockData();
    m_config.mockDataIntervalMs = config->mockDataIntervalMs();
    m_config.defaultExportDirectory = config->defaultExportDirectory();
    m_config.defaultExportFormat = config->defaultExportFormat();
    qDebug() << "[ApplicationController] 配置加载完成，backend=" << m_config.backendType;
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

    if (!m_realtimeRecorder) {
        m_realtimeRecorder.reset(new RealtimeSqlRecorder(this));
    }
    const QString startupTimestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString recorderDbPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/device_realtime_%1.db").arg(startupTimestamp));
    if (!m_realtimeRecorder->start(recorderDbPath)) {
        qWarning() << "实时SQLite记录器启动失败:" << recorderDbPath;
    } else {
        qInfo() << "实时SQLite记录器文件:" << recorderDbPath;
        // 同步只读打开到 HistoryDataProvider，供历史总览/范围查询使用
        HistoryDataProvider* hdp = HistoryDataProvider::instance();
        hdp->setSessionDatabasePath(recorderDbPath);
        hdp->setSourceMode(HistoryDataProvider::HistorySourceMode::SessionRealtime);
        if (!hdp->openDatabase(recorderDbPath)) {
            qWarning() << "HistoryDataProvider 打开只读连接失败:" << recorderDbPath;
        }
    }
    
    // 按照依赖顺序初始化各模块
    if (!initCacheManager()) {
        qCritical() << "初始化数据缓存管理器失败";
        return false;
    }
    
    if (!initReceiverBackend()) {
        qCritical() << "初始化接收后端失败";
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
    if (m_connectInProgress) {
        qWarning() << "应用正在连接后端，请稍候";
        return;
    }
    
    reloadRuntimeConfig();

#ifdef QT_COMPILE_FOR_WASM
    // WebAssembly环境下强制使用模拟数据，因为没有真实串口
    m_config.useMockData = true;
    qInfo() << "WebAssembly环境：强制使用模拟数据";
#endif
    
    // 确保串口线程在运行状态
    if (m_config.backendType.compare(m_activeBackendType, Qt::CaseInsensitive) != 0) {
        qInfo() << "检测到后端类型变化，重建接收后端:" << m_activeBackendType << "->" << m_config.backendType;
        if (!initReceiverBackend()) {
            qCritical() << "重建接收后端失败";
            emit started(false);
            return;
        }
        connectReceiverToMainWindow();
    } else if (m_serialThread && !m_serialThread->isRunning()) {
        qInfo() << "接收线程未运行，重新启动线程";
        m_serialThread->start();
    }
    
    // 显示绘图窗口
    if (m_plotWindow) {
        m_plotWindow->show();
    }
    
    if (!m_serialReceiver) {
        qWarning() << "start: 接收后端未初始化";
        m_isRunning = false;
        emit started(false);
        return;
    }

    // 启动数据接收，支持重试/切换到模拟数据
    bool startedReceiving = false;
    m_isPaused = false;
    const bool isGrpcBackend = (m_config.backendType.compare("grpc", Qt::CaseInsensitive) == 0);
    const bool isGrpcLikeBackend = isGrpcBackend;

    if (isGrpcLikeBackend) {
        m_connectInProgress = true;
        emit connectionInProgressChanged(true);

        QMetaObject::invokeMethod(m_serialReceiver.get(), "setMockMode",
                                  Qt::QueuedConnection,
                                  Q_ARG(bool, m_config.useMockData));
        startGrpcBackendConnectAsync(m_config.grpcEndpoint);
        return;
    }

    if (m_config.useMockData && !isGrpcLikeBackend) {
        QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, m_config.mockDataIntervalMs));
        qInfo() << "已启动模拟数据，间隔" << m_config.mockDataIntervalMs << "ms";
        startedReceiving = true;
    } else {
        // 非 gRPC 后端保留同步重试路径
        while (true) {
            bool connected = false;
            const QString endpoint = QString("%1|%2").arg(m_config.serialPort).arg(m_config.baudRate);

            QMetaObject::invokeMethod(m_serialReceiver.get(), "connectBackend",
                                      Qt::BlockingQueuedConnection,
                                      Q_RETURN_ARG(bool, connected),
                                      Q_ARG(QString, endpoint));

            if (connected) {
                qInfo() << "后端连接成功" << m_config.backendType << endpoint;
                QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                                          Qt::QueuedConnection,
                                          Q_ARG(int, m_config.mockDataIntervalMs));
                startedReceiving = true;
                break;
            }

            QMessageBox msgBox;
            msgBox.setWindowTitle("后端连接失败");
            msgBox.setText(QString("无法连接后端 %1").arg(endpoint));
            msgBox.setInformativeText("请选择重试、使用模拟数据或取消。");
            QPushButton* retryBtn = msgBox.addButton("重试", QMessageBox::AcceptRole);
            QPushButton* mockBtn = msgBox.addButton("使用模拟数据", QMessageBox::DestructiveRole);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.exec();

            if (msgBox.clickedButton() == retryBtn) {
                continue;
            }
            if (msgBox.clickedButton() == mockBtn) {
                m_config.useMockData = true;
                QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                                          Qt::QueuedConnection,
                                          Q_ARG(int, m_config.mockDataIntervalMs));
                qInfo() << "切换到模拟数据，间隔" << m_config.mockDataIntervalMs << "ms";
                startedReceiving = true;
                break;
            }

            qInfo() << "用户取消后端连接，放弃启动数据接收";
            startedReceiving = false;
            break;
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
    if (m_connectInProgress) {
        m_connectInProgress = false;
        emit connectionInProgressChanged(false);
        if (m_serialReceiver) {
            QMetaObject::invokeMethod(m_serialReceiver.get(), "disconnectBackend",
                                      Qt::QueuedConnection);
        }
    }

    if (!m_isRunning) {
        return;
    }
    
    qInfo() << "正在停止应用...";
    
    // 停止数据接收，但保持线程运行以便重新连接
    if (m_serialReceiver) {
        QMetaObject::invokeMethod(m_serialReceiver.get(), "stopAcquisition",
                      Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_serialReceiver.get(), "disconnectBackend",
                                  Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_serialReceiver.get(), "setPaused",
                                  Qt::BlockingQueuedConnection,
                                  Q_ARG(bool, false));
        qInfo() << "接收后端已停止，接收线程保持运行";
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
    m_isPaused = false;
    qInfo() << "应用已停止";
    
    // 发射停止信号
    emit stopped();
}

PlotWindowBase* ApplicationController::plotWindow() const
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

bool ApplicationController::initReceiverBackend()
{
    if (m_serialReceiver) {
        if (m_serialThread && m_serialThread->isRunning()) {
            QMetaObject::invokeMethod(m_serialReceiver.get(), "stopAcquisition",
                                      Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(m_serialReceiver.get(), "disconnectBackend",
                                      Qt::BlockingQueuedConnection);
            m_serialThread->quit();
            m_serialThread->wait(3000);
        }
        m_serialReceiver.reset();
        m_serialThread.reset();
    }

    QString backendType = m_config.backendType.trimmed().toLower();
    if (backendType.compare(QStringLiteral("stage"), Qt::CaseInsensitive) == 0) {
        backendType = QStringLiteral("grpc");
    }
    if (backendType.compare("grpc", Qt::CaseInsensitive) == 0) {
        m_serialReceiver.reset(new GrpcReceiverBackend);
    } else {
        m_serialReceiver.reset(new SerialReceiver);
    }
    if (!m_serialReceiver) {
        qCritical() << "创建接收后端失败";
        return false;
    }
    
    // 创建并启动串口线程
    m_serialThread.reset(new QThread);
    m_serialReceiver->moveToThread(m_serialThread.get());
    m_serialThread->start();

    QObject::connect(m_serialReceiver.get(), &IReceiverBackend::frameReceived,
                     this, [this](const FrameData& frame) {
                         if (!m_cacheManager) {
                             return;
                         }
                         if (m_realtimeRecorder) {
                             m_realtimeRecorder->enqueueFrame(frame);
                         }
                         FrameData copy = frame;
                         // 附加最近一次三轴台位（mm + pulse）；与 DUT 时间戳可能不同，见 FrameData 注释
                         m_stagePoseLatch.applyToFrame(copy);
                         m_cacheManager->addFrame(copy);
                     }, Qt::DirectConnection);

    if (auto* grpcBackend = qobject_cast<GrpcReceiverBackend*>(m_serialReceiver.get())) {
        QObject::connect(grpcBackend, &GrpcReceiverBackend::connectAttemptFinished,
                         this, &ApplicationController::handleGrpcConnectAttemptFinished,
                         Qt::QueuedConnection);
    }

    m_activeBackendType = backendType;
    qInfo() << "接收后端已初始化（被测设备），运行在独立线程，类型=" << m_activeBackendType;
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
    PlotWindowManager::PlotType windowType = normalizeStoredPlotType(static_cast<int>(m_config.initialWindowType));
    
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
    PlotWindowBase* plotWindow = m_plotWindowManager->createWindowInMdiArea(mdiArea, windowType);
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
    
    // 窗口标题由 MainWindow::initialize() 根据 AppConfig::appTitle()（config.ini General/AppTitle）设置
    
    // 初始化主窗口界面
    m_mainWindow->initialize();
    
    connectReceiverToMainWindow();
    
    // 连接应用控制器信号到主窗口的更新连接状态
    if (m_mainWindow) {
        QObject::connect(this, &ApplicationController::started,
                         m_mainWindow.get(), &MainWindow::updateConnectionStatus, Qt::QueuedConnection);
        QObject::connect(this, &ApplicationController::stopped,
                         m_mainWindow.get(), [this]() { m_mainWindow->updateConnectionStatus(false); }, Qt::QueuedConnection);
        QObject::connect(this, &ApplicationController::connectionInProgressChanged,
                         m_mainWindow.get(), &MainWindow::onConnectionProgressChanged, Qt::QueuedConnection);
        if (m_realtimeRecorder) {
            QObject::disconnect(m_realtimeRecorder.get(), nullptr, m_mainWindow.get(), nullptr);
            QObject::connect(m_realtimeRecorder.get(), &RealtimeSqlRecorder::dropFrameAlert,
                             m_mainWindow.get(), &MainWindow::onRecorderDropAlert, Qt::QueuedConnection);
        }
    }
    
    // 显示主窗口
    m_mainWindow->show();
    
    qInfo() << "主界面窗口已初始化并显示";
    return true;
}

void ApplicationController::connectReceiverToMainWindow()
{
    if (!m_serialReceiver || !m_mainWindow) {
        return;
    }

    QObject::disconnect(m_serialReceiver.get(), nullptr, m_mainWindow.get(), nullptr);

    QObject::connect(m_serialReceiver.get(), &IReceiverBackend::dataReceived,
                     m_mainWindow.get(), &MainWindow::onDataReceived, Qt::QueuedConnection);
    QObject::connect(m_serialReceiver.get(), &IReceiverBackend::commandSent,
                     m_mainWindow.get(), &MainWindow::onCommandSent, Qt::QueuedConnection);
    QObject::connect(m_serialReceiver.get(), &IReceiverBackend::commandError,
                     m_mainWindow.get(), &MainWindow::onCommandError, Qt::QueuedConnection);
    QObject::connect(m_serialReceiver.get(), &IReceiverBackend::connectionStateChanged,
                     m_mainWindow.get(), &MainWindow::updateConnectionStatus, Qt::QueuedConnection);
}

void ApplicationController::startGrpcBackendConnectAsync(const QString& endpoint)
{
    if (!m_serialReceiver) {
        qWarning() << "startGrpcBackendConnectAsync: 接收后端未初始化";
        m_connectInProgress = false;
        emit connectionInProgressChanged(false);
        emit started(false);
        return;
    }

    qInfo() << "开始异步连接 gRPC 后端:" << endpoint << "mock=" << m_config.useMockData;
    QMetaObject::invokeMethod(m_serialReceiver.get(), "connectBackend",
                              Qt::QueuedConnection,
                              Q_ARG(QString, endpoint));
}

void ApplicationController::handleGrpcConnectAttemptFinished(bool connected, const QString& detail)
{
    if (!m_connectInProgress) {
        return;
    }

    if (connected) {
        qInfo() << "后端连接成功 grpc" << m_config.grpcEndpoint << detail;
        QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, m_config.mockDataIntervalMs));
        if (m_plotWindowManager) {
            m_plotWindowManager->startUpdates();
            qInfo() << "已启动绘图窗口管理器数据更新";
        }

        m_isRunning = true;
        m_connectInProgress = false;
        emit connectionInProgressChanged(false);
        emit started(true);
        return;
    }

    QMessageBox msgBox;
    msgBox.setWindowTitle("后端连接失败");
    msgBox.setText(QString("无法连接后端 %1").arg(m_config.grpcEndpoint));
    if (!detail.trimmed().isEmpty()) {
        msgBox.setInformativeText(detail);
    } else {
        msgBox.setInformativeText("请选择重试、使用模拟数据或取消。");
    }
    QPushButton* retryBtn = msgBox.addButton("重试", QMessageBox::AcceptRole);
    QPushButton* mockBtn = msgBox.addButton("使用模拟数据", QMessageBox::DestructiveRole);
    msgBox.addButton(QMessageBox::Cancel);
    msgBox.exec();

    if (msgBox.clickedButton() == retryBtn) {
        startGrpcBackendConnectAsync(m_config.grpcEndpoint);
        return;
    }

    if (msgBox.clickedButton() == mockBtn) {
        m_config.useMockData = true;
        QMetaObject::invokeMethod(m_serialReceiver.get(), "setMockMode",
                                  Qt::QueuedConnection,
                                  Q_ARG(bool, true));
        startGrpcBackendConnectAsync(m_config.grpcEndpoint);
        return;
    }

    qInfo() << "用户取消后端连接，放弃启动数据接收";
    m_isRunning = false;
    m_connectInProgress = false;
    emit connectionInProgressChanged(false);
    emit started(false);
}

void ApplicationController::connectStageReceiverToMainWindow()
{
    if (!m_stageReceiver || !m_mainWindow) {
        return;
    }
    QObject::disconnect(m_stageReceiver.get(), nullptr, m_mainWindow.get(), nullptr);
    QObject::connect(m_stageReceiver.get(), &IReceiverBackend::dataReceived,
                     m_mainWindow.get(), &MainWindow::onStageDataReceived, Qt::QueuedConnection);
    QObject::connect(m_stageReceiver.get(), &IReceiverBackend::commandSent,
                     m_mainWindow.get(), &MainWindow::onStageCommandSent, Qt::QueuedConnection);
    QObject::connect(m_stageReceiver.get(), &IReceiverBackend::commandError,
                     m_mainWindow.get(), &MainWindow::onStageCommandError, Qt::QueuedConnection);
    QObject::connect(m_stageReceiver.get(), &IReceiverBackend::connectionStateChanged,
                     m_mainWindow.get(), &MainWindow::onStageConnectionStateChanged, Qt::QueuedConnection);
}

void ApplicationController::disconnectStageReceiverFromMainWindow()
{
    if (!m_stageReceiver || !m_mainWindow) {
        return;
    }
    QObject::disconnect(m_stageReceiver.get(), nullptr, m_mainWindow.get(), nullptr);
}

bool ApplicationController::connectStageBackend(const QString& endpoint)
{
    if (!m_mainWindow) {
        return false;
    }
    const QString ep = endpoint.trimmed();
    if (ep.isEmpty()) {
        qWarning() << "connectStageBackend: endpoint 为空";
        return false;
    }

    disconnectStageBackend();

    m_stageReceiver.reset(new StageReceiverBackend);
    m_stageThread.reset(new QThread);
    m_stageReceiver->moveToThread(m_stageThread.get());
    m_stageThread->start();

    connectStageReceiverToMainWindow();

    if (auto* stage = qobject_cast<StageReceiverBackend*>(m_stageReceiver.get())) {
        QObject::connect(stage, &StageReceiverBackend::stagePoseUpdated,
                         this,
                         [this](double xMm, double yMm, double zMm, int xPulse, int yPulse, int zPulse, qint64 unixMs) {
                             m_stagePoseLatch.update(xMm, yMm, zMm, xPulse, yPulse, zPulse, unixMs);
                         },
                         Qt::DirectConnection);
    }

    bool connected = false;
    QMetaObject::invokeMethod(m_stageReceiver.get(), "connectBackend",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, connected),
                              Q_ARG(QString, ep));
    if (!connected) {
        qWarning() << "connectStageBackend: 连接失败" << ep;
        disconnectStageBackend();
        return false;
    }
    qInfo() << "三轴台 Stage 后端已连接:" << ep;
    return true;
}

void ApplicationController::disconnectStageBackend()
{
    disconnectStageReceiverFromMainWindow();

    m_stagePoseLatch.clear();

    if (m_stageReceiver && m_stageThread && m_stageThread->isRunning()) {
        QMetaObject::invokeMethod(m_stageReceiver.get(), "stopAcquisition",
                                  Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_stageReceiver.get(), "disconnectBackend",
                                  Qt::BlockingQueuedConnection);
    }

    m_stageReceiver.reset();

    if (m_stageThread) {
        if (m_stageThread->isRunning()) {
            m_stageThread->quit();
            m_stageThread->wait(3000);
        }
        m_stageThread.reset();
    }

    emit stageConnectionStateChanged(false);
    qInfo() << "三轴台 Stage 后端已断开";
}

void ApplicationController::sendStageCommand(const QString& command, bool isHex)
{
    if (!m_stageReceiver) {
        qWarning() << "sendStageCommand: 三轴台后端未初始化";
        return;
    }
    QMetaObject::invokeMethod(m_stageReceiver.get(), "sendCommand",
                              Qt::QueuedConnection,
                              Q_ARG(QString, command),
                              Q_ARG(bool, isHex));
}

bool ApplicationController::isStageConnected()
{
    if (!m_stageReceiver) {
        return false;
    }
    bool ok = false;
    QMetaObject::invokeMethod(m_stageReceiver.get(), "isBackendConnected",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, ok));
    return ok;
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

void ApplicationController::pauseAcquisition()
{
    if (!m_serialReceiver) {
        return;
    }

    QMetaObject::invokeMethod(m_serialReceiver.get(), "setPaused",
                              Qt::BlockingQueuedConnection,
                              Q_ARG(bool, true));
    m_isPaused = true;
    emit paused(true);
}

void ApplicationController::resumeAcquisition()
{
    if (!m_serialReceiver) {
        return;
    }

    QMetaObject::invokeMethod(m_serialReceiver.get(), "setPaused",
                              Qt::BlockingQueuedConnection,
                              Q_ARG(bool, false));
    if (m_isRunning &&
        (m_config.useMockData ||
         m_config.backendType.compare("grpc", Qt::CaseInsensitive) == 0)) {
        QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, m_config.mockDataIntervalMs));
    }
    m_isPaused = false;
    emit paused(false);
}

bool ApplicationController::exportCacheToFile(const QString& filePath,
                                              const QString& format,
                                              qint64 startTimeMs,
                                              qint64 endTimeMs,
                                              QString* errorMessage)
{
    if (!m_cacheManager) {
        if (errorMessage) {
            *errorMessage = "缓存管理器未初始化";
        }
        return false;
    }

    QVector<FrameData> frames;
    if (startTimeMs > 0 && endTimeMs > 0 && endTimeMs >= startTimeMs) {
        frames = m_cacheManager->getFramesInTimeRange(startTimeMs, endTimeMs);
    } else {
        frames = m_cacheManager->getAllFrames();
    }

    DataExporter::Format exportFormat = DataExporter::Format::Csv;
    QString formatLower = format.toLower();
    if (formatLower == "hdf5" || formatLower == "h5") {
        exportFormat = DataExporter::Format::Hdf5;
    }

    DataExporter::ExportOptions options;
    options.filePath = filePath;
    options.sourceTag = m_config.useMockData ? "mock" : "device";
    options.startTimeMs = startTimeMs;
    options.endTimeMs = endTimeMs;

    return DataExporter::exportFrames(frames, exportFormat, options, errorMessage);
}

PlotWindowManager* ApplicationController::plotWindowManager() const
{
    return m_plotWindowManager;
}

PlotWindowBase* ApplicationController::createPlotWindow(PlotType type)
{
    if (!m_plotWindowManager) {
        qWarning() << "绘图窗口管理器未初始化";
        return nullptr;
    }
    
    PlotWindowManager::PlotType windowType = normalizeStoredPlotType(static_cast<int>(type));
    PlotWindowBase* window = m_plotWindowManager->createWindow(windowType);
    if (window) {
        window->show();
        qInfo() << "创建新绘图窗口:" << window->windowTitle();
    }
    
    return window;
}

void ApplicationController::cleanup()
{
    if (m_realtimeRecorder) {
        m_realtimeRecorder->stop();
        m_realtimeRecorder.reset();
    }

    // 先断开三轴台（信号指向 MainWindow，须在销毁主窗口前）
    disconnectStageBackend();

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
    if (m_serialThread && m_serialThread->isRunning()) {
        m_serialThread->quit();
        m_serialThread->wait(3000);
    }
    m_serialThread.reset();
    
    // 清理绘图窗口管理器（单例，由管理器自身管理销毁）
    if (m_plotWindowManager) {
        PlotWindowManager::destroy();
        m_plotWindowManager = nullptr;
    }
    
    qInfo() << "应用资源已清理";
}
