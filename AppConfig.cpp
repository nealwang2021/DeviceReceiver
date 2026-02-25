#include "AppConfig.h"
#include <QSettings>
#include <QFile>
#include <QDebug>
#include <QApplication>
#include <QSerialPort>

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

bool AppConfig::loadFromFile(const QString& filename)
{
    QSettings settings(filename, QSettings::IniFormat);
    
    if (!settings.contains("General/AppTitle")) {
        qWarning() << "配置文件格式不正确或不存在，使用默认配置";
        return false;
    }
    
    // 加载应用配置
    m_appTitle = settings.value("General/AppTitle", m_appTitle).toString();
    
    // 加载窗口配置
    m_windowSize = settings.value("Window/Size", m_windowSize).toSize();
    
    // 加载缓存配置
    m_maxCacheSize = settings.value("Cache/MaxSize", m_maxCacheSize).toInt();
    m_expireTimeMs = settings.value("Cache/ExpireTimeMs", m_expireTimeMs).toLongLong();
    
    // 加载串口配置
    m_serialPort = settings.value("Serial/Port", m_serialPort).toString();
    m_baudRate = settings.value("Serial/BaudRate", m_baudRate).toInt();
    m_useMockData = settings.value("Serial/UseMockData", m_useMockData).toBool();
    m_mockDataIntervalMs = settings.value("Serial/MockDataIntervalMs", m_mockDataIntervalMs).toInt();
    
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
    settings.setValue("Serial/UseMockData", m_useMockData);
    settings.setValue("Serial/MockDataIntervalMs", m_mockDataIntervalMs);
    
    // 保存绘图配置
    settings.setValue("Plot/MaxPoints", m_maxPlotPoints);
    settings.setValue("Plot/RefreshIntervalMs", m_plotRefreshIntervalMs);
    
    // 保存统计配置
    settings.setValue("Stats/IntervalMs", m_statsIntervalMs);
    
    // 保存报警配置
    settings.setValue("Alarm/TemperatureThreshold", m_temperatureAlarmThreshold);
    
    // 保存样式配置
    settings.setValue("Style/CurrentStyle", static_cast<int>(m_currentStyle));
    
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
    m_useMockData = true;
    m_mockDataIntervalMs = 100;
    m_maxPlotPoints = 200;
    m_plotRefreshIntervalMs = 50;
    m_statsIntervalMs = 1000;
    m_temperatureAlarmThreshold = 80.0f;
    m_appTitle = "实时数据监控";
    m_windowSize = QSize(800, 600);
    m_logLevel = "INFO";
    m_currentStyle = LightStyle;  // 确保默认使用浅色主题
    
    qInfo() << "已加载默认配置（浅色主题）";
}
