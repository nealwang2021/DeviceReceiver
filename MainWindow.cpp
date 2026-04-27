#include "MainWindow.h"
#include "ApplicationController.h"
#include "PlotWindowManager.h"
#include "PlotWindowBase.h"
#include "AppConfig.h"
#include "SerialReceiver.h"
#include "DataCacheManager.h"
#include "HistoryOverviewWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QMessageBox>
#include <QMenuBar>
#include <QScreen>
#include <QStatusBar>
#include <QAction>
#include <QToolBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QScrollBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>
#include <QLabel>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QFrame>
#include <QWindow>
#include <QShowEvent>
#include <cmath>

#ifndef QT_COMPILE_FOR_WASM
#include <QProcess>
#endif

#ifndef QT_COMPILE_FOR_WASM
#include <QSerialPortInfo>
#endif

MainWindow::MainWindow(ApplicationController* controller, QWidget* parent)
    : QMainWindow(parent)
    , m_appController(controller)
    , m_plotWindowManager(nullptr)
    , m_currentStyle(AppConfig::LightStyle) // 默认浅色样式
    , m_mdiArea(nullptr)
    , m_isConnected(false)
    , m_connectionInProgress(false)
    , m_frameCount(0)
    , m_alarmCount(0)
    , m_lastUpdateTime(0)
    , m_lastMonitorAppendTime(0)
    , m_monitorAppendIntervalMs(200)
    , m_lastGrpcStreamTimestampMs(0)
    , m_grpcSelfTestPending(false)
    , m_grpcSelfTestCommandAcked(false)
    , m_grpcSelfTestModeSwitchAcked(false)
    , m_grpcSelfTestStreamReceived(false)
    , m_autoSelfTestTriggeredForConnection(false)
    , m_grpcPeriodicPacketCount(0)
    , m_grpcPeriodicIntervalSumMs(0)
    , m_lastGrpcStreamPayloadTimestampMs(0)
#ifndef QT_COMPILE_FOR_WASM
    , m_grpcTestServerProcess(nullptr)
    , m_grpcTestServerStartTimeoutTimer(nullptr)
    , m_grpcTestServerLaunchInProgress(false)
    , m_grpcTestServerStopRequested(false)
#endif
{
    setObjectName(QStringLiteral("mainWindow"));
    try {
        qDebug() << "[MainWindow] 构造函数开始";
        
        // 初始化定时器
        m_updateTimer = new QTimer(this);
        m_updateTimer->setInterval(1000); // 1秒更新一次
        connect(m_updateTimer, &QTimer::timeout, this, &MainWindow::onUpdateTimer);

        m_grpcSelfTestTimeoutTimer = new QTimer(this);
        m_grpcSelfTestTimeoutTimer->setSingleShot(true);
        m_grpcSelfTestTimeoutTimer->setInterval(5000);

    #ifndef QT_COMPILE_FOR_WASM
        m_grpcTestServerProcess = new QProcess(this);
        m_grpcTestServerProcess->setProcessChannelMode(QProcess::SeparateChannels);
        m_grpcTestServerStartTimeoutTimer = new QTimer(this);
        m_grpcTestServerStartTimeoutTimer->setSingleShot(true);
        m_grpcTestServerStartTimeoutTimer->setInterval(1800);
        connect(m_grpcTestServerStartTimeoutTimer,
                &QTimer::timeout,
                this,
                &MainWindow::onGrpcTestServerStartTimeout);
    #endif
        
        qDebug() << "[MainWindow] 构造函数结束";
    } catch (const std::exception& e) {
        qCritical() << "[MainWindow] 构造函数异常:" << e.what();
        throw;
    } catch (...) {
        qCritical() << "[MainWindow] 构造函数未知异常";
        throw;
    }
}

MainWindow::~MainWindow()
{
    // 注意：不在析构时 saveConfigFromUI() — 此时子控件多已析构/隐藏，isVisible() 不可靠，
    // 会误把各面板存成“隐藏”，覆盖 closeEvent 中已写入的正确布局。
    saveCommandHistory();
}

void MainWindow::initialize()
{
    try {
        qDebug() << "[MainWindow::initialize] 开始";
        
        // 获取PlotWindowManager
        qDebug() << "[MainWindow::initialize] 获取PlotWindowManager...";
        if (m_appController) {
            m_plotWindowManager = m_appController->plotWindowManager();
        }
        qDebug() << "[MainWindow::initialize] PlotWindowManager获取完成";
        
        // 初始化界面组件
        qDebug() << "[MainWindow::initialize] 调用initUI...";
        initUI();
        qDebug() << "[MainWindow::initialize] initUI完成";
        
        // 初始化信号槽连接
        qDebug() << "[MainWindow::initialize] 初始化信号槽连接...";
        initConnections();
        qDebug() << "[MainWindow::initialize] 信号槽连接完成";
        
        // 更新串口列表
        qDebug() << "[MainWindow::initialize] 更新串口列表...";
        updateSerialPortList();
        qDebug() << "[MainWindow::initialize] 串口列表更新完成";

        // 加载配置到界面
        qDebug() << "[MainWindow::initialize] 加载配置到界面...";
        loadConfigToUI();
        qDebug() << "[MainWindow::initialize] 配置加载完成";
        
        // 加载指令历史
        qDebug() << "[MainWindow::initialize] 加载指令历史...";
        loadCommandHistory();
        qDebug() << "[MainWindow::initialize] 指令历史加载完成";

        // 恢复上次打开的 MDI 绘图窗口（QMainWindow::restoreState 不会自动重建 MDI 子窗口）
        restoreSavedPlotWindowsFromConfig();
        
        // 更新窗口列表
        qDebug() << "[MainWindow::initialize] 更新窗口列表...";
        updateWindowList();
        qDebug() << "[MainWindow::initialize] 窗口列表更新完成";
        
        // 启动更新定时器
        qDebug() << "[MainWindow::initialize] 启动更新定时器...";
        m_updateTimer->start();
        qDebug() << "[MainWindow::initialize] 更新定时器已启动";
        
        // 设置窗口标题
        qDebug() << "[MainWindow::initialize] 设置窗口属性...";
        AppConfig* config = AppConfig::instance();
        if (config) {
            setWindowTitle(config->appTitle());
            
            // 应用保存的样式
            setStyle(config->currentStyle());
        }
        qDebug() << "[MainWindow::initialize] 窗口属性设置完成";
        
        qDebug() << "[MainWindow::initialize] 完成";
    } catch (const std::exception& e) {
        qCritical() << "[MainWindow::initialize] 异常:" << e.what();
        throw;
    } catch (...) {
        qCritical() << "[MainWindow::initialize] 未知异常";
        throw;
    }
}

void MainWindow::updateConnectionStatus(bool connected)
{
    m_isConnected = connected;
    m_connectionInProgress = false;
    
    if (connected) {
        m_connectionStatusLabel->setText("已连接");
        m_connectionStatusLabel->setStyleSheet("color: green;");
        m_connectButton->setEnabled(false);
        m_disconnectButton->setEnabled(true);
        m_pauseButton->setEnabled(true);
        m_resumeButton->setEnabled(false);
    } else {
        m_connectionStatusLabel->setText("未连接");
        m_connectionStatusLabel->setStyleSheet("color: red;");
        m_connectButton->setEnabled(true);
        m_disconnectButton->setEnabled(false);
        m_pauseButton->setEnabled(false);
        m_resumeButton->setEnabled(false);

        const bool interruptedSelfTest = m_grpcSelfTestPending;
        m_autoSelfTestTriggeredForConnection = false;
        if (m_grpcSelfTestPending) {
            m_grpcSelfTestPending = false;
            m_grpcSelfTestTimeoutTimer->stop();
        }
        m_grpcSelfTestCommandAcked = false;
        m_grpcSelfTestModeSwitchAcked = false;
        m_grpcSelfTestStreamReceived = false;
        m_grpcSelfTestPendingAcks.clear();
        m_grpcSelfTestTargetMode.clear();
        resetGrpcSelfTestLabelStates();
        if (interruptedSelfTest) {
            setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("连接断开"), GrpcLabelTone::Error);
        }
        m_grpcPeriodicPacketCount = 0;
        m_grpcPeriodicIntervalSumMs = 0;
        m_lastGrpcStreamPayloadTimestampMs = 0;
        m_lastGrpcStreamTimestampMs = 0;
    }

    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
    const bool isGrpcRealMode = (isGrpcBackend && m_useMockDataCheck && !m_useMockDataCheck->isChecked());

    if (isGrpcBackend) {
        logGrpcInteraction("connect-state", connected ? "客户端状态: 已连接" : "客户端状态: 已断开");
    }

    // 当前 device.proto 协议未提供通用 SendCommand，自动 selftest 会产生误导性错误日志，默认关闭。

    updateStagePanelUiState();
    updateGrpcTestUiState();
}

