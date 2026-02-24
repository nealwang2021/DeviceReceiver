#include <QApplication>
#include <QDebug>
#include "FrameData.h"
#include "ApplicationController.h"
#include "AppConfig.h"
#include <QFile>
#include <QTextStream>
#include <QDateTime>

static QFile* g_logFile = nullptr;

static void realtimeMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
    if (!g_logFile) return;
    QTextStream out(g_logFile);
    QString level;
    switch (type) {
    case QtDebugMsg: level = "DEBUG"; break;
    case QtInfoMsg: level = "INFO"; break;
    case QtWarningMsg: level = "WARNING"; break;
    case QtCriticalMsg: level = "CRITICAL"; break;
    case QtFatalMsg: level = "FATAL"; break;
    }
    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    out << time << " [" << level << "] " << msg << Qt::endl;
    out.flush();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // 注册FrameData类型用于跨线程信号槽
    qRegisterMetaType<FrameData>("FrameData");

    // 加载配置文件（不存在或格式错误时回退到默认配置）
    AppConfig::instance()->loadFromFile("config.ini");

    // 安装简单日志处理器（写入应用目录下 realtime_data.log）
    QString logPath = QApplication::applicationDirPath() + "/realtime_data.log";
    g_logFile = new QFile(logPath, qApp);
    if (g_logFile->open(QIODevice::Append | QIODevice::Text)) {
        qInstallMessageHandler(realtimeMessageHandler);
        qInfo() << "日志已打开:" << logPath;
    } else {
        qWarning() << "无法打开日志文件:" << logPath;
        delete g_logFile; g_logFile = nullptr;
    }

    // 创建应用控制器
    ApplicationController controller;
    
    // 初始化所有模块
    if (!controller.initialize()) {
        qCritical() << "应用初始化失败，程序退出";
        return -1;
    }
    
    // 启动应用（显示窗口，开始数据接收）
    controller.start();
    
    // 程序退出清理
    QObject::connect(&app, &QApplication::aboutToQuit, [&controller]() {
        controller.stop();
        qInfo() << "程序正常退出";
    });

    return app.exec();
}
