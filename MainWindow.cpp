#include "MainWindow.h"
#include "ApplicationController.h"
#include "PlotWindowManager.h"
#include "PlotWindow.h"
#include "AppConfig.h"
#include "SerialReceiver.h"
#include "DataCacheManager.h"

#include <QSerialPortInfo>
#include <QCloseEvent>
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
#include <QTimer>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QListWidget>
#include <QRegularExpression>
#include <cmath>

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
{
    // 初始化定时器
    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(1000); // 1秒更新一次
    connect(m_updateTimer, &QTimer::timeout, this, &MainWindow::onUpdateTimer);
}

MainWindow::~MainWindow()
{
    // 保存配置
    saveConfigFromUI();
    saveCommandHistory();
}

void MainWindow::initialize()
{
    // 获取PlotWindowManager
    if (m_appController) {
        m_plotWindowManager = m_appController->plotWindowManager();
    }
    
    // 初始化界面组件
    initUI();
    
    // 初始化信号槽连接
    initConnections();
    
    // 加载配置到界面
    loadConfigToUI();
    
    // 加载指令历史
    loadCommandHistory();
    
    // 更新串口列表
    updateSerialPortList();
    
    // 更新窗口列表
    updateWindowList();
    
    // 启动更新定时器
    m_updateTimer->start();
    
    // 设置窗口标题
    AppConfig* config = AppConfig::instance();
    if (config) {
        setWindowTitle(config->appTitle());
        
        // 应用保存的样式
        setStyle(config->currentStyle());
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
    } else {
        m_connectionStatusLabel->setText("未连接");
        m_connectionStatusLabel->setStyleSheet("color: red;");
        m_connectButton->setEnabled(true);
        m_disconnectButton->setEnabled(false);
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
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
    // 创建菜单栏
    QMenuBar* menuBar = new QMenuBar(this);
    setMenuBar(menuBar);
    
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
    
    // 串口配置组
    QGroupBox* serialGroup = new QGroupBox("串口配置");
    QFormLayout* serialLayout = new QFormLayout(serialGroup);
    
    m_serialPortCombo = new QComboBox();
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
    
    serialLayout->addRow("端口:", m_serialPortCombo);
    serialLayout->addRow("波特率:", m_baudRateCombo);
    serialLayout->addRow("数据位:", m_dataBitsCombo);
    serialLayout->addRow("停止位:", m_stopBitsCombo);
    serialLayout->addRow("校验位:", m_parityCombo);
    serialLayout->addRow("流控制:", m_flowControlCombo);
    
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
    m_disconnectButton->setEnabled(false);
    m_connectionStatusLabel = new QLabel("未连接");
    m_connectionStatusLabel->setStyleSheet("color: red;");
    
    controlLayout->addWidget(m_connectButton);
    controlLayout->addWidget(m_disconnectButton);
    controlLayout->addStretch();
    controlLayout->addWidget(m_connectionStatusLabel);
    
    deviceLayout->addWidget(serialGroup);
    deviceLayout->addWidget(mockGroup);
    deviceLayout->addWidget(controlGroup);
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
    m_windowTypeCombo->addItems({"组合图", "温度图", "湿度图", "电压图", "历史图", "热力图", "阵列图"});
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
    
    m_monitorPanel->setWidget(monitorWidget);
    addDockWidget(Qt::BottomDockWidgetArea, m_monitorPanel);
    
    // 创建状态栏
    QStatusBar* statusBar = new QStatusBar(this);
    setStatusBar(statusBar);
    
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
}

void MainWindow::initConnections()
{
    // 设备控制信号槽
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(m_useMockDataCheck, &QCheckBox::toggled, this, &MainWindow::onUseMockDataChanged);
    
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
    
    // 其他信号槽将在后续连接到SerialReceiver
}

void MainWindow::loadConfigToUI()
{
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }
    
    // 加载串口配置
    m_serialPortCombo->setCurrentText(config->serialPort());
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
}

void MainWindow::saveConfigFromUI()
{
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }
    
    // 保存串口配置
    config->setSerialPort(m_serialPortCombo->currentText());
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

void MainWindow::addDataToMonitor(const QString& data, bool isHex, bool isReceived)
{
    if (data.isEmpty()) {
        return;
    }
    
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString direction = isReceived ? "[接收]" : "[发送]";
    QString format = isHex ? "HEX" : "TXT";
    
    QString displayText = QString("%1 %2 %3: %4")
                             .arg(timestamp)
                             .arg(direction)
                             .arg(format)
                             .arg(data);
    
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
        m_appController->start();
        updateConnectionStatus(m_appController->isRunning());
    }
}

void MainWindow::onDisconnectClicked()
{
    if (m_appController) {
        m_appController->stop();
        updateConnectionStatus(false);
    }
}

void MainWindow::onUseMockDataChanged(bool use)
{
    Q_UNUSED(use)
    // 这个功能将在ApplicationController中处理
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
    if (!m_plotWindowManager || !m_mdiArea) {
        return;
    }
    
    PlotWindowManager::PlotType type = PlotWindowManager::CombinedPlot;
    int typeIndex = m_windowTypeCombo->currentIndex();
    
    switch (typeIndex) {
    case 0: type = PlotWindowManager::CombinedPlot; break;
    case 1: type = PlotWindowManager::TemperaturePlot; break;
    case 2: type = PlotWindowManager::HumidityPlot; break;
    case 3: type = PlotWindowManager::VoltagePlot; break;
    case 4: type = PlotWindowManager::HistoryPlot; break;
    case 5: type = PlotWindowManager::HeatmapPlot; break;
    case 6: type = PlotWindowManager::ArrayPlot; break;
    }
    
    PlotWindow* window = m_plotWindowManager->createWindowInMdiArea(m_mdiArea, type);
    if (window) {
        updateWindowList();
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
}

void MainWindow::onCommandError(const QString& error)
{
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