void MainWindow::onConnectionProgressChanged(bool inProgress)
{
    m_connectionInProgress = inProgress;
    if (!inProgress) {
        if (!m_isConnected) {
            updateConnectionStatus(false);
        }
        return;
    }

    m_connectionStatusLabel->setText(QStringLiteral("连接中..."));
    m_connectionStatusLabel->setStyleSheet(QStringLiteral("color: #d97706;"));
    m_connectButton->setEnabled(false);
    m_disconnectButton->setEnabled(false);
    m_pauseButton->setEnabled(false);
    m_resumeButton->setEnabled(false);
    updateGrpcTestUiState();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
#ifndef QT_COMPILE_FOR_WASM
    if (m_grpcTestServerProcess && m_grpcTestServerProcess->state() != QProcess::NotRunning) {
        m_grpcTestServerLaunchInProgress = false;
        m_grpcTestServerStopRequested = true;
        m_grpcTestServerLaunchQueue.clear();
        m_grpcTestServerStartErrors.clear();
        m_grpcTestServerCurrentDisplayName.clear();
        if (m_grpcTestServerStartTimeoutTimer) {
            m_grpcTestServerStartTimeoutTimer->stop();
        }
        m_grpcTestServerProcess->terminate();
        m_grpcTestServerProcess->kill();
    }
#endif

    // 保存界面配置、面板可见性和窗口状态
    saveConfigFromUI();

    AppConfig* config = AppConfig::instance();
    if (config) {
        config->saveToFile(AppConfig::defaultConfigFilePath());
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (QWindow* wh = windowHandle()) {
        if (!m_screenGeometryScreenHooked) {
            connect(wh, &QWindow::screenChanged, this, &MainWindow::applyScreenGeometryConstraints);
            m_screenGeometryScreenHooked = true;
        }
    }
    applyScreenGeometryConstraints();
}

void MainWindow::applyScreenGeometryConstraints()
{
    if (isFullScreen()) {
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        return;
    }

    QScreen* scr = nullptr;
    if (QWindow* wh = windowHandle()) {
        scr = wh->screen();
    }
    if (!scr) {
        scr = QGuiApplication::primaryScreen();
    }
    if (!scr) {
        return;
    }

    const QRect available = scr->availableGeometry();
    const QRect current = geometry();
    const bool exceedsScreen =
        (current.width() > available.width())
        || (current.height() > available.height());

    // Only constrain maximum size when current window exceeds the screen.
    // Otherwise keep QWIDGETSIZE_MAX to not block maximize button behavior.
    if (exceedsScreen) {
        setMaximumSize(available.size());
    } else {
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }

    if (isMaximized()) {
        return;
    }

    QRect geo = geometry();
    const QSize minimumHint = minimumSizeHint().expandedTo(minimumSize());
    const int minWidth = qMin(qMax(320, minimumHint.width()), available.width());
    const int minHeight = qMin(qMax(320, minimumHint.height()), available.height());

    geo.setWidth(qBound(minWidth, geo.width(), available.width()));
    geo.setHeight(qBound(minHeight, geo.height(), available.height()));

    if (geo.left() < available.left()) {
        geo.moveLeft(available.left());
    }
    if (geo.top() < available.top()) {
        geo.moveTop(available.top());
    }
    if (geo.right() > available.right()) {
        geo.moveLeft(available.right() - geo.width() + 1);
    }
    if (geo.bottom() > available.bottom()) {
        geo.moveTop(available.bottom() - geo.height() + 1);
    }

    setGeometry(geo);

    // Restore unconstrained maximum size so the window remains maximizable.
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
}

void MainWindow::initUI()
{
    qDebug() << "[MainWindow::initUI] 开始";
    
    try {
        // 创建菜单栏
        qDebug() << "[MainWindow::initUI] 创建菜单栏...";
        QMenuBar* menuBar = new QMenuBar(this);
        setMenuBar(menuBar);
        qDebug() << "[MainWindow::initUI] 菜单栏创建完成";
    
        // 文件菜单
        QMenu* fileMenu = menuBar->addMenu("文件(&F)");
        QAction* exitAction = fileMenu->addAction("退出(&X)");
        connect(exitAction, &QAction::triggered, this, &MainWindow::close);
        
        // 视图菜单
        QMenu* viewMenu = menuBar->addMenu("视图(&V)");
        QAction* showDeviceAction = viewMenu->addAction(QStringLiteral("被测设备面板(&D)"));
        QAction* showCommandAction = viewMenu->addAction("指令面板(&C)");
        QAction* showPlotAction = viewMenu->addAction("绘图面板(&P)");
        QAction* showStageAction = viewMenu->addAction(QStringLiteral("三轴台测试装置(&S)"));
        QAction* showMonitorAction = viewMenu->addAction("监控面板(&M)");
        QAction* showOverviewAction = viewMenu->addAction(QStringLiteral("历史总览(&O)"));
        
        showDeviceAction->setCheckable(true);
        showCommandAction->setCheckable(true);
        showPlotAction->setCheckable(true);
        showStageAction->setCheckable(true);
        showMonitorAction->setCheckable(true);
        showOverviewAction->setCheckable(true);
        
        connect(showDeviceAction, &QAction::toggled, this, &MainWindow::onShowDevicePanelChanged);
        connect(showCommandAction, &QAction::toggled, this, &MainWindow::onShowCommandPanelChanged);
        connect(showPlotAction, &QAction::toggled, this, &MainWindow::onShowPlotPanelChanged);
        connect(showStageAction, &QAction::toggled, this, &MainWindow::onShowStagePanelChanged);
        connect(showMonitorAction, &QAction::toggled, this, &MainWindow::onShowMonitorPanelChanged);
        connect(showOverviewAction, &QAction::toggled, this, &MainWindow::onShowOverviewPanelChanged);
        m_showStagePanelAction = showStageAction;
        m_showOverviewPanelAction = showOverviewAction;
        
        // 窗口菜单
        QMenu* windowMenu = menuBar->addMenu("窗口(&W)");
        QAction* createWindowAction = windowMenu->addAction("新建窗口(&N)");
        QAction* tileAction = windowMenu->addAction("平铺窗口(&T)");
        QAction* cascadeAction = windowMenu->addAction("层叠窗口(&C)");
        
        connect(createWindowAction, &QAction::triggered, this, &MainWindow::onCreateWindowClicked);
        connect(tileAction, &QAction::triggered, this, &MainWindow::onTileWindowsClicked);
        connect(cascadeAction, &QAction::triggered, this, &MainWindow::onCascadeWindowsClicked);
        
        // 样式菜单
        QMenu* styleMenu = menuBar->addMenu("样式(&S)");
        QAction* darkStyleAction = styleMenu->addAction("深色主题(&D)");
        QAction* lightStyleAction = styleMenu->addAction("浅色主题(&L)");
        QAction* switchStyleAction = styleMenu->addAction("切换主题(&T)");
        
        darkStyleAction->setCheckable(true);
        lightStyleAction->setCheckable(true);
        
        // 样式菜单连接
        connect(darkStyleAction, &QAction::triggered, [this]() {
            setStyle(AppConfig::DarkStyle);
        });
        connect(lightStyleAction, &QAction::triggered, [this]() {
            setStyle(AppConfig::LightStyle);
        });
        connect(switchStyleAction, &QAction::triggered, this, &MainWindow::switchStyle);
        
        // 工具栏
        QToolBar* toolBar = addToolBar("工具");
        toolBar->addAction(exitAction);
        toolBar->addSeparator();
        
        // 创建MDI区域作为中心部件
        m_mdiArea = new QMdiArea(this);
        m_mdiArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_mdiArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_mdiArea->setViewMode(QMdiArea::TabbedView); // 可以改为子窗口模式
        m_mdiArea->setTabsClosable(true);
        m_mdiArea->setTabsMovable(true);
        setCentralWidget(m_mdiArea);
        
        // 创建浮动面板
        
        // 1. 被测设备（DUT）— 与右侧三轴台测试装置（gRPC）概念分离
        m_devicePanel = new QDockWidget(QStringLiteral("被测设备"), this);
        m_devicePanel->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        
        QWidget* deviceWidget = new QWidget();
        deviceWidget->setMinimumWidth(260);
        deviceWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        QVBoxLayout* deviceLayout = new QVBoxLayout(deviceWidget);
        
        QGroupBox* serialGroup = new QGroupBox(QStringLiteral("被测设备连接"));
        serialGroup->setToolTip(
            QStringLiteral("配置被测设备（DUT）的数据来源：串口或设备数据 gRPC。\n"
                          "与右侧「三轴台测试装置」无关；后者为工装/定位，走独立 gRPC。"));
        QFormLayout* serialLayout = new QFormLayout(serialGroup);
        
        m_serialPortCombo = new QComboBox();
        m_backendTypeCombo = new QComboBox();
        m_backendTypeCombo->addItem(QStringLiteral("串口（被测设备）"), "serial");
        m_backendTypeCombo->addItem(QStringLiteral("gRPC（被测设备数据）"), "grpc");
        // 三轴台测试装置为独立 gRPC，不在此列出（见右侧「三轴台测试装置」面板）
        m_grpcEndpointEdit = new QLineEdit();
        m_grpcEndpointEdit->setPlaceholderText(QStringLiteral("被测设备 gRPC，如 127.0.0.1:50051 或 [::1]:50051"));
        m_baudRateCombo = new QComboBox();
        m_dataBitsCombo = new QComboBox();
        m_stopBitsCombo = new QComboBox();
        m_parityCombo = new QComboBox();
        m_flowControlCombo = new QComboBox();
        
        m_baudRateCombo->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800"});
        m_dataBitsCombo->addItems({"5", "6", "7", "8"});
        m_stopBitsCombo->addItems({"1", "1.5", "2"});
        m_parityCombo->addItems({"无", "偶校验", "奇校验", "标记", "空格"});
        m_flowControlCombo->addItems({"无", "硬件", "软件"});

        m_serialOnlyFields.clear();
        m_serialOnlyLabels.clear();

        auto addSerialOnlyRow = [this, serialLayout](const QString& labelText, QWidget* field) {
            serialLayout->addRow(labelText, field);
            m_serialOnlyFields.append(field);
            if (QWidget* label = serialLayout->labelForField(field)) {
                m_serialOnlyLabels.append(label);
            }
        };
        
        serialLayout->addRow(QStringLiteral("数据来源:"), m_backendTypeCombo);
        serialLayout->addRow(QStringLiteral("被测设备 gRPC:"), m_grpcEndpointEdit);
        addSerialOnlyRow("端口:", m_serialPortCombo);
        addSerialOnlyRow("波特率:", m_baudRateCombo);
        addSerialOnlyRow("数据位:", m_dataBitsCombo);
        addSerialOnlyRow("停止位:", m_stopBitsCombo);
        addSerialOnlyRow("校验位:", m_parityCombo);
        addSerialOnlyRow("流控制:", m_flowControlCombo);
        
        // 模拟数据组
        QGroupBox* mockGroup = new QGroupBox("模拟数据");
        QVBoxLayout* mockLayout = new QVBoxLayout(mockGroup);
        
        m_useMockDataCheck = new QCheckBox("启用模拟数据");
        QHBoxLayout* intervalLayout = new QHBoxLayout();
        intervalLayout->addWidget(new QLabel("间隔(ms):"));
        m_mockIntervalSpin = new QSpinBox();
        m_mockIntervalSpin->setRange(10, 5000);
        m_mockIntervalSpin->setValue(100);
        intervalLayout->addWidget(m_mockIntervalSpin);
        intervalLayout->addStretch();
        
        mockLayout->addWidget(m_useMockDataCheck);
        mockLayout->addLayout(intervalLayout);
        
        // 控制按钮组
        QGroupBox* controlGroup = new QGroupBox(QStringLiteral("被测设备采集"));
        controlGroup->setToolTip(QStringLiteral("启动/停止的是被测设备主数据通道（串口或 gRPC）；三轴台为独立连接"));
        QVBoxLayout* controlLayout = new QVBoxLayout(controlGroup);
        QHBoxLayout* controlRow1Layout = new QHBoxLayout();
        QHBoxLayout* controlRow2Layout = new QHBoxLayout();
        QHBoxLayout* controlStatusLayout = new QHBoxLayout();
        
        m_connectButton = new QPushButton("连接");
        m_disconnectButton = new QPushButton("断开");
        m_pauseButton = new QPushButton("暂停采集");
        m_resumeButton = new QPushButton("恢复采集");
        m_disconnectButton->setEnabled(false);
        m_pauseButton->setEnabled(false);
        m_resumeButton->setEnabled(false);
        m_connectionStatusLabel = new QLabel("未连接");
        m_connectionStatusLabel->setStyleSheet("color: red;");

        for (QPushButton* button : {m_connectButton, m_disconnectButton, m_pauseButton, m_resumeButton}) {
            button->setMinimumWidth(0);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }

        controlRow1Layout->addWidget(m_connectButton);
        controlRow1Layout->addWidget(m_disconnectButton);
        controlRow1Layout->addWidget(m_pauseButton);

        controlRow2Layout->addWidget(m_resumeButton);
        controlRow2Layout->addStretch();

        controlStatusLayout->addWidget(m_connectionStatusLabel);
        controlStatusLayout->addStretch();
        
        controlLayout->addLayout(controlRow1Layout);
        controlLayout->addLayout(controlRow2Layout);
        controlLayout->addLayout(controlStatusLayout);

        QGroupBox* grpcTestGroup = new QGroupBox(QStringLiteral("被测设备 gRPC 测试验证"));
        m_grpcTestGroup = grpcTestGroup;
        QVBoxLayout* grpcTestLayout = new QVBoxLayout(grpcTestGroup);

        QHBoxLayout* grpcServiceLayout = new QHBoxLayout();
        grpcServiceLayout->addWidget(new QLabel("测试服务:"));
        m_grpcTestServiceStatusLabel = new QLabel("未启动");
        m_grpcTestServiceStatusLabel->setStyleSheet("color: gray;");
        grpcServiceLayout->addWidget(m_grpcTestServiceStatusLabel);
        grpcServiceLayout->addStretch();

        QHBoxLayout* grpcSelfTestLayout = new QHBoxLayout();
        grpcSelfTestLayout->addWidget(new QLabel("收发自检:"));
        m_grpcSelfTestStatusLabel = new QLabel("未执行");
        grpcSelfTestLayout->addWidget(m_grpcSelfTestStatusLabel);
        grpcSelfTestLayout->addStretch();

        QHBoxLayout* grpcResultLayout = new QHBoxLayout();
        m_grpcSelfTestTxStatusLabel = new QLabel("发送(Ping): 待验证");
        m_grpcSelfTestRxStatusLabel = new QLabel("接收(流数据): 待验证");
        grpcResultLayout->addWidget(m_grpcSelfTestTxStatusLabel);
        grpcResultLayout->addWidget(m_grpcSelfTestRxStatusLabel);
        grpcResultLayout->addStretch();

        QHBoxLayout* grpcModeLayout = new QHBoxLayout();
        grpcModeLayout->addWidget(new QLabel("目标模式:"));
        m_grpcModeCombo = new QComboBox();
        m_grpcModeCombo->addItem("complex (复数)", "complex");
        m_grpcModeCombo->addItem("real (幅值/相位)", "real");
        m_grpcModeCombo->addItem("legacy (旧模式)", "legacy");
        grpcModeLayout->addWidget(m_grpcModeCombo);
        grpcModeLayout->addStretch();

        QHBoxLayout* grpcModeStatusLayout = new QHBoxLayout();
        grpcModeStatusLayout->addWidget(new QLabel("模式切换:"));
        m_grpcModeSwitchStatusLabel = new QLabel("待验证");
        grpcModeStatusLayout->addWidget(m_grpcModeSwitchStatusLabel);
        grpcModeStatusLayout->addStretch();

        QHBoxLayout* grpcPeriodicLayout = new QHBoxLayout();
        grpcPeriodicLayout->addWidget(new QLabel("周期数据:"));
        m_grpcPeriodicDataStatusLabel = new QLabel("待验证");
        grpcPeriodicLayout->addWidget(m_grpcPeriodicDataStatusLabel);
        grpcPeriodicLayout->addStretch();

        resetGrpcSelfTestLabelStates();
        applyGrpcSelfTestLabelStates();

        QHBoxLayout* grpcActionLayout = new QHBoxLayout();
        m_startGrpcTestServerButton = new QPushButton("启动测试服务");
        m_stopGrpcTestServerButton = new QPushButton("停止测试服务");
        m_runGrpcSelfTestButton = new QPushButton("立即自检");
        m_stopGrpcTestServerButton->setEnabled(false);
        m_runGrpcSelfTestButton->setEnabled(false);
        grpcActionLayout->addWidget(m_startGrpcTestServerButton);
        grpcActionLayout->addWidget(m_stopGrpcTestServerButton);
        grpcActionLayout->addWidget(m_runGrpcSelfTestButton);
        grpcActionLayout->addStretch();

    #ifdef QT_COMPILE_FOR_WASM
        m_startGrpcTestServerButton->setEnabled(false);
        m_stopGrpcTestServerButton->setEnabled(false);
        m_grpcTestServiceStatusLabel->setText("WASM 不支持本地服务");
    #endif

        grpcTestLayout->addLayout(grpcServiceLayout);
        grpcTestLayout->addLayout(grpcSelfTestLayout);
        grpcTestLayout->addLayout(grpcResultLayout);
        grpcTestLayout->addLayout(grpcModeLayout);
        grpcTestLayout->addLayout(grpcModeStatusLayout);
        grpcTestLayout->addLayout(grpcPeriodicLayout);
        grpcTestLayout->addLayout(grpcActionLayout);
        
        deviceLayout->addWidget(serialGroup);
        deviceLayout->addWidget(mockGroup);
        deviceLayout->addWidget(controlGroup);
        deviceLayout->addWidget(grpcTestGroup);
        // 与「三轴台测试装置」一致：停靠条内用纵向滚动条承载内容。
        // 须限制 QScrollArea 最大高度：否则其 minimumSizeHint 会随子控件总高度变大，主窗口最小尺寸被撑出屏幕（看起来像「滚动未生效」）。
        auto* deviceScroll = new QScrollArea();
        deviceScroll->setWidgetResizable(true);
        deviceScroll->setFrameShape(QFrame::NoFrame);
        deviceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        deviceScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        deviceScroll->setMinimumSize(0, 0);
        deviceScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        if (QScreen* scr = QGuiApplication::primaryScreen()) {
            const int cap = qMax(360, scr->availableGeometry().height() - 140);
            deviceScroll->setMaximumHeight(cap);
        }
        deviceScroll->setWidget(deviceWidget);

        m_devicePanel->setWidget(deviceScroll);
        addDockWidget(Qt::LeftDockWidgetArea, m_devicePanel);

        // 2. 三轴台测试装置（工装，gRPC StageService）— 非被测设备
        m_stagePanel = new QDockWidget(QStringLiteral("三轴台测试装置"), this);
        m_stagePanel->setObjectName(QStringLiteral("stage_panelDock"));
        m_stagePanel->setAllowedAreas(Qt::RightDockWidgetArea);
        m_stagePanel->setMinimumWidth(300);

        QWidget* stageWidget = new QWidget();
        stageWidget->setMinimumWidth(280);
        stageWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        QVBoxLayout* stageLayout = new QVBoxLayout(stageWidget);
        stageLayout->setContentsMargins(4, 4, 4, 6);
        stageLayout->setSpacing(8);

        auto createStageDoubleSpin = [](double minValue,
                                        double maxValue,
                                        double step,
                                        double defaultValue,
                                        int decimals = 3) {
            QDoubleSpinBox* spin = new QDoubleSpinBox();
            spin->setDecimals(decimals);
            spin->setRange(minValue, maxValue);
            spin->setSingleStep(step);
            spin->setValue(defaultValue);
            return spin;
        };

        // —— 连接与地址 ——
        QGroupBox* stageGrpcGroup = new QGroupBox(QStringLiteral("连接与工装地址"));
        stageGrpcGroup->setObjectName(QStringLiteral("stage_group_grpc"));
        stageGrpcGroup->setToolTip(QStringLiteral(
            "三轴台 gRPC：host:port 或 [IPv6]:port；若与台下位机 TCP 不同，可用 grpc段|台下位机host:port"));
        QVBoxLayout* grpcVBox = new QVBoxLayout(stageGrpcGroup);
        grpcVBox->setContentsMargins(8, 8, 8, 8);
        grpcVBox->setSpacing(6);
        QHBoxLayout* stageEndpointLayout = new QHBoxLayout();
        stageEndpointLayout->setSpacing(6);
        QLabel* stageAddrLbl = new QLabel(QStringLiteral("gRPC 地址"));
        stageAddrLbl->setMinimumWidth(56);
        m_stageEndpointEdit = new QLineEdit();
        m_stageEndpointEdit->setObjectName(QStringLiteral("stage_endpointEdit"));
        m_stageEndpointEdit->setPlaceholderText(QStringLiteral("例: 127.0.0.1:50052 或 [::1]:50052 或 [::1]:50052|192.168.1.10:9000"));
        m_stageEndpointEdit->setToolTip(QStringLiteral(
            "独立保存于 config.ini 的 [Receiver]/StageGrpcEndpoint；与被测设备 GrpcEndpoint 可不同"));
        m_stageApplyEndpointButton = new QPushButton(QStringLiteral("应用"));
        m_stageApplyEndpointButton->setObjectName(QStringLiteral("stage_applyEndpointButton"));
        m_stageApplyEndpointButton->setToolTip(QStringLiteral("将地址写入配置（下次启动仍有效）"));
        stageEndpointLayout->addWidget(stageAddrLbl);
        stageEndpointLayout->addWidget(m_stageEndpointEdit, 1);
        stageEndpointLayout->addWidget(m_stageApplyEndpointButton);
        QHBoxLayout* stageConnectRowLayout = new QHBoxLayout();
        stageConnectRowLayout->setSpacing(6);
        m_stageConnectButton = new QPushButton(QStringLiteral("连接"));
        m_stageConnectButton->setObjectName(QStringLiteral("stage_connectButton"));
        m_stageDisconnectButton = new QPushButton(QStringLiteral("断开"));
        m_stageDisconnectButton->setObjectName(QStringLiteral("stage_disconnectButton"));
        stageConnectRowLayout->addWidget(m_stageConnectButton);
        stageConnectRowLayout->addWidget(m_stageDisconnectButton);
        stageConnectRowLayout->addStretch();
        m_stageBackendStatusLabel = new QLabel(QStringLiteral("状态: 未连接"));
        m_stageBackendStatusLabel->setObjectName(QStringLiteral("stage_backendStatusLabel"));
        m_stageBackendStatusLabel->setStyleSheet(QStringLiteral("color: gray;"));
        m_stageBackendStatusLabel->setWordWrap(true);
        grpcVBox->addLayout(stageEndpointLayout);
        grpcVBox->addLayout(stageConnectRowLayout);
        grpcVBox->addWidget(m_stageBackendStatusLabel);
        stageLayout->addWidget(stageGrpcGroup);

        // —— 位置反馈（突出显示）——
        QGroupBox* stagePosGroup = new QGroupBox(QStringLiteral("位置反馈"));
        stagePosGroup->setObjectName(QStringLiteral("stage_group_position"));
        stagePosGroup->setToolTip(QStringLiteral(
            "GetPositions 单次读取；PositionStream 按下方间隔持续推送（JSON 带 source/unixMs）"));
        QVBoxLayout* posVBox = new QVBoxLayout(stagePosGroup);
        posVBox->setContentsMargins(8, 8, 8, 8);
        posVBox->setSpacing(6);
        QFrame* posFrame = new QFrame();
        posFrame->setObjectName(QStringLiteral("stage_positionFrame"));
        posFrame->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
        posFrame->setStyleSheet(QStringLiteral(
            "QFrame#stage_positionFrame { background: palette(base); border: 1px solid palette(mid); "
            "border-radius: 4px; padding: 4px; }"));
        QVBoxLayout* posFrameLay = new QVBoxLayout(posFrame);
        posFrameLay->setContentsMargins(6, 6, 6, 6);
        QLabel* posCaption = new QLabel(QStringLiteral("当前位置 (mm / pulse 由下位机返回)"));
        QFont capFont = posCaption->font();
        capFont.setPointSizeF(capFont.pointSizeF() * 0.95);
        capFont.setBold(false);
        posCaption->setFont(capFont);
        posCaption->setStyleSheet(QStringLiteral("color: palette(mid);"));
        m_stagePositionLabel = new QLabel(QStringLiteral("X: —   Y: —   Z: —"));
        m_stagePositionLabel->setObjectName(QStringLiteral("stage_positionLabel"));
        m_stagePositionLabel->setWordWrap(true);
        m_stagePositionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = m_stagePositionLabel->font();
        mono.setFamily(QStringLiteral("Consolas"));
        if (!mono.exactMatch()) {
            mono.setStyleHint(QFont::Monospace);
        }
        mono.setPointSizeF(mono.pointSizeF() + 1.0);
        m_stagePositionLabel->setFont(mono);
        posFrameLay->addWidget(posCaption);
        posFrameLay->addWidget(m_stagePositionLabel);
        posVBox->addWidget(posFrame);
        QHBoxLayout* stageStreamLayout = new QHBoxLayout();
        stageStreamLayout->setSpacing(6);
        QLabel* intervalLbl = new QLabel(QStringLiteral("推流间隔"));
        intervalLbl->setToolTip(QStringLiteral("PositionStream 推送周期"));
        m_stageStreamIntervalSpin = new QSpinBox();
        m_stageStreamIntervalSpin->setObjectName(QStringLiteral("stage_streamIntervalSpin"));
        m_stageStreamIntervalSpin->setRange(10, 5000);
        m_stageStreamIntervalSpin->setValue(100);
        m_stageStreamIntervalSpin->setSuffix(QStringLiteral(" ms"));
        m_stageStreamIntervalSpin->setMinimumWidth(88);
        stageStreamLayout->addWidget(intervalLbl);
        stageStreamLayout->addWidget(m_stageStreamIntervalSpin);
        stageStreamLayout->addStretch();
        QHBoxLayout* stagePositionButtonsLayout = new QHBoxLayout();
        stagePositionButtonsLayout->setSpacing(6);
        m_stageGetPositionButton = new QPushButton(QStringLiteral("读一次"));
        m_stageGetPositionButton->setObjectName(QStringLiteral("stage_getPositionButton"));
        m_stageGetPositionButton->setToolTip(QStringLiteral("GetPositions"));
        m_stageStartStreamButton = new QPushButton(QStringLiteral("订阅位置流"));
        m_stageStartStreamButton->setObjectName(QStringLiteral("stage_startStreamButton"));
        m_stageStartStreamButton->setToolTip(QStringLiteral("PositionStream"));
        m_stageStopStreamButton = new QPushButton(QStringLiteral("停止推流"));
        m_stageStopStreamButton->setObjectName(QStringLiteral("stage_stopStreamButton"));
        stagePositionButtonsLayout->addWidget(m_stageGetPositionButton);
        stagePositionButtonsLayout->addWidget(m_stageStartStreamButton);
        stagePositionButtonsLayout->addWidget(m_stageStopStreamButton);
        stagePositionButtonsLayout->addStretch();
        posVBox->addLayout(stageStreamLayout);
        posVBox->addLayout(stagePositionButtonsLayout);
        stageLayout->addWidget(stagePosGroup);

        // —— 点动 ——
        QGroupBox* stageJogGroup = new QGroupBox(QStringLiteral("点动 (Jog)"));
        stageJogGroup->setObjectName(QStringLiteral("stage_group_jog"));
        stageJogGroup->setToolTip(QStringLiteral("JogStart / JogStop，连续运动"));
        QVBoxLayout* jogVBox = new QVBoxLayout(stageJogGroup);
        jogVBox->setContentsMargins(8, 8, 8, 8);
        jogVBox->setSpacing(6);
        QHBoxLayout* jogPickRow = new QHBoxLayout();
        jogPickRow->setSpacing(6);
        jogPickRow->addWidget(new QLabel(QStringLiteral("轴")));
        m_stageJogAxisCombo = new QComboBox();
        m_stageJogAxisCombo->setObjectName(QStringLiteral("stage_jogAxisCombo"));
        m_stageJogAxisCombo->addItem(QStringLiteral("X"), QStringLiteral("x"));
        m_stageJogAxisCombo->addItem(QStringLiteral("Y"), QStringLiteral("y"));
        m_stageJogAxisCombo->addItem(QStringLiteral("Z"), QStringLiteral("z"));
        m_stageJogAxisCombo->setMinimumWidth(56);
        jogPickRow->addWidget(m_stageJogAxisCombo);
        jogPickRow->addWidget(new QLabel(QStringLiteral("方向")));
        m_stageJogDirectionCombo = new QComboBox();
        m_stageJogDirectionCombo->setObjectName(QStringLiteral("stage_jogDirectionCombo"));
        m_stageJogDirectionCombo->addItem(QStringLiteral("正向 +"), QStringLiteral("+"));
        m_stageJogDirectionCombo->addItem(QStringLiteral("负向 -"), QStringLiteral("-"));
        m_stageJogDirectionCombo->setMinimumWidth(72);
        jogPickRow->addWidget(m_stageJogDirectionCombo);
        jogPickRow->addStretch();
        jogVBox->addLayout(jogPickRow);
        QHBoxLayout* jogBtnRow = new QHBoxLayout();
        jogBtnRow->setSpacing(6);
        m_stageJogStartButton = new QPushButton(QStringLiteral("开始点动"));
        m_stageJogStartButton->setObjectName(QStringLiteral("stage_jogStartButton"));
        m_stageJogStartButton->setToolTip(QStringLiteral("JogStart"));
        m_stageJogStopButton = new QPushButton(QStringLiteral("停止"));
        m_stageJogStopButton->setObjectName(QStringLiteral("stage_jogStopButton"));
        m_stageJogStopButton->setToolTip(QStringLiteral("JogStop"));
        jogBtnRow->addWidget(m_stageJogStartButton);
        jogBtnRow->addWidget(m_stageJogStopButton);
        jogBtnRow->addStretch();
        jogVBox->addLayout(jogBtnRow);
        stageLayout->addWidget(stageJogGroup);

        // —— 绝对 / 相对运动 ——
        QGroupBox* stageMoveGroup = new QGroupBox(QStringLiteral("目标运动 (MoveAbs / MoveRel)"));
        stageMoveGroup->setObjectName(QStringLiteral("stage_group_move"));
        stageMoveGroup->setToolTip(QStringLiteral("绝对坐标与相对位移，单位 mm"));
        QVBoxLayout* moveVBox = new QVBoxLayout(stageMoveGroup);
        moveVBox->setContentsMargins(8, 8, 8, 8);
        moveVBox->setSpacing(8);
        QLabel* absCap = new QLabel(QStringLiteral("绝对坐标 (mm)"));
        absCap->setStyleSheet(QStringLiteral("font-weight: bold;"));
        moveVBox->addWidget(absCap);
        QGridLayout* stageAbsLayout = new QGridLayout();
        stageAbsLayout->setHorizontalSpacing(8);
        stageAbsLayout->setVerticalSpacing(4);
        stageAbsLayout->addWidget(new QLabel(QStringLiteral("X")), 0, 0);
        stageAbsLayout->addWidget(new QLabel(QStringLiteral("Y")), 0, 1);
        stageAbsLayout->addWidget(new QLabel(QStringLiteral("Z")), 0, 2);
        m_stageMoveAbsXSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 0.0);
        m_stageMoveAbsXSpin->setObjectName(QStringLiteral("stage_moveAbsXSpin"));
        m_stageMoveAbsYSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 0.0);
        m_stageMoveAbsYSpin->setObjectName(QStringLiteral("stage_moveAbsYSpin"));
        m_stageMoveAbsZSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 0.0);
        m_stageMoveAbsZSpin->setObjectName(QStringLiteral("stage_moveAbsZSpin"));
        for (QDoubleSpinBox* s : {m_stageMoveAbsXSpin, m_stageMoveAbsYSpin, m_stageMoveAbsZSpin}) {
            s->setMinimumWidth(90);
        }
        stageAbsLayout->addWidget(m_stageMoveAbsXSpin, 1, 0);
        stageAbsLayout->addWidget(m_stageMoveAbsYSpin, 1, 1);
        stageAbsLayout->addWidget(m_stageMoveAbsZSpin, 1, 2);
        moveVBox->addLayout(stageAbsLayout);
        QHBoxLayout* stageAbsActionLayout = new QHBoxLayout();
        stageAbsActionLayout->setSpacing(6);
        QLabel* toLbl = new QLabel(QStringLiteral("到位超时"));
        toLbl->setToolTip(QStringLiteral("MoveAbs 等待完成的最长时间"));
        m_stageMoveTimeoutSpin = new QSpinBox();
        m_stageMoveTimeoutSpin->setObjectName(QStringLiteral("stage_moveTimeoutSpin"));
        m_stageMoveTimeoutSpin->setRange(100, 600000);
        m_stageMoveTimeoutSpin->setValue(10000);
        m_stageMoveTimeoutSpin->setMinimumWidth(100);
        m_stageMoveTimeoutSpin->setSuffix(QStringLiteral(" ms"));
        m_stageMoveAbsButton = new QPushButton(QStringLiteral("执行绝对运动"));
        m_stageMoveAbsButton->setObjectName(QStringLiteral("stage_moveAbsButton"));
        m_stageMoveAbsButton->setToolTip(QStringLiteral("MoveAbs"));
        stageAbsActionLayout->addWidget(toLbl);
        stageAbsActionLayout->addWidget(m_stageMoveTimeoutSpin);
        stageAbsActionLayout->addWidget(m_stageMoveAbsButton);
        stageAbsActionLayout->addStretch();
        moveVBox->addLayout(stageAbsActionLayout);
        QFrame* moveSep = new QFrame();
        moveSep->setFrameShape(QFrame::HLine);
        moveSep->setFrameShadow(QFrame::Sunken);
        moveVBox->addWidget(moveSep);
        QLabel* relCap = new QLabel(QStringLiteral("相对位移 (mm)"));
        relCap->setStyleSheet(QStringLiteral("font-weight: bold;"));
        moveVBox->addWidget(relCap);
        QHBoxLayout* stageRelLayout = new QHBoxLayout();
        stageRelLayout->setSpacing(6);
        stageRelLayout->addWidget(new QLabel(QStringLiteral("轴")));
        m_stageMoveRelAxisCombo = new QComboBox();
        m_stageMoveRelAxisCombo->setObjectName(QStringLiteral("stage_moveRelAxisCombo"));
        m_stageMoveRelAxisCombo->addItem(QStringLiteral("X"), QStringLiteral("x"));
        m_stageMoveRelAxisCombo->addItem(QStringLiteral("Y"), QStringLiteral("y"));
        m_stageMoveRelAxisCombo->addItem(QStringLiteral("Z"), QStringLiteral("z"));
        m_stageMoveRelAxisCombo->setMinimumWidth(56);
        m_stageMoveRelDeltaSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 1.0);
        m_stageMoveRelDeltaSpin->setObjectName(QStringLiteral("stage_moveRelDeltaSpin"));
        m_stageMoveRelDeltaSpin->setMinimumWidth(90);
        m_stageMoveRelDeltaSpin->setToolTip(QStringLiteral("沿选定轴移动的增量（mm），即 MoveRel 的 delta"));
        m_stageMoveRelButton = new QPushButton(QStringLiteral("执行相对运动"));
        m_stageMoveRelButton->setObjectName(QStringLiteral("stage_moveRelButton"));
        m_stageMoveRelButton->setToolTip(QStringLiteral("MoveRel"));
        stageRelLayout->addWidget(m_stageMoveRelAxisCombo);
        QLabel* relDeltaLbl = new QLabel(QStringLiteral("增量"));
        relDeltaLbl->setToolTip(QStringLiteral("相对位移量 (mm)"));
        stageRelLayout->addWidget(relDeltaLbl);
        stageRelLayout->addWidget(m_stageMoveRelDeltaSpin);
        stageRelLayout->addWidget(m_stageMoveRelButton);
        stageRelLayout->addStretch();
        moveVBox->addLayout(stageRelLayout);
        stageLayout->addWidget(stageMoveGroup);

        // —— 速度 / 加速度 ——
        QGroupBox* stageSpeedGroup = new QGroupBox(QStringLiteral("运动参数 (SetSpeed)"));
        stageSpeedGroup->setObjectName(QStringLiteral("stage_group_speed"));
        stageSpeedGroup->setToolTip(QStringLiteral("脉冲/秒 与 加减速时间 (ms)"));
        QHBoxLayout* stageSpeedLayout = new QHBoxLayout(stageSpeedGroup);
        stageSpeedLayout->setContentsMargins(8, 8, 8, 8);
        stageSpeedLayout->setSpacing(6);
        QLabel* spdLbl = new QLabel(QStringLiteral("速度"));
        spdLbl->setToolTip(QStringLiteral("pulse/s"));
        m_stageSpeedSpin = new QSpinBox();
        m_stageSpeedSpin->setObjectName(QStringLiteral("stage_speedSpin"));
        m_stageSpeedSpin->setRange(1, 10000000);
        m_stageSpeedSpin->setValue(20000);
        m_stageSpeedSpin->setSuffix(QStringLiteral(" 脉冲/s"));
        m_stageSpeedSpin->setMinimumWidth(112);
        stageSpeedLayout->addWidget(spdLbl);
        stageSpeedLayout->addWidget(m_stageSpeedSpin);
        QLabel* accLbl = new QLabel(QStringLiteral("加速"));
        accLbl->setToolTip(QStringLiteral("加减速时间"));
        m_stageAccelSpin = new QSpinBox();
        m_stageAccelSpin->setObjectName(QStringLiteral("stage_accelSpin"));
        m_stageAccelSpin->setRange(0, 600000);
        m_stageAccelSpin->setValue(100);
        m_stageAccelSpin->setSuffix(QStringLiteral(" ms"));
        m_stageAccelSpin->setMinimumWidth(88);
        stageSpeedLayout->addWidget(accLbl);
        stageSpeedLayout->addWidget(m_stageAccelSpin);
        m_stageSetSpeedButton = new QPushButton(QStringLiteral("应用"));
        m_stageSetSpeedButton->setObjectName(QStringLiteral("stage_setSpeedButton"));
        m_stageSetSpeedButton->setToolTip(QStringLiteral("SetSpeed"));
        stageSpeedLayout->addWidget(m_stageSetSpeedButton);
        stageSpeedLayout->addStretch();
        stageLayout->addWidget(stageSpeedGroup);

        // —— 区域扫描 ——
        QGroupBox* stageScanGroup = new QGroupBox(QStringLiteral("区域扫描 (StartScan / StopScan)"));
        stageScanGroup->setObjectName(QStringLiteral("stage_group_scan"));
        stageScanGroup->setToolTip(QStringLiteral("在 XY 平面按模式步进扫描；Z 为固定高度"));
        QVBoxLayout* scanVBox = new QVBoxLayout(stageScanGroup);
        scanVBox->setContentsMargins(8, 8, 8, 8);
        scanVBox->setSpacing(6);
        QFormLayout* stageScanFormLayout = new QFormLayout();
        stageScanFormLayout->setSpacing(6);
        stageScanFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        m_stageScanModeCombo = new QComboBox();
        m_stageScanModeCombo->setObjectName(QStringLiteral("stage_scanModeCombo"));
        m_stageScanModeCombo->addItem(QStringLiteral("蛇形"), QStringLiteral("snake"));
        m_stageScanModeCombo->addItem(QStringLiteral("往返"), QStringLiteral("alternate_return"));
        m_stageScanXsSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 0.0);
        m_stageScanXsSpin->setObjectName(QStringLiteral("stage_scanXsSpin"));
        m_stageScanXeSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 10.0);
        m_stageScanXeSpin->setObjectName(QStringLiteral("stage_scanXeSpin"));
        m_stageScanYsSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 0.0);
        m_stageScanYsSpin->setObjectName(QStringLiteral("stage_scanYsSpin"));
        m_stageScanYeSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 10.0);
        m_stageScanYeSpin->setObjectName(QStringLiteral("stage_scanYeSpin"));
        m_stageScanStepSpin = createStageDoubleSpin(0.001, 10000.0, 0.1, 1.0);
        m_stageScanStepSpin->setObjectName(QStringLiteral("stage_scanStepSpin"));
        m_stageScanZFixSpin = createStageDoubleSpin(-10000.0, 10000.0, 0.1, 0.0);
        m_stageScanZFixSpin->setObjectName(QStringLiteral("stage_scanZFixSpin"));
        for (QDoubleSpinBox* s : {m_stageScanXsSpin,
                                  m_stageScanXeSpin,
                                  m_stageScanYsSpin,
                                  m_stageScanYeSpin,
                                  m_stageScanStepSpin,
                                  m_stageScanZFixSpin}) {
            s->setMinimumWidth(88);
        }
        stageScanFormLayout->addRow(QStringLiteral("扫描模式"), m_stageScanModeCombo);
        auto makeRangeRow = [](QDoubleSpinBox* from, QDoubleSpinBox* to) {
            QWidget* w = new QWidget();
            QHBoxLayout* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(4);
            h->addWidget(new QLabel(QStringLiteral("从")));
            h->addWidget(from, 1);
            h->addWidget(new QLabel(QStringLiteral("到")));
            h->addWidget(to, 1);
            return w;
        };
        stageScanFormLayout->addRow(QStringLiteral("X 范围 (mm)"), makeRangeRow(m_stageScanXsSpin, m_stageScanXeSpin));
        stageScanFormLayout->addRow(QStringLiteral("Y 范围 (mm)"), makeRangeRow(m_stageScanYsSpin, m_stageScanYeSpin));
        stageScanFormLayout->addRow(QStringLiteral("步长 (mm)"), m_stageScanStepSpin);
        stageScanFormLayout->addRow(QStringLiteral("Z 固定 (mm)"), m_stageScanZFixSpin);
        scanVBox->addLayout(stageScanFormLayout);
        QHBoxLayout* stageScanActionLayout = new QHBoxLayout();
        stageScanActionLayout->setSpacing(6);
        m_stageStartScanButton = new QPushButton(QStringLiteral("开始扫描"));
        m_stageStartScanButton->setObjectName(QStringLiteral("stage_startScanButton"));
        m_stageStartScanButton->setToolTip(QStringLiteral("StartScan"));
        m_stageStopScanButton = new QPushButton(QStringLiteral("停止扫描"));
        m_stageStopScanButton->setObjectName(QStringLiteral("stage_stopScanButton"));
        m_stageStopScanButton->setToolTip(QStringLiteral("StopScan"));
        m_stageScanStatusButton = new QPushButton(QStringLiteral("查询状态"));
        m_stageScanStatusButton->setObjectName(QStringLiteral("stage_scanStatusButton"));
        m_stageScanStatusButton->setToolTip(QStringLiteral("GetScanStatus"));
        stageScanActionLayout->addWidget(m_stageStartScanButton);
        stageScanActionLayout->addWidget(m_stageStopScanButton);
        stageScanActionLayout->addWidget(m_stageScanStatusButton);
        stageScanActionLayout->addStretch();
        scanVBox->addLayout(stageScanActionLayout);
        stageLayout->addWidget(stageScanGroup);

        // —— 反馈信息 ——
        QGroupBox* stageFbGroup = new QGroupBox(QStringLiteral("执行反馈"));
        stageFbGroup->setObjectName(QStringLiteral("stage_group_feedback"));
        QVBoxLayout* fbVBox = new QVBoxLayout(stageFbGroup);
        fbVBox->setContentsMargins(8, 8, 8, 8);
        fbVBox->setSpacing(4);
        m_stageScanStatusLabel = new QLabel(QStringLiteral("扫描状态: —"));
        m_stageScanStatusLabel->setObjectName(QStringLiteral("stage_scanStatusLabel"));
        m_stageScanStatusLabel->setWordWrap(true);
        m_stageCommandResultLabel = new QLabel(QStringLiteral("最近结果: —"));
        m_stageCommandResultLabel->setObjectName(QStringLiteral("stage_commandResultLabel"));
        m_stageCommandResultLabel->setWordWrap(true);
        m_stageCommandResultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        fbVBox->addWidget(m_stageScanStatusLabel);
        fbVBox->addWidget(m_stageCommandResultLabel);
        stageLayout->addWidget(stageFbGroup);

        // —— 文本指令 ——
        QGroupBox* stageTxtGroup = new QGroupBox(QStringLiteral("文本指令 (调试)"));
        stageTxtGroup->setObjectName(QStringLiteral("stage_group_textcmd"));
        stageTxtGroup->setToolTip(QStringLiteral(
            "发往三轴台工装 gRPC 的文本命令；勿使用底部「HEX发送」。输入 help 查看列表。"));
        QVBoxLayout* txtVBox = new QVBoxLayout(stageTxtGroup);
        txtVBox->setContentsMargins(8, 8, 8, 8);
        txtVBox->setSpacing(6);
        m_stageCustomCommandEdit = new QLineEdit();
        m_stageCustomCommandEdit->setObjectName(QStringLiteral("stage_customCommandEdit"));
        m_stageCustomCommandEdit->setPlaceholderText(QStringLiteral("例: get_positions / help / …"));
        QHBoxLayout* stageCustomCmdButtons = new QHBoxLayout();
        stageCustomCmdButtons->setSpacing(6);
        m_stageCustomCommandSendButton = new QPushButton(QStringLiteral("发送"));
        m_stageCustomCommandSendButton->setObjectName(QStringLiteral("stage_customCommandSendButton"));
        m_stageCommandHelpButton = new QPushButton(QStringLiteral("帮助"));
        m_stageCommandHelpButton->setObjectName(QStringLiteral("stage_commandHelpButton"));
        stageCustomCmdButtons->addWidget(m_stageCustomCommandSendButton);
        stageCustomCmdButtons->addWidget(m_stageCommandHelpButton);
        stageCustomCmdButtons->addStretch();
        txtVBox->addWidget(m_stageCustomCommandEdit);
        txtVBox->addLayout(stageCustomCmdButtons);
        stageLayout->addWidget(stageTxtGroup);

        auto* stageScroll = new QScrollArea();
        stageScroll->setWidgetResizable(true);
        stageScroll->setFrameShape(QFrame::NoFrame);
        // 停靠条内横向不滚动：由换行与弹性布局适配窄宽度；纵向过长由滚动条承担
        stageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        stageScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        stageScroll->setMinimumSize(0, 0);
        stageScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        if (QScreen* scr = QGuiApplication::primaryScreen()) {
            const int cap = qMax(360, scr->availableGeometry().height() - 140);
            stageScroll->setMaximumHeight(cap);
        }
        stageScroll->setWidget(stageWidget);

        m_stagePanel->setWidget(stageScroll);
        connect(m_stagePanel, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (!m_showStagePanelAction || m_showStagePanelAction->isChecked() == visible) {
                return;
            }
            const QSignalBlocker blocker(m_showStagePanelAction);
            m_showStagePanelAction->setChecked(visible);
        });
        
        // 3. 指令发送面板
        m_commandPanel = new QDockWidget("指令发送", this);
        m_commandPanel->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        
        QWidget* commandWidget = new QWidget();
        QVBoxLayout* commandLayout = new QVBoxLayout(commandWidget);
        
        // 指令输入
        QGroupBox* inputGroup = new QGroupBox("指令输入");
        QVBoxLayout* inputLayout = new QVBoxLayout(inputGroup);
        
        m_commandInput = new QTextEdit();
        m_commandInput->setMaximumHeight(100);
        inputLayout->addWidget(m_commandInput);
        
        // 发送选项
        QHBoxLayout* optionLayout = new QHBoxLayout();
        m_hexFormatCheck = new QCheckBox("十六进制");
        m_autoNewlineCheck = new QCheckBox("自动换行");
        m_newlineCombo = new QComboBox();
        m_newlineCombo->addItems({"\r\n (CR+LF)", "\r (CR)", "\n (LF)", "无"});
        optionLayout->addWidget(m_hexFormatCheck);
        optionLayout->addWidget(m_autoNewlineCheck);
        optionLayout->addWidget(new QLabel("换行符:"));
        optionLayout->addWidget(m_newlineCombo);
        optionLayout->addStretch();
        inputLayout->addLayout(optionLayout);
        
        // 发送按钮
        QHBoxLayout* buttonLayout = new QHBoxLayout();
        m_sendButton = new QPushButton("发送");
        m_clearButton = new QPushButton("清空");
        buttonLayout->addWidget(m_sendButton);
        buttonLayout->addWidget(m_clearButton);
        buttonLayout->addStretch();
        inputLayout->addLayout(buttonLayout);
        
        // 指令历史
        QGroupBox* historyGroup = new QGroupBox("指令历史");
        QVBoxLayout* historyLayout = new QVBoxLayout(historyGroup);
        
        m_commandHistoryList = new QListWidget();
        historyLayout->addWidget(m_commandHistoryList);
        
        commandLayout->addWidget(inputGroup);
        commandLayout->addWidget(historyGroup);
        
        m_commandPanel->setWidget(commandWidget);
        addDockWidget(Qt::RightDockWidgetArea, m_commandPanel);
        addDockWidget(Qt::RightDockWidgetArea, m_stagePanel);
        tabifyDockWidget(m_commandPanel, m_stagePanel);
        m_commandPanel->raise();
        
        // 4. 绘图管理面板
        m_plotPanel = new QDockWidget("绘图管理", this);
        m_plotPanel->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        
        QWidget* plotWidget = new QWidget();
        QVBoxLayout* plotLayout = new QVBoxLayout(plotWidget);
        
        // 窗口创建
        QGroupBox* createGroup = new QGroupBox("创建窗口");
        QFormLayout* createLayout = new QFormLayout(createGroup);
        
        m_windowTypeCombo = new QComboBox();
        m_windowTypeCombo->addItems({QStringLiteral("组合图"), QStringLiteral("热力图"), QStringLiteral("阵列图"),
                                     QStringLiteral("脉冲衰减"), QStringLiteral("检测分析"), QStringLiteral("阵列热力图")});
        m_createWindowButton = new QPushButton("新建窗口");
        
        createLayout->addRow("窗口类型:", m_windowTypeCombo);
        createLayout->addRow(m_createWindowButton);
        
        // 窗口控制
        QGroupBox* controlGroup2 = new QGroupBox("窗口控制");
        QHBoxLayout* controlLayout2 = new QHBoxLayout(controlGroup2);
        
        m_tileWindowsButton = new QPushButton("平铺");
        m_cascadeWindowsButton = new QPushButton("层叠");
        m_closeWindowButton = new QPushButton("关闭选中");
        
        controlLayout2->addWidget(m_tileWindowsButton);
        controlLayout2->addWidget(m_cascadeWindowsButton);
        controlLayout2->addWidget(m_closeWindowButton);
        controlLayout2->addStretch();
        
        // 窗口列表
        QGroupBox* listGroup = new QGroupBox("窗口列表");
        QVBoxLayout* listLayout = new QVBoxLayout(listGroup);
        
        m_windowList = new QListWidget();
        listLayout->addWidget(m_windowList);
        
        plotLayout->addWidget(createGroup);
        plotLayout->addWidget(controlGroup2);
        plotLayout->addWidget(listGroup);
        
        m_plotPanel->setWidget(plotWidget);
        addDockWidget(Qt::LeftDockWidgetArea, m_plotPanel);

        // 左侧：被测设备 + 绘图；右侧：三轴台测试装置（与 DUT 分离）
        tabifyDockWidget(m_devicePanel, m_plotPanel);
        m_devicePanel->raise();
        
        // 5. 数据监控面板
        m_monitorPanel = new QDockWidget("数据监控", this);
        m_monitorPanel->setAllowedAreas(Qt::BottomDockWidgetArea);
        
        QWidget* monitorWidget = new QWidget();
        QVBoxLayout* monitorLayout = new QVBoxLayout(monitorWidget);
        
        m_dataMonitor = new QTextEdit();
        m_dataMonitor->setReadOnly(true);
        m_dataMonitor->document()->setMaximumBlockCount(1500);
        
        // 状态栏
        QHBoxLayout* statusLayout = new QHBoxLayout();
        m_frameRateLabel = new QLabel("帧率: 0 fps");
        m_uiRefreshLabel = new QLabel(QStringLiteral("绘图周期: -- ms (-- Hz) | 拉取 -- 收到 -- 分发 -- 未消费累计 --"));
        m_dataCountLabel = new QLabel(QStringLiteral("数据: 累计 0 帧 | 缓存 0/0 帧"));
        m_alarmCountLabel = new QLabel("报警: 0 次");
        
        statusLayout->addWidget(m_frameRateLabel);
        statusLayout->addWidget(m_uiRefreshLabel);
        statusLayout->addWidget(m_dataCountLabel);
        statusLayout->addWidget(m_alarmCountLabel);
        statusLayout->addStretch();
        
        monitorLayout->addWidget(m_dataMonitor);
        monitorLayout->addLayout(statusLayout);
        
        qDebug() << "[MainWindow::initUI] 添加monitorPanel到DockWidget";
        m_monitorPanel->setWidget(monitorWidget);
        addDockWidget(Qt::BottomDockWidgetArea, m_monitorPanel);
        qDebug() << "[MainWindow::initUI] monitorPanel添加完成";

        // 6. 历史总览面板（与数据监控同区域，tab 并列）
        m_overviewPanel = new QDockWidget(QStringLiteral("历史总览"), this);
        m_overviewPanel->setAllowedAreas(Qt::BottomDockWidgetArea);
        m_overviewWindow = new HistoryOverviewWindow(this);
        m_overviewPanel->setWidget(m_overviewWindow);
        addDockWidget(Qt::BottomDockWidgetArea, m_overviewPanel);
        tabifyDockWidget(m_monitorPanel, m_overviewPanel);
        m_monitorPanel->raise();

        // 创建状态栏
        qDebug() << "[MainWindow::initUI] 创建状态栏...";
        QStatusBar* statusBar = new QStatusBar(this);
        setStatusBar(statusBar);
        qDebug() << "[MainWindow::initUI] 状态栏创建完成";
        
        qDebug() << "[MainWindow::initUI] 加载保存的窗口状态...";
        // 加载保存的窗口状态
        AppConfig* config = AppConfig::instance();
        if (config) {
            if (!config->mainWindowState().isEmpty()) {
                restoreState(config->mainWindowState());
            }

            bool geometryRestored = false;
            if (!config->mainWindowGeometry().isEmpty()) {
                geometryRestored = restoreGeometry(config->mainWindowGeometry());
            }
            if (!geometryRestored) {
                const QSize fallbackSize = config->windowSize().isValid()
                    ? config->windowSize()
                    : QSize(1200, 800);
                resize(fallbackSize);
            }
            
            // 更新面板显示状态
            showDeviceAction->setChecked(config->showDevicePanel());
            showCommandAction->setChecked(config->showCommandPanel());
            showPlotAction->setChecked(config->showPlotPanel());
            showStageAction->setChecked(config->showStagePanel());
            showMonitorAction->setChecked(config->showMonitorPanel());
            showOverviewAction->setChecked(config->showOverviewPanel());
            
            m_devicePanel->setVisible(config->showDevicePanel());
            m_commandPanel->setVisible(config->showCommandPanel());
            m_plotPanel->setVisible(config->showPlotPanel());
            if (m_stagePanel) {
                m_stagePanel->setVisible(config->showStagePanel());
            }
            m_monitorPanel->setVisible(config->showMonitorPanel());
            if (m_overviewPanel) {
                m_overviewPanel->setVisible(config->showOverviewPanel());
            }

            if (m_devicePanel->isVisible()) {
                m_devicePanel->raise();
            } else if (m_plotPanel->isVisible()) {
                m_plotPanel->raise();
            }
            if (m_commandPanel && m_commandPanel->isVisible()) {
                m_commandPanel->raise();
            } else if (m_stagePanel && m_stagePanel->isVisible()) {
                m_stagePanel->raise();
            }

            applyScreenGeometryConstraints();
        }
        qDebug() << "[MainWindow::initUI] 完成";
    } catch (const std::exception& e) {
        qCritical() << "[MainWindow::initUI] 异常:" << e.what();
        throw;
    } catch (...) {
        qCritical() << "[MainWindow::initUI] 未知异常";
        throw;
    }
}

