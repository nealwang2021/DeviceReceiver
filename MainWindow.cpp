#include "MainWindow.h"
#include "ApplicationController.h"
#include "PlotWindowManager.h"
#include "PlotWindow.h"
#include "AppConfig.h"
#include "SerialReceiver.h"
#include "DataCacheManager.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QMenuBar>
#include <QStatusBar>
#include <QAction>
#include <QToolBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QScrollBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>
#include <QLabel>
#include <QTextEdit>
#include <QTextDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QRegularExpression>
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
#endif
{
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
    // 保存配置
    saveConfigFromUI();
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
    
    if (connected) {
        m_connectionStatusLabel->setText("已连接");
        m_connectionStatusLabel->setStyleSheet("color: green;");
        m_connectButton->setEnabled(false);
        m_disconnectButton->setEnabled(true);
        m_pauseButton->setEnabled(true);
        m_resumeButton->setEnabled(false);
        m_exportButton->setEnabled(true);
    } else {
        m_connectionStatusLabel->setText("未连接");
        m_connectionStatusLabel->setStyleSheet("color: red;");
        m_connectButton->setEnabled(true);
        m_disconnectButton->setEnabled(false);
        m_pauseButton->setEnabled(false);
        m_resumeButton->setEnabled(false);
        m_exportButton->setEnabled(true);

        m_autoSelfTestTriggeredForConnection = false;
        if (m_grpcSelfTestPending) {
            m_grpcSelfTestPending = false;
            m_grpcSelfTestTimeoutTimer->stop();
            m_grpcSelfTestStatusLabel->setText("连接断开");
            m_grpcSelfTestStatusLabel->setStyleSheet("color: red;");
        }
        m_grpcSelfTestCommandAcked = false;
        m_grpcSelfTestModeSwitchAcked = false;
        m_grpcSelfTestStreamReceived = false;
        m_grpcSelfTestPendingAcks.clear();
        m_grpcSelfTestTargetMode.clear();
        m_grpcSelfTestTxStatusLabel->setText("发送(Ping): 待验证");
        m_grpcSelfTestRxStatusLabel->setText("接收(流数据): 待验证");
        m_grpcModeSwitchStatusLabel->setText("待验证");
        m_grpcPeriodicDataStatusLabel->setText("待验证");
        m_grpcSelfTestTxStatusLabel->setStyleSheet("color: gray;");
        m_grpcSelfTestRxStatusLabel->setStyleSheet("color: gray;");
        m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
        m_grpcPeriodicDataStatusLabel->setStyleSheet("color: gray;");
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

    if (connected && isGrpcRealMode && !m_autoSelfTestTriggeredForConnection) {
        m_autoSelfTestTriggeredForConnection = true;
        QTimer::singleShot(250, this, [this]() {
            startGrpcSelfTest(true);
        });
    }

    updateGrpcTestUiState();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
#ifndef QT_COMPILE_FOR_WASM
    if (m_grpcTestServerProcess && m_grpcTestServerProcess->state() != QProcess::NotRunning) {
        m_grpcTestServerProcess->terminate();
        if (!m_grpcTestServerProcess->waitForFinished(2000)) {
            m_grpcTestServerProcess->kill();
            m_grpcTestServerProcess->waitForFinished(1000);
        }
    }
#endif

    // 保存窗口状态和几何信息
    AppConfig* config = AppConfig::instance();
    if (config) {
        config->setMainWindowState(saveState());
        config->setMainWindowGeometry(saveGeometry());
        config->saveToFile("config.ini");
    }
    
    QMainWindow::closeEvent(event);
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
        QAction* showDeviceAction = viewMenu->addAction("设备面板(&D)");
        QAction* showCommandAction = viewMenu->addAction("指令面板(&C)");
        QAction* showPlotAction = viewMenu->addAction("绘图面板(&P)");
        QAction* showMonitorAction = viewMenu->addAction("监控面板(&M)");
        
        showDeviceAction->setCheckable(true);
        showCommandAction->setCheckable(true);
        showPlotAction->setCheckable(true);
        showMonitorAction->setCheckable(true);
        
        connect(showDeviceAction, &QAction::toggled, this, &MainWindow::onShowDevicePanelChanged);
        connect(showCommandAction, &QAction::toggled, this, &MainWindow::onShowCommandPanelChanged);
        connect(showPlotAction, &QAction::toggled, this, &MainWindow::onShowPlotPanelChanged);
        connect(showMonitorAction, &QAction::toggled, this, &MainWindow::onShowMonitorPanelChanged);
        
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
        
        // 1. 设备配置面板
        m_devicePanel = new QDockWidget("设备配置", this);
        m_devicePanel->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        
        QWidget* deviceWidget = new QWidget();
        QVBoxLayout* deviceLayout = new QVBoxLayout(deviceWidget);
        
        // 连接配置组
        QGroupBox* serialGroup = new QGroupBox("连接配置");
        QFormLayout* serialLayout = new QFormLayout(serialGroup);
        
        m_serialPortCombo = new QComboBox();
        m_backendTypeCombo = new QComboBox();
        m_backendTypeCombo->addItem("串口 (Serial)", "serial");
        m_backendTypeCombo->addItem("gRPC Client", "grpc");
        m_grpcEndpointEdit = new QLineEdit();
        m_grpcEndpointEdit->setPlaceholderText("127.0.0.1:50051");
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
        
        serialLayout->addRow("接收后端:", m_backendTypeCombo);
        serialLayout->addRow("gRPC地址:", m_grpcEndpointEdit);
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
        QGroupBox* controlGroup = new QGroupBox("设备控制");
        QHBoxLayout* controlLayout = new QHBoxLayout(controlGroup);
        
        m_connectButton = new QPushButton("连接");
        m_disconnectButton = new QPushButton("断开");
        m_pauseButton = new QPushButton("暂停采集");
        m_resumeButton = new QPushButton("恢复采集");
        m_exportButton = new QPushButton("导出缓存");
        m_disconnectButton->setEnabled(false);
        m_pauseButton->setEnabled(false);
        m_resumeButton->setEnabled(false);
        m_connectionStatusLabel = new QLabel("未连接");
        m_connectionStatusLabel->setStyleSheet("color: red;");
        
        controlLayout->addWidget(m_connectButton);
        controlLayout->addWidget(m_disconnectButton);
        controlLayout->addWidget(m_pauseButton);
        controlLayout->addWidget(m_resumeButton);
        controlLayout->addWidget(m_exportButton);
        controlLayout->addStretch();
        controlLayout->addWidget(m_connectionStatusLabel);

        QGroupBox* grpcTestGroup = new QGroupBox("gRPC 测试验证");
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
        m_grpcSelfTestStatusLabel->setStyleSheet("color: gray;");
        grpcSelfTestLayout->addWidget(m_grpcSelfTestStatusLabel);
        grpcSelfTestLayout->addStretch();

        QHBoxLayout* grpcResultLayout = new QHBoxLayout();
        m_grpcSelfTestTxStatusLabel = new QLabel("发送(Ping): 待验证");
        m_grpcSelfTestRxStatusLabel = new QLabel("接收(流数据): 待验证");
        m_grpcSelfTestTxStatusLabel->setStyleSheet("color: gray;");
        m_grpcSelfTestRxStatusLabel->setStyleSheet("color: gray;");
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
        m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
        grpcModeStatusLayout->addWidget(m_grpcModeSwitchStatusLabel);
        grpcModeStatusLayout->addStretch();

        QHBoxLayout* grpcPeriodicLayout = new QHBoxLayout();
        grpcPeriodicLayout->addWidget(new QLabel("周期数据:"));
        m_grpcPeriodicDataStatusLabel = new QLabel("待验证");
        m_grpcPeriodicDataStatusLabel->setStyleSheet("color: gray;");
        grpcPeriodicLayout->addWidget(m_grpcPeriodicDataStatusLabel);
        grpcPeriodicLayout->addStretch();

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
        deviceLayout->addStretch();
        
        m_devicePanel->setWidget(deviceWidget);
        addDockWidget(Qt::LeftDockWidgetArea, m_devicePanel);
        
        // 2. 指令发送面板
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
        
        // 3. 绘图管理面板
        m_plotPanel = new QDockWidget("绘图管理", this);
        m_plotPanel->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        
        QWidget* plotWidget = new QWidget();
        QVBoxLayout* plotLayout = new QVBoxLayout(plotWidget);
        
        // 窗口创建
        QGroupBox* createGroup = new QGroupBox("创建窗口");
        QFormLayout* createLayout = new QFormLayout(createGroup);
        
        m_windowTypeCombo = new QComboBox();
        m_windowTypeCombo->addItems({"组合图",  "历史图", "热力图", "阵列图"});
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
        
        // 4. 数据监控面板
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
        m_dataCountLabel = new QLabel("数据: 0 帧");
        m_alarmCountLabel = new QLabel("报警: 0 次");
        
        statusLayout->addWidget(m_frameRateLabel);
        statusLayout->addWidget(m_dataCountLabel);
        statusLayout->addWidget(m_alarmCountLabel);
        statusLayout->addStretch();
        
        monitorLayout->addWidget(m_dataMonitor);
        monitorLayout->addLayout(statusLayout);
        
        qDebug() << "[MainWindow::initUI] 添加monitorPanel到DockWidget";
        m_monitorPanel->setWidget(monitorWidget);
        addDockWidget(Qt::BottomDockWidgetArea, m_monitorPanel);
        qDebug() << "[MainWindow::initUI] monitorPanel添加完成";
        
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
            if (!config->mainWindowGeometry().isEmpty()) {
                restoreGeometry(config->mainWindowGeometry());
            }
            
            // 更新面板显示状态
            showDeviceAction->setChecked(config->showDevicePanel());
            showCommandAction->setChecked(config->showCommandPanel());
            showPlotAction->setChecked(config->showPlotPanel());
            showMonitorAction->setChecked(config->showMonitorPanel());
            
            m_devicePanel->setVisible(config->showDevicePanel());
            m_commandPanel->setVisible(config->showCommandPanel());
            m_plotPanel->setVisible(config->showPlotPanel());
            m_monitorPanel->setVisible(config->showMonitorPanel());
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
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::onExportClicked);
    connect(m_useMockDataCheck, &QCheckBox::toggled, this, &MainWindow::onUseMockDataChanged);
        connect(m_backendTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBackendTypeChanged);
    
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

    connect(m_startGrpcTestServerButton, &QPushButton::clicked, this, &MainWindow::onStartGrpcTestServerClicked);
    connect(m_stopGrpcTestServerButton, &QPushButton::clicked, this, &MainWindow::onStopGrpcTestServerClicked);
    connect(m_runGrpcSelfTestButton, &QPushButton::clicked, this, &MainWindow::onRunGrpcSelfTestClicked);
    connect(m_grpcSelfTestTimeoutTimer, &QTimer::timeout, this, &MainWindow::onGrpcSelfTestTimeout);
    connect(m_grpcModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_grpcSelfTestPending) {
            m_grpcModeSwitchStatusLabel->setText("待验证");
            m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
        }
    });

#ifndef QT_COMPILE_FOR_WASM
    if (m_grpcTestServerProcess) {
        connect(m_grpcTestServerProcess, &QProcess::started, this, [this]() {
            if (m_grpcTestServiceStatusLabel) {
                m_grpcTestServiceStatusLabel->setText("运行中");
                m_grpcTestServiceStatusLabel->setStyleSheet("color: green;");
            }
            logGrpcInteraction("test-server", "本地 gRPC 测试服务已启动");
            updateGrpcTestUiState();
        });

        connect(m_grpcTestServerProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
                    if (m_grpcTestServiceStatusLabel) {
                        if (exitStatus == QProcess::NormalExit) {
                            m_grpcTestServiceStatusLabel->setText(QString("已停止(%1)").arg(exitCode));
                            m_grpcTestServiceStatusLabel->setStyleSheet("color: gray;");
                            logGrpcInteraction("test-server", QString("测试服务已停止，exitCode=%1").arg(exitCode));
                        } else {
                            m_grpcTestServiceStatusLabel->setText("异常退出");
                            m_grpcTestServiceStatusLabel->setStyleSheet("color: red;");
                            logGrpcInteraction("test-server", "测试服务异常退出");
                        }
                    }
                    updateGrpcTestUiState();
                });

        connect(m_grpcTestServerProcess,
                &QProcess::errorOccurred,
                this,
                [this](QProcess::ProcessError error) {
                    Q_UNUSED(error)
                    if (m_grpcTestServiceStatusLabel) {
                        m_grpcTestServiceStatusLabel->setText("启动失败");
                        m_grpcTestServiceStatusLabel->setStyleSheet("color: red;");
                    }
                    logGrpcInteraction("test-server", "测试服务启动失败");
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
    const QString backendType = config->receiverBackendType().trimmed().toLower();
    const int backendIndex = m_backendTypeCombo->findData(backendType.isEmpty() ? "grpc" : backendType);
    m_backendTypeCombo->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
    m_grpcEndpointEdit->setText(config->grpcEndpoint());

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
    config->setShowMonitorPanel(m_monitorPanel->isVisible());
    
    // 保存窗口状态
    config->setMainWindowState(saveState());
    config->setMainWindowGeometry(saveGeometry());
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
    QList<PlotWindow*> windows = m_plotWindowManager->windows();
    
    for (PlotWindow* window : windows) {
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
            m_grpcSelfTestStatusLabel->setText("仅 gRPC 后端可用");
            m_grpcSelfTestStatusLabel->setStyleSheet("color: gray;");
            m_grpcModeSwitchStatusLabel->setText("仅 gRPC 后端可用");
            m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
            m_grpcPeriodicDataStatusLabel->setText("仅 gRPC 后端可用");
            m_grpcPeriodicDataStatusLabel->setStyleSheet("color: gray;");
        } else if (!isGrpcRealMode) {
            m_grpcSelfTestStatusLabel->setText("Mock 模式不验证真实链路");
            m_grpcSelfTestStatusLabel->setStyleSheet("color: gray;");
            m_grpcModeSwitchStatusLabel->setText("Mock 模式不验证切换");
            m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
            m_grpcPeriodicDataStatusLabel->setText("Mock 模式不验证周期");
            m_grpcPeriodicDataStatusLabel->setStyleSheet("color: gray;");
        } else if (!m_isConnected) {
            m_grpcSelfTestStatusLabel->setText("等待连接");
            m_grpcSelfTestStatusLabel->setStyleSheet("color: gray;");
            m_grpcModeSwitchStatusLabel->setText("等待连接");
            m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
            m_grpcPeriodicDataStatusLabel->setText("等待周期数据");
            m_grpcPeriodicDataStatusLabel->setStyleSheet("color: gray;");
        } else if (m_grpcPeriodicPacketCount <= 0) {
            m_grpcPeriodicDataStatusLabel->setText("等待周期数据");
            m_grpcPeriodicDataStatusLabel->setStyleSheet("color: #c08000;");
        }
    }
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

    m_grpcSelfTestTxStatusLabel->setText("发送(Ping): 验证中...");
    m_grpcSelfTestRxStatusLabel->setText("接收(流数据): 验证中...");
    m_grpcModeSwitchStatusLabel->setText(QString("切换(%1): 验证中...").arg(m_grpcSelfTestTargetMode));
    m_grpcPeriodicDataStatusLabel->setText("周期数据: 验证中...");
    m_grpcSelfTestTxStatusLabel->setStyleSheet("color: #c08000;");
    m_grpcSelfTestRxStatusLabel->setStyleSheet("color: #c08000;");
    m_grpcModeSwitchStatusLabel->setStyleSheet("color: #c08000;");
    m_grpcPeriodicDataStatusLabel->setStyleSheet("color: #c08000;");
    m_grpcSelfTestStatusLabel->setText(autoTriggered ? "自动自检中..." : "手动自检中...");
    m_grpcSelfTestStatusLabel->setStyleSheet("color: #c08000;");

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastGrpcStreamTimestampMs <= 2000) {
        m_grpcSelfTestStreamReceived = true;
        m_grpcSelfTestRxStatusLabel->setText("接收(流数据): 已收到");
        m_grpcSelfTestRxStatusLabel->setStyleSheet("color: green;");
        m_grpcPeriodicDataStatusLabel->setText("周期数据: 已收到最近周期包");
        m_grpcPeriodicDataStatusLabel->setStyleSheet("color: green;");
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
        m_grpcSelfTestRxStatusLabel->setText("接收(流数据): 已收到");
        m_grpcSelfTestRxStatusLabel->setStyleSheet("color: green;");

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
            m_grpcPeriodicDataStatusLabel->setText(
                QString("已接收%1条, 平均%2ms")
                    .arg(m_grpcPeriodicPacketCount)
                    .arg(avgInterval, 0, 'f', 1));
            m_grpcPeriodicDataStatusLabel->setStyleSheet("color: green;");
        } else {
            m_grpcPeriodicDataStatusLabel->setText("已接收首条周期数据");
            m_grpcPeriodicDataStatusLabel->setStyleSheet("color: #c08000;");
        }

        logGrpcInteraction(
            "stream",
            QString("frameId=%1 timestamp=%2 channelCount=%3 mode=%4")
                .arg(frameId)
                .arg(payloadTimestamp)
                .arg(channelCount)
                .arg(mode));

        if (m_grpcSelfTestPending) {
            m_grpcSelfTestStreamReceived = true;
            finalizeGrpcSelfTest();
        }
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
                m_grpcSelfTestTxStatusLabel->setText(message.isEmpty() ? "发送(Ping): 命令回包成功"
                                                                        : QString("发送(Ping): %1").arg(message));
                m_grpcSelfTestTxStatusLabel->setStyleSheet("color: green;");
                m_grpcSelfTestCommandAcked = true;
            } else if (ackStage == "mode") {
                m_grpcModeSwitchStatusLabel->setText(message.isEmpty()
                    ? QString("切换(%1): 回包成功").arg(m_grpcSelfTestTargetMode)
                    : QString("切换(%1): %2").arg(m_grpcSelfTestTargetMode, message));
                m_grpcModeSwitchStatusLabel->setStyleSheet("color: green;");
                m_grpcSelfTestModeSwitchAcked = true;
            } else {
                m_grpcSelfTestTxStatusLabel->setText(message.isEmpty() ? "发送: 命令回包成功"
                                                                        : QString("发送: %1").arg(message));
                m_grpcSelfTestTxStatusLabel->setStyleSheet("color: green;");

                if (message.contains("模式已切换", Qt::CaseInsensitive)) {
                    m_grpcModeSwitchStatusLabel->setText(QString("切换: %1").arg(message));
                    m_grpcModeSwitchStatusLabel->setStyleSheet("color: green;");
                }
            }

            if (m_grpcSelfTestPending) {
                finalizeGrpcSelfTest();
            }
        } else {
            if (ackStage == "mode") {
                m_grpcModeSwitchStatusLabel->setText(QString("切换(%1): 回包失败").arg(m_grpcSelfTestTargetMode));
                m_grpcModeSwitchStatusLabel->setStyleSheet("color: red;");
            } else {
                m_grpcSelfTestTxStatusLabel->setText("发送(Ping): 回包失败");
                m_grpcSelfTestTxStatusLabel->setStyleSheet("color: red;");
            }

            if (m_grpcSelfTestPending) {
                m_grpcSelfTestPending = false;
                m_grpcSelfTestTimeoutTimer->stop();
                m_grpcSelfTestStatusLabel->setText("收发验证失败");
                m_grpcSelfTestStatusLabel->setStyleSheet("color: red;");
            }
            updateGrpcTestUiState();
        }
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
            m_grpcSelfTestStatusLabel->setText("已连接，等待收发验证");
            m_grpcSelfTestStatusLabel->setStyleSheet("color: #c08000;");
            m_grpcModeSwitchStatusLabel->setText("等待模式切换验证");
            m_grpcModeSwitchStatusLabel->setStyleSheet("color: #c08000;");
        }

        if (status == "disconnected" && m_grpcSelfTestPending) {
            m_grpcSelfTestPending = false;
            m_grpcSelfTestTimeoutTimer->stop();
            m_grpcSelfTestStatusLabel->setText("收发验证失败（连接中断）");
            m_grpcSelfTestStatusLabel->setStyleSheet("color: red;");
            updateGrpcTestUiState();
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
    m_grpcSelfTestStatusLabel->setText("收发验证通过");
    m_grpcSelfTestStatusLabel->setStyleSheet("color: green;");
    logGrpcInteraction("self-test", "自检通过：ping/pong、模式切换、周期数据验证全部成功");
    updateGrpcTestUiState();
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
    m_dataMonitor->append(displayText);
    
    // 自动滚动到底部
    QScrollBar* scrollBar = m_dataMonitor->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

// 槽函数实现
void MainWindow::onConnectClicked()
{
    if (m_appController) {
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
            config->saveToFile("config.ini");
        }
        m_appController->reloadRuntimeConfig();
        m_appController->start();
        updateConnectionStatus(m_appController->isRunning());
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

void MainWindow::onExportClicked()
{
    if (!m_appController) {
        QMessageBox::warning(this, "导出失败", "应用控制器未初始化");
        return;
    }

    QString selectedFilter;
    const QString defaultPath = QDir::currentPath() + "/exports/cache_export_" +
                                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".h5";
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        "导出缓存数据",
        defaultPath,
        "HDF5 文件 (*.h5 *.hdf5);;CSV 文件 (*.csv)",
        &selectedFilter
    );

    if (filePath.isEmpty()) {
        return;
    }

    qint64 startTimeMs = -1;
    qint64 endTimeMs = -1;
    const auto rangeChoice = QMessageBox::question(
        this,
        "导出范围",
        "是否按时间范围导出？\n选择“否”将导出全部缓存。",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (rangeChoice == QMessageBox::Yes) {
        const QVector<FrameData> allFrames = DataCacheManager::instance()->getAllFrames();
        if (allFrames.isEmpty()) {
            QMessageBox::information(this, "提示", "当前没有可导出的缓存数据");
            return;
        }

        const qint64 minTs = allFrames.first().timestamp;
        const qint64 maxTs = allFrames.last().timestamp;

        QDialog dialog(this);
        dialog.setWindowTitle("选择时间范围");
        QFormLayout* formLayout = new QFormLayout(&dialog);

        QDateTimeEdit* startEdit = new QDateTimeEdit(QDateTime::fromMSecsSinceEpoch(minTs), &dialog);
        QDateTimeEdit* endEdit = new QDateTimeEdit(QDateTime::fromMSecsSinceEpoch(maxTs), &dialog);
        startEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss.zzz");
        endEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss.zzz");
        startEdit->setCalendarPopup(true);
        endEdit->setCalendarPopup(true);

        formLayout->addRow("开始时间", startEdit);
        formLayout->addRow("结束时间", endEdit);

        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        formLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        startTimeMs = startEdit->dateTime().toMSecsSinceEpoch();
        endTimeMs = endEdit->dateTime().toMSecsSinceEpoch();
        if (endTimeMs < startTimeMs) {
            QMessageBox::warning(this, "导出失败", "结束时间不能早于开始时间");
            return;
        }
    }

    QString format = "csv";
    if (selectedFilter.contains("HDF5", Qt::CaseInsensitive) ||
        filePath.endsWith(".h5", Qt::CaseInsensitive) ||
        filePath.endsWith(".hdf5", Qt::CaseInsensitive)) {
        format = "hdf5";
    }

    QString error;
    const bool ok = m_appController->exportCacheToFile(filePath, format, startTimeMs, endTimeMs, &error);
    if (!ok) {
        QMessageBox::warning(this, "导出失败", error);
        return;
    }

    QMessageBox::information(this, "导出成功", QString("已导出到：\n%1").arg(filePath));
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

    m_useMockDataCheck->setText(isGrpc ? "启用 gRPC 本地模拟" : "启用模拟数据");

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
    m_grpcSelfTestTxStatusLabel->setText("发送(Ping): 待验证");
    m_grpcSelfTestRxStatusLabel->setText("接收(流数据): 待验证");
    m_grpcModeSwitchStatusLabel->setText("待验证");
    m_grpcPeriodicDataStatusLabel->setText("待验证");
    m_grpcSelfTestTxStatusLabel->setStyleSheet("color: gray;");
    m_grpcSelfTestRxStatusLabel->setStyleSheet("color: gray;");
    m_grpcModeSwitchStatusLabel->setStyleSheet("color: gray;");
    m_grpcPeriodicDataStatusLabel->setStyleSheet("color: gray;");

    updateGrpcTestUiState();
}

void MainWindow::onStartGrpcTestServerClicked()
{
#ifdef QT_COMPILE_FOR_WASM
    QMessageBox::information(this, "提示", "WASM 环境不支持启动本地 Python 测试服务");
    return;
#else
    if (!m_grpcTestServerProcess ||
        m_grpcTestServerProcess->state() != QProcess::NotRunning) {
        return;
    }

    logGrpcInteraction("test-server", "请求启动本地 gRPC 测试服务");

    const QString scriptPath = resolveGrpcTestServerScriptPath();
    if (scriptPath.isEmpty()) {
        m_grpcTestServiceStatusLabel->setText("脚本未找到");
        m_grpcTestServiceStatusLabel->setStyleSheet("color: red;");
        QMessageBox::warning(this,
                             "启动失败",
                             "未找到 grpc_test_server.py，请确认程序运行目录包含该脚本");
        logGrpcInteraction("test-server", "启动失败：未找到 grpc_test_server.py");
        updateGrpcTestUiState();
        return;
    }

    const QString port = QString::number(grpcEndpointPort());
    const QString workingDir = QFileInfo(scriptPath).absolutePath();
    m_grpcTestServerProcess->setWorkingDirectory(workingDir);

    m_grpcTestServiceStatusLabel->setText("启动中...");
    m_grpcTestServiceStatusLabel->setStyleSheet("color: #c08000;");

    m_grpcTestServerProcess->start("python", QStringList() << scriptPath << "--port" << port);
    if (!m_grpcTestServerProcess->waitForStarted(1500)) {
        const QString firstError = m_grpcTestServerProcess->errorString();
        m_grpcTestServerProcess->start("py", QStringList() << "-3" << scriptPath << "--port" << port);

        if (!m_grpcTestServerProcess->waitForStarted(1500)) {
            m_grpcTestServiceStatusLabel->setText("启动失败");
            m_grpcTestServiceStatusLabel->setStyleSheet("color: red;");
            QMessageBox::warning(this,
                                 "启动失败",
                                 QString("无法启动本地 gRPC 测试服务。\npython错误: %1\npy错误: %2")
                                     .arg(firstError,
                                          m_grpcTestServerProcess->errorString()));
            logGrpcInteraction("test-server", QString("启动失败：python错误=%1, py错误=%2")
                                               .arg(firstError, m_grpcTestServerProcess->errorString()));
            updateGrpcTestUiState();
            return;
        }
    }

    updateGrpcTestUiState();
#endif
}

void MainWindow::onStopGrpcTestServerClicked()
{
#ifndef QT_COMPILE_FOR_WASM
    if (!m_grpcTestServerProcess ||
        m_grpcTestServerProcess->state() == QProcess::NotRunning) {
        return;
    }

    logGrpcInteraction("test-server", "请求停止本地 gRPC 测试服务");

    m_grpcTestServerProcess->terminate();
    if (!m_grpcTestServerProcess->waitForFinished(2000)) {
        m_grpcTestServerProcess->kill();
        m_grpcTestServerProcess->waitForFinished(1000);
    }

    updateGrpcTestUiState();
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
        m_grpcSelfTestTxStatusLabel->setText("发送(Ping): 超时未回包");
        m_grpcSelfTestTxStatusLabel->setStyleSheet("color: red;");
    }

    if (!m_grpcSelfTestModeSwitchAcked) {
        m_grpcModeSwitchStatusLabel->setText(QString("切换(%1): 超时未回包")
                                             .arg(m_grpcSelfTestTargetMode.isEmpty() ? QStringLiteral("-")
                                                                                    : m_grpcSelfTestTargetMode));
        m_grpcModeSwitchStatusLabel->setStyleSheet("color: red;");
    }

    if (!m_grpcSelfTestStreamReceived) {
        m_grpcSelfTestRxStatusLabel->setText("接收(流数据): 未收到");
        m_grpcSelfTestRxStatusLabel->setStyleSheet("color: red;");
        m_grpcPeriodicDataStatusLabel->setText("周期数据: 未收到");
        m_grpcPeriodicDataStatusLabel->setStyleSheet("color: red;");
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

    m_grpcSelfTestStatusLabel->setText(
        QString("收发验证失败（缺少%1）").arg(missingItems.join("、")));
    m_grpcSelfTestStatusLabel->setStyleSheet("color: red;");
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
    
    // 添加到历史记录
    addCommandToHistory(command);
    
    // 获取发送格式
    bool isHex = m_hexFormatCheck->isChecked();
    
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
    case 1: type = PlotWindowManager::HistoryPlot; break;
    case 2: type = PlotWindowManager::HeatmapPlot; break;
    case 3: type = PlotWindowManager::ArrayPlot; break;
    }
    
    qDebug() << "[MainWindow] creating window type" << type;
    PlotWindow* window = m_plotWindowManager->createWindowInMdiArea(m_mdiArea, type);
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
    
    PlotWindow* window = item->data(Qt::UserRole).value<PlotWindow*>();
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
    
    PlotWindow* window = item->data(Qt::UserRole).value<PlotWindow*>();
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
        m_grpcSelfTestStatusLabel->setText("收发验证失败（命令错误）");
        m_grpcSelfTestStatusLabel->setStyleSheet("color: red;");

        if (!m_grpcSelfTestCommandAcked) {
            m_grpcSelfTestTxStatusLabel->setText("发送(Ping): 命令失败");
            m_grpcSelfTestTxStatusLabel->setStyleSheet("color: red;");
        }
        if (!m_grpcSelfTestModeSwitchAcked) {
            m_grpcModeSwitchStatusLabel->setText(QString("切换(%1): 命令失败")
                                                 .arg(m_grpcSelfTestTargetMode.isEmpty() ? QStringLiteral("-")
                                                                                        : m_grpcSelfTestTargetMode));
            m_grpcModeSwitchStatusLabel->setStyleSheet("color: red;");
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
    
    m_dataMonitor->append(errorText);
    
    QScrollBar* scrollBar = m_dataMonitor->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void MainWindow::onUpdateTimer()
{
    // 更新帧率显示
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (m_lastUpdateTime > 0) {
        double elapsed = (currentTime - m_lastUpdateTime) / 1000.0;
        if (elapsed > 0) {
            // 从DataCacheManager获取帧数统计
            DataCacheManager* cache = DataCacheManager::instance();
            if (cache) {
                int currentFrameCount = cache->getCacheSize();
                int framesInPeriod = currentFrameCount - m_frameCount;
                
                double frameRate = framesInPeriod / elapsed;
                m_frameRateLabel->setText(QString("帧率: %1 fps").arg(frameRate, 0, 'f', 1));
                
                m_frameCount = currentFrameCount;
                m_dataCountLabel->setText(QString("数据: %1 帧").arg(m_frameCount));
            }
        }
    }
    
    m_lastUpdateTime = currentTime;
    
    // 更新窗口列表
    updateWindowList();
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
    
    // 尝试从多个可能的路径读取样式文件
    QStringList stylePaths;
    // 1. 资源路径
    stylePaths << stylefile;
    // 2. 相对于可执行文件的路径（build/release目录）
    stylePaths << QCoreApplication::applicationDirPath() + "/files/css/" + QFileInfo(stylefile).fileName();
    // 3. 相对于项目根目录的路径
    stylePaths << QDir::currentPath() + "/files/css/" + QFileInfo(stylefile).fileName();
    // 4. 绝对路径（如果知道项目位置）
    stylePaths << "f:/vsPro/DeviceReceiver/files/css/" + QFileInfo(stylefile).fileName();
    
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
    toolbarPaths << "f:/vsPro/DeviceReceiver/files/css/" + QFileInfo(styletoolbar).fileName();
    
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
        config->saveToFile("config.ini");
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
