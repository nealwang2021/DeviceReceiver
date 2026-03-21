#include "AppConfig.h"
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <QApplication>
#ifndef QT_COMPILE_FOR_WASM
#include <QSerialPort>
#endif

AppConfig* AppConfig::m_instance = nullptr;

AppConfig::AppConfig(QObject *parent)
    : QObject(parent)
{
    // 构造函数留空，配置已在成员变量中初始化
}

AppConfig* AppConfig::instance()
{
    if (!m_instance) {
        m_instance = new AppConfig(qApp);
    }
    return m_instance;
}

QString AppConfig::defaultConfigFilePath()
{
    if (QCoreApplication::instance()) {
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.ini"));
    }
    return QStringLiteral("config.ini");
}

bool AppConfig::loadFromFile(const QString& filename)
{
    const QFileInfo fi(filename);
    if (!fi.exists() || !fi.isFile()) {
        qWarning() << "配置文件不存在：" << filename;
        return false;
    }

    QSettings settings(filename, QSettings::IniFormat);

    // 兼容 [General] 与 Qt/环境下可能出现的 [%General]（键名分别为 General/* 与 %General/*）
    auto valueGeneral = [&settings](const QString& key, const QVariant& defaultValue) -> QVariant {
        const QString kStd = QStringLiteral("General/") + key;
        const QString kPct = QStringLiteral("%General/") + key;
        if (settings.contains(kStd)) {
            return settings.value(kStd);
        }
        if (settings.contains(kPct)) {
            return settings.value(kPct);
        }
        return defaultValue;
    };

    // 加载应用配置（不再因缺少 AppTitle 整文件失败，否则 UI/MainWindowState 等永远不会被读入）
    m_appTitle = valueGeneral(QStringLiteral("AppTitle"), m_appTitle).toString();
    if (m_appTitle.isEmpty()) {
        m_appTitle = QStringLiteral("实时数据监控");
    }
    
    // 加载窗口配置
    m_windowSize = settings.value("Window/Size", m_windowSize).toSize();
    
    // 加载缓存配置
    m_maxCacheSize = settings.value("Cache/MaxSize", m_maxCacheSize).toInt();
    m_expireTimeMs = settings.value("Cache/ExpireTimeMs", m_expireTimeMs).toLongLong();
    
    // 加载串口配置
    m_serialPort = settings.value("Serial/Port", m_serialPort).toString();
    m_baudRate = settings.value("Serial/BaudRate", m_baudRate).toInt();
    m_receiverBackendType = settings.value("Receiver/BackendType", m_receiverBackendType).toString();
    // 三轴台不再作为被测设备采集源；旧配置迁移为 grpc
    if (m_receiverBackendType.compare(QStringLiteral("stage"), Qt::CaseInsensitive) == 0) {
        m_receiverBackendType = QStringLiteral("grpc");
    }
    m_grpcEndpoint = settings.value("Receiver/GrpcEndpoint", m_grpcEndpoint).toString();
    // 优先读 [Receiver] 下的配置，兼容历史版本中存放在 [Serial] 下的写法
    if (settings.contains("Receiver/UseMockData")) {
        m_useMockData = settings.value("Receiver/UseMockData").toBool();
    } else {
        m_useMockData = settings.value("Serial/UseMockData", m_useMockData).toBool();
    }
    if (settings.contains("Receiver/MockDataIntervalMs")) {
        m_mockDataIntervalMs = settings.value("Receiver/MockDataIntervalMs").toInt();
    } else {
        m_mockDataIntervalMs = settings.value("Serial/MockDataIntervalMs", m_mockDataIntervalMs).toInt();
    }
    
    // 加载绘图配置
    m_maxPlotPoints = settings.value("Plot/MaxPoints", m_maxPlotPoints).toInt();
    m_plotRefreshIntervalMs = settings.value("Plot/RefreshIntervalMs", m_plotRefreshIntervalMs).toInt();
    
    // 加载统计配置
    m_statsIntervalMs = settings.value("Stats/IntervalMs", m_statsIntervalMs).toInt();
    
    // 加载报警配置
    m_temperatureAlarmThreshold = settings.value("Alarm/TemperatureThreshold", m_temperatureAlarmThreshold).toFloat();
    
    // 加载样式配置
    int styleValue = settings.value("Style/CurrentStyle", static_cast<int>(m_currentStyle)).toInt();
    m_currentStyle = (styleValue == 0) ? DarkStyle : LightStyle;

    // 加载UI配置
    m_showDevicePanel = settings.value("UI/ShowDevicePanel", m_showDevicePanel).toBool();
    m_showCommandPanel = settings.value("UI/ShowCommandPanel", m_showCommandPanel).toBool();
    m_showPlotPanel = settings.value("UI/ShowPlotPanel", m_showPlotPanel).toBool();
    m_showMonitorPanel = settings.value("UI/ShowMonitorPanel", m_showMonitorPanel).toBool();
    m_showStagePanel = settings.value("UI/ShowStagePanel", m_showStagePanel).toBool();
    m_mainWindowState = settings.value("UI/MainWindowState", m_mainWindowState).toByteArray();
    m_mainWindowGeometry = settings.value("UI/MainWindowGeometry", m_mainWindowGeometry).toByteArray();

    // 导出配置
    m_defaultExportDirectory = settings.value("Export/Directory", m_defaultExportDirectory).toString();
    m_defaultExportFormat = settings.value("Export/Format", m_defaultExportFormat).toString();
    
    // 日志配置
    m_logLevel = settings.value("Log/Level", m_logLevel).toString();
    
    qInfo() << "配置文件加载成功：" << filename;
    return true;
}

bool AppConfig::saveToFile(const QString& filename)
{
    QSettings settings(filename, QSettings::IniFormat);
    
    // 保存应用配置
    settings.setValue("General/AppTitle", m_appTitle);
    
    // 保存窗口配置
    settings.setValue("Window/Size", m_windowSize);
    
    // 保存缓存配置
    settings.setValue("Cache/MaxSize", m_maxCacheSize);
    settings.setValue("Cache/ExpireTimeMs", m_expireTimeMs);
    
    // 保存串口配置
    settings.setValue("Serial/Port", m_serialPort);
    settings.setValue("Serial/BaudRate", m_baudRate);
    settings.setValue("Receiver/BackendType", m_receiverBackendType);
    settings.setValue("Receiver/GrpcEndpoint", m_grpcEndpoint);
    settings.setValue("Receiver/UseMockData", m_useMockData);
    settings.setValue("Receiver/MockDataIntervalMs", m_mockDataIntervalMs);
    
    // 保存绘图配置
    settings.setValue("Plot/MaxPoints", m_maxPlotPoints);
    settings.setValue("Plot/RefreshIntervalMs", m_plotRefreshIntervalMs);
    
    // 保存统计配置
    settings.setValue("Stats/IntervalMs", m_statsIntervalMs);
    
    // 保存报警配置
    settings.setValue("Alarm/TemperatureThreshold", m_temperatureAlarmThreshold);
    
    // 保存样式配置
    settings.setValue("Style/CurrentStyle", static_cast<int>(m_currentStyle));

    // 保存UI配置
    settings.setValue("UI/ShowDevicePanel", m_showDevicePanel);
    settings.setValue("UI/ShowCommandPanel", m_showCommandPanel);
    settings.setValue("UI/ShowPlotPanel", m_showPlotPanel);
    settings.setValue("UI/ShowMonitorPanel", m_showMonitorPanel);
    settings.setValue("UI/ShowStagePanel", m_showStagePanel);
    settings.setValue("UI/MainWindowState", m_mainWindowState);
    settings.setValue("UI/MainWindowGeometry", m_mainWindowGeometry);

    // 保存导出配置
    settings.setValue("Export/Directory", m_defaultExportDirectory);
    settings.setValue("Export/Format", m_defaultExportFormat);
    
    // 保存日志配置
    settings.setValue("Log/Level", m_logLevel);
    
    settings.sync();
    
    if (settings.status() == QSettings::NoError) {
        qInfo() << "配置文件保存成功：" << filename;
        return true;
    } else {
        qWarning() << "配置文件保存失败：" << filename;
        return false;
    }
}

void AppConfig::loadDefaults()
{
    // 这里可以使用默认构造函数已经初始化的值
    // 如果需要重新加载默认值，可以在这里重置
    m_maxCacheSize = 600;
    m_expireTimeMs = 60000;
    m_serialPort = "COM3";
    m_baudRate = 115200;
    m_receiverBackendType = "grpc";
    m_grpcEndpoint = "127.0.0.1:50051";
    m_useMockData = false;
    m_mockDataIntervalMs = 100;
    m_maxPlotPoints = 200;
    m_plotRefreshIntervalMs = 50;
    m_statsIntervalMs = 1000;
    m_temperatureAlarmThreshold = 80.0f;
    m_appTitle = "实时数据监控";
    m_windowSize = QSize(800, 600);
    m_showDevicePanel = true;
    m_showCommandPanel = true;
    m_showPlotPanel = true;
    m_showMonitorPanel = true;
    m_showStagePanel = true;
    m_mainWindowState.clear();
    m_mainWindowGeometry.clear();
    m_logLevel = "INFO";
    m_currentStyle = LightStyle;  // 确保默认使用浅色主题
    m_defaultExportDirectory = "exports";
    m_defaultExportFormat = "hdf5";
    
    qInfo() << "已加载默认配置（浅色主题）";
}
