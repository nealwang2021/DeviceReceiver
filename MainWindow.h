#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDockWidget>
#ifndef QT_COMPILE_FOR_WASM
#include <QSerialPort>
#endif
#include <QTimer>
#include <QList>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QFile>
#include <QDateTime>
#include "AppConfig.h"

// 前置声明
class ApplicationController;
class PlotWindowManager;
class PlotWindowBase;
class QTextEdit;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QListWidget;
class QCheckBox;
class QSpinBox;
class QLineEdit;
class QProcess;
class QJsonObject;
class QDoubleSpinBox;
class QGroupBox;
class QShowEvent;

/**
 * @brief 主界面窗口类，提供设备配置、控制、指令发送和窗口管理功能
 * 
 * 采用浮动面板布局，支持：
 * 1. 被测设备参数（串口/gRPC 等，与三轴台测试装置分离）
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
    /// 三轴台独立后端（与 onDataReceived 分离）
    void onStageDataReceived(const QByteArray& data, bool isHex = false);
    void onStageCommandSent(const QByteArray& command);
    void onStageCommandError(const QString& error);
    void onStageConnectionStateChanged(bool connected);
    void onRecorderDropAlert(const QString& message);

protected:
    /**
     * @brief 关闭事件处理
     * @param event 关闭事件
     */
    void closeEvent(QCloseEvent* event) override;

    void showEvent(QShowEvent* event) override;

private slots:
    // 设备控制槽函数
    void onConnectClicked();
    void onDisconnectClicked();
    void onPauseClicked();
    void onResumeClicked();
    void onExportClicked();
    void onUseMockDataChanged(bool use);
    void onBackendTypeChanged(int index);
    
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
    void onShowStagePanelChanged(bool show);
    
    // （这些槽已移至 public slots）
    
    // 定时器槽函数
    void onUpdateTimer();
    void onPlotManagerTelemetryUpdated(int configuredIntervalMs,
                                       int actualIntervalMs,
                                       int fetchCount,
                                       qint64 receivedFramesSinceLast,
                                       int dispatchedFrames,
                                       qint64 unconsumedFramesTotal);
    void onStartGrpcTestServerClicked();
    void onStopGrpcTestServerClicked();
    void onRunGrpcSelfTestClicked();
    void onGrpcSelfTestTimeout();
    void onGrpcTestServerStartTimeout();

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
    void appendMonitorLog(const QString& text, const QString& color = QString());

    /**
     * @brief 切换样式（深色/浅色）
     */
    void switchStyle();

    /**
     * @brief 设置指定样式
     * @param style 要设置的样式
     */
    void setStyle(AppConfig::Style style);

    enum class GrpcLabelTone {
        Neutral,
        Warning,
        Success,
        Error,
    };

    struct GrpcLabelState {
        QString text;
        GrpcLabelTone tone = GrpcLabelTone::Neutral;
    };

    void updateGrpcTestUiState();
    void resetGrpcSelfTestLabelStates();
    void setGrpcLabelState(GrpcLabelState& target, const QString& text, GrpcLabelTone tone);
    void applyGrpcLabelState(QLabel* label, const GrpcLabelState& state) const;
    QString grpcLabelToneColor(GrpcLabelTone tone) const;
    void applyGrpcSelfTestLabelStates();
    void startGrpcSelfTest(bool autoTriggered);
    void handleGrpcBackendPacket(const QJsonObject& packet);
    void handleStageBackendPacket(const QJsonObject& packet);
    void finalizeGrpcSelfTest();
    void updateStagePanelUiState();
    void sendStageCommandText(const QString& command);
    /// 主窗口限制在当前屏幕工作区内（横向/竖向不超出可用区域），并在换屏时更新
    void applyScreenGeometryConstraints();
    void setGrpcTestServiceStatus(const QString& text, const QString& color);
    QString resolveGrpcTestServerExecutablePath() const;
    QString resolveGrpcTestServerScriptPath() const;
    QString resolveGrpcTestServerPythonExecutablePath(const QString& scriptPath) const;
#ifndef QT_COMPILE_FOR_WASM
    struct GrpcTestServerLaunchCandidate {
        QString program;
        QStringList arguments;
        QString displayName;
        QString workingDirectory;
    };
    QList<GrpcTestServerLaunchCandidate> buildGrpcTestServerLaunchCandidates(const QString& port) const;
    void tryStartNextGrpcTestServerCandidate();
#endif
    int grpcEndpointPort() const;
    void logGrpcInteraction(const QString& category, const QString& detail) const;

    /**
     * @brief 获取默认样式
     * @param style 样式类型
     * @return 默认样式字符串
     */
    QString getDefaultStyle(AppConfig::Style style);

    /**
     * @brief 获取默认工具栏样式
     * @param style 样式类型
     * @return 默认工具栏样式字符串
     */
    QString getDefaultToolbarStyle(AppConfig::Style style);

private:
    ApplicationController* m_appController; // 应用控制器
    PlotWindowManager* m_plotWindowManager; // 绘图窗口管理器
    
    // 样式相关
    AppConfig::Style m_currentStyle; // 当前样式
    
    // MDI区域
    QMdiArea* m_mdiArea;
    
    // 设备控制组件
    QList<QWidget*> m_serialOnlyFields;
    QList<QWidget*> m_serialOnlyLabels;
    QComboBox* m_serialPortCombo;
    QComboBox* m_backendTypeCombo;
    QLineEdit* m_grpcEndpointEdit;
    QComboBox* m_baudRateCombo;
    QComboBox* m_dataBitsCombo;
    QComboBox* m_stopBitsCombo;
    QComboBox* m_parityCombo;
    QComboBox* m_flowControlCombo;
    QCheckBox* m_useMockDataCheck;
    QSpinBox* m_mockIntervalSpin;
    QPushButton* m_connectButton;
    QPushButton* m_disconnectButton;
    QPushButton* m_pauseButton;
    QPushButton* m_resumeButton;
    QPushButton* m_exportButton;
    QPushButton* m_startGrpcTestServerButton;
    QPushButton* m_stopGrpcTestServerButton;
    QPushButton* m_runGrpcSelfTestButton;
    QComboBox* m_grpcModeCombo;
    QLabel* m_connectionStatusLabel;
    QLabel* m_grpcTestServiceStatusLabel;
    QLabel* m_grpcSelfTestStatusLabel;
    QLabel* m_grpcSelfTestTxStatusLabel;
    QLabel* m_grpcSelfTestRxStatusLabel;
    QLabel* m_grpcModeSwitchStatusLabel;
    QLabel* m_grpcPeriodicDataStatusLabel;

    // Stage 专用控制组件
    QLineEdit* m_stageEndpointEdit = nullptr;
    QPushButton* m_stageApplyEndpointButton = nullptr;
    QPushButton* m_stageConnectButton = nullptr;
    QPushButton* m_stageDisconnectButton = nullptr;
    QLabel* m_stageBackendStatusLabel = nullptr;
    QLabel* m_stagePositionLabel = nullptr;
    QLabel* m_stageScanStatusLabel = nullptr;
    QLabel* m_stageCommandResultLabel = nullptr;
    QSpinBox* m_stageStreamIntervalSpin = nullptr;
    QPushButton* m_stageGetPositionButton = nullptr;
    QPushButton* m_stageStartStreamButton = nullptr;
    QPushButton* m_stageStopStreamButton = nullptr;
    QComboBox* m_stageJogAxisCombo = nullptr;
    QComboBox* m_stageJogDirectionCombo = nullptr;
    QPushButton* m_stageJogStartButton = nullptr;
    QPushButton* m_stageJogStopButton = nullptr;
    QDoubleSpinBox* m_stageMoveAbsXSpin = nullptr;
    QDoubleSpinBox* m_stageMoveAbsYSpin = nullptr;
    QDoubleSpinBox* m_stageMoveAbsZSpin = nullptr;
    QSpinBox* m_stageMoveTimeoutSpin = nullptr;
    QPushButton* m_stageMoveAbsButton = nullptr;
    QComboBox* m_stageMoveRelAxisCombo = nullptr;
    QDoubleSpinBox* m_stageMoveRelDeltaSpin = nullptr;
    QPushButton* m_stageMoveRelButton = nullptr;
    QSpinBox* m_stageSpeedSpin = nullptr;
    QSpinBox* m_stageAccelSpin = nullptr;
    QPushButton* m_stageSetSpeedButton = nullptr;
    QComboBox* m_stageScanModeCombo = nullptr;
    QDoubleSpinBox* m_stageScanXsSpin = nullptr;
    QDoubleSpinBox* m_stageScanXeSpin = nullptr;
    QDoubleSpinBox* m_stageScanYsSpin = nullptr;
    QDoubleSpinBox* m_stageScanYeSpin = nullptr;
    QDoubleSpinBox* m_stageScanStepSpin = nullptr;
    QDoubleSpinBox* m_stageScanZFixSpin = nullptr;
    QPushButton* m_stageStartScanButton = nullptr;
    QPushButton* m_stageStopScanButton = nullptr;
    QPushButton* m_stageScanStatusButton = nullptr;
    QLineEdit* m_stageCustomCommandEdit = nullptr;
    QPushButton* m_stageCustomCommandSendButton = nullptr;
    QPushButton* m_stageCommandHelpButton = nullptr;

    /// Stage 后端下曾挂起 HEX 勾选状态，切回其它后端时恢复
    
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
    QLabel* m_uiRefreshLabel;
    QLabel* m_dataCountLabel;
    QLabel* m_alarmCountLabel;
    
    // 浮动面板
    QDockWidget* m_devicePanel;
    QDockWidget* m_stagePanel = nullptr;
    QDockWidget* m_commandPanel;
    QDockWidget* m_plotPanel;
    QDockWidget* m_monitorPanel;

    /// 视图菜单「三轴台测试装置」项，用于与停靠栏关闭按钮同步勾选状态
    QAction* m_showStagePanelAction = nullptr;

    QGroupBox* m_grpcTestGroup = nullptr;
    
    // 定时器
    QTimer* m_updateTimer;
    QTimer* m_grpcSelfTestTimeoutTimer;
    
    // 状态变量
    bool m_isConnected;
    bool m_screenGeometryScreenHooked = false;
    int m_frameCount;
    int m_alarmCount;
    qint64 m_lastUpdateTime;
    qint64 m_lastMonitorAppendTime;
    int m_monitorAppendIntervalMs;
    qint64 m_lastGrpcStreamTimestampMs;
    bool m_grpcSelfTestPending;
    bool m_grpcSelfTestCommandAcked;
    bool m_grpcSelfTestModeSwitchAcked;
    bool m_grpcSelfTestStreamReceived;
    bool m_autoSelfTestTriggeredForConnection;
    qint64 m_grpcPeriodicPacketCount;
    qint64 m_grpcPeriodicIntervalSumMs;
    qint64 m_lastGrpcStreamPayloadTimestampMs;
    QStringList m_grpcSelfTestPendingAcks;
    QString m_grpcSelfTestTargetMode;
    GrpcLabelState m_grpcSelfTestOverallState;
    GrpcLabelState m_grpcSelfTestTxState;
    GrpcLabelState m_grpcSelfTestRxState;
    GrpcLabelState m_grpcModeSwitchState;
    GrpcLabelState m_grpcPeriodicDataState;

#ifndef QT_COMPILE_FOR_WASM
    QProcess* m_grpcTestServerProcess;
    QTimer* m_grpcTestServerStartTimeoutTimer;
    QList<GrpcTestServerLaunchCandidate> m_grpcTestServerLaunchQueue;
    QStringList m_grpcTestServerStartErrors;
    QString m_grpcTestServerCurrentDisplayName;
    bool m_grpcTestServerLaunchInProgress;
    bool m_grpcTestServerStopRequested;
#endif
};

#endif // MAINWINDOW_H