void MainWindow::initConnections()
{
    // 设备控制信号槽
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::onPauseClicked);
    connect(m_resumeButton, &QPushButton::clicked, this, &MainWindow::onResumeClicked);
    connect(m_useMockDataCheck, &QCheckBox::toggled, this, &MainWindow::onUseMockDataChanged);
        connect(m_backendTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBackendTypeChanged);

    connect(m_stageApplyEndpointButton, &QPushButton::clicked, this, [this]() {
        const QString endpoint = m_stageEndpointEdit ? m_stageEndpointEdit->text().trimmed() : QString();
        if (endpoint.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("三轴台 gRPC 服务地址不能为空"));
            return;
        }

        if (m_stageCommandResultLabel) {
            m_stageCommandResultLabel->setText(QString("最近结果: 已应用服务地址 %1").arg(endpoint));
            m_stageCommandResultLabel->setStyleSheet("color: #1d4ed8;");
        }

        saveConfigFromUI();
        if (m_appController) {
            m_appController->reloadRuntimeConfig();
        }
    });

    connect(m_stageConnectButton, &QPushButton::clicked, this, [this]() {
        const QString endpoint = m_stageEndpointEdit ? m_stageEndpointEdit->text().trimmed() : QString();
        if (endpoint.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("三轴台 gRPC 服务地址不能为空"));
            return;
        }
        saveConfigFromUI();
        if (!m_appController) {
            return;
        }
        if (!m_appController->connectStageBackend(endpoint)) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("三轴台 gRPC 连接失败"));
            return;
        }
        updateStagePanelUiState();
    });

    connect(m_stageDisconnectButton, &QPushButton::clicked, this, [this]() {
        if (m_appController) {
            m_appController->disconnectStageBackend();
            updateStagePanelUiState();
        }
    });

    connect(m_stageGetPositionButton, &QPushButton::clicked, this, [this]() {
        sendStageCommandText(QStringLiteral("get_positions"));
    });

    connect(m_stageStartStreamButton, &QPushButton::clicked, this, [this]() {
        const int intervalMs = m_stageStreamIntervalSpin ? m_stageStreamIntervalSpin->value() : 100;
        sendStageCommandText(QStringLiteral("start_stream %1").arg(intervalMs));
    });

    connect(m_stageStopStreamButton, &QPushButton::clicked, this, [this]() {
        sendStageCommandText(QStringLiteral("stop_stream"));
    });

    connect(m_stageJogStartButton, &QPushButton::clicked, this, [this]() {
        const QString axis = m_stageJogAxisCombo ? m_stageJogAxisCombo->currentData().toString() : QStringLiteral("x");
        const QString dir = m_stageJogDirectionCombo ? m_stageJogDirectionCombo->currentData().toString() : QStringLiteral("+");
        sendStageCommandText(QStringLiteral("jog %1 %2 on").arg(axis, dir));
    });

    connect(m_stageJogStopButton, &QPushButton::clicked, this, [this]() {
        const QString axis = m_stageJogAxisCombo ? m_stageJogAxisCombo->currentData().toString() : QStringLiteral("x");
        const QString dir = m_stageJogDirectionCombo ? m_stageJogDirectionCombo->currentData().toString() : QStringLiteral("+");
        sendStageCommandText(QStringLiteral("jog %1 %2 off").arg(axis, dir));
    });

    connect(m_stageMoveAbsButton, &QPushButton::clicked, this, [this]() {
        const QString command = QStringLiteral("move_abs %1 %2 %3 %4")
                                    .arg(m_stageMoveAbsXSpin ? m_stageMoveAbsXSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageMoveAbsYSpin ? m_stageMoveAbsYSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageMoveAbsZSpin ? m_stageMoveAbsZSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageMoveTimeoutSpin ? m_stageMoveTimeoutSpin->value() : 10000);
        sendStageCommandText(command);
    });

    connect(m_stageMoveRelButton, &QPushButton::clicked, this, [this]() {
        const QString axis = m_stageMoveRelAxisCombo ? m_stageMoveRelAxisCombo->currentData().toString() : QStringLiteral("x");
        const QString command = QStringLiteral("move_rel %1 %2 %3")
                                    .arg(axis)
                                    .arg(m_stageMoveRelDeltaSpin ? m_stageMoveRelDeltaSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageMoveTimeoutSpin ? m_stageMoveTimeoutSpin->value() : 10000);
        sendStageCommandText(command);
    });

    connect(m_stageSetSpeedButton, &QPushButton::clicked, this, [this]() {
        const int speed = m_stageSpeedSpin ? m_stageSpeedSpin->value() : 20000;
        const int accel = m_stageAccelSpin ? m_stageAccelSpin->value() : 100;
        sendStageCommandText(QStringLiteral("set_speed %1 %2").arg(speed).arg(accel));
    });

    connect(m_stageStartScanButton, &QPushButton::clicked, this, [this]() {
        const QString mode = m_stageScanModeCombo ? m_stageScanModeCombo->currentData().toString() : QStringLiteral("snake");
        const QString command = QStringLiteral("start_scan %1 %2 %3 %4 %5 %6 %7")
                                    .arg(mode)
                                    .arg(m_stageScanXsSpin ? m_stageScanXsSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageScanXeSpin ? m_stageScanXeSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageScanYsSpin ? m_stageScanYsSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageScanYeSpin ? m_stageScanYeSpin->value() : 0.0, 0, 'f', 3)
                                    .arg(m_stageScanStepSpin ? m_stageScanStepSpin->value() : 1.0, 0, 'f', 3)
                                    .arg(m_stageScanZFixSpin ? m_stageScanZFixSpin->value() : 0.0, 0, 'f', 3);
        sendStageCommandText(command);
    });

    connect(m_stageStopScanButton, &QPushButton::clicked, this, [this]() {
        sendStageCommandText(QStringLiteral("stop_scan"));
    });

    connect(m_stageScanStatusButton, &QPushButton::clicked, this, [this]() {
        sendStageCommandText(QStringLiteral("scan_status"));
    });

    if (m_stageCustomCommandEdit) {
        connect(m_stageCustomCommandEdit, &QLineEdit::returnPressed, this, [this]() {
            if (!m_stageCustomCommandEdit) {
                return;
            }
            sendStageCommandText(m_stageCustomCommandEdit->text());
        });
    }
    if (m_stageCustomCommandSendButton) {
        connect(m_stageCustomCommandSendButton, &QPushButton::clicked, this, [this]() {
            if (!m_stageCustomCommandEdit) {
                return;
            }
            sendStageCommandText(m_stageCustomCommandEdit->text());
        });
    }
    if (m_stageCommandHelpButton) {
        connect(m_stageCommandHelpButton, &QPushButton::clicked, this, [this]() {
            sendStageCommandText(QStringLiteral("help"));
        });
    }
    
    // 指令发送信号槽
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &MainWindow::onClearClicked);
    connect(m_commandHistoryList, &QListWidget::doubleClicked, this, &MainWindow::onCommandHistoryDoubleClicked);
    connect(m_hexFormatCheck, &QCheckBox::toggled, this, &MainWindow::onSendFormatChanged);
    
    // 窗口管理信号槽
    connect(m_createWindowButton, &QPushButton::clicked, this, &MainWindow::onCreateWindowClicked);
    connect(m_closeWindowButton, &QPushButton::clicked, this, &MainWindow::onCloseWindowClicked);
    connect(m_tileWindowsButton, &QPushButton::clicked, this, &MainWindow::onTileWindowsClicked);
    connect(m_cascadeWindowsButton, &QPushButton::clicked, this, &MainWindow::onCascadeWindowsClicked);
    connect(m_windowList, &QListWidget::doubleClicked, this, &MainWindow::onWindowListDoubleClicked);

    if (m_plotWindowManager) {
        connect(m_plotWindowManager,
                &PlotWindowManager::telemetryUpdated,
                this,
                &MainWindow::onPlotManagerTelemetryUpdated);
    }

    connect(m_startGrpcTestServerButton, &QPushButton::clicked, this, &MainWindow::onStartGrpcTestServerClicked);
    connect(m_stopGrpcTestServerButton, &QPushButton::clicked, this, &MainWindow::onStopGrpcTestServerClicked);
    connect(m_runGrpcSelfTestButton, &QPushButton::clicked, this, &MainWindow::onRunGrpcSelfTestClicked);
    connect(m_grpcSelfTestTimeoutTimer, &QTimer::timeout, this, &MainWindow::onGrpcSelfTestTimeout);
    connect(m_grpcModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_grpcSelfTestPending) {
            setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("待验证"), GrpcLabelTone::Neutral);
            updateGrpcTestUiState();
        }
    });

