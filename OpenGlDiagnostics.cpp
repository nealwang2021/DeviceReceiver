#include "OpenGlDiagnostics.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

namespace {

QString profileName(QSurfaceFormat::OpenGLContextProfile p)
{
    switch (p) {
    case QSurfaceFormat::CoreProfile:
        return QStringLiteral("Core");
    case QSurfaceFormat::CompatibilityProfile:
        return QStringLiteral("Compatibility");
    case QSurfaceFormat::NoProfile:
    default:
        return QStringLiteral("None");
    }
}

QString renderableTypeName(QSurfaceFormat::RenderableType t)
{
    switch (t) {
    case QSurfaceFormat::OpenGL:
        return QStringLiteral("OpenGL");
    case QSurfaceFormat::OpenGLES:
        return QStringLiteral("OpenGLES");
    case QSurfaceFormat::OpenVG:
        return QStringLiteral("OpenVG");
    case QSurfaceFormat::DefaultRenderableType:
    default:
        return QStringLiteral("Default");
    }
}

QString glStringFor(QOpenGLFunctions* f, GLenum name)
{
    if (!f) {
        return QString();
    }
    const GLubyte* p = f->glGetString(name);
    if (!p) {
        return QString();
    }
    return QString::fromLatin1(reinterpret_cast<const char*>(p));
}

} // namespace

OpenGlDiagnostics::Result OpenGlDiagnostics::probe()
{
    Result r;

    if (!QCoreApplication::instance()) {
        r.errorMessage = QStringLiteral("QCoreApplication 尚未创建");
        return r;
    }

    // QOpenGLContext 仅在 GUI app 下可用；纯 QCoreApplication 探测会缺少平台 OpenGL 集成。
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        r.errorMessage = QStringLiteral("当前进程不是 QGuiApplication，无法探测 OpenGL");
        return r;
    }

    // 沿用应用默认 SurfaceFormat（受 Qt::AA_ShareOpenGLContexts 等属性影响）。
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();

    QOpenGLContext ctx;
    ctx.setFormat(fmt);
    if (!ctx.create()) {
        r.errorMessage = QStringLiteral("QOpenGLContext::create 返回 false");
        return r;
    }
    r.contextCreated = true;

    QOffscreenSurface surface;
    surface.setFormat(ctx.format());
    surface.create();
    if (!surface.isValid()) {
        r.errorMessage = QStringLiteral("QOffscreenSurface 创建失败/无效");
        return r;
    }

    if (!ctx.makeCurrent(&surface)) {
        r.errorMessage = QStringLiteral("QOpenGLContext::makeCurrent 失败");
        return r;
    }
    r.madeCurrent = true;

    const QSurfaceFormat actual = ctx.format();
    r.versionMajor = actual.majorVersion();
    r.versionMinor = actual.minorVersion();
    r.samples = actual.samples();
    r.profile = profileName(actual.profile());
    r.renderableType = renderableTypeName(actual.renderableType());

    QOpenGLFunctions* f = ctx.functions();
    if (f) {
        f->initializeOpenGLFunctions();
        r.version = glStringFor(f, GL_VERSION);
        r.vendor = glStringFor(f, GL_VENDOR);
        r.renderer = glStringFor(f, GL_RENDERER);
        r.glsl = glStringFor(f, GL_SHADING_LANGUAGE_VERSION);
    }

    ctx.doneCurrent();
    return r;
}

void OpenGlDiagnostics::logSummary()
{
    const Result r = probe();

    if (!r.contextCreated || !r.madeCurrent) {
        qWarning().noquote() << QStringLiteral(
            "[OpenGL] 启动检测失败：%1 (contextCreated=%2 madeCurrent=%3)")
            .arg(r.errorMessage.isEmpty() ? QStringLiteral("未知错误") : r.errorMessage)
            .arg(r.contextCreated ? 1 : 0)
            .arg(r.madeCurrent ? 1 : 0);
        qWarning().noquote() << QStringLiteral(
            "[OpenGL] 当前显卡/驱动可能不支持 QCustomPlot OpenGL 渲染；"
            "若窗口出现绘图崩溃，可在「渲染」菜单关闭 OpenGL 加速。");
        return;
    }

    qInfo().noquote() << QStringLiteral(
        "[OpenGL] 启动检测通过：上下文 %1.%2 / Profile=%3 / Type=%4 / Samples=%5")
        .arg(r.versionMajor)
        .arg(r.versionMinor)
        .arg(r.profile)
        .arg(r.renderableType)
        .arg(r.samples);
    qInfo().noquote() << QStringLiteral("[OpenGL] GL_VERSION  : %1").arg(r.version);
    qInfo().noquote() << QStringLiteral("[OpenGL] GL_VENDOR   : %1").arg(r.vendor);
    qInfo().noquote() << QStringLiteral("[OpenGL] GL_RENDERER : %1").arg(r.renderer);
    qInfo().noquote() << QStringLiteral("[OpenGL] GLSL        : %1").arg(r.glsl);
}
