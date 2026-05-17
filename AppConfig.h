#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
#include <QObject>
#include <QSize>
#include <QDate>
#ifndef QT_COMPILE_FOR_WASM
#include <QSerialPort>
#endif

#include "AppLogger.h"

/**
 * @brief 应用配置管理类，集中管理所有配置参数
 * 
 * 重构目标：将散落在各处的硬编码配置参数集中管理，支持从配置文件加载
 */
class AppConfig : public QObject
{
    Q_OBJECT
public:
    // 样式枚举
    enum Style {
        DarkStyle,
        LightStyle
    };
    
    // 单例模式访问
    static AppConfig* instance();

    /** 与可执行文件同目录的 config.ini（避免依赖当前工作目录导致配置/布局未加载） */
    static QString defaultConfigFilePath();

    /** `<exe>/data` 根目录（不自动创建） */
    static QString dataRootPath();

    /** 确保 `<exe>/data/<yyyyMMdd>` 存在并返回其绝对路径（日志、实时库、导入预览等按日落盘） */
    static QString ensureDatedDataDirectory(const QDate& date = QDate::currentDate());

    /**
     * 在 `data` 下递归查找最新的 `device_realtime_*.db`（会话路径无效时兜底）。
     * 返回空串表示未找到。
     */
    static QString findNewestDeviceRealtimeDatabaseUnderDataRoot();

    // 禁止拷贝和赋值
    AppConfig(const AppConfig&) = delete;
    AppConfig& operator=(const AppConfig&) = delete;
    
    // ========== 数据缓存配置 ==========
    int maxCacheSize() const { return m_maxCacheSize; }
    void setMaxCacheSize(int size) { m_maxCacheSize = size; }
    
    qint64 expireTimeMs() const { return m_expireTimeMs; }
    void setExpireTimeMs(qint64 ms) { m_expireTimeMs = ms; }
    
    // ========== 串口配置 ==========
    QString serialPort() const { return m_serialPort; }
    void setSerialPort(const QString& port) { m_serialPort = port; }
    
    int baudRate() const { return m_baudRate; }
    void setBaudRate(int rate) { m_baudRate = rate; }

    QString receiverBackendType() const { return m_receiverBackendType; }
    void setReceiverBackendType(const QString& type) { m_receiverBackendType = type; }

    /** 被测设备 DeviceDataService gRPC 地址（如 [::1]:50051） */
    QString grpcEndpoint() const { return m_grpcEndpoint; }
    void setGrpcEndpoint(const QString& endpoint) { m_grpcEndpoint = endpoint; }

    /** 三轴台 StageService gRPC 地址（可与被测设备不同端口/主机） */
    QString stageGrpcEndpoint() const { return m_stageGrpcEndpoint; }
    void setStageGrpcEndpoint(const QString& endpoint) { m_stageGrpcEndpoint = endpoint; }
    
    bool useMockData() const { return m_useMockData; }
    void setUseMockData(bool use) { m_useMockData = use; }
    
    int mockDataIntervalMs() const { return m_mockDataIntervalMs; }
    void setMockDataIntervalMs(int ms) { m_mockDataIntervalMs = ms; }

    int grpcConnectTimeoutMs() const { return m_grpcConnectTimeoutMs; }
    void setGrpcConnectTimeoutMs(int ms) { m_grpcConnectTimeoutMs = ms; }
    
#ifndef QT_COMPILE_FOR_WASM
    // ========== 串口高级配置（仅在非WASM环境下可用）==========
    QSerialPort::DataBits dataBits() const { return m_dataBits; }
    void setDataBits(QSerialPort::DataBits bits) { m_dataBits = bits; }
    
    QSerialPort::StopBits stopBits() const { return m_stopBits; }
    void setStopBits(QSerialPort::StopBits bits) { m_stopBits = bits; }
    
    QSerialPort::Parity parity() const { return m_parity; }
    void setParity(QSerialPort::Parity parity) { m_parity = parity; }
    
    QSerialPort::FlowControl flowControl() const { return m_flowControl; }
    void setFlowControl(QSerialPort::FlowControl flow) { m_flowControl = flow; }
#endif
    
    // ========== 绘图配置 ==========
    int maxPlotPoints() const { return m_maxPlotPoints; }
    void setMaxPlotPoints(int points) { m_maxPlotPoints = points; }
    
    int plotRefreshIntervalMs() const { return m_plotRefreshIntervalMs; }
    void setPlotRefreshIntervalMs(int ms) { m_plotRefreshIntervalMs = ms; }

    int arrayPlotRowHeightPx() const { return m_arrayPlotRowHeightPx; }
    void setArrayPlotRowHeightPx(int px) { m_arrayPlotRowHeightPx = qBound(0, px, 300); }