#ifndef QT_COMPILE_FOR_WASM
    if (m_grpcTestServerProcess) {
        connect(m_grpcTestServerProcess, &QProcess::started, this, [this]() {
            if (m_grpcTestServerStartTimeoutTimer) {
                m_grpcTestServerStartTimeoutTimer->stop();
            }

            const bool wasLaunchInProgress = m_grpcTestServerLaunchInProgress;
            const QString launchDisplayName = m_grpcTestServerCurrentDisplayName;

            m_grpcTestServerLaunchInProgress = false;
            m_grpcTestServerStopRequested = false;
            m_grpcTestServerLaunchQueue.clear();
            m_grpcTestServerStartErrors.clear();
            m_grpcTestServerCurrentDisplayName.clear();

            setGrpcTestServiceStatus("运行中", "green");
            if (wasLaunchInProgress && !launchDisplayName.isEmpty()) {
                logGrpcInteraction("test-server", QString("本地 gRPC 测试服务已启动（%1）").arg(launchDisplayName));
            } else {
                logGrpcInteraction("test-server", "本地 gRPC 测试服务已启动");
            }
            updateGrpcTestUiState();
        });

        connect(m_grpcTestServerProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
                    if (m_grpcTestServerStartTimeoutTimer) {
                        m_grpcTestServerStartTimeoutTimer->stop();
                    }

                    const bool wasLaunchInProgress = m_grpcTestServerLaunchInProgress;
                    const QString launchDisplayName = m_grpcTestServerCurrentDisplayName;

                    if (wasLaunchInProgress && !m_grpcTestServerStopRequested) {
                        const QString displayName = launchDisplayName.isEmpty()
                            ? QStringLiteral("候选启动项")
                            : launchDisplayName;
                        const QString reason = QString("%1: 启动后立即退出，exitCode=%2")
                            .arg(displayName)
                            .arg(exitCode);
                        m_grpcTestServerStartErrors << reason;
                        logGrpcInteraction("test-server", reason);
                        m_grpcTestServerCurrentDisplayName.clear();
                        tryStartNextGrpcTestServerCandidate();
                        return;
                    }

                    m_grpcTestServerLaunchInProgress = false;
                    m_grpcTestServerLaunchQueue.clear();
                    m_grpcTestServerCurrentDisplayName.clear();

                    if (exitStatus == QProcess::NormalExit) {
                        setGrpcTestServiceStatus(QString("已停止(%1)").arg(exitCode), "gray");
                        logGrpcInteraction("test-server", QString("测试服务已停止，exitCode=%1").arg(exitCode));
                    } else {
                        setGrpcTestServiceStatus("异常退出", "red");
                        logGrpcInteraction("test-server", "测试服务异常退出");
                    }

                    m_grpcTestServerStopRequested = false;
                    updateGrpcTestUiState();
                });

        connect(m_grpcTestServerProcess,
                &QProcess::errorOccurred,
                this,
                [this](QProcess::ProcessError error) {
                    if (m_grpcTestServerStartTimeoutTimer) {
                        m_grpcTestServerStartTimeoutTimer->stop();
                    }

                    if (m_grpcTestServerLaunchInProgress) {
                        const QString displayName = m_grpcTestServerCurrentDisplayName.isEmpty()
                            ? QStringLiteral("候选启动项")
                            : m_grpcTestServerCurrentDisplayName;
                        const QString errorDetail = m_grpcTestServerProcess
                            ? m_grpcTestServerProcess->errorString()
                            : QStringLiteral("未知错误");
                        const QString reason = QString("%1: %2").arg(displayName, errorDetail);
                        m_grpcTestServerStartErrors << reason;
                        logGrpcInteraction(
                            "test-server",
                            QString("启动失败（error=%1）：%2")
                                .arg(static_cast<int>(error))
                                .arg(reason));
                        m_grpcTestServerCurrentDisplayName.clear();
                        tryStartNextGrpcTestServerCandidate();
                        return;
                    }

                    if (m_grpcTestServerStopRequested) {
                        logGrpcInteraction(
                            "test-server",
                            QString("停止流程中收到进程错误，error=%1, detail=%2")
                                .arg(static_cast<int>(error))
                                .arg(m_grpcTestServerProcess ? m_grpcTestServerProcess->errorString() : QStringLiteral("未知错误")));
                        return;
                    }

                    setGrpcTestServiceStatus("启动失败", "red");
                    logGrpcInteraction(
                        "test-server",
                        QString("测试服务启动/运行错误，error=%1, detail=%2")
                            .arg(static_cast<int>(error))
                            .arg(m_grpcTestServerProcess ? m_grpcTestServerProcess->errorString() : QStringLiteral("未知错误")));
                    updateGrpcTestUiState();
                });

        auto appendGrpcServerOutput = [this](const QString& output, const QString& streamTag) {
            if (!m_dataMonitor) {
                return;
            }

            const QString trimmed = output.trimmed();
            if (trimmed.isEmpty()) {
                return;
            }

            const QStringList lines = trimmed.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
                m_dataMonitor->append(QString("%1 [gRPC测试服务-%2] %3")
                                          .arg(timestamp, streamTag, line.trimmed()));
                logGrpcInteraction("test-server", QString("[%1] %2").arg(streamTag, line.trimmed()));
            }

            QScrollBar* scrollBar = m_dataMonitor->verticalScrollBar();
            scrollBar->setValue(scrollBar->maximum());
        };

        connect(m_grpcTestServerProcess, &QProcess::readyReadStandardOutput, this, [this, appendGrpcServerOutput]() {
            appendGrpcServerOutput(QString::fromUtf8(m_grpcTestServerProcess->readAllStandardOutput()), "OUT");
        });

        connect(m_grpcTestServerProcess, &QProcess::readyReadStandardError, this, [this, appendGrpcServerOutput]() {
            appendGrpcServerOutput(QString::fromUtf8(m_grpcTestServerProcess->readAllStandardError()), "ERR");
        });
    }
#endif

    updateStagePanelUiState();
    updateGrpcTestUiState();
    
    // 其他信号槽将在后续连接到SerialReceiver
}

void MainWindow::loadConfigToUI()
{
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }
    
    // 加载串口配置
    // 必须先写入 gRPC 地址，再 setCurrentIndex(BackendType)。
    // 否则 setCurrentIndex 会同步触发 onBackendTypeChanged → saveConfigFromUI()，
    // 此时输入框仍为空，会把 AppConfig 里的地址覆盖成空串，界面永远显示空白。
    m_grpcEndpointEdit->setText(config->grpcEndpoint());
    if (m_stageEndpointEdit) {
        m_stageEndpointEdit->setText(config->stageGrpcEndpoint());
    }

    const QString backendType = config->receiverBackendType().trimmed().toLower();
    QString effectiveBackend = backendType.isEmpty() ? QStringLiteral("grpc") : backendType;
    if (effectiveBackend.compare(QStringLiteral("stage"), Qt::CaseInsensitive) == 0) {
        effectiveBackend = QStringLiteral("grpc");
        config->setReceiverBackendType(QStringLiteral("grpc"));
    }
    const int backendIndex = m_backendTypeCombo->findData(effectiveBackend);
    {
        const QSignalBlocker blocker(m_backendTypeCombo);
        m_backendTypeCombo->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
    }

    const QString configuredSerialPort = config->serialPort().trimmed();
    const int serialPortIndex = m_serialPortCombo->findData(configuredSerialPort);
    if (serialPortIndex >= 0) {
        m_serialPortCombo->setCurrentIndex(serialPortIndex);
    } else if (!configuredSerialPort.isEmpty()) {
        m_serialPortCombo->setCurrentText(configuredSerialPort);
    }

    m_baudRateCombo->setCurrentText(QString::number(config->baudRate()));
    
    // 加载模拟数据配置
    m_useMockDataCheck->setChecked(config->useMockData());
    m_mockIntervalSpin->setValue(config->mockDataIntervalMs());
    
    // 加载发送配置
    m_hexFormatCheck->setChecked(config->sendAsHex());
    m_autoNewlineCheck->setChecked(config->autoSendNewline());
    
    // 设置换行符选择
    QString newline = config->newlineSequence();
    if (newline == "\r\n") {
        m_newlineCombo->setCurrentIndex(0);
    } else if (newline == "\r") {
        m_newlineCombo->setCurrentIndex(1);
    } else if (newline == "\n") {
        m_newlineCombo->setCurrentIndex(2);
    } else {
        m_newlineCombo->setCurrentIndex(3);
    }

    onBackendTypeChanged(m_backendTypeCombo->currentIndex());
}

