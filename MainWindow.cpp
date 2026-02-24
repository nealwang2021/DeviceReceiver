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
    m_windowTypeCombo->addItems({"组合图", "温度图", "湿度图", "电压图", "历史图"});
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