#include <QApplication>
#include <QDebug>
#include "FrameData.h"
#include "ApplicationController.h"
#include "AppConfig.h"
#include <QFile>
#include <QDateTime>
#include <QFontDatabase>
#include <QFont>
#include <iostream>

static QFile* g_logFile = nullptr;

static void writeUtf8LogLine(const QString& line)
{
    if (!g_logFile) {
        return;
    }

    const QByteArray utf8 = line.toUtf8();
    g_logFile->write(utf8);
    g_logFile->write("\n", 1);
    g_logFile->flush();
}

static void realtimeMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
    if (!g_logFile) return;

    QString level;
    switch (type) {
    case QtDebugMsg: level = "DEBUG"; break;
    case QtInfoMsg: level = "INFO"; break;
    case QtWarningMsg: level = "WARNING"; break;
    case QtCriticalMsg: level = "CRITICAL"; break;
    case QtFatalMsg: level = "FATAL"; break;
    }
    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString full = time + " [" + level + "] " + msg;
    writeUtf8LogLine(full);

    // also print to standard error so output appears in console
    fprintf(stderr, "%s\n", full.toLocal8Bit().constData());
}

int main(int argc, char *argv[])
{
    try {
        QApplication app(argc, argv);
        
        // 注册FrameData类型用于跨线程信号槽
        qRegisterMetaType<FrameData>("FrameData");

#ifdef QT_COMPILE_FOR_WASM
        // WASM环境没有系统字体，需要手动加载中文字体
        {
            int fontId = QFontDatabase::addApplicationFont(":/fonts/files/fonts/simhei.ttf");
            if (fontId != -1) {
                QStringList families = QFontDatabase::applicationFontFamilies(fontId);
                if (!families.isEmpty()) {
                    QFont defaultFont(families.first(), 9);
                    QApplication::setFont(defaultFont);
                    qDebug() << "WASM: 已加载中文字体:" << families.first();
                }
            } else {
                qWarning() << "WASM: 加载中文字体失败";
            }
        }
#endif

        // 加载配置文件（不存在或格式错误时回退到默认配置）
        qDebug() << "正在加载配置文件...";
        AppConfig::instance()->loadFromFile("config.ini");
        qDebug() << "配置文件加载完成";

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
        qDebug() << "正在创建应用控制器...";
        ApplicationController controller;
        qDebug() << "应用控制器创建完成";
        
        // 初始化所有模块
        qDebug() << "开始初始化应用模块...";
        if (!controller.initialize()) {
            qCritical() << "应用初始化失败，程序退出";
            return -1;
        }
        qDebug() << "应用模块初始化完成";
        
        // 启动应用（显示窗口，开始数据接收）
        qDebug() << "启动应用...";
        controller.start();
        qDebug() << "应用启动完成";
        
        // 程序退出清理
        QObject::connect(&app, &QApplication::aboutToQuit, [&controller]() {
            controller.stop();
            qInfo() << "程序正常退出";
        });

        qDebug() << "进入事件循环...";
        return app.exec();
    } catch (const std::exception& e) {
        std::cerr << "异常：" << e.what() << std::endl;
        if (g_logFile) {
            writeUtf8LogLine("异常：" + QString::fromStdString(std::string(e.what())));
            g_logFile->close();
        }
        return -1;
    } catch (...) {
        std::cerr << "未知异常" << std::endl;
        if (g_logFile) {
            writeUtf8LogLine("未知异常");
            g_logFile->close();
        }
        return -1;
    }
}
