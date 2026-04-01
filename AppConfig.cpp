#include "AppConfig.h"
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <QApplication>
#include <QTextCodec>
#include <QTemporaryFile>
#include <cstdio>
#ifndef QT_COMPILE_FOR_WASM
#include <QAbstractSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSerialPort>
#endif

namespace {

#ifndef QT_COMPILE_FOR_WASM
QString bracketedPreferredLocalIpv6()
{
    // 使用 allAddresses()，避免在源码中出现 QNetworkInterfaceAddressEntry
    // （部分 MSVC + Windows 头文件组合下会与宏/解析冲突导致 C4430）
    QString globalV6;
    QString linkLocalV6;
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() != QAbstractSocket::IPv6Protocol) {
            continue;
        }
        if (addr.isLoopback()) {
            continue;
        }
        const QString base = addr.toString().split(QLatin1Char('%')).first();
        if (addr.isLinkLocal()) {
            if (linkLocalV6.isEmpty()) {
                linkLocalV6 = base;
            }
            continue;
        }
        if (globalV6.isEmpty()) {
            globalV6 = base;
        }
    }

    const QString chosen = !globalV6.isEmpty() ? globalV6 : (!linkLocalV6.isEmpty() ? linkLocalV6 : QStringLiteral("::1"));
    return QStringLiteral("[%1]").arg(chosen);
}
#else
QString bracketedPreferredLocalIpv6()
{
    return QStringLiteral("[::1]");
}
#endif

QString defaultGrpcEndpoint(int port)
{
    return QStringLiteral("%1:%2").arg(bracketedPreferredLocalIpv6()).arg(port);
}

/** 每次加载 config 时输出 AppTitle 解析步骤，便于排查乱码（无需环境变量） */
void logAppTitleStep(const char* step, const QString& detail)
{
    const QString line = QStringLiteral("[AppConfig/AppTitle] ") + QLatin1String(step) + QLatin1Char(' ') + detail;
    qWarning().noquote() << line;
    fprintf(stderr, "%s\n", line.toUtf8().constData());
}

QString hexPreviewBytes(const QByteArray& data, int maxBytes = 40)
{
    const QByteArray p = data.left(maxBytes);
    QString s;
    s.reserve(p.size() * 3);
    for (int i = 0; i < p.size(); ++i) {
        const auto c = static_cast<unsigned char>(p[i]);
        s += QString::asprintf("%02X ", c);
    }
    return s.trimmed();
}

/** 将 config.ini 整文件按 UTF-8 解码：先去掉 UTF-8 BOM(EF BB BF)，避免首行节名无法匹配；仅当 UTF-8 非法时再尝试 GB18030（旧 ANSI 配置） */
QString decodeIniFileBytes(const QByteArray& data)
{
    if (data.isEmpty()) {
        return {};
    }
    QByteArray d = data;
    bool strippedUtf8Bom = false;
    if (d.startsWith("\xEF\xBB\xBF")) {
        d = d.mid(3);
        strippedUtf8Bom = true;
    }
    QString text = QString::fromUtf8(d);
    if (text.contains(QChar(0xFFFD))) {
        QTextCodec* gb = QTextCodec::codecForName("GB18030");
        if (!gb) {
            gb = QTextCodec::codecForName("GBK");
        }
        if (gb) {
            const QString alt = gb->toUnicode(data);
            if (!alt.contains(QChar(0xFFFD))) {
                logAppTitleStep("decodeIniFileBytes", QStringLiteral("fallback GB18030 (UTF-8 had replacement chars)"));
                text = alt;
                if (text.startsWith(QChar(0xFEFF))) {
                    text = text.mid(1);
                }
                return text;
            }
        }
    }
    if (strippedUtf8Bom) {
        logAppTitleStep("decodeIniFileBytes", QStringLiteral("stripped UTF-8 BOM, QString length=%1").arg(text.size()));
    }
    // 若未走字节 BOM 剥离，仍可能有 U+FEFF（部分工具写入）
    while (text.startsWith(QChar(0xFEFF))) {
        text = text.mid(1);
    }
    return text;
}

/**
 * 将 QSettings 写入 INI 时产生的转义还原为 QString。
 * 常见两种：① 整段为 UTF-8 字节的 \\xHH 序列；② 整段为 BMP 字符的 \\xHHHH（每字符 6 个字符：\\x + 4 位十六进制）。
 * 若无法完整匹配则返回原文（避免误伤已正常的中文）。
 */