void MainWindow::saveConfigFromUI()
{
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }
    
    // 保存串口配置
    config->setReceiverBackendType(m_backendTypeCombo->currentData().toString());
    config->setGrpcEndpoint(m_grpcEndpointEdit->text().trimmed());
    if (m_stageEndpointEdit) {
        config->setStageGrpcEndpoint(m_stageEndpointEdit->text().trimmed());
    }

    QString serialPortValue = m_serialPortCombo->currentData().toString().trimmed();
    if (serialPortValue.isEmpty()) {
        const QString currentPortText = m_serialPortCombo->currentText().trimmed();
        if (!currentPortText.isEmpty() && !currentPortText.contains("无可用串口")) {
            const int infoSeparator = currentPortText.indexOf(" (");
            serialPortValue = (infoSeparator > 0)
                ? currentPortText.left(infoSeparator).trimmed()
                : currentPortText;
        }
    }
    if (!serialPortValue.isEmpty()) {
        config->setSerialPort(serialPortValue);
    }

    config->setBaudRate(m_baudRateCombo->currentText().toInt());
    
    // 保存模拟数据配置
    config->setUseMockData(m_useMockDataCheck->isChecked());
    config->setMockDataIntervalMs(m_mockIntervalSpin->value());
    
    // 保存发送配置
    config->setSendAsHex(m_hexFormatCheck->isChecked());
    config->setAutoSendNewline(m_autoNewlineCheck->isChecked());
    
    // 保存换行符选择
    int newlineIndex = m_newlineCombo->currentIndex();
    switch (newlineIndex) {
    case 0: config->setNewlineSequence("\r\n"); break;
    case 1: config->setNewlineSequence("\r"); break;
    case 2: config->setNewlineSequence("\n"); break;
    case 3: config->setNewlineSequence(""); break;
    }
    
    // 保存面板显示状态
    config->setShowDevicePanel(m_devicePanel->isVisible());
    config->setShowCommandPanel(m_commandPanel->isVisible());
    config->setShowPlotPanel(m_plotPanel->isVisible());
    if (m_stagePanel) {
        config->setShowStagePanel(m_stagePanel->isVisible());
    }
    config->setShowMonitorPanel(m_monitorPanel->isVisible());
    if (m_overviewPanel) {
        config->setShowOverviewPanel(m_overviewPanel->isVisible());
    }
    
    // 保存窗口状态
    const QSize windowSize = (isMaximized() || isFullScreen()) ? normalGeometry().size() : size();
    if (windowSize.isValid()) {
        config->setWindowSize(windowSize);
    }
    config->setMainWindowState(saveState());
    config->setMainWindowGeometry(saveGeometry());
    config->setSavedPlotWindowTypes(collectCurrentPlotWindowTypes());
}

QStringList MainWindow::collectCurrentPlotWindowTypes() const
{
    QStringList types;
    if (!m_mdiArea) {
        return types;
    }

    const QList<QMdiSubWindow*> subWindows = m_mdiArea->subWindowList();
    for (QMdiSubWindow* sub : subWindows) {
        if (!sub) {
            continue;
        }
        QWidget* widget = sub->widget();
        if (!widget) {
            continue;
        }

        QVariant v = widget->property("plotType");
        if (v.isValid()) {
            types.append(QString::number(v.toInt()));
            continue;
        }

        const QString title = widget->windowTitle();
        if (title.contains(QStringLiteral("阵列热力图"))) {
            types.append(QString::number(static_cast<int>(PlotWindowManager::ArrayHeatmapPlot)));
        } else if (title.contains(QStringLiteral("检测分析"))) {
            types.append(QString::number(static_cast<int>(PlotWindowManager::InspectionPlot)));
        } else if (title.contains(QStringLiteral("脉冲"))) {
            types.append(QString::number(static_cast<int>(PlotWindowManager::PulsedDecayPlot)));
        } else if (title.contains(QStringLiteral("阵列"))) {
            types.append(QString::number(static_cast<int>(PlotWindowManager::ArrayPlot)));
        } else if (title.contains(QStringLiteral("热力图"))) {
            types.append(QString::number(static_cast<int>(PlotWindowManager::HeatmapPlot)));
        } else {
            types.append(QString::number(static_cast<int>(PlotWindowManager::CombinedPlot)));
        }
    }

    return types;
}

void MainWindow::restoreSavedPlotWindowsFromConfig()
{
    if (!m_plotWindowManager || !m_mdiArea) {
        return;
    }

    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }

    if (!m_mdiArea->subWindowList().isEmpty()) {
        return;
    }

    const QStringList savedTypes = config->savedPlotWindowTypes();
    if (savedTypes.isEmpty()) {
        return;
    }

    auto normalizeType = [](int v) -> PlotWindowManager::PlotType {
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
        return PlotWindowManager::CombinedPlot;
    };

    int created = 0;
    for (const QString& item : savedTypes) {
        bool ok = false;
        const int rawType = item.toInt(&ok);
        if (!ok) {
            continue;
        }
        PlotWindowBase* w = m_plotWindowManager->createWindowInMdiArea(m_mdiArea, normalizeType(rawType));
        if (w) {
            ++created;
        }
    }

    if (created > 0) {
        qInfo() << "MainWindow: 已恢复绘图窗口数量" << created;
    }
}

void MainWindow::updateSerialPortList()
{
    m_serialPortCombo->clear();
    
#ifdef QT_COMPILE_FOR_WASM
    // WebAssembly环境下没有串口硬件，提供模拟串口或禁用功能
    m_serialPortCombo->addItem("COM1 (WebAssembly 模拟串口)");
    m_serialPortCombo->addItem("COM2 (WebAssembly 模拟串口)");
    m_serialPortCombo->setEnabled(true);
#else
    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    
    for (const QSerialPortInfo& port : ports) {
        QString portInfo = QString("%1 (%2)")
                              .arg(port.portName())
                              .arg(port.description());
        m_serialPortCombo->addItem(portInfo, port.portName());
    }
    
    if (ports.isEmpty()) {
        m_serialPortCombo->addItem("无可用串口");
        m_serialPortCombo->setEnabled(false);
    } else {
        m_serialPortCombo->setEnabled(true);
    }
#endif
}

void MainWindow::updateWindowList()
{
    if (!m_plotWindowManager) {
        return;
    }
    
    m_windowList->clear();
    QList<PlotWindowBase*> windows = m_plotWindowManager->windows();
    
    for (PlotWindowBase* window : windows) {
        QListWidgetItem* item = new QListWidgetItem(window->windowTitle());
        item->setData(Qt::UserRole, QVariant::fromValue(window));
        m_windowList->addItem(item);
    }
}

void MainWindow::addCommandToHistory(const QString& command)
{
    if (command.trimmed().isEmpty()) {
        return;
    }
    
    // 添加到列表
    m_commandHistoryList->insertItem(0, command.trimmed());
    
    // 限制历史记录数量
    AppConfig* config = AppConfig::instance();
    if (config) {
        int maxHistory = config->maxCommandHistory();
        while (m_commandHistoryList->count() > maxHistory) {
            delete m_commandHistoryList->takeItem(maxHistory);
        }
    }
}

void MainWindow::loadCommandHistory()
{
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }
    
    m_commandHistoryList->clear();
    QStringList history = config->commandHistory();
    
    for (const QString& command : history) {
        m_commandHistoryList->addItem(command);
    }
}

void MainWindow::saveCommandHistory()
{
    AppConfig* config = AppConfig::instance();
    if (!config || !config->saveCommandHistory()) {
        return;
    }
    
    QStringList history;
    for (int i = 0; i < m_commandHistoryList->count(); ++i) {
        QListWidgetItem* item = m_commandHistoryList->item(i);
        history.append(item->text());
    }
    
    config->setCommandHistory(history);
}

void MainWindow::logGrpcInteraction(const QString& category, const QString& detail) const
{
    qInfo().noquote() << QString("[gRPC][%1] %2").arg(category, detail);
}

void MainWindow::setGrpcLabelState(GrpcLabelState& target, const QString& text, GrpcLabelTone tone)
{
    target.text = text;
    target.tone = tone;
}

QString MainWindow::grpcLabelToneColor(GrpcLabelTone tone) const
{
    switch (tone) {
    case GrpcLabelTone::Warning:
        return QStringLiteral("#c08000");
    case GrpcLabelTone::Success:
        return QStringLiteral("green");
    case GrpcLabelTone::Error:
        return QStringLiteral("red");
    case GrpcLabelTone::Neutral:
    default:
        return QStringLiteral("gray");
    }
}

void MainWindow::applyGrpcLabelState(QLabel* label, const GrpcLabelState& state) const
{
    if (!label) {
        return;
    }

    label->setText(state.text);
    label->setStyleSheet(QString("color: %1;").arg(grpcLabelToneColor(state.tone)));
}

void MainWindow::applyGrpcSelfTestLabelStates()
{
    applyGrpcLabelState(m_grpcSelfTestStatusLabel, m_grpcSelfTestOverallState);
    applyGrpcLabelState(m_grpcSelfTestTxStatusLabel, m_grpcSelfTestTxState);
    applyGrpcLabelState(m_grpcSelfTestRxStatusLabel, m_grpcSelfTestRxState);
    applyGrpcLabelState(m_grpcModeSwitchStatusLabel, m_grpcModeSwitchState);
    applyGrpcLabelState(m_grpcPeriodicDataStatusLabel, m_grpcPeriodicDataState);
}

void MainWindow::resetGrpcSelfTestLabelStates()
{
    setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("未执行"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcSelfTestTxState, QStringLiteral("发送(Ping): 待验证"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcSelfTestRxState, QStringLiteral("接收(流数据): 待验证"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("待验证"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("待验证"), GrpcLabelTone::Neutral);
}

void MainWindow::updateGrpcTestUiState()
{
    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
    const bool isGrpcRealMode = (isGrpcBackend && m_useMockDataCheck && !m_useMockDataCheck->isChecked());

#ifndef QT_COMPILE_FOR_WASM
    const bool serverRunning = m_grpcTestServerProcess && m_grpcTestServerProcess->state() != QProcess::NotRunning;
    m_startGrpcTestServerButton->setEnabled(isGrpcBackend && !serverRunning);
    m_stopGrpcTestServerButton->setEnabled(isGrpcBackend && serverRunning);
#else
    m_startGrpcTestServerButton->setEnabled(false);
    m_stopGrpcTestServerButton->setEnabled(false);
#endif

    m_runGrpcSelfTestButton->setEnabled(isGrpcRealMode && m_isConnected && !m_grpcSelfTestPending);
    m_grpcModeCombo->setEnabled(isGrpcRealMode && m_isConnected && !m_grpcSelfTestPending);

    if (!m_grpcSelfTestPending) {
        if (!isGrpcBackend) {
            setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("仅 gRPC 后端可用"), GrpcLabelTone::Neutral);
            setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("仅 gRPC 后端可用"), GrpcLabelTone::Neutral);
            setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("仅 gRPC 后端可用"), GrpcLabelTone::Neutral);
        } else if (!isGrpcRealMode) {
            setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("Mock 模式不验证真实链路"), GrpcLabelTone::Neutral);
            setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("Mock 模式不验证切换"), GrpcLabelTone::Neutral);
            setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("Mock 模式不验证周期"), GrpcLabelTone::Neutral);
        } else if (!m_isConnected) {
            setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("等待连接"), GrpcLabelTone::Neutral);
            setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("等待连接"), GrpcLabelTone::Neutral);
            setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("等待周期数据"), GrpcLabelTone::Neutral);
        } else if (m_grpcPeriodicPacketCount <= 0) {
            setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("等待周期数据"), GrpcLabelTone::Warning);
        }
    }

    applyGrpcSelfTestLabelStates();
}

void MainWindow::startGrpcSelfTest(bool autoTriggered)
{
    if (!m_appController) {
        return;
    }

    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
    if (!isGrpcBackend) {
        if (!autoTriggered) {
            QMessageBox::information(this, "提示", "当前后端不是 gRPC，无法执行 gRPC 自检");
        }
        updateGrpcTestUiState();
        return;
    }

    if (m_useMockDataCheck && m_useMockDataCheck->isChecked()) {
        if (!autoTriggered) {
            QMessageBox::information(this, "提示", "请先关闭“gRPC 本地模拟”，再执行真实链路自检");
        }
        updateGrpcTestUiState();
        return;
    }

    if (!m_isConnected) {
        if (!autoTriggered) {
            QMessageBox::information(this, "提示", "当前未连接 gRPC 服务端");
        }
        updateGrpcTestUiState();
        return;
    }

    m_grpcSelfTestPending = true;
    m_grpcSelfTestCommandAcked = false;
    m_grpcSelfTestModeSwitchAcked = false;
    m_grpcSelfTestStreamReceived = false;
    m_grpcSelfTestPendingAcks.clear();
    m_grpcSelfTestTargetMode = m_grpcModeCombo ? m_grpcModeCombo->currentData().toString() : QStringLiteral("complex");
    if (m_grpcSelfTestTargetMode.isEmpty()) {
        m_grpcSelfTestTargetMode = QStringLiteral("complex");
    }
    m_grpcSelfTestPendingAcks << "ping" << "mode";

    setGrpcLabelState(m_grpcSelfTestTxState, QStringLiteral("发送(Ping): 验证中..."), GrpcLabelTone::Warning);
    setGrpcLabelState(m_grpcSelfTestRxState, QStringLiteral("接收(流数据): 验证中..."), GrpcLabelTone::Warning);
    setGrpcLabelState(
        m_grpcModeSwitchState,
        QString("切换(%1): 验证中...").arg(m_grpcSelfTestTargetMode),
        GrpcLabelTone::Warning);
    setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("周期数据: 验证中..."), GrpcLabelTone::Warning);
    setGrpcLabelState(
        m_grpcSelfTestOverallState,
        autoTriggered ? QStringLiteral("自动自检中...") : QStringLiteral("手动自检中..."),
        GrpcLabelTone::Warning);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastGrpcStreamTimestampMs <= 2000) {
        m_grpcSelfTestStreamReceived = true;
        setGrpcLabelState(m_grpcSelfTestRxState, QStringLiteral("接收(流数据): 已收到"), GrpcLabelTone::Success);
        setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("周期数据: 已收到最近周期包"), GrpcLabelTone::Success);
    }

    logGrpcInteraction(
        "self-test",
        QString("%1触发自检，目标模式=%2，命令协议=selftest_ping/selftest_set_mode")
            .arg(autoTriggered ? "自动" : "手动", m_grpcSelfTestTargetMode));

    m_appController->sendCommand("selftest_ping", false);
    m_appController->sendCommand(QString("selftest_set_mode %1").arg(m_grpcSelfTestTargetMode), false);
    m_grpcSelfTestTimeoutTimer->start();
    updateGrpcTestUiState();
}

void MainWindow::handleGrpcBackendPacket(const QJsonObject& packet)
{
    const QString packetType = packet.value("type").toString();

    if (packetType == "streamFrame") {
        const qint64 localTs = QDateTime::currentMSecsSinceEpoch();
        const qint64 payloadTimestamp = packet.value("timestamp").toVariant().toLongLong();
        const int frameId = packet.value("frameId").toInt(-1);
        const int channelCount = packet.value("channelCount").toInt(-1);
        const QString mode = packet.value("mode").toString();

        m_lastGrpcStreamTimestampMs = localTs;
        setGrpcLabelState(m_grpcSelfTestRxState, QStringLiteral("接收(流数据): 已收到"), GrpcLabelTone::Success);

        ++m_grpcPeriodicPacketCount;
        if (payloadTimestamp > 0) {
            if (m_lastGrpcStreamPayloadTimestampMs > 0 && payloadTimestamp > m_lastGrpcStreamPayloadTimestampMs) {
                m_grpcPeriodicIntervalSumMs += (payloadTimestamp - m_lastGrpcStreamPayloadTimestampMs);
            }
            m_lastGrpcStreamPayloadTimestampMs = payloadTimestamp;
        }

        if (m_grpcPeriodicPacketCount > 1) {
            const double avgInterval = static_cast<double>(m_grpcPeriodicIntervalSumMs)
                / static_cast<double>(m_grpcPeriodicPacketCount - 1);
            setGrpcLabelState(
                m_grpcPeriodicDataState,
                QString("已接收%1条, 平均%2ms")
                    .arg(m_grpcPeriodicPacketCount)
                    .arg(avgInterval, 0, 'f', 1),
                GrpcLabelTone::Success);
        } else {
            setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("已接收首条周期数据"), GrpcLabelTone::Warning);
        }

        // logGrpcInteraction(
        //     "stream",
        //     QString("frameId=%1 timestamp=%2 channelCount=%3 mode=%4")
        //         .arg(frameId)
        //         .arg(payloadTimestamp)
        //         .arg(channelCount)
        //         .arg(mode));

        if (m_grpcSelfTestPending) {
            m_grpcSelfTestStreamReceived = true;
            finalizeGrpcSelfTest();
        }
        updateGrpcTestUiState();
        return;
    }

    if (packetType == "commandAck") {
        const bool isSuccess = !packet.contains("success") || packet.value("success").toBool();
        const QString message = packet.value("message").toString();
        const QString commandId = packet.value("command_id").toString();
        const QString mode = packet.value("mode").toString();
        const QString normalizedMsg = message.trimmed();
        const QString normalizedUpperMsg = normalizedMsg.toUpper();

        const bool isSelfTestPingAck = normalizedUpperMsg.startsWith("SELFTEST_ACK:PING")
            || normalizedMsg.compare("pong", Qt::CaseInsensitive) == 0;
        const bool isSelfTestModeAck = normalizedUpperMsg.startsWith("SELFTEST_ACK:MODE:")
            || normalizedMsg.contains("模式已切换", Qt::CaseInsensitive);

        logGrpcInteraction(
            "commandAck",
            QString("success=%1 mode=%2 commandId=%3 message=%4")
                .arg(isSuccess ? "true" : "false", mode, commandId, message));

        QString ackStage;
        if (m_grpcSelfTestPending && !m_grpcSelfTestPendingAcks.isEmpty()) {
            if (isSelfTestPingAck) {
                ackStage = "ping";
                m_grpcSelfTestPendingAcks.removeAll("ping");
            } else if (isSelfTestModeAck) {
                ackStage = "mode";
                m_grpcSelfTestPendingAcks.removeAll("mode");
            } else {
                ackStage = m_grpcSelfTestPendingAcks.takeFirst();
            }
        }

        if (isSuccess) {
            if (ackStage == "ping") {
                setGrpcLabelState(
                    m_grpcSelfTestTxState,
                    message.isEmpty() ? QStringLiteral("发送(Ping): 命令回包成功")
                                      : QString("发送(Ping): %1").arg(message),
                    GrpcLabelTone::Success);
                m_grpcSelfTestCommandAcked = true;
            } else if (ackStage == "mode") {
                setGrpcLabelState(
                    m_grpcModeSwitchState,
                    message.isEmpty() ? QString("切换(%1): 回包成功").arg(m_grpcSelfTestTargetMode)
                                      : QString("切换(%1): %2").arg(m_grpcSelfTestTargetMode, message),
                    GrpcLabelTone::Success);
                m_grpcSelfTestModeSwitchAcked = true;
            } else {
                setGrpcLabelState(
                    m_grpcSelfTestTxState,
                    message.isEmpty() ? QStringLiteral("发送: 命令回包成功")
                                      : QString("发送: %1").arg(message),
                    GrpcLabelTone::Success);

                if (message.contains("模式已切换", Qt::CaseInsensitive)) {
                    setGrpcLabelState(
                        m_grpcModeSwitchState,
                        QString("切换: %1").arg(message),
                        GrpcLabelTone::Success);
                }
            }

            if (m_grpcSelfTestPending) {
                finalizeGrpcSelfTest();
            }
        } else {
            if (ackStage == "mode") {
                setGrpcLabelState(
                    m_grpcModeSwitchState,
                    QString("切换(%1): 回包失败").arg(m_grpcSelfTestTargetMode),
                    GrpcLabelTone::Error);
            } else {
                setGrpcLabelState(m_grpcSelfTestTxState, QStringLiteral("发送(Ping): 回包失败"), GrpcLabelTone::Error);
            }

            if (m_grpcSelfTestPending) {
                m_grpcSelfTestPending = false;
                m_grpcSelfTestTimeoutTimer->stop();
                setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("收发验证失败"), GrpcLabelTone::Error);
            }
        }
        updateGrpcTestUiState();
        return;
    }

    if (packetType == "grpcStatus") {
        const QString status = packet.value("status").toString();
        const QString mode = packet.value("mode").toString();
        const QString detail = packet.value("detail").toString();
        const QString endpoint = packet.value("endpoint").toString();

        logGrpcInteraction(
            "status",
            QString("status=%1 mode=%2 endpoint=%3 detail=%4")
                .arg(status, mode, endpoint, detail));

        if (status == "connected" && mode == "real" && !m_grpcSelfTestPending) {
            setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("已连接，等待收发验证"), GrpcLabelTone::Warning);
            setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("等待模式切换验证"), GrpcLabelTone::Warning);
        }

        if (status == "disconnected" && m_grpcSelfTestPending) {
            m_grpcSelfTestPending = false;
            m_grpcSelfTestTimeoutTimer->stop();
            setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("收发验证失败（连接中断）"), GrpcLabelTone::Error);
        }
        updateGrpcTestUiState();
    }
}