    /**
     * QCustomPlot OpenGL 加速开关。
     * - 默认 true，保持历史行为；用户可通过菜单关闭以规避特定显卡/驱动问题。
     * - 该值仅在编译时定义了 QCUSTOMPLOT_USE_OPENGL 才真正生效；
     *   未编译进 OpenGL 时 QCustomPlot 内部会忽略 setOpenGl 的 enable 路径。
     * - setter 仅在值变化时发出 qcustomPlotOpenGlEnabledChanged 信号，
     *   PlotWindowManager 据此把 setOpenGl 广播给所有现存 plot 控件。
     */
    bool qcustomPlotOpenGlEnabled() const { return m_qcustomPlotOpenGlEnabled; }
    void setQcustomPlotOpenGlEnabled(bool enabled);

    double arrayRgbHeatmapAmpMin() const { return m_arrayRgbHeatmapAmpMin; }
    void setArrayRgbHeatmapAmpMin(double v);

    double arrayRgbHeatmapAmpMax() const { return m_arrayRgbHeatmapAmpMax; }
    void setArrayRgbHeatmapAmpMax(double v);

    // ========== 检测分析窗口配置 ==========
    int inspectionChannelsPerGroup() const { return m_inspectionChannelsPerGroup; }
    void setInspectionChannelsPerGroup(int n) { m_inspectionChannelsPerGroup = qBound(1, n, 256); }
    
    // ========== 数据统计配置 ==========
    int statsIntervalMs() const { return m_statsIntervalMs; }
    void setStatsIntervalMs(int ms) { m_statsIntervalMs = ms; }
    
    // ========== 报警配置 ==========
    float temperatureAlarmThreshold() const { return m_temperatureAlarmThreshold; }
    void setTemperatureAlarmThreshold(float threshold) { m_temperatureAlarmThreshold = threshold; }
    
    // ========== 应用配置 ==========
    /** 主窗口标题，对应 config.ini [General] AppTitle（默认「测试软件」） */
    QString appTitle() const { return m_appTitle; }
    void setAppTitle(const QString& title) { m_appTitle = title; }
    
    QSize windowSize() const { return m_windowSize; }
    void setWindowSize(const QSize& size) { m_windowSize = size; }
    
    // ========== UI配置 ==========
    bool showDevicePanel() const { return m_showDevicePanel; }
    void setShowDevicePanel(bool show) { m_showDevicePanel = show; }
    
    bool showCommandPanel() const { return m_showCommandPanel; }
    void setShowCommandPanel(bool show) { m_showCommandPanel = show; }
    
    bool showPlotPanel() const { return m_showPlotPanel; }
    void setShowPlotPanel(bool show) { m_showPlotPanel = show; }
    
    bool showMonitorPanel() const { return m_showMonitorPanel; }
    void setShowMonitorPanel(bool show) { m_showMonitorPanel = show; }

    bool showStagePanel() const { return m_showStagePanel; }
    void setShowStagePanel(bool show) { m_showStagePanel = show; }

    bool showOverviewPanel() const { return m_showOverviewPanel; }
    void setShowOverviewPanel(bool show) { m_showOverviewPanel = show; }
    
    QByteArray mainWindowState() const { return m_mainWindowState; }
    void setMainWindowState(const QByteArray& state) { m_mainWindowState = state; }
    
    QByteArray mainWindowGeometry() const { return m_mainWindowGeometry; }
    void setMainWindowGeometry(const QByteArray& geometry) { m_mainWindowGeometry = geometry; }

    QStringList savedPlotWindowTypes() const { return m_savedPlotWindowTypes; }
    void setSavedPlotWindowTypes(const QStringList& types) { m_savedPlotWindowTypes = types; }
    
    // ========== 指令历史配置 ==========
    int maxCommandHistory() const { return m_maxCommandHistory; }
    void setMaxCommandHistory(int max) { m_maxCommandHistory = max; }
    
    bool saveCommandHistory() const { return m_saveCommandHistory; }
    void setSaveCommandHistory(bool save) { m_saveCommandHistory = save; }
    
    QStringList commandHistory() const { return m_commandHistory; }
    void setCommandHistory(const QStringList& history) { m_commandHistory = history; }
    
    // ========== 发送配置 ==========
    bool sendAsHex() const { return m_sendAsHex; }
    void setSendAsHex(bool hex) { m_sendAsHex = hex; }
    
    bool autoSendNewline() const { return m_autoSendNewline; }
    void setAutoSendNewline(bool autoSend) { m_autoSendNewline = autoSend; }
    
    QString newlineSequence() const { return m_newlineSequence; }
    void setNewlineSequence(const QString& sequence) { m_newlineSequence = sequence; }

    // ========== 导出配置 ==========
    QString defaultExportDirectory() const { return m_defaultExportDirectory; }
    void setDefaultExportDirectory(const QString& dir) { m_defaultExportDirectory = dir; }