QString decodeQtIniEscapedString(QString v)
{
    // INI / QSettings 写入时会把 \ 写成 \\；若不先折叠，值里会出现 \\xe6...，导致无法按 \\xHH 解析且标题里反斜杠变多
    for (int pass = 0; pass < 8; ++pass) {
        const QString before = v;
        v.replace(QLatin1String("\\\\"), QLatin1String("\\"));
        if (v == before) {
            break;
        }
    }

    if (!v.contains(QLatin1String("\\x"))) {
        return v;
    }

    // ① 仅由 \\x + 2 位十六进制构成，整体为 UTF-8 字节流（标题显示为 \\xe6\\xb5\\x8b... 即此情况）
    {
        QByteArray bytes;
        int i = 0;
        while (i < v.size()) {
            if (i + 3 >= v.size() || v.at(i) != QLatin1Char('\\') || v.at(i + 1) != QLatin1Char('x')) {
                bytes.clear();
                break;
            }
            bool ok = false;
            const int b = v.mid(i + 2, 2).toInt(&ok, 16);
            if (!ok || b < 0 || b > 255) {
                bytes.clear();
                break;
            }
            bytes.append(static_cast<char>(b & 0xff));
            i += 4;
        }
        if (i == v.size() && !bytes.isEmpty()) {
            const QString u = QString::fromUtf8(bytes);
            if (!u.contains(QChar(0xFFFD))) {
                return u;
            }
        }
    }

    // ② 仅由 \\x + 4 位十六进制构成（每个码位 6 个字符）
    if (v.size() % 6 == 0 && v.size() >= 6) {
        QString out;
        int i = 0;
        while (i < v.size()) {
            if (i + 5 >= v.size() || v.at(i) != QLatin1Char('\\') || v.at(i + 1) != QLatin1Char('x')) {
                out.clear();
                break;
            }
            bool ok = false;
            const uint cp = v.mid(i + 2, 4).toUInt(&ok, 16);
            if (!ok || cp > 0xFFFF) {
                out.clear();
                break;
            }
            out += QChar(static_cast<ushort>(cp));
            i += 6;
        }
        if (i == v.size() && !out.isEmpty()) {
            return out;
        }
    }

    return v;
}

/** INI 值常见为 "..." 或 '...'（与 QSettings 写入一致）；去掉成对引号，保留内部字符 */
QString stripOptionalIniQuotes(const QString& vIn)
{
    QString v = vIn.trimmed();
    if (v.size() >= 2) {
        const QChar a = v.at(0);
        const QChar b = v.at(v.size() - 1);
        if ((a == QLatin1Char('"') && b == QLatin1Char('"')) || (a == QLatin1Char('\'') && b == QLatin1Char('\''))) {
            return v.mid(1, v.size() - 2);
        }
    }
    return v;
}

/** 不经过 QSettings，直接从 INI 文本解析 [General]/[%General] 下的 AppTitle，避免 Windows 上 IniFormat 编码误判导致标题乱码 */
QString readAppTitleFromIniText(const QString& contentIn)
{
    QString content = contentIn;
    content = content.trimmed();
    while (content.startsWith(QChar(0xFEFF))) {
        content.remove(0, 1);
    }

    const int nl0 = content.indexOf(QLatin1Char('\n'));
    const QString firstLinePreview = nl0 < 0 ? content.left(120) : content.left(nl0).left(120);
    logAppTitleStep("parse_content", QStringLiteral("firstLine=%1").arg(firstLinePreview));

    bool inGeneral = false;
    const QStringList sectionNames = { QStringLiteral("General"), QStringLiteral("%General") };

    QStringList lines;
    {
        int pos = 0;
        while (pos <= content.size()) {
            const int nl = content.indexOf(QLatin1Char('\n'), pos);
            const int end = nl < 0 ? content.size() : nl;
            QString line = content.mid(pos, end - pos);
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
            }
            lines.append(line);
            if (nl < 0) {
                break;
            }
            pos = nl + 1;
        }
    }

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            const QString sec = line.mid(1, line.length() - 2).trimmed();
            inGeneral = sectionNames.contains(sec);
            continue;
        }
        if (!inGeneral) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QString k = line.left(eq).trimmed();
        if (k.compare(QStringLiteral("AppTitle"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        QString v = line.mid(eq + 1).trimmed();
        logAppTitleStep("app_title_raw_value", v);
        v = stripOptionalIniQuotes(v);
        logAppTitleStep("after_strip_quotes", v);
        const QString unescaped = decodeQtIniEscapedString(v);
        logAppTitleStep("after_unescape", unescaped);
        return unescaped;
    }
    logAppTitleStep("parse_missing_apptitle", QStringLiteral("no AppTitle under [General]/[%General]"));
    return {};
}

QString readAppTitleFromIniFileUtf8(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        logAppTitleStep("read_file", QStringLiteral("open failed: %1").arg(filePath));
        return {};
    }
    const QByteArray raw = f.readAll();
    f.close();
    logAppTitleStep("raw_head", QStringLiteral("bytes=%1 hex=%2").arg(raw.size()).arg(hexPreviewBytes(raw)));
    const QString decoded = decodeIniFileBytes(raw);
    logAppTitleStep("decoded_chars", QStringLiteral("count=%1").arg(decoded.size()));
    return readAppTitleFromIniText(decoded);
}

} // namespace

AppConfig* AppConfig::m_instance = nullptr;