void MainWindow::updateStagePanelUiState()
{
    if (!m_stagePanel) {
        return;
    }

    const bool stageConnected = m_appController && m_appController->isStageConnected();

    if (m_stageEndpointEdit) {
        m_stageEndpointEdit->setEnabled(true);
    }
    if (m_stageApplyEndpointButton) {
        m_stageApplyEndpointButton->setEnabled(true);
    }
    if (m_stageConnectButton) {
        m_stageConnectButton->setEnabled(!stageConnected);
    }
    if (m_stageDisconnectButton) {
        m_stageDisconnectButton->setEnabled(stageConnected);
    }

    for (QWidget* widget : {
             static_cast<QWidget*>(m_stageGetPositionButton),
             static_cast<QWidget*>(m_stageStartStreamButton),
             static_cast<QWidget*>(m_stageStopStreamButton),
             static_cast<QWidget*>(m_stageJogAxisCombo),
             static_cast<QWidget*>(m_stageJogDirectionCombo),
             static_cast<QWidget*>(m_stageJogStartButton),
             static_cast<QWidget*>(m_stageJogStopButton),
             static_cast<QWidget*>(m_stageMoveAbsXSpin),
             static_cast<QWidget*>(m_stageMoveAbsYSpin),
             static_cast<QWidget*>(m_stageMoveAbsZSpin),
             static_cast<QWidget*>(m_stageMoveTimeoutSpin),
             static_cast<QWidget*>(m_stageMoveAbsButton),
             static_cast<QWidget*>(m_stageMoveRelAxisCombo),
             static_cast<QWidget*>(m_stageMoveRelDeltaSpin),
             static_cast<QWidget*>(m_stageMoveRelButton),
             static_cast<QWidget*>(m_stageSpeedSpin),
             static_cast<QWidget*>(m_stageAccelSpin),
             static_cast<QWidget*>(m_stageSetSpeedButton),
             static_cast<QWidget*>(m_stageScanModeCombo),
             static_cast<QWidget*>(m_stageScanXsSpin),
             static_cast<QWidget*>(m_stageScanXeSpin),
             static_cast<QWidget*>(m_stageScanYsSpin),
             static_cast<QWidget*>(m_stageScanYeSpin),
             static_cast<QWidget*>(m_stageScanStepSpin),
             static_cast<QWidget*>(m_stageScanZFixSpin),
             static_cast<QWidget*>(m_stageStartScanButton),
             static_cast<QWidget*>(m_stageStopScanButton),
             static_cast<QWidget*>(m_stageScanStatusButton),
             static_cast<QWidget*>(m_stageCustomCommandEdit),
             static_cast<QWidget*>(m_stageCustomCommandSendButton),
             static_cast<QWidget*>(m_stageCommandHelpButton),
         }) {
        if (widget) {
            widget->setEnabled(stageConnected);
        }
    }

    if (m_stageBackendStatusLabel) {
        if (!stageConnected) {
            m_stageBackendStatusLabel->setText(QStringLiteral("状态: 三轴台测试装置未连接"));
            m_stageBackendStatusLabel->setStyleSheet("color: gray;");
        } else {
            const QString t = m_stageBackendStatusLabel->text();
            if (t.contains(QStringLiteral("未连接")) && !t.contains(QLatin1Char('@'))) {
                m_stageBackendStatusLabel->setText(QStringLiteral("状态: 已连接"));
                m_stageBackendStatusLabel->setStyleSheet("color: green;");
            }
        }
    }
}

void MainWindow::sendStageCommandText(const QString& command)
{
    const QString normalizedCommand = command.trimmed();
    if (normalizedCommand.isEmpty()) {
        return;
    }

    if (!m_appController) {
        QMessageBox::warning(
            this,
            QStringLiteral("错误"),
            QStringLiteral("应用控制器未初始化，无法发送三轴台指令"));
        return;
    }

    if (!m_appController->isStageConnected()) {
        QMessageBox::information(
            this,
            QStringLiteral("提示"),
            QStringLiteral("请先在右侧点击「连接」三轴台 gRPC。"));
        return;
    }

    addCommandToHistory(normalizedCommand);
    m_appController->sendStageCommand(normalizedCommand, false);

    if (m_stageCommandResultLabel) {
        m_stageCommandResultLabel->setText(QString("最近结果: 指令已发送 [%1]").arg(normalizedCommand));
        m_stageCommandResultLabel->setStyleSheet("color: #1d4ed8;");
    }
}

void MainWindow::handleStageBackendPacket(const QJsonObject& packet)
{
    const QString packetType = packet.value("type").toString();

    if (packetType == "stageStatus") {
        const QString status = packet.value("status").toString();
        const QString endpoint = packet.value("endpoint").toString();
        const QString detail = packet.value("detail").toString();
        QString displayText = QString("状态: %1").arg(status.isEmpty() ? QStringLiteral("unknown") : status);
        if (!endpoint.isEmpty()) {
            displayText += QString(" @ %1").arg(endpoint);
        }
        if (!detail.isEmpty()) {
            displayText += QString(" | %1").arg(detail);
        }

        QString color = "gray";
        if (status == "connected" || status == "reconnected") {
            color = "green";
        } else if (status == "reconnecting") {
            color = "#d97706";
        } else if (status == "streamClosed" ||
                   status.contains("fail", Qt::CaseInsensitive) ||
                   status.contains("reject", Qt::CaseInsensitive)) {
            color = "red";
        }

        if (m_stageBackendStatusLabel) {
            m_stageBackendStatusLabel->setText(displayText);
            m_stageBackendStatusLabel->setStyleSheet(QString("color: %1;").arg(color));
        }

        updateStagePanelUiState();
        return;
    }

    if (packetType == "stagePositions") {
        const double xMm = packet.value("xMm").toDouble(0.0);
        const double yMm = packet.value("yMm").toDouble(0.0);
        const double zMm = packet.value("zMm").toDouble(0.0);
        const int xPulse = packet.value("xPulse").toInt(0);
        const int yPulse = packet.value("yPulse").toInt(0);
        const int zPulse = packet.value("zPulse").toInt(0);
        const QString source = packet.value("source").toString();
        const qint64 unixMs = packet.value("unixMs").toVariant().toLongLong();

        if (m_stagePositionLabel) {
            QString suffix;
            if (!source.isEmpty() || unixMs > 0) {
                suffix = QStringLiteral(" | src=%1 | t=%2")
                             .arg(source.isEmpty() ? QStringLiteral("-") : source)
                             .arg(unixMs > 0 ? QString::number(unixMs) : QStringLiteral("-"));
            }
            m_stagePositionLabel->setText(
                QString("X: %1 mm (%2 pulse) | Y: %3 mm (%4 pulse) | Z: %5 mm (%6 pulse)%7")
                    .arg(xMm, 0, 'f', 3)
                    .arg(xPulse)
                    .arg(yMm, 0, 'f', 3)
                    .arg(yPulse)
                    .arg(zMm, 0, 'f', 3)
                    .arg(zPulse)
                    .arg(suffix));
        }
        return;
    }

    if (packetType == "stageCommandResult") {
        const bool ok = packet.value("ok").toBool(false);
        const QString command = packet.value("command").toString();
        const QString message = packet.value("message").toString();

        if (m_stageCommandResultLabel) {
            m_stageCommandResultLabel->setText(
                QString("最近结果: [%1] %2")
                    .arg(command, message.isEmpty() ? QStringLiteral("(无消息)") : message));
            m_stageCommandResultLabel->setStyleSheet(ok ? "color: green;" : "color: red;");
        }
        return;
    }

    if (packetType == "stageScanStatus") {
        const bool running = packet.value("running").toBool(false);
        const QString statusText = packet.value("status").toString();
        if (m_stageScanStatusLabel) {
            m_stageScanStatusLabel->setText(
                QString("扫描状态: %1 | %2")
                    .arg(running ? QStringLiteral("运行中") : QStringLiteral("已停止"),
                         statusText.isEmpty() ? QStringLiteral("-") : statusText));
            m_stageScanStatusLabel->setStyleSheet(running ? "color: #d97706;" : "color: gray;");
        }
    }
}

void MainWindow::finalizeGrpcSelfTest()
{
    if (!m_grpcSelfTestPending) {
        return;
    }

    if (!m_grpcSelfTestCommandAcked || !m_grpcSelfTestModeSwitchAcked || !m_grpcSelfTestStreamReceived) {
        return;
    }

    m_grpcSelfTestPending = false;
    m_grpcSelfTestTimeoutTimer->stop();
    m_grpcSelfTestPendingAcks.clear();
    setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("收发验证通过"), GrpcLabelTone::Success);
    logGrpcInteraction("self-test", "自检通过：ping/pong、模式切换、周期数据验证全部成功");
    updateGrpcTestUiState();
}

QString MainWindow::resolveGrpcTestServerExecutablePath() const
{
    const QDir currentDir = QDir::current();
    const QDir appDir(QCoreApplication::applicationDirPath());

    QStringList candidatePaths;
#ifdef Q_OS_WIN
    const QString executableName = "grpc_test_server.exe";
#else
    const QString executableName = "grpc_test_server";
#endif

    candidatePaths << currentDir.filePath(executableName)
                   << currentDir.filePath("build/release/" + executableName)
                   << currentDir.filePath("build/debug/" + executableName)
                   << currentDir.filePath("dist/" + executableName)
                   << appDir.filePath(executableName)
                   << appDir.filePath("../" + executableName)
                   << appDir.filePath("../../" + executableName)
                   << appDir.filePath("../release/" + executableName)
                   << appDir.filePath("../debug/" + executableName)
                   << appDir.filePath("../../build/release/" + executableName)
                   << appDir.filePath("../../build/debug/" + executableName)
                   << appDir.filePath("../../dist/" + executableName);

    for (const QString& path : candidatePaths) {
        QFileInfo fileInfo(QDir::cleanPath(path));
        if (fileInfo.exists() && fileInfo.isFile()) {
            return fileInfo.absoluteFilePath();
        }
    }

    return QString();
}

QString MainWindow::resolveGrpcTestServerScriptPath() const
{
    QStringList candidatePaths;
    candidatePaths << QDir::current().filePath("grpc_test_server.py");

    const QDir appDir(QCoreApplication::applicationDirPath());
    candidatePaths << appDir.filePath("grpc_test_server.py")
                   << appDir.filePath("../grpc_test_server.py")
                   << appDir.filePath("../../grpc_test_server.py");

    for (const QString& path : candidatePaths) {
        QFileInfo fileInfo(QDir::cleanPath(path));
        if (fileInfo.exists() && fileInfo.isFile()) {
            return fileInfo.absoluteFilePath();
        }
    }

    return QString();
}

QString MainWindow::resolveGrpcTestServerPythonExecutablePath(const QString& scriptPath) const
{
    const QFileInfo scriptInfo(scriptPath);
    const QDir scriptDir(scriptInfo.absolutePath());
    const QDir currentDir = QDir::current();
    const QDir appDir(QCoreApplication::applicationDirPath());

    QStringList candidatePaths;
#ifdef Q_OS_WIN
    const QStringList venvNames{ ".venv", "venv" };
    for (const QString& venvName : venvNames) {
        candidatePaths << scriptDir.filePath(venvName + "/Scripts/python.exe")
                       << currentDir.filePath(venvName + "/Scripts/python.exe")
                       << appDir.filePath(venvName + "/Scripts/python.exe")
                       << appDir.filePath("../" + venvName + "/Scripts/python.exe")
                       << appDir.filePath("../../" + venvName + "/Scripts/python.exe");
    }
#else
    const QStringList venvNames{ ".venv", "venv" };
    for (const QString& venvName : venvNames) {
        candidatePaths << scriptDir.filePath(venvName + "/bin/python3")
                       << scriptDir.filePath(venvName + "/bin/python")
                       << currentDir.filePath(venvName + "/bin/python3")
                       << currentDir.filePath(venvName + "/bin/python")
                       << appDir.filePath(venvName + "/bin/python3")
                       << appDir.filePath(venvName + "/bin/python")
                       << appDir.filePath("../" + venvName + "/bin/python3")
                       << appDir.filePath("../" + venvName + "/bin/python")
                       << appDir.filePath("../../" + venvName + "/bin/python3")
                       << appDir.filePath("../../" + venvName + "/bin/python");
    }
#endif

    for (const QString& path : candidatePaths) {
        QFileInfo fileInfo(QDir::cleanPath(path));
        if (fileInfo.exists() && fileInfo.isFile()) {
            return fileInfo.absoluteFilePath();
        }
    }

    return QString();
}

void MainWindow::setGrpcTestServiceStatus(const QString& text, const QString& color)
{
    if (!m_grpcTestServiceStatusLabel) {
        return;
    }

    m_grpcTestServiceStatusLabel->setText(text);
    m_grpcTestServiceStatusLabel->setStyleSheet(QString("color: %1;").arg(color));
}

#ifndef QT_COMPILE_FOR_WASM
QList<MainWindow::GrpcTestServerLaunchCandidate>
MainWindow::buildGrpcTestServerLaunchCandidates(const QString& port) const
{
    QList<GrpcTestServerLaunchCandidate> candidates;

    const QString executablePath = resolveGrpcTestServerExecutablePath();
    if (!executablePath.isEmpty()) {
        candidates.append({
            executablePath,
            QStringList() << "--port" << port,
            QStringLiteral("grpc_test_server.exe"),
            QFileInfo(executablePath).absolutePath()
        });
    }

    const QString scriptPath = resolveGrpcTestServerScriptPath();
    if (!scriptPath.isEmpty()) {
        const QString scriptWorkingDir = QFileInfo(scriptPath).absolutePath();
        const QStringList scriptArgs = QStringList() << scriptPath << "--port" << port;

        const QString preferredPythonPath = resolveGrpcTestServerPythonExecutablePath(scriptPath);
        if (!preferredPythonPath.isEmpty()) {
            candidates.append({
                preferredPythonPath,
                scriptArgs,
                QStringLiteral("项目虚拟环境 Python"),
                scriptWorkingDir
            });
        }

        candidates.append({
            QStringLiteral("python"),
            scriptArgs,
            QStringLiteral("系统 Python"),
            scriptWorkingDir
        });

        candidates.append({
            QStringLiteral("py"),
            QStringList() << "-3" << scriptPath << "--port" << port,
            QStringLiteral("py -3"),
            scriptWorkingDir
        });
    }

    return candidates;
}

void MainWindow::tryStartNextGrpcTestServerCandidate()
{
    if (!m_grpcTestServerProcess) {
        return;
    }

    if (m_grpcTestServerLaunchQueue.isEmpty()) {
        m_grpcTestServerLaunchInProgress = false;
        m_grpcTestServerCurrentDisplayName.clear();

        setGrpcTestServiceStatus("启动失败", "red");

        const QString details = m_grpcTestServerStartErrors.isEmpty()
            ? QStringLiteral("未找到可用的启动候选项")
            : m_grpcTestServerStartErrors.join("\n");

        QMessageBox::warning(this,
                             "启动失败",
                             QString("无法启动本地 gRPC 测试服务。\n"
                                     "生产环境请优先提供 grpc_test_server.exe。\n\n"
                                     "启动详情:\n%1")
                                 .arg(details));
        QString detailForLog = details;
        detailForLog.replace("\n", " | ");
        logGrpcInteraction("test-server", QString("启动失败详情：%1").arg(detailForLog));

        updateGrpcTestUiState();
        return;
    }

    const GrpcTestServerLaunchCandidate candidate = m_grpcTestServerLaunchQueue.takeFirst();
    m_grpcTestServerCurrentDisplayName = candidate.displayName;
    m_grpcTestServerProcess->setWorkingDirectory(candidate.workingDirectory);
    m_grpcTestServerProcess->start(candidate.program, candidate.arguments);

    if (m_grpcTestServerStartTimeoutTimer) {
        m_grpcTestServerStartTimeoutTimer->start();
    }

    logGrpcInteraction(
        "test-server",
        QString("尝试启动：%1, program=%2, args=%3")
            .arg(candidate.displayName, candidate.program, candidate.arguments.join(' ')));

    updateGrpcTestUiState();
}
#endif

int MainWindow::grpcEndpointPort() const
{
    if (!m_grpcEndpointEdit) {
        return 50051;
    }

    const QRegularExpression portRegex(R"(:(\d+)$)");
    const QRegularExpressionMatch match = portRegex.match(m_grpcEndpointEdit->text().trimmed());
    if (!match.hasMatch()) {
        return 50051;
    }

    const int port = match.captured(1).toInt();
    if (port <= 0 || port > 65535) {
        return 50051;
    }

    return port;
}

