#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
#include <QObject>
#include <QSize>
#include <QSerialPort>

/**
 * @brief 应用配置管理类，集中管理所有配置参数
 * 
 * 重构目标：将散落在各处的硬编码配置参数集中管理，支持从配置文件加载
 */
class AppConfig : public QObject
{
    Q_OBJECT
public:
    // 单例模式访问
    static AppConfig* instance();
    
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
    
    bool useMockData() const { return m_useMockData; }
    void setUseMockData(bool use) { m_useMockData = use; }
    
    int mockDataIntervalMs() const { return m_mockDataIntervalMs; }
    void setMockDataIntervalMs(int ms) { m_mockDataIntervalMs = ms; }
    
    // ========== 串口高级配置 ==========
    QSerialPort::DataBits dataBits() const { return m_dataBits; }
    void setDataBits(QSerialPort::DataBits bits) { m_dataBits = bits; }
    
    QSerialPort::StopBits stopBits() const { return m_stopBits; }
    void setStopBits(QSerialPort::StopBits bits) { m_stopBits = bits; }
    
    QSerialPort::Parity parity() const { return m_parity; }
    void setParity(QSerialPort::Parity parity) { m_parity = parity; }
    
    QSerialPort::FlowControl flowControl() const { return m_flowControl; }
    void setFlowControl(QSerialPort::FlowControl flow) { m_flowControl = flow; }
    
    // ========== 绘图配置 ==========
    int maxPlotPoints() const { return m_maxPlotPoints; }
    void setMaxPlotPoints(int points) { m_maxPlotPoints = points; }
    
    int plotRefreshIntervalMs() const { return m_plotRefreshIntervalMs; }
    void setPlotRefreshIntervalMs(int ms) { m_plotRefreshIntervalMs = ms; }
    
    // ========== 数据统计配置 ==========
    int statsIntervalMs() const { return m_statsIntervalMs; }
    void setStatsIntervalMs(int ms) { m_statsIntervalMs = ms; }
    
    // ========== 报警配置 ==========
    float temperatureAlarmThreshold() const { return m_temperatureAlarmThreshold; }
    void setTemperatureAlarmThreshold(float threshold) { m_temperatureAlarmThreshold = threshold; }
    
    // ========== 应用配置 ==========
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
    
    QByteArray mainWindowState() const { return m_mainWindowState; }
    void setMainWindowState(const QByteArray& state) { m_mainWindowState = state; }
    
    QByteArray mainWindowGeometry() const { return m_mainWindowGeometry; }
    void setMainWindowGeometry(const QByteArray& geometry) { m_mainWindowGeometry = geometry; }
    
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
    
    // ========== 文件操作 ==========
    bool loadFromFile(const QString& filename);
    bool saveToFile(const QString& filename);
    
    // 加载默认配置
    void loadDefaults();

    // 日志配置
    QString logLevel() const { return m_logLevel; }
    void setLogLevel(const QString& level) { m_logLevel = level; }
    
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
    bool m_useMockData = true;         // 是否使用模拟数据
    int m_mockDataIntervalMs = 100;    // 模拟数据间隔（毫秒）
    
    // 串口高级配置
    QSerialPort::DataBits m_dataBits = QSerialPort::Data8;
    QSerialPort::StopBits m_stopBits = QSerialPort::OneStop;
    QSerialPort::Parity m_parity = QSerialPort::NoParity;
    QSerialPort::FlowControl m_flowControl = QSerialPort::NoFlowControl;
    
    // 绘图配置
    int m_maxPlotPoints = 200;         // 最大绘图点数
    int m_plotRefreshIntervalMs = 50;  // 绘图刷新间隔（毫秒）
    
    // 数据统计配置
    int m_statsIntervalMs = 1000;      // 统计间隔（毫秒）
    
    // 报警配置
    float m_temperatureAlarmThreshold = 80.0f; // 温度报警阈值（℃）
    
    // 应用配置
    QString m_appTitle = "实时数据监控";
    QSize m_windowSize = QSize(800, 600);
    
    // UI配置
    bool m_showDevicePanel = true;
    bool m_showCommandPanel = true;
    bool m_showPlotPanel = true;
    bool m_showMonitorPanel = true;
    QByteArray m_mainWindowState;
    QByteArray m_mainWindowGeometry;
    
    // 指令历史配置
    int m_maxCommandHistory = 20;
    bool m_saveCommandHistory = true;
    QStringList m_commandHistory;
    
    // 发送配置
    bool m_sendAsHex = false;
    bool m_autoSendNewline = true;
    QString m_newlineSequence = "\r\n";

    // 日志级别: DEBUG/INFO/WARNING/ERROR
    QString m_logLevel = "INFO";
};

#endif // APPCONFIG_H