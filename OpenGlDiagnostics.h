#ifndef OPENGLDIAGNOSTICS_H
#define OPENGLDIAGNOSTICS_H

#include <QString>

/**
 * 启动期 OpenGL 环境探针。
 *
 * 主要作用：
 *  - 在创建任何窗口前，离屏（QOffscreenSurface）尝试创建 QOpenGLContext 并 makeCurrent，
 *    据此拿到 GL_VERSION/VENDOR/RENDERER/GLSL 与上下文格式，写入日志。
 *  - 不抛异常、不修改任何全局状态；失败信息一律走 errorMessage 返回。
 *
 * 使用约束：
 *  - 必须在 QGuiApplication/QApplication 构造之后调用（依赖事件分发器与平台插件）。
 *  - 仅做诊断与日志，不会主动覆盖 AppConfig::qcustomPlotOpenGlEnabled，避免改变用户配置；
 *    若用户在显卡/驱动有问题的环境下看到 [OpenGL] 检测失败 的告警，可手动从「渲染」菜单关闭。
 */
class OpenGlDiagnostics
{
public:
    struct Result {
        bool contextCreated{false};
        bool madeCurrent{false};
        QString errorMessage;

        QString version;        // GL_VERSION
        QString vendor;         // GL_VENDOR
        QString renderer;       // GL_RENDERER
        QString glsl;           // GL_SHADING_LANGUAGE_VERSION

        int versionMajor{0};
        int versionMinor{0};
        int samples{0};
        QString profile;        // QSurfaceFormat::profile() 文本
        QString renderableType; // QSurfaceFormat::renderableType() 文本
    };

    /// 创建临时 QOpenGLContext + QOffscreenSurface，返回探测结果；调用方负责日志/告警。
    static Result probe();

    /// 一行入口：probe + 写入 qInfo（成功）或 qWarning（失败）。
    static void logSummary();
};

#endif // OPENGLDIAGNOSTICS_H