    QString defaultExportFormat() const { return m_defaultExportFormat; }
    void setDefaultExportFormat(const QString& format) { m_defaultExportFormat = format; }
    
    // ========== 样式配置 ==========
    Style currentStyle() const { return m_currentStyle; }
    void setCurrentStyle(Style style) { m_currentStyle = style; }
    
    // ========== 文件操作 ==========
    bool loadFromFile(const QString& filename);
    bool saveToFile(const QString& filename);
    
    // 加载默认配置
    void loadDefaults();

    // 日志配置
    QString logLevel() const { return m_logLevel; }
    void setLogLevel(const QString& level) { m_logLevel = level; }

    AppLogLevel monitorLogMinimumLevel() const { return m_monitorLogMinimumLevel; }
    void setMonitorLogMinimumLevel(AppLogLevel level) { m_monitorLogMinimumLevel = level; }

signals:
    /// 仅在 setQcustomPlotOpenGlEnabled 改变值时触发。
    void qcustomPlotOpenGlEnabledChanged(bool enabled);
    /// 仅在 arrayRgbHeatmapAmpMin 或 arrayRgbHeatmapAmpMax 改变值时触发。
    void arrayRgbHeatmapAmpRangeChanged();

private:
    explicit AppConfig(QObject *parent = nullptr);
    ~AppConfig() override = default;
    
    static AppConfig* m_instance;
    
    // 数据缓存配置
    int m_maxCacheSize = 600;          // 最大缓存帧数
    qint64 m_expireTimeMs = 60000;     // 数据过期时间（毫秒）
    
    // 串口配置
    QString m_serialPort = "COM3";     // 串口端口
    int m_baudRate = 115200;           // 波特率
    QString m_receiverBackendType = "grpc";
    QString m_grpcEndpoint;            // 构造时填默认本机 IPv6 + 端口
    QString m_stageGrpcEndpoint;
    bool m_useMockData = false;        // 是否使用模拟数据
    int m_mockDataIntervalMs = 100;    // 模拟数据间隔；gRPC 真机时为 Subscribe 请求的 interval_ms（毫秒）
    int m_grpcConnectTimeoutMs = 6000; // gRPC 单次连接等待超时（毫秒）
    
#ifndef QT_COMPILE_FOR_WASM
    // 串口高级配置（仅在非WASM环境下可用）
    QSerialPort::DataBits m_dataBits = QSerialPort::Data8;
    QSerialPort::StopBits m_stopBits = QSerialPort::OneStop;
    QSerialPort::Parity m_parity = QSerialPort::NoParity;
    QSerialPort::FlowControl m_flowControl = QSerialPort::NoFlowControl;
#endif
    
    // 绘图配置
    int m_maxPlotPoints = 2000;        // 最大绘图点数
    int m_plotRefreshIntervalMs = 50;  // 绘图刷新间隔（毫秒）
    int m_arrayPlotRowHeightPx = 0;    // 阵列图每通道高度（像素，0=使用密度默认值）
    bool m_qcustomPlotOpenGlEnabled = true; // QCustomPlot OpenGL 加速开关
    double m_arrayRgbHeatmapAmpMin = 0.05;
    double m_arrayRgbHeatmapAmpMax = 0.3;
    int m_inspectionChannelsPerGroup = 8; // 检测分析窗口每组通道数
    
    // 数据统计配置
    int m_statsIntervalMs = 1000;      // 统计间隔（毫秒）
    
    // 报警配置
    float m_temperatureAlarmThreshold = 80.0f; // 温度报警阈值（℃）
    
    // 应用配置
    QString m_appTitle = QStringLiteral("测试软件");
    QSize m_windowSize = QSize(800, 600);
    
    // UI配置
    bool m_showDevicePanel = true;
    bool m_showCommandPanel = true;
    bool m_showPlotPanel = true;
    bool m_showMonitorPanel = true;
    bool m_showStagePanel = true;
    bool m_showOverviewPanel = true;
    QByteArray m_mainWindowState;
    QByteArray m_mainWindowGeometry;
    QStringList m_savedPlotWindowTypes;
    
    // 指令历史配置
    int m_maxCommandHistory = 20;
    bool m_saveCommandHistory = true;
    QStringList m_commandHistory;
    
    // 发送配置
    bool m_sendAsHex = false;
    bool m_autoSendNewline = true;
    QString m_newlineSequence = "\r\n";

    // 导出配置
    QString m_defaultExportDirectory = "exports";
    QString m_defaultExportFormat = "hdf5";

    // 日志级别: DEBUG/INFO/WARNING/ERROR
    QString m_logLevel = "INFO";
    AppLogLevel m_monitorLogMinimumLevel = AppLogLevel::Info;
    
    // 样式配置
    Style m_currentStyle = LightStyle;
};

#endif // APPCONFIG_H