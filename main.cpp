#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include "FrameData.h"
#include "PlotWindowBase.h"
#include "ApplicationController.h"
#include "AppConfig.h"
#include "AppLogger.h"
#include "CrashHandlerWin.h"
#include "OpenGlDiagnostics.h"
#include <QDateTime>
#include <QFontDatabase>
#include <QFont>
#include <QTimer>
#include <iostream>

static void crashHandlerLogBridge(const char* utf8Message)
{
    const QString line = QString::fromUtf8(utf8Message);
    AppLogger::instance()->logCrashLine(line);
}

int main(int argc, char *argv[])
{
    try {
        CrashHandlerWin::setLogCallback(crashHandlerLogBridge);
        CrashHandlerWin::installHandlers();
        // 默认请求桌面 OpenGL。不要设置 AA_UseSoftwareOpenGL，否则 Qt 会走 llvmpipe/opengl32sw，
        // QCustomPlot::setOpenGl(true) 会因非 opengl32.dll 后端而无法启用硬件加速。
        QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
        QApplication app(argc, argv);
        
        // 注册FrameData类型用于跨线程信号槽
        qRegisterMetaType<FrameData>("FrameData");
        qRegisterMetaType<PlotWindowBase*>("PlotWindowBase*");

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

        // 先安装日志处理器，再加载 config.ini，否则 AppConfig::loadFromFile 内的 qInfo/qWarning 不会写入 realtime_data.log
        const QString startupTag = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        CrashHandlerWin::setSessionTag(startupTag);
        QString logPath = QApplication::applicationDirPath() + QString("/realtime_data_%1.log").arg(startupTag);
        if (AppLogger::instance()->initialize(logPath)) {
            AppLogger::instance()->installQtMessageHandler();
            qInfo() << "日志已打开:" << logPath;
            qInfo() << "Crash dump目录:" << CrashHandlerWin::crashDirectoryPath();
            qInfo() << "[OpenGL] Qt 启动属性: AA_UseDesktopOpenGL=1 AA_UseSoftwareOpenGL=0";
            const qint64 startupEpochMs = QDateTime::currentMSecsSinceEpoch();
            const QString startupIso = QDateTime::fromMSecsSinceEpoch(startupEpochMs).toString(Qt::ISODateWithMs);
            qInfo().noquote() << QString("=== APP_START startup_iso=%1 startup_epoch_ms=%2 ===")
                                     .arg(startupIso)
                                     .arg(startupEpochMs);
        } else {
            fprintf(stderr, "无法初始化日志文件: %s\n", logPath.toLocal8Bit().constData());
        }

        // 探测 OpenGL 环境（仅日志，不修改 AppConfig）
        // 必须在 QApplication 构造之后、首个绘图窗口创建之前执行；
        // 在配置加载之前先做，便于排查"配置开了 OpenGL 但驱动不支持"的场景。
        OpenGlDiagnostics::logSummary();

        // 加载配置文件（与可执行文件同目录，避免 cwd 不同导致布局/状态未加载）
        {
            const QString cfgPath = AppConfig::defaultConfigFilePath();
            qDebug() << "正在加载配置文件:" << cfgPath;
            if (!AppConfig::instance()->loadFromFile(cfgPath)) {
                qWarning() << "将使用内存默认配置（窗口布局可能未恢复，关闭程序正常退出后会写入 config.ini）";
            }
        }
        qDebug() << "配置文件处理完成";

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
            controller.stopWithReason(QStringLiteral("aboutToQuit"));
            qInfo() << "程序正常退出";
        });

        bool autoCloseOk = false;
        const int autoCloseMs = qEnvironmentVariableIntValue("DEVICE_RECEIVER_AUTOCLOSE_MS", &autoCloseOk);
        if (autoCloseOk && autoCloseMs > 0) {
            qInfo() << "启用自动退出定时器，毫秒=" << autoCloseMs;
            QTimer::singleShot(autoCloseMs, &app, &QCoreApplication::quit);
        }

        bool triggerCrash = false;
        const int crashFlag = qEnvironmentVariableIntValue("DEVICE_RECEIVER_TRIGGER_CRASH_ON_START", &triggerCrash);
        if (triggerCrash && crashFlag == 1) {
            qCritical() << "触发受控崩溃测试：DEVICE_RECEIVER_TRIGGER_CRASH_ON_START=1";
            volatile int* p = nullptr;
            *p = 1;
        }

        qDebug() << "进入事件循环...";
        const int exitCode = app.exec();
        AppLogger::instance()->shutdown();
        return exitCode;
    } catch (const std::exception& e) {
        std::cerr << "异常：" << e.what() << std::endl;
        AppLogger::instance()->logCrashLine("异常：" + QString::fromStdString(std::string(e.what())));
        AppLogger::instance()->shutdown();
        return -1;
    } catch (...) {
        std::cerr << "未知异常" << std::endl;
        AppLogger::instance()->logCrashLine(QStringLiteral("未知异常"));
        AppLogger::instance()->shutdown();
        return -1;
    }
}
