#ifndef PLOTWINDOWBASE_H
#define PLOTWINDOWBASE_H

#include <QWidget>
#include <QVector>
#include "FrameData.h"
#include "PlotDataHub.h"

class QCustomPlot;
class QEvent;

class PlotWindowBase : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWindowBase(QWidget* parent = nullptr) : QWidget(parent) {}
    ~PlotWindowBase() override = default;

public slots:
    virtual void onDataUpdated(const QVector<FrameData>& frames) = 0;
    virtual void onCriticalFrame(const FrameData& frame) = 0;
    virtual void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) = 0;

    /**
     * 按 AppConfig 当前 qcustomPlotOpenGlEnabled() 值，把 setOpenGl 应用到单个 plot。
     * 仅在编译时定义了 QCUSTOMPLOT_USE_OPENGL 才会真正切换；
     * 若编译期已关，本函数不会调用 setOpenGl，避免触发 QCustomPlot 的 qDebug 提示。
     * 各窗口构造时应调用一次（替换原直接 setOpenGl(true) 的写法），
     * 运行时切换由 PlotWindowManager 监听 AppConfig 信号统一广播。
     *
     * 该函数同时会调用 applyConfiguredPerformance(plot)，按当前 OpenGL 开关切换
     * QCustomPlot 渲染性能档（Quality / Performance），子窗口无需感知。
     */
    static void applyConfiguredOpenGl(QCustomPlot* plot);

    /// 对 widget 子树内所有 QCustomPlot 实例统一应用当前 OpenGL 配置 + 性能档 + replot。
    static void applyConfiguredOpenGlToTree(QWidget* root);

    /**
     * 按 AppConfig::qcustomPlotOpenGlEnabled() 切换 QCustomPlot 的渲染性能档：
     * - Quality（OpenGL 启用）：QCustomPlot 默认行为，曲线抗锯齿、拖拽时保持抗锯齿。
     * - Performance（OpenGL 关闭）：启用 phFastPolylines、setNoAntialiasingOnDrag、
     *   关闭每条 QCPGraph 的 antialiased，以缓解软件渲染下点数线性退化。
     * ColorMap 类绘制不动 interpolate / antialiased，避免改变热力图视觉。
     * 不在内部 replot，由调用方负责。
     */
    static void applyConfiguredPerformance(QCustomPlot* plot);

    /// 对 widget 子树内所有 QCustomPlot 实例统一应用当前性能档（不 replot）。
    static void applyConfiguredPerformanceToTree(QWidget* root);

protected:
    void changeEvent(QEvent* event) override;
    bool isDarkThemeActive() const;
    void applyThemeToPlot(QCustomPlot* plot, bool dark) const;
    void applyThemeToAllPlots() const;
    virtual void onThemeChanged();
};

Q_DECLARE_METATYPE(PlotWindowBase*)

#endif // PLOTWINDOWBASE_H
