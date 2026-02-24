#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDockWidget>
#include <QSerialPort>
#include <QTimer>
#include <QList>
#include <QMdiArea>
#include <QMdiSubWindow>

// 前置声明
class ApplicationController;
class PlotWindowManager;
class PlotWindow;
class QTextEdit;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QListWidget;
class QCheckBox;
class QSpinBox;

/**
 * @brief 主界面窗口类，提供设备配置、控制、指令发送和窗口管理功能
 * 
 * 采用浮动面板布局，支持：
 * 1. 设备参数配置（串口、波特率、数据位、停止位、校验位）
 * 2. 启动/停止控制
 * 3. 自定义串口指令发送（支持十六进制和ASCII）
 * 4. PlotWindow窗口管理
 * 5. 数据监控显示
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param controller 应用控制器实例
     * @param parent 父窗口指针
     */
    explicit MainWindow(ApplicationController* controller = nullptr, QWidget* parent = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~MainWindow();

    /**
     * @brief 初始化界面
     */
    void initialize();

    /**
     * @brief 更新连接状态显示
     * @param connected 连接状态
     */
    void updateConnectionStatus(bool connected);

    /**
     * @brief 获取应用控制器
     * @return ApplicationController指针
     */
    ApplicationController* appController() const { return m_appController; }

public slots:
    // 数据接收槽函数（对外可见以允许跨模块连接）
    void onDataReceived(const QByteArray& data, bool isHex = false);
    void onCommandSent(const QByteArray& command);
    void onCommandError(const QString& error);

protected:
    /**
     * @brief 关闭事件处理
     * @param event 关闭事件
     */
    void closeEvent(QCloseEvent* event) override;

private slots:
    // 设备控制槽函数
    void onConnectClicked();
    void onDisconnectClicked();
    void onUseMockDataChanged(bool use);
    
    // 指令发送槽函数
    void onSendClicked();
    void onClearClicked();
    void onCommandHistoryDoubleClicked(const QModelIndex& index);
    void onSendFormatChanged(bool isHex);
    
    // 窗口管理槽函数
    void onCreateWindowClicked();
    void onCloseWindowClicked();
    void onTileWindowsClicked();
    void onCascadeWindowsClicked();
    void onWindowListDoubleClicked(const QModelIndex& index);
    
    // 面板显示控制
    void onShowDevicePanelChanged(bool show);
    void onShowCommandPanelChanged(bool show);
    void onShowPlotPanelChanged(bool show);
    void onShowMonitorPanelChanged(bool show);
    
    // （这些槽已移至 public slots）
    
    // 定时器槽函数
    void onUpdateTimer();

private:
    /**
     * @brief 初始化界面组件
     */
    void initUI();
    
    /**
     * @brief 初始化信号槽连接
     */
    void initConnections();
    
    /**
     * @brief 加载配置到界面
     */
    void loadConfigToUI();
    
    /**
     * @brief 保存界面配置
     */
    void saveConfigFromUI();
    
    /**
     * @brief 更新串口列表
     */
    void updateSerialPortList();
    
    /**
     * @brief 更新窗口列表
     */
    void updateWindowList();
    
    /**
     * @brief 添加指令到历史记录
     * @param command 指令内容
     */
    void addCommandToHistory(const QString& command);
    
    /**
     * @brief 加载指令历史
     */
    void loadCommandHistory();
    
    /**
     * @brief 保存指令历史
     */
    void saveCommandHistory();
    
    /**
     * @brief 添加接收数据到显示区
     * @param data 数据内容
     * @param isHex 是否为十六进制格式
     * @param isReceived 是否为接收数据（true：接收，false：发送）
     */
    void addDataToMonitor(const QString& data, bool isHex = false, bool isReceived = true);

private:
    ApplicationController* m_appController; // 应用控制器
    PlotWindowManager* m_plotWindowManager; // 绘图窗口管理器
    
    // MDI区域
    QMdiArea* m_mdiArea;
    
    // 设备控制组件
    QComboBox* m_serialPortCombo;
    QComboBox* m_baudRateCombo;
    QComboBox* m_dataBitsCombo;
    QComboBox* m_stopBitsCombo;
    QComboBox* m_parityCombo;
    QComboBox* m_flowControlCombo;
    QCheckBox* m_useMockDataCheck;
    QSpinBox* m_mockIntervalSpin;
    QPushButton* m_connectButton;
    QPushButton* m_disconnectButton;
    QLabel* m_connectionStatusLabel;
    
    // 指令发送组件
    QTextEdit* m_commandInput;
    QCheckBox* m_hexFormatCheck;
    QCheckBox* m_autoNewlineCheck;
    QComboBox* m_newlineCombo;
    QPushButton* m_sendButton;
    QPushButton* m_clearButton;
    QListWidget* m_commandHistoryList;
    
    // 窗口管理组件
    QComboBox* m_windowTypeCombo;
    QPushButton* m_createWindowButton;
    QPushButton* m_closeWindowButton;
    QPushButton* m_tileWindowsButton;
    QPushButton* m_cascadeWindowsButton;
    QListWidget* m_windowList;
    
    // 数据监控组件
    QTextEdit* m_dataMonitor;
    QLabel* m_frameRateLabel;
    QLabel* m_dataCountLabel;
    QLabel* m_alarmCountLabel;
    
    // 浮动面板
    QDockWidget* m_devicePanel;
    QDockWidget* m_commandPanel;
    QDockWidget* m_plotPanel;
    QDockWidget* m_monitorPanel;
    
    // 定时器
    QTimer* m_updateTimer;
    
    // 状态变量
    bool m_isConnected;
    int m_frameCount;
    int m_alarmCount;
    qint64 m_lastUpdateTime;
};

#endif // MAINWINDOW_H