void MainWindow::addDataToMonitor(const QString& data, bool isHex, bool isReceived)
{
    if (data.isEmpty() || !m_dataMonitor) {
        return;
    }

    if (isReceived) {
        if (m_monitorPanel && !m_monitorPanel->isVisible()) {
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_lastMonitorAppendTime < m_monitorAppendIntervalMs) {
            return;
        }
        m_lastMonitorAppendTime = nowMs;
    }

    QString payload = data;
    if (!isHex) {
        const QString decodedPayload = QTextDocumentFragment::fromHtml(payload).toPlainText();
        if (!decodedPayload.isEmpty()) {
            payload = decodedPayload;
        }
    }

    if (payload.size() > 256) {
        payload = payload.left(256) + " ...";
    }
    
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString direction = isReceived ? "[接收]" : "[发送]";
    QString format = isHex ? "HEX" : "TXT";
    
    QString displayText = QString("%1 %2 %3: %4")
                             .arg(timestamp)
                             .arg(direction)
                             .arg(format)
                             .arg(payload);
    
    // 添加到监控区域
    appendMonitorLog(displayText);
    
    // 自动滚动到底部
    QScrollBar* scrollBar = m_dataMonitor->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void MainWindow::appendMonitorLog(const QString& text, const QString& color)
{
    if (!m_dataMonitor || text.isEmpty()) {
        return;
    }

    QString normalized = text;
    for (int i = 0; i < 3; ++i) {
        const QString decoded = QTextDocumentFragment::fromHtml(normalized).toPlainText();
        if (decoded.isEmpty() || decoded == normalized) {
            break;
        }
        normalized = decoded;
    }

    if (color.isEmpty()) {
        m_dataMonitor->append(normalized);
    } else {
        const QString escaped = normalized.toHtmlEscaped();
        m_dataMonitor->append(QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(color, escaped));
    }

    if (QScrollBar* scrollBar = m_dataMonitor->verticalScrollBar()) {
        scrollBar->setValue(scrollBar->maximum());
    }
}

// 槽函数实现
void MainWindow::onConnectClicked()
{
    if (m_appController) {
        if (m_connectionInProgress) {
            return;
        }
        const bool isGrpcBackend = (m_backendTypeCombo &&
                                    m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
        if (isGrpcBackend) {
            logGrpcInteraction("connect", QString("点击连接，endpoint=%1 mock=%2")
                                             .arg(m_grpcEndpointEdit->text().trimmed(),
                                                  m_useMockDataCheck->isChecked() ? "true" : "false"));
        }
        saveConfigFromUI();
        AppConfig* config = AppConfig::instance();
        if (config) {
            config->saveToFile(AppConfig::defaultConfigFilePath());
        }
        m_appController->reloadRuntimeConfig();
        onConnectionProgressChanged(isGrpcBackend);
        m_appController->start();
    }
}

void MainWindow::onDisconnectClicked()
{
    if (m_appController) {
        const bool isGrpcBackend = (m_backendTypeCombo &&
                                    m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
        if (isGrpcBackend) {
            logGrpcInteraction("connect", "点击断开");
        }
        m_appController->stop();
        updateConnectionStatus(false);
    }
}

void MainWindow::onPauseClicked()
{
    if (!m_appController) {
        return;
    }

    m_appController->pauseAcquisition();
    m_connectionStatusLabel->setText("已暂停");
    m_connectionStatusLabel->setStyleSheet("color: orange;");
    m_pauseButton->setEnabled(false);
    m_resumeButton->setEnabled(true);
}

void MainWindow::onResumeClicked()
{
    if (!m_appController) {
        return;
    }

    m_appController->resumeAcquisition();
    m_connectionStatusLabel->setText("已连接");
    m_connectionStatusLabel->setStyleSheet("color: green;");
    m_pauseButton->setEnabled(true);
    m_resumeButton->setEnabled(false);
}

void MainWindow::onUseMockDataChanged(bool use)
{
    m_mockIntervalSpin->setEnabled(use);
    updateGrpcTestUiState();
}

void MainWindow::onBackendTypeChanged(int index)
{
    Q_UNUSED(index)
    const QString backendType = m_backendTypeCombo->currentData().toString();
    const bool isGrpc = (backendType.compare("grpc", Qt::CaseInsensitive) == 0);

    if (isGrpc) {
        m_useMockDataCheck->setText(QStringLiteral("被测设备 gRPC 本地模拟"));
    } else {
        m_useMockDataCheck->setText(QStringLiteral("启用模拟数据"));
    }

    m_grpcEndpointEdit->setEnabled(isGrpc);

    for (QWidget* field : m_serialOnlyFields) {
        if (!field) {
            continue;
        }
        field->setVisible(!isGrpc);
        field->setEnabled(!isGrpc);
    }
    for (QWidget* label : m_serialOnlyLabels) {
        if (!label) {
            continue;
        }
        label->setVisible(!isGrpc);
    }

    if (m_grpcTestGroup) {
        m_grpcTestGroup->setVisible(true);
    }

    if (!isGrpc) {
        updateSerialPortList();
    }

    m_grpcSelfTestPending = false;
    m_grpcSelfTestTimeoutTimer->stop();
    m_grpcSelfTestPendingAcks.clear();
    m_grpcSelfTestCommandAcked = false;
    m_grpcSelfTestModeSwitchAcked = false;
    m_grpcSelfTestStreamReceived = false;
    m_grpcPeriodicPacketCount = 0;
    m_grpcPeriodicIntervalSumMs = 0;
    m_lastGrpcStreamPayloadTimestampMs = 0;
    m_lastGrpcStreamTimestampMs = 0;
    setGrpcLabelState(m_grpcSelfTestTxState, QStringLiteral("发送(Ping): 待验证"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcSelfTestRxState, QStringLiteral("接收(流数据): 待验证"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcModeSwitchState, QStringLiteral("待验证"), GrpcLabelTone::Neutral);
    setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("待验证"), GrpcLabelTone::Neutral);

    saveConfigFromUI();
    if (m_appController) {
        m_appController->applyReceiverBackendFromConfig();
    }

    updateStagePanelUiState();
    updateGrpcTestUiState();
}

void MainWindow::onStartGrpcTestServerClicked()
{
#ifdef QT_COMPILE_FOR_WASM
    QMessageBox::information(this, "提示", "WASM 环境不支持启动本地 gRPC 测试服务");
    return;
#else
    if (!m_grpcTestServerProcess ||
        m_grpcTestServerProcess->state() != QProcess::NotRunning ||
        m_grpcTestServerLaunchInProgress) {
        return;
    }

    logGrpcInteraction("test-server", "请求启动本地 gRPC 测试服务");

    const QString port = QString::number(grpcEndpointPort());
    m_grpcTestServerLaunchQueue = buildGrpcTestServerLaunchCandidates(port);
    m_grpcTestServerStartErrors.clear();
    m_grpcTestServerCurrentDisplayName.clear();
    m_grpcTestServerStopRequested = false;
    m_grpcTestServerLaunchInProgress = true;

    bool hasExecutableCandidate = false;
    bool hasPythonFallbackCandidate = false;
    for (const GrpcTestServerLaunchCandidate& candidate : m_grpcTestServerLaunchQueue) {
        if (candidate.displayName == "grpc_test_server.exe") {
            hasExecutableCandidate = true;
        } else {
            hasPythonFallbackCandidate = true;
        }
    }

    if (!hasExecutableCandidate) {
        logGrpcInteraction("test-server", "未找到 grpc_test_server.exe，回退到 Python 脚本模式");
    }
    if (!hasPythonFallbackCandidate) {
        logGrpcInteraction("test-server", "未找到 grpc_test_server.py，无法执行 Python 回退启动");
    }

    setGrpcTestServiceStatus("启动中...", "#c08000");

    tryStartNextGrpcTestServerCandidate();
#endif
}

void MainWindow::onStopGrpcTestServerClicked()
{
#ifndef QT_COMPILE_FOR_WASM
    if (!m_grpcTestServerProcess ||
        (m_grpcTestServerProcess->state() == QProcess::NotRunning && !m_grpcTestServerLaunchInProgress)) {
        return;
    }

    logGrpcInteraction("test-server", "请求停止本地 gRPC 测试服务");

    m_grpcTestServerStopRequested = true;
    m_grpcTestServerLaunchInProgress = false;
    m_grpcTestServerLaunchQueue.clear();
    m_grpcTestServerCurrentDisplayName.clear();
    if (m_grpcTestServerStartTimeoutTimer) {
        m_grpcTestServerStartTimeoutTimer->stop();
    }

    setGrpcTestServiceStatus("停止中...", "#c08000");

    m_grpcTestServerProcess->terminate();
    QTimer::singleShot(2000, this, [this]() {
        if (!m_grpcTestServerProcess) {
            return;
        }
        if (m_grpcTestServerProcess->state() != QProcess::NotRunning) {
            logGrpcInteraction("test-server", "停止超时，执行强制结束");
            m_grpcTestServerProcess->kill();
        }
    });

    updateGrpcTestUiState();
#endif
}

void MainWindow::onGrpcTestServerStartTimeout()
{
#ifndef QT_COMPILE_FOR_WASM
    if (!m_grpcTestServerProcess || !m_grpcTestServerLaunchInProgress) {
        return;
    }

    if (m_grpcTestServerProcess->state() != QProcess::NotRunning) {
        if (m_grpcTestServerStartTimeoutTimer) {
            m_grpcTestServerStartTimeoutTimer->start();
        }
        return;
    }

    const QString displayName = m_grpcTestServerCurrentDisplayName.isEmpty()
        ? QStringLiteral("候选启动项")
        : m_grpcTestServerCurrentDisplayName;
    const QString reason = QString("%1: 启动超时").arg(displayName);
    m_grpcTestServerStartErrors << reason;
    logGrpcInteraction("test-server", reason);
    m_grpcTestServerCurrentDisplayName.clear();

    tryStartNextGrpcTestServerCandidate();
#endif
}

void MainWindow::onRunGrpcSelfTestClicked()
{
    startGrpcSelfTest(false);
}

void MainWindow::onGrpcSelfTestTimeout()
{
    if (!m_grpcSelfTestPending) {
        return;
    }

    m_grpcSelfTestPending = false;
    m_grpcSelfTestPendingAcks.clear();

    if (!m_grpcSelfTestCommandAcked) {
        setGrpcLabelState(m_grpcSelfTestTxState, QStringLiteral("发送(Ping): 超时未回包"), GrpcLabelTone::Error);
    }

    if (!m_grpcSelfTestModeSwitchAcked) {
        setGrpcLabelState(
            m_grpcModeSwitchState,
            QString("切换(%1): 超时未回包")
                .arg(m_grpcSelfTestTargetMode.isEmpty() ? QStringLiteral("-")
                                                        : m_grpcSelfTestTargetMode),
            GrpcLabelTone::Error);
    }

    if (!m_grpcSelfTestStreamReceived) {
        setGrpcLabelState(m_grpcSelfTestRxState, QStringLiteral("接收(流数据): 未收到"), GrpcLabelTone::Error);
        setGrpcLabelState(m_grpcPeriodicDataState, QStringLiteral("周期数据: 未收到"), GrpcLabelTone::Error);
    }

    QStringList missingItems;
    if (!m_grpcSelfTestCommandAcked) {
        missingItems << "Ping回包";
    }
    if (!m_grpcSelfTestModeSwitchAcked) {
        missingItems << "模式切换回包";
    }
    if (!m_grpcSelfTestStreamReceived) {
        missingItems << "流数据";
    }

    setGrpcLabelState(
        m_grpcSelfTestOverallState,
        QString("收发验证失败（缺少%1）").arg(missingItems.join("、")),
        GrpcLabelTone::Error);
    logGrpcInteraction("self-test", QString("自检超时失败，缺少: %1").arg(missingItems.join("、")));
    updateGrpcTestUiState();
}

void MainWindow::onSendClicked()
{
    QString command = m_commandInput->toPlainText().trimmed();
    if (command.isEmpty()) {
        QMessageBox::warning(this, "警告", "指令不能为空");
        return;
    }

    const bool isHex = m_hexFormatCheck->isChecked();
    
    // 添加到历史记录
    addCommandToHistory(command);
    
    // 获取换行符
    QString newline = "";
    if (m_autoNewlineCheck->isChecked()) {
        int index = m_newlineCombo->currentIndex();
        switch (index) {
        case 0: newline = "\r\n"; break;
        case 1: newline = "\r"; break;
        case 2: newline = "\n"; break;
        }
    }
    
    // 在监控区显示发送的指令
    addDataToMonitor(command, isHex, false);
    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
    if (isGrpcBackend) {
        logGrpcInteraction("send", QString("发送命令: %1 (isHex=%2)").arg(command, isHex ? "true" : "false"));
    }
    
    // 通过 ApplicationController 转发指令到串口模块（线程安全）
    if (m_appController) {
        QString outCmd = command;
        if (!isHex && !newline.isEmpty()) {
            outCmd += newline;
        }
        m_appController->sendCommand(outCmd, isHex);
    } else {
        QMessageBox::warning(this, "错误", "未初始化应用控制器，无法发送指令");
    }
    
    // 清空输入框
    if (m_autoNewlineCheck->isChecked()) {
        m_commandInput->clear();
    }
}

void MainWindow::onClearClicked()
{
    m_commandInput->clear();
}

void MainWindow::onCommandHistoryDoubleClicked(const QModelIndex& index)
{
    QListWidgetItem* item = m_commandHistoryList->item(index.row());
    if (item) {
        m_commandInput->setPlainText(item->text());
    }
}

void MainWindow::onSendFormatChanged(bool isHex)
{
    Q_UNUSED(isHex)
    // 更新显示格式
}

void MainWindow::onCreateWindowClicked()
{
    qDebug() << "[MainWindow] onCreateWindowClicked called";
    if (!m_plotWindowManager || !m_mdiArea) {
        qWarning() << "[MainWindow] cannot create window, manager or mdiArea null";
        return;
    }
    
    PlotWindowManager::PlotType type = PlotWindowManager::CombinedPlot;
    int typeIndex = m_windowTypeCombo->currentIndex();
    
    switch (typeIndex) {
    case 0: type = PlotWindowManager::CombinedPlot; break;
    case 1: type = PlotWindowManager::HeatmapPlot; break;
    case 2: type = PlotWindowManager::ArrayPlot; break;
    case 3: type = PlotWindowManager::PulsedDecayPlot; break;
    case 4: type = PlotWindowManager::InspectionPlot; break;
    case 5: type = PlotWindowManager::ArrayHeatmapPlot; break;
    default:
        qWarning() << "[MainWindow] 窗口类型索引异常:" << typeIndex << "，使用组合图";
        type = PlotWindowManager::CombinedPlot;
        break;
    }
    
    qDebug() << "[MainWindow] creating window type" << type;
    PlotWindowBase* window = m_plotWindowManager->createWindowInMdiArea(m_mdiArea, type);
    qDebug() << "[MainWindow] createWindowInMdiArea returned" << window;
    if (window) {
        updateWindowList();
        qDebug() << "[MainWindow] window list updated";
    }
}


void MainWindow::onCloseWindowClicked()
{
    QListWidgetItem* item = m_windowList->currentItem();
    if (!item || !m_plotWindowManager) {
        return;
    }
    
    PlotWindowBase* window = item->data(Qt::UserRole).value<PlotWindowBase*>();
    if (window) {
        window->close();
        updateWindowList();
    }
}

void MainWindow::onTileWindowsClicked()
{
    if (!m_mdiArea) {
        return;
    }
    
    // 平铺MDI子窗口
    QList<QMdiSubWindow*> subWindows = m_mdiArea->subWindowList();
    if (subWindows.isEmpty()) {
        return;
    }
    
    m_mdiArea->tileSubWindows();
}

void MainWindow::onCascadeWindowsClicked()
{
    if (!m_mdiArea) {
        return;
    }
    
    // 层叠MDI子窗口
    QList<QMdiSubWindow*> subWindows = m_mdiArea->subWindowList();
    if (subWindows.isEmpty()) {
        return;
    }
    
    m_mdiArea->cascadeSubWindows();
}

void MainWindow::onWindowListDoubleClicked(const QModelIndex& index)
{
    QListWidgetItem* item = m_windowList->item(index.row());
    if (!item) {
        return;
    }
    
    PlotWindowBase* window = item->data(Qt::UserRole).value<PlotWindowBase*>();
    if (window) {
        window->raise();
        window->activateWindow();
    }
}

void MainWindow::onShowDevicePanelChanged(bool show)
{
    m_devicePanel->setVisible(show);
}

void MainWindow::onShowCommandPanelChanged(bool show)
{
    m_commandPanel->setVisible(show);
}

void MainWindow::onShowPlotPanelChanged(bool show)
{
    m_plotPanel->setVisible(show);
}

void MainWindow::onShowMonitorPanelChanged(bool show)
{
    m_monitorPanel->setVisible(show);
}

void MainWindow::onShowStagePanelChanged(bool show)
{
    if (m_stagePanel) {
        m_stagePanel->setVisible(show);
    }
}

void MainWindow::onShowOverviewPanelChanged(bool show)
{
    if (m_overviewPanel) {
        m_overviewPanel->setVisible(show);
    }
}

void MainWindow::onDataReceived(const QByteArray& data, bool isHex)
{
    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);

    if (isGrpcBackend) {
        QJsonParseError parseError;
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error == QJsonParseError::NoError && jsonDoc.isObject()) {
            handleGrpcBackendPacket(jsonDoc.object());
        }
    }

    if (m_monitorPanel && !m_monitorPanel->isVisible()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastMonitorAppendTime < m_monitorAppendIntervalMs) {
        return;
    }

    QString displayData;
    if (isHex) {
        displayData = data.toHex(' ').toUpper();
    } else {
        displayData = QString::fromUtf8(data);
    }
    
    addDataToMonitor(displayData, isHex, true);
}

void MainWindow::onStageDataReceived(const QByteArray& data, bool isHex)
{
    QJsonParseError parseError;
    const QJsonDocument jsonDoc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error == QJsonParseError::NoError && jsonDoc.isObject()) {
        handleStageBackendPacket(jsonDoc.object());
    }

    if (m_monitorPanel && !m_monitorPanel->isVisible()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastMonitorAppendTime < m_monitorAppendIntervalMs) {
        return;
    }

    QString displayData;
    if (isHex) {
        displayData = data.toHex(' ').toUpper();
    } else {
        displayData = QString::fromUtf8(data);
    }

    addDataToMonitor(displayData, isHex, true);
}

void MainWindow::onStageCommandSent(const QByteArray& command)
{
    const QString text = QString::fromUtf8(command).trimmed();
    addDataToMonitor(text, false, false);
    if (m_stageCommandResultLabel) {
        m_stageCommandResultLabel->setText(QStringLiteral("最近结果: 已发送 %1").arg(text));
        m_stageCommandResultLabel->setStyleSheet(QStringLiteral("color: #1d4ed8;"));
    }
}

void MainWindow::onStageCommandError(const QString& error)
{
    if (m_stageCommandResultLabel) {
        m_stageCommandResultLabel->setText(QStringLiteral("最近结果: 命令失败 - %1").arg(error));
        m_stageCommandResultLabel->setStyleSheet(QStringLiteral("color: red;"));
    }
    if (m_stageBackendStatusLabel) {
        m_stageBackendStatusLabel->setText(QStringLiteral("状态: 异常 | %1").arg(error));
        m_stageBackendStatusLabel->setStyleSheet(QStringLiteral("color: red;"));
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"));
    const QString errorText = QStringLiteral("%1 [错误]: %2").arg(timestamp, error);
    appendMonitorLog(errorText, QStringLiteral("red"));
}

void MainWindow::onRecorderDropAlert(const QString& message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"));
    const QString text = QStringLiteral("%1 [丢帧告警]: %2").arg(timestamp, message);
    appendMonitorLog(text, QStringLiteral("red"));
}

void MainWindow::onStageConnectionStateChanged(bool connected)
{
    Q_UNUSED(connected)
    updateStagePanelUiState();
}

void MainWindow::onCommandSent(const QByteArray& command)
{
    // 指令发送成功处理
    bool isHex = m_hexFormatCheck->isChecked();
    QString displayData = isHex ? command.toHex(' ').toUpper() : QString::fromUtf8(command);
    
    addDataToMonitor(displayData, isHex, false);

    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
    if (isGrpcBackend) {
        logGrpcInteraction("send", QString("命令已发送: %1")
                                     .arg(QString::fromUtf8(command).trimmed()));
    }
}

void MainWindow::onCommandError(const QString& error)
{
    if (m_grpcSelfTestPending) {
        m_grpcSelfTestPending = false;
        m_grpcSelfTestTimeoutTimer->stop();
        m_grpcSelfTestPendingAcks.clear();
        setGrpcLabelState(m_grpcSelfTestOverallState, QStringLiteral("收发验证失败（命令错误）"), GrpcLabelTone::Error);

        if (!m_grpcSelfTestCommandAcked) {
            setGrpcLabelState(m_grpcSelfTestTxState, QStringLiteral("发送(Ping): 命令失败"), GrpcLabelTone::Error);
        }
        if (!m_grpcSelfTestModeSwitchAcked) {
            setGrpcLabelState(
                m_grpcModeSwitchState,
                QString("切换(%1): 命令失败")
                    .arg(m_grpcSelfTestTargetMode.isEmpty() ? QStringLiteral("-")
                                                            : m_grpcSelfTestTargetMode),
                GrpcLabelTone::Error);
        }

        logGrpcInteraction("self-test", QString("自检失败（命令错误）: %1").arg(error));
        updateGrpcTestUiState();
    }

    const bool isGrpcBackend = (m_backendTypeCombo &&
                                m_backendTypeCombo->currentData().toString().compare("grpc", Qt::CaseInsensitive) == 0);
    if (isGrpcBackend) {
        logGrpcInteraction("error", error);
    }

    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString errorText = QString("%1 [错误]: %2").arg(timestamp).arg(error);

    appendMonitorLog(errorText, QStringLiteral("red"));
}

void MainWindow::onUpdateTimer()
{
    // 更新帧率显示
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    DataCacheManager* cache = DataCacheManager::instance();
    if (cache) {
        const qint64 totalFrameCount = cache->getTotalFrameCount();
        if (m_lastUpdateTime <= 0) {
            m_frameCount = static_cast<int>(totalFrameCount);
            m_frameRateLabel->setText(QStringLiteral("帧率: 0.0 fps"));
            const int cacheSize = cache->getCacheSize();
            const int maxCacheSize = cache->getMaxCacheSize();
            m_dataCountLabel->setText(QStringLiteral("数据: 累计 %1 帧 | 缓存 %2/%3 帧")
                                      .arg(totalFrameCount)
                                      .arg(cacheSize)
                                      .arg(maxCacheSize));
            m_lastUpdateTime = currentTime;
            updateWindowList();
            return;
        }

        double elapsed = (currentTime - m_lastUpdateTime) / 1000.0;
        if (elapsed > 0) {
            const qint64 currentFrameCount = totalFrameCount;
            const qint64 framesInPeriod = currentFrameCount - m_frameCount;

            const double frameRate = framesInPeriod / elapsed;
            m_frameRateLabel->setText(QString("帧率: %1 fps").arg(frameRate, 0, 'f', 1));

            m_frameCount = static_cast<int>(currentFrameCount);
            const int cacheSize = cache->getCacheSize();
            const int maxCacheSize = cache->getMaxCacheSize();
            m_dataCountLabel->setText(QStringLiteral("数据: 累计 %1 帧 | 缓存 %2/%3 帧")
                                      .arg(currentFrameCount)
                                      .arg(cacheSize)
                                      .arg(maxCacheSize));
        }
    }
    
    m_lastUpdateTime = currentTime;
    
    // 更新窗口列表
    updateWindowList();
}

void MainWindow::onPlotManagerTelemetryUpdated(int configuredIntervalMs,
                                               int actualIntervalMs,
                                               int fetchCount,
                                               qint64 receivedFramesSinceLast,
                                               int dispatchedFrames,
                                               qint64 unconsumedFramesTotal)
{
    Q_UNUSED(configuredIntervalMs)
    if (!m_uiRefreshLabel) {
        return;
    }

    if (actualIntervalMs > 0) {
        const double hz = 1000.0 / static_cast<double>(actualIntervalMs);
        m_uiRefreshLabel->setText(QStringLiteral("绘图周期: %1 ms (%2 Hz) | 拉取 %3 收到 %4 分发 %5 未消费累计 %6")
                                  .arg(actualIntervalMs)
                                  .arg(hz, 0, 'f', 1)
                                  .arg(fetchCount)
                                  .arg(receivedFramesSinceLast)
                                  .arg(dispatchedFrames)
                                  .arg(unconsumedFramesTotal));
    } else {
        m_uiRefreshLabel->setText(QStringLiteral("绘图周期: -- ms (-- Hz) | 拉取 %1 收到 %2 分发 %3 未消费累计 %4")
                                  .arg(fetchCount)
                                  .arg(receivedFramesSinceLast)
                                  .arg(dispatchedFrames)
                                  .arg(unconsumedFramesTotal));
    }
}

void MainWindow::switchStyle()
{
    if (m_currentStyle == AppConfig::DarkStyle)
        setStyle(AppConfig::LightStyle);
    else
        setStyle(AppConfig::DarkStyle);
}

void MainWindow::setStyle(AppConfig::Style style)
{
    QString stylefile;
    QString styletoolbar;
    QColor bgMdiColor(0xd3, 0xd3, 0xd3); // 浅色默认

    switch (style) {
    case AppConfig::DarkStyle: {
        stylefile = ":/files/css/darkStyle.css";
        styletoolbar = ":/files/css/toolBar_dark.css";
        bgMdiColor = QColor(0x1d, 0x1d, 0x1d); // 深色背景
        
        qDebug() << "Setting Dark Style";
    } break;
    case AppConfig::LightStyle: {
        stylefile = ":/files/css/lightStyle.css";
        styletoolbar = ":/files/css/toolBar_light.css";
        bgMdiColor = QColor(0xd3, 0xd3, 0xd3); // 浅色背景
        
        qDebug() << "Setting Light Style";
    } break;
    }

    // Application style
    QString css;
    
    // 尝试从多个可能的路径读取样式文件（优先内嵌 qrc，避免依赖「当前工作目录」）
    QStringList stylePaths;
    // 1. 资源路径（:/files/css/...，与 realtime_data.qrc 一致）
    stylePaths << stylefile;
    // 2. 与可执行文件同目录下散文件（便于无 qrc 时调试）
    stylePaths << QCoreApplication::applicationDirPath() + "/files/css/" + QFileInfo(stylefile).fileName();
    // 3. 当前工作目录（从项目根用命令行启动时可能命中）
    stylePaths << QDir::currentPath() + "/files/css/" + QFileInfo(stylefile).fileName();
    
    bool styleLoaded = false;
    for (const QString& path : stylePaths) {
        QFile f(path);
        if (f.open(QFile::ReadOnly)) {
            css = QLatin1String(f.readAll());
            f.close();
            qDebug() << "Loaded style file from:" << path << "size:" << css.size();
            styleLoaded = true;
            break;
        }
    }
    
    if (!styleLoaded) {
        qWarning() << "Failed to load style file from any path";
        // 设置默认样式
        css = getDefaultStyle(style);
        qDebug() << "Using default style for:" << style;
    }
    
    // Toolbar style
    QStringList toolbarPaths;
    toolbarPaths << styletoolbar;
    toolbarPaths << QCoreApplication::applicationDirPath() + "/files/css/" + QFileInfo(styletoolbar).fileName();
    toolbarPaths << QDir::currentPath() + "/files/css/" + QFileInfo(styletoolbar).fileName();
    
    bool toolbarLoaded = false;
    for (const QString& path : toolbarPaths) {
        QFile f2(path);
        if (f2.open(QFile::ReadOnly)) {
            css.append(QLatin1String(f2.readAll()));
            f2.close();
            qDebug() << "Loaded toolbar style from:" << path;
            toolbarLoaded = true;
            break;
        }
    }
    
    if (!toolbarLoaded) {
        qWarning() << "Failed to load toolbar style from any path";
        // 添加默认工具栏样式
        css.append(getDefaultToolbarStyle(style));
    }
    
    // Apply the style sheet
    qApp->setStyleSheet(css);
    qDebug() << "Style sheet applied to application";
    
    // Workaround. Background is not updated via style sheet.
    if (m_mdiArea) {
        m_mdiArea->setBackground(QBrush(bgMdiColor, Qt::SolidPattern));
        qDebug() << "MDI area background color set to:" << bgMdiColor;
    }

    m_currentStyle = style;
    
    // 保存样式配置
    AppConfig* config = AppConfig::instance();
    if (config) {
        config->setCurrentStyle(style);
        config->saveToFile(AppConfig::defaultConfigFilePath());
        qDebug() << "Style saved to config:" << style;
    }
}

// 默认样式定义
QString MainWindow::getDefaultStyle(AppConfig::Style style)
{
    if (style == AppConfig::DarkStyle) {
        return R"(
            /* 深色主题 - 现代风格 */
            QMainWindow, QDialog, QWidget {
                background-color: #1e1e1e;
                color: #e0e0e0;
                font-family: "Segoe UI", "Microsoft YaHei", sans-serif;
                font-size: 9pt;
            }
            
            /* 标题和标签 */
            QLabel {
                color: #e0e0e0;
            }
            QLabel[type="status"] {
                color: #a0a0a0;
                font-size: 8pt;
            }
            
            /* 按钮样式 */
            QPushButton {
                background-color: #2d2d2d;
                border: 1px solid #444;
                border-radius: 4px;
                padding: 6px 12px;
                color: #e0e0e0;
                font-weight: normal;
            }
            QPushButton:hover {
                background-color: #3a3a3a;
                border-color: #555;
            }
            QPushButton:pressed {
                background-color: #1f1f1f;
            }
            QPushButton:disabled {
                background-color: #2a2a2a;
                color: #888;
                border-color: #333;
            }
            
            /* 主要操作按钮 */
            QPushButton[important="true"] {
                background-color: #0d6188;
                border-color: #1283b7;
                color: white;
                font-weight: bold;
            }
            QPushButton[important="true"]:hover {
                background-color: #1492ca;
            }
            QPushButton[important="true"]:pressed {
                background-color: #0a4d6c;
            }
            
            /* 输入控件 */
            QLineEdit, QTextEdit, QPlainTextEdit {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 3px;
                padding: 5px;
                color: #e0e0e0;
                selection-background-color: #0d6188;
                selection-color: white;
            }
            QLineEdit:focus, QTextEdit:focus {
                border-color: #0d6188;
            }
            QTextEdit {
                background-color: #2a2a2a;
            }
            
            /* 下拉框 */
            QComboBox {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 3px;
                padding: 5px;
                color: #e0e0e0;
                min-height: 22px;
            }
            QComboBox:editable {
                background-color: #2a2a2a;
            }
            QComboBox:on {
                border-color: #0d6188;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-top: 5px solid #888;
            }
            QComboBox QAbstractItemView {
                background-color: #2a2a2a;
                border: 1px solid #444;
                color: #e0e0e0;
                selection-background-color: #0d6188;
                selection-color: white;
            }
            
            /* 复选框和单选按钮 */
            QCheckBox, QRadioButton {
                color: #e0e0e0;
                spacing: 5px;
            }
            QCheckBox::indicator, QRadioButton::indicator {
                width: 16px;
                height: 16px;
            }
            QCheckBox::indicator:unchecked {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 2px;
            }
            QCheckBox::indicator:checked {
                background-color: #0d6188;
                border: 1px solid #1283b7;
                border-radius: 2px;
                image: url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="white" d="M10.3 3.3L5 8.6 1.7 5.3c-.4-.4-.4-1 0-1.4s1-.4 1.4 0L5 5.9l3.9-3.9c.4-.4 1-.4 1.4 0s.4 1 0 1.4z"/></svg>');
            }
            QRadioButton::indicator:unchecked {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 8px;
            }
            QRadioButton::indicator:checked {
                background-color: #0d6188;
                border: 1px solid #1283b7;
                border-radius: 8px;
            }
            
            /* 分组框 */
            QGroupBox {
                border: 1px solid #444;
                border-radius: 5px;
                margin-top: 10px;
                padding-top: 15px;
                color: #e0e0e0;
                font-weight: bold;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px;
                background-color: #1e1e1e;
            }
            
            /* 列表框 */
            QListWidget {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 3px;
                color: #e0e0e0;
                outline: none;
            }
            QListWidget::item {
                padding: 5px;
                border-bottom: 1px solid #333;
            }
            QListWidget::item:selected {
                background-color: #0d6188;
                color: white;
            }
            QListWidget::item:hover {
                background-color: #3a3a3a;
            }
            
            /* 滚动条 */
            QScrollBar:vertical {
                background: #2a2a2a;
                width: 10px;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical {
                background: #555;
                border-radius: 5px;
                min-height: 20px;
            }
            QScrollBar::handle:vertical:hover {
                background: #666;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0px;
            }
            
            /* 状态栏 */
            QStatusBar {
                background-color: #2a2a2a;
                border-top: 1px solid #444;
                color: #a0a0a0;
            }
            
            /* MDI区域 */
            QMdiArea {
                background-color: #252525;
            }
            QMdiSubWindow {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 3px;
            }
            QMdiSubWindow:active {
                border-color: #0d6188;
            }
            
            /* Dock窗口 */
            QDockWidget {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 3px;
                titlebar-close-icon: url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="%23e0e0e0" d="M10.3 3.3L6.6 7l3.7 3.7c.4.4.4 1 0 1.4s-1 .4-1.4 0L5.2 8.4l-3.7 3.7c-.4.4-1 .4-1.4 0s-.4-1 0-1.4L3.8 7 .1 3.3c-.4-.4-.4-1 0-1.4s1-.4 1.4 0L5.2 5.6 8.9 1.9c.4-.4 1-.4 1.4 0s.4 1 0 1.4z"/></svg>');
                titlebar-normal-icon: url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="%23e0e0e0" d="M11 1H1v10h10V1zM2 9V3h8v6H2z"/></svg>');
            }
            QDockWidget::title {
                background-color: #2a2a2a;
                padding: 5px;
                border-bottom: 1px solid #444;
            }
            QDockWidget::close-button, QDockWidget::float-button {
                background-color: transparent;
                border: none;
                padding: 2px;
            }
            QDockWidget::close-button:hover, QDockWidget::float-button:hover {
                background-color: #3a3a3a;
                border-radius: 2px;
            }
            
            /* SpinBox */
            QSpinBox, QDoubleSpinBox {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 3px;
                padding: 5px;
                color: #e0e0e0;
            }
            QSpinBox:focus, QDoubleSpinBox:focus {
                border-color: #0d6188;
            }
            QSpinBox::up-button, QSpinBox::down-button,
            QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
                background-color: #333;
                border: 1px solid #444;
                width: 16px;
            }
            QSpinBox::up-button:hover, QSpinBox::down-button:hover,
            QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover {
                background-color: #3a3a3a;
            }
            
            /* 分隔线 */
            QSplitter::handle {
                background-color: #444;
            }
            QSplitter::handle:hover {
                background-color: #555;
            }
        )";
    } else { // LightStyle
        return R"(
            /* 浅色主题 - 现代风格 */
            QMainWindow, QDialog, QWidget {
                background-color: #f5f5f5;
                color: #333333;
                font-family: "Segoe UI", "Microsoft YaHei", sans-serif;
                font-size: 9pt;
            }
            
            /* 标题和标签 */
            QLabel {
                color: #333333;
            }
            QLabel[type="status"] {
                color: #666666;
                font-size: 8pt;
            }
            
            /* 按钮样式 */
            QPushButton {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 4px;
                padding: 6px 12px;
                color: #333333;
                font-weight: normal;
            }
            QPushButton:hover {
                background-color: #f0f0f0;
                border-color: #aaaaaa;
            }
            QPushButton:pressed {
                background-color: #e0e0e0;
            }
            QPushButton:disabled {
                background-color: #f8f8f8;
                color: #aaaaaa;
                border-color: #dddddd;
            }
            
            /* 主要操作按钮 */
            QPushButton[important="true"] {
                background-color: #1492ca;
                border-color: #1283b7;
                color: white;
                font-weight: bold;
            }
            QPushButton[important="true"]:hover {
                background-color: #18a7ea;
            }
            QPushButton[important="true"]:pressed {
                background-color: #0d6188;
            }
            
            /* 输入控件 */
            QLineEdit, QTextEdit, QPlainTextEdit {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 3px;
                padding: 5px;
                color: #333333;
                selection-background-color: #1492ca;
                selection-color: white;
            }
            QLineEdit:focus, QTextEdit:focus {
                border-color: #1492ca;
            }
            QTextEdit {
                background-color: #ffffff;
            }
            
            /* 下拉框 */
            QComboBox {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 3px;
                padding: 5px;
                color: #333333;
                min-height: 22px;
            }
            QComboBox:editable {
                background-color: #ffffff;
            }
            QComboBox:on {
                border-color: #1492ca;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-top: 5px solid #666666;
            }
            QComboBox QAbstractItemView {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                color: #333333;
                selection-background-color: #1492ca;
                selection-color: white;
            }
            
            /* 复选框和单选按钮 */
            QCheckBox, QRadioButton {
                color: #333333;
                spacing: 5px;
            }
            QCheckBox::indicator, QRadioButton::indicator {
                width: 16px;
                height: 16px;
            }
            QCheckBox::indicator:unchecked {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 2px;
            }
            QCheckBox::indicator:checked {
                background-color: #1492ca;
                border: 1px solid #1283b7;
                border-radius: 2px;
                image: url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="white" d="M10.3 3.3L5 8.6 1.7 5.3c-.4-.4-.4-1 0-1.4s1-.4 1.4 0L5 5.9l3.9-3.9c.4-.4 1-.4 1.4 0s.4 1 0 1.4z"/></svg>');
            }
            QRadioButton::indicator:unchecked {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 8px;
            }
            QRadioButton::indicator:checked {
                background-color: #1492ca;
                border: 1px solid #1283b7;
                border-radius: 8px;
            }
            
            /* 分组框 */
            QGroupBox {
                border: 1px solid #cccccc;
                border-radius: 5px;
                margin-top: 10px;
                padding-top: 15px;
                color: #333333;
                font-weight: bold;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px;
                background-color: #f5f5f5;
            }
            
            /* 列表框 */
            QListWidget {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 3px;
                color: #333333;
                outline: none;
            }
            QListWidget::item {
                padding: 5px;
                border-bottom: 1px solid #eeeeee;
            }
            QListWidget::item:selected {
                background-color: #1492ca;
                color: white;
            }
            QListWidget::item:hover {
                background-color: #f0f0f0;
            }
            
            /* 滚动条 */
            QScrollBar:vertical {
                background: #f0f0f0;
                width: 10px;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical {
                background: #cccccc;
                border-radius: 5px;
                min-height: 20px;
            }
            QScrollBar::handle:vertical:hover {
                background: #aaaaaa;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0px;
            }
            
            /* 状态栏 */
            QStatusBar {
                background-color: #f0f0f0;
                border-top: 1px solid #cccccc;
                color: #666666;
            }
            
            /* MDI区域 */
            QMdiArea {
                background-color: #e8e8e8;
            }
            QMdiSubWindow {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 3px;
            }
            QMdiSubWindow:active {
                border-color: #1492ca;
            }
            
            /* Dock窗口 */
            QDockWidget {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 3px;
                titlebar-close-icon: url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="%23333333" d="M10.3 3.3L6.6 7l3.7 3.7c.4.4.4 1 0 1.4s-1 .4-1.4 0L5.2 8.4l-3.7 3.7c-.4.4-1 .4-1.4 0s-.4-1 0-1.4L3.8 7 .1 3.3c-.4-.4-.4-1 0-1.4s1-.4 1.4 0L5.2 5.6 8.9 1.9c.4-.4 1-.4 1.4 0s.4 1 0 1.4z"/></svg>');
                titlebar-normal-icon: url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="%23333333" d="M11 1H1v10h10V1zM2 9V3h8v6H2z"/></svg>');
            }
            QDockWidget::title {
                background-color: #ffffff;
                padding: 5px;
                border-bottom: 1px solid #cccccc;
            }
            QDockWidget::close-button, QDockWidget::float-button {
                background-color: transparent;
                border: none;
                padding: 2px;
            }
            QDockWidget::close-button:hover, QDockWidget::float-button:hover {
                background-color: #f0f0f0;
                border-radius: 2px;
            }
            
            /* SpinBox */
            QSpinBox, QDoubleSpinBox {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                border-radius: 3px;
                padding: 5px;
                color: #333333;
            }
            QSpinBox:focus, QDoubleSpinBox:focus {
                border-color: #1492ca;
            }
            QSpinBox::up-button, QSpinBox::down-button,
            QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
                background-color: #f8f8f8;
                border: 1px solid #cccccc;
                width: 16px;
            }
            QSpinBox::up-button:hover, QSpinBox::down-button:hover,
            QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover {
                background-color: #f0f0f0;
            }
            
            /* 分隔线 */
            QSplitter::handle {
                background-color: #cccccc;
            }
            QSplitter::handle:hover {
                background-color: #aaaaaa;
            }
        )";
    }
}

QString MainWindow::getDefaultToolbarStyle(AppConfig::Style style)
{
    if (style == AppConfig::DarkStyle) {
        return R"(
            /* 深色主题工具栏样式 */
            QToolBar {
                background-color: #2a2a2a;
                border: none;
                border-bottom: 1px solid #444;
                spacing: 3px;
                padding: 3px;
            }
            QToolButton {
                background-color: transparent;
                border: 1px solid transparent;
                border-radius: 3px;
                padding: 5px;
                color: #e0e0e0;
            }
            QToolButton:hover {
                background-color: #3a3a3a;
                border: 1px solid #555;
            }
            QToolButton:pressed {
                background-color: #1f1f1f;
            }
            QToolButton:checked {
                background-color: #0d6188;
                color: white;
            }
            QToolButton:disabled {
                color: #666666;
            }
            
            /* 菜单栏 */
            QMenuBar {
                background-color: #2a2a2a;
                border-bottom: 1px solid #444;
                color: #e0e0e0;
            }
            QMenuBar::item {
                padding: 5px 10px;
                background-color: transparent;
            }
            QMenuBar::item:selected {
                background-color: #3a3a3a;
            }
            QMenuBar::item:pressed {
                background-color: #1f1f1f;
            }
            
            /* 菜单 */
            QMenu {
                background-color: #2a2a2a;
                border: 1px solid #444;
                color: #e0e0e0;
            }
            QMenu::item {
                padding: 5px 20px 5px 30px;
            }
            QMenu::item:selected {
                background-color: #0d6188;
                color: white;
            }
            QMenu::separator {
                height: 1px;
                background-color: #444;
                margin: 5px 10px;
            }
        )";
    } else { // LightStyle
        return R"(
            /* 浅色主题工具栏样式 */
            QToolBar {
                background-color: #ffffff;
                border: none;
                border-bottom: 1px solid #cccccc;
                spacing: 3px;
                padding: 3px;
            }
            QToolButton {
                background-color: transparent;
                border: 1px solid transparent;
                border-radius: 3px;
                padding: 5px;
                color: #333333;
            }
            QToolButton:hover {
                background-color: #f0f0f0;
                border: 1px solid #aaaaaa;
            }
            QToolButton:pressed {
                background-color: #e0e0e0;
            }
            QToolButton:checked {
                background-color: #1492ca;
                color: white;
            }
            QToolButton:disabled {
                color: #aaaaaa;
            }
            
            /* 菜单栏 */
            QMenuBar {
                background-color: #ffffff;
                border-bottom: 1px solid #cccccc;
                color: #333333;
            }
            QMenuBar::item {
                padding: 5px 10px;
                background-color: transparent;
            }
            QMenuBar::item:selected {
                background-color: #f0f0f0;
            }
            QMenuBar::item:pressed {
                background-color: #e0e0e0;
            }
            
            /* 菜单 */
            QMenu {
                background-color: #ffffff;
                border: 1px solid #cccccc;
                color: #333333;
            }
            QMenu::item {
                padding: 5px 20px 5px 30px;
            }
            QMenu::item:selected {
                background-color: #1492ca;
                color: white;
            }
            QMenu::separator {
                height: 1px;
                background-color: #cccccc;
                margin: 5px 10px;
            }
        )";
    }
}