AppConfig::AppConfig(QObject* parent)
    : QObject(parent)
    , m_grpcEndpoint(defaultGrpcEndpoint(50051))
    , m_stageGrpcEndpoint(defaultGrpcEndpoint(50052))
{
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

    // UTF-8 BOM 时，部分环境下 QSettings(IniFormat) 无法正确解析整文件，导致 UI/MainWindowState 等为空、布局丢失。
    // 对 QSettings 使用去掉 BOM 后的临时副本；AppTitle 仍由 readAppTitleFromIniFileUtf8(原路径) 解析（内部已处理 BOM）。
    QFile inFile(filename);
    if (!inFile.open(QIODevice::ReadOnly)) {
        qWarning() << "无法打开配置文件读取：" << filename;
        return false;
    }
    const QByteArray rawFileContent = inFile.readAll();
    inFile.close();

    QTemporaryFile tmpIni;
    tmpIni.setAutoRemove(true);
    QString settingsPath = filename;
    if (rawFileContent.startsWith("\xEF\xBB\xBF")) {
        tmpIni.setFileTemplate(QDir::tempPath() + QStringLiteral("/device_receiver_cfg_XXXXXX.ini"));
        if (tmpIni.open()) {
            tmpIni.write(rawFileContent.mid(3));
            tmpIni.flush();
            settingsPath = tmpIni.fileName();
            qInfo() << "[AppConfig] 检测到 UTF-8 BOM：已用无 BOM 临时文件供 QSettings 加载（恢复布局/窗口状态）";
        } else {
            qWarning() << "[AppConfig] 无法创建临时配置文件，QSettings 仍读取原文件，带 BOM 时布局状态可能无法加载";
        }
    }

    QSettings settings(settingsPath, QSettings::IniFormat);

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

    // AppTitle：优先从文件按 UTF-8/GB18030 解析，避免 QSettings(IniFormat) 在 Windows 下按系统代码页读入导致中文标题乱码
    {
        const QString titleFromUtf8Ini = readAppTitleFromIniFileUtf8(filename);
        if (!titleFromUtf8Ini.isEmpty()) {
            m_appTitle = titleFromUtf8Ini;
        } else {
            // QSettings 通常已把 \\x 转义解码为 QString；若仍异常则再解一次
            m_appTitle = decodeQtIniEscapedString(valueGeneral(QStringLiteral("AppTitle"), m_appTitle).toString());
        }
    }
    // 从 UTF-8 文本解析出的标题也可能含 Qt 转义字面量
    m_appTitle = decodeQtIniEscapedString(m_appTitle);
    logAppTitleStep("final_appTitle", m_appTitle);
    if (m_appTitle.isEmpty()) {
        m_appTitle = QStringLiteral("测试软件");
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
    if (m_grpcEndpoint.trimmed().isEmpty()) {
        m_grpcEndpoint = defaultGrpcEndpoint(50051);
    }

    if (settings.contains(QStringLiteral("Receiver/StageGrpcEndpoint"))) {
        m_stageGrpcEndpoint = settings.value(QStringLiteral("Receiver/StageGrpcEndpoint")).toString();
    } else {
        // 旧版仅一路 GrpcEndpoint 时，三轴台与被测设备共用同一字段
        m_stageGrpcEndpoint = settings.value(QStringLiteral("Receiver/GrpcEndpoint"), m_stageGrpcEndpoint).toString();
    }
    if (m_stageGrpcEndpoint.trimmed().isEmpty()) {
        m_stageGrpcEndpoint = defaultGrpcEndpoint(50052);
    }

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
    m_mockDataIntervalMs = qBound(10, m_mockDataIntervalMs, 60000);
    
    // 加载绘图配置
    m_maxPlotPoints = settings.value("Plot/MaxPoints", m_maxPlotPoints).toInt();
    m_plotRefreshIntervalMs = settings.value("Plot/RefreshIntervalMs", m_plotRefreshIntervalMs).toInt();

    // 检测分析窗口
    m_inspectionChannelsPerGroup = qBound(1,
        settings.value("InspectionPlot/ChannelsPerGroup", m_inspectionChannelsPerGroup).toInt(), 256);
    
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
    if (settingsPath != filename) {
        qInfo() << "[AppConfig] UI/MainWindowState 已加载字节数:" << m_mainWindowState.size()
                << "UI/MainWindowGeometry 字节数:" << m_mainWindowGeometry.size();
    }

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
    settings.setValue("Receiver/StageGrpcEndpoint", m_stageGrpcEndpoint);
    settings.setValue("Receiver/UseMockData", m_useMockData);
    settings.setValue("Receiver/MockDataIntervalMs", m_mockDataIntervalMs);
    
    // 保存绘图配置
    settings.setValue("Plot/MaxPoints", m_maxPlotPoints);
    settings.setValue("Plot/RefreshIntervalMs", m_plotRefreshIntervalMs);

    // 检测分析窗口
    settings.setValue("InspectionPlot/ChannelsPerGroup", m_inspectionChannelsPerGroup);
    
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
    m_grpcEndpoint = defaultGrpcEndpoint(50051);
    m_stageGrpcEndpoint = defaultGrpcEndpoint(50052);
    m_useMockData = false;
    m_mockDataIntervalMs = 100;
    m_maxPlotPoints = 200;
    m_plotRefreshIntervalMs = 50;
    m_inspectionChannelsPerGroup = 8;
    m_statsIntervalMs = 1000;
    m_temperatureAlarmThreshold = 80.0f;
    m_appTitle = QStringLiteral("测试软件");
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
