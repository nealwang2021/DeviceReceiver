#include "PlotWindow.h"
#include "PlotDataHub.h"
#include "AppConfig.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QDebug>
#include <cmath>

PlotWindow::PlotWindow(QWidget *parent) : PlotWindowBase(parent)
{
    qDebug() << "PlotWindow constructor begin";

    int refreshIntervalMs = 50;
    if (AppConfig* config = AppConfig::instance()) {
        m_baseMaxPlotPoints = qMax(100, config->maxPlotPoints());
        refreshIntervalMs = qBound(10, config->plotRefreshIntervalMs(), 1000);
    }

    // 窗口基础配置
    setWindowTitle("实时数据监控");
    resize(800, 600);

    // 控件面板：仅视图类型选择
    QWidget* ctrlWidget = new QWidget(this);
    qDebug() << "created ctrlWidget" << ctrlWidget;
    ctrlWidget->setMaximumHeight(30); // 限制控件面板高度
    QHBoxLayout* ctrlLayout = new QHBoxLayout(ctrlWidget);
    ctrlLayout->setContentsMargins(0,0,0,0);
    ctrlLayout->setSpacing(5);
    QLabel* viewLabel = new QLabel("视图:", ctrlWidget);
    qDebug() << "created viewLabel" << viewLabel;
    m_viewTypeCombo = new QComboBox(ctrlWidget);
    qDebug() << "created viewTypeCombo" << m_viewTypeCombo;
    m_viewTypeCombo->addItem("实部/虚部");
    m_viewTypeCombo->addItem("幅值/相位");
    m_viewTypeCombo->setVisible(false);
    ctrlLayout->addWidget(viewLabel);
    ctrlLayout->addWidget(m_viewTypeCombo);
    ctrlLayout->addStretch();

    // 不再使用外部QListWidget控制通道显示隐藏，改用QCustomPlot图例交互
    // m_channelList已移除，不再使用

    // 初始化绘图控件
    m_plot = new QCustomPlot(this);
#ifdef QCUSTOMPLOT_USE_OPENGL
    m_plot->setOpenGl(true);
#endif
    qDebug() << "created plot" << m_plot;
    // 设置绘图控件大小策略：可扩展
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->addWidget(ctrlWidget);
    mainLayout->addWidget(m_plot);
    // 设置拉伸因子：控件面板高度固定，绘图区域占据剩余空间
    mainLayout->setStretchFactor(m_plot, 1);
    setLayout(mainLayout);

    // 初始化绘图样式
    initPlot();
    // 控件信号
    connect(m_viewTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PlotWindow::onViewTypeChanged);
    
    // 连接图例交互信号
    qDebug() << "Connecting legend signals...";
    bool connected1 = connect(m_plot, &QCustomPlot::legendClick, this, &PlotWindow::onLegendClick);
    bool connected2 = connect(m_plot, &QCustomPlot::legendDoubleClick, this, &PlotWindow::onLegendDoubleClick);
    qDebug() << "legendClick connection:" << connected1;
    qDebug() << "legendDoubleClick connection:" << connected2;

    // 保留定时器用于平滑动画（可选，可以移除或保留）
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(refreshIntervalMs);
    // 不再连接onRefreshTimer，数据由PlotWindowManager提供
    // m_refreshTimer->start(); // 暂时不启动，等待数据更新
    qDebug() << "PlotWindow constructor end";
}

PlotWindow::~PlotWindow()
{
    if (m_refreshTimer) {
        m_refreshTimer->stop();
    }
}

int PlotWindow::effectiveMaxPlotPoints() const
{
    if (!m_plot) {
        return m_baseMaxPlotPoints;
    }

    // 目标控制总绘制点数，通道越多每条曲线保留点数越少
    const int graphCount = qMax(1, m_plot->graphCount());
    const int budgetPerGraph = 120000 / graphCount;
    return qBound(200, qMin(m_baseMaxPlotPoints, budgetPerGraph), m_baseMaxPlotPoints);
}

void PlotWindow::initPlot()
{
    // 不再添加温度/湿度曲线，留空等待多通道数据
    // 坐标轴配置
    m_plot->xAxis->setLabel("时间(ms)");
    m_plot->yAxis->setLabel("数值");
    m_plot->yAxis->setRange(0, 100);
    m_plot->xAxis->setRange(0, 10000);

    // 图例配置 - 启用交互功能
    m_plot->legend->setVisible(true);
    m_plot->legend->setFont(QFont("Microsoft YaHei", 9));
    m_plot->legend->setSelectableParts(QCPLegend::spItems); // 允许选择图例项
    // 图例位置默认在右上角，由QCustomPlot自动管理
    
    // 启用QCustomPlot交互功能
    m_plot->setInteractions(QCP::iSelectLegend | QCP::iSelectPlottables | QCP::iRangeDrag | QCP::iRangeZoom);

    // 样式优化
    // 关闭抗锯齿，显著提升实时曲线渲染性能
    m_plot->setNotAntialiasedElements(QCP::aeAll);
    m_plot->setNoAntialiasingOnDrag(true);
    m_plot->xAxis->setTickLabelFont(QFont("Microsoft YaHei", 8));
    m_plot->yAxis->setTickLabelFont(QFont("Microsoft YaHei", 8));

    onThemeChanged();
}

void PlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    Q_UNUSED(frames);
}

void PlotWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!snapshot || snapshot->version == m_lastSnapshotVersion) {
        return;
    }
    m_lastSnapshotVersion = snapshot->version;
    updatePlotDataFromSnapshot(snapshot);
}

void PlotWindow::updatePlotDataFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!m_plot || !snapshot || snapshot->timeMs.isEmpty()) {
        return;
    }

    const FrameData::DetectionMode mode = snapshot->mode;
    const int ch = snapshot->channelCount;
    if (mode == FrameData::Legacy || ch <= 0) {
        return;
    }

    if (mode == FrameData::MultiChannelReal) {
        if (m_lastMode == FrameData::MultiChannelComplex) {
            m_axisRects.clear();
            if (m_plot->plotLayout()) {
                m_plot->plotLayout()->clear();
                m_plot->clearGraphs();
                m_plot->plotLayout()->addElement(0, 0, new QCPAxisRect(m_plot));
                initPlot();
            }
        }
        if (m_plot && m_plot->yAxis) {
            m_plot->yAxis->setLabel("幅值");
        }
        if (m_currentChannelCount != ch) {
            m_currentChannelCount = ch;
            m_plot->clearGraphs();
            for (int i = 0; i < ch; ++i) {
                QCPGraph* g = m_plot->addGraph();
                QColor color = QColor::fromHsv((i * 36) % 360, 200, 200);
                g->setPen(QPen(color, 1));
                g->setSmooth(0);
                g->setName(QString("Ch%1(Amp)").arg(i + 1));
            }
        }
        if (m_viewTypeCombo) {
            m_viewTypeCombo->setVisible(false);
        }
        for (int i = 0; i < ch && i < m_plot->graphCount() && i < snapshot->realAmp.size(); ++i) {
            m_plot->graph(i)->setData(snapshot->timeMs, snapshot->realAmp[i], true);
        }
    } else if (mode == FrameData::MultiChannelComplex) {
        if (m_lastMode != mode || m_currentChannelCount != ch) {
            m_currentChannelCount = ch;
            setupComplexLayout(ch);
        }
        if (m_viewTypeCombo) {
            m_viewTypeCombo->setVisible(true);
        }

        const QVector<QVector<double>>& top =
            (m_complexViewType == RealImag) ? snapshot->complexReal : snapshot->complexMag;
        const QVector<QVector<double>>& bottom =
            (m_complexViewType == RealImag) ? snapshot->complexImag : snapshot->complexPhase;

        for (int i = 0; i < ch && i < m_plot->graphCount() && i < top.size(); ++i) {
            m_plot->graph(i)->setData(snapshot->timeMs, top[i], true);
        }
        for (int i = 0; i < ch; ++i) {
            const int bottomIdx = ch + i;
            if (bottomIdx < m_plot->graphCount() && i < bottom.size()) {
                m_plot->graph(bottomIdx)->setData(snapshot->timeMs, bottom[i], true);
            }
        }
    }

    const double latestTime = snapshot->timeMs.last();
    if (mode == FrameData::MultiChannelComplex) {
        for (auto rect : m_axisRects) {
            if (rect && rect->axis(QCPAxis::atBottom)) {
                rect->axis(QCPAxis::atBottom)->setRange(latestTime - 10000, latestTime);
            }
        }
    } else {
        if (m_plot->xAxis) {
            m_plot->xAxis->setRange(latestTime - 10000, latestTime);
        }
    }

    m_lastMode = mode;
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}


// ---------- 辅助函数与槽 ----------

void PlotWindow::setupComplexLayout(int channelCount)
{
    qDebug() << "[setupComplexLayout] 开始，channelCount=" << channelCount;
    try {
        // 清空现有布局和图形
        qDebug() << "[setupComplexLayout] 清空图形...";
        m_plot->clearGraphs();
        qDebug() << "[setupComplexLayout] 图形已清空";
        
        qDebug() << "[setupComplexLayout] 清空布局...";
        if (m_plot->plotLayout()) {
            m_plot->plotLayout()->clear();
        }
        qDebug() << "[setupComplexLayout] 布局已清空";
        
        m_axisRects.clear();
        m_complexTopLegend = nullptr; // 旧图例随 layout clear 一起被销毁

        // 创建上下两个轴矩形
        qDebug() << "[setupComplexLayout] 创建轴矩形...";
        QCPAxisRect* topRect = new QCPAxisRect(m_plot);
        if (!topRect) {
            qCritical() << "[setupComplexLayout] 创建topRect失败";
            return;
        }
        qDebug() << "[setupComplexLayout] topRect创建成功";
        
        QCPAxisRect* bottomRect = new QCPAxisRect(m_plot);
        if (!bottomRect) {
            qCritical() << "[setupComplexLayout] 创建bottomRect失败";
            delete topRect;
            return;
        }
        qDebug() << "[setupComplexLayout] bottomRect创建成功";
        
        qDebug() << "[setupComplexLayout] 添加轴矩形到布局...";
        if (m_plot->plotLayout()) {
            m_plot->plotLayout()->addElement(0, 0, topRect);
            m_plot->plotLayout()->addElement(1, 0, bottomRect);
        }
        qDebug() << "[setupComplexLayout] 轴矩形已添加";
        
        m_axisRects << topRect << bottomRect;

        // 共享 X 轴
        qDebug() << "[setupComplexLayout] 配置轴标签...";
        topRect->axis(QCPAxis::atBottom)->setLabel("时间(ms)");
        bottomRect->axis(QCPAxis::atBottom)->setLabel("时间(ms)");
        topRect->axis(QCPAxis::atLeft)->setLabel("数值");
        bottomRect->axis(QCPAxis::atLeft)->setLabel("数值");
        qDebug() << "[setupComplexLayout] 轴标签配置完成";

        // 创建通道曲线
        qDebug() << "[setupComplexLayout] 创建通道曲线，数量=" << channelCount;
        for (int i = 0; i < channelCount; ++i) {
            QCPGraph* gTop = m_plot->addGraph(topRect->axis(QCPAxis::atBottom), topRect->axis(QCPAxis::atLeft));
            if (!gTop) {
                qCritical() << "[setupComplexLayout] 创建顶部图形" << i << "失败";
                continue;
            }
            QCPGraph* gBottom = m_plot->addGraph(bottomRect->axis(QCPAxis::atBottom), bottomRect->axis(QCPAxis::atLeft));
            if (!gBottom) {
                qCritical() << "[setupComplexLayout] 创建底部图形" << i << "失败";
                continue;
            }
            QColor color = QColor::fromHsv((i * 36) % 360, 200, 200);
            gTop->setPen(QPen(color, 1));
            gBottom->setPen(QPen(color, 1));
            gTop->setSmooth(0);
            gBottom->setSmooth(0);
            gTop->setName(QString("Ch%1(R)").arg(i + 1));
            gBottom->setName(QString("Ch%1(I)").arg(i + 1));
            qDebug() << "[setupComplexLayout] 通道" << i << "图形创建完成";
        }
        qDebug() << "[setupComplexLayout] 所有通道曲线创建完成";

        // Legend: 在 topRect 的 inset layout 中放置独立图例
        // 注意：m_plot->legend 在 plotLayout()->clear() 后已成悬空指针，不能使用
        qDebug() << "[setupComplexLayout] 配置图例...";
        QCPLegend* topLegend = new QCPLegend;
        topRect->insetLayout()->addElement(topLegend, Qt::AlignRight | Qt::AlignTop);
        topLegend->setLayer("legend");
        topLegend->setFont(QFont("Microsoft YaHei", 9));
        topLegend->setSelectableParts(QCPLegend::spItems);
        topLegend->setVisible(true);
        // 将图形加入顶部图例（图形名称已在 setName 时设置）
        for (int i = 0; i < m_plot->graphCount(); ++i) {
            QCPGraph* g = m_plot->graph(i);
            if (g && g->valueAxis() == topRect->axis(QCPAxis::atLeft)) {
                topLegend->addItem(new QCPPlottableLegendItem(topLegend, g));
            }
        }
        m_complexTopLegend = topLegend;
        const bool dark = isDarkThemeActive();
        topLegend->setBrush(QBrush(dark ? QColor(42, 46, 52, 220) : QColor(255, 255, 255, 220)));
        topLegend->setBorderPen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
        topLegend->setTextColor(dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
        qDebug() << "[setupComplexLayout] 图例配置完成";

        onThemeChanged();

        qDebug() << "[setupComplexLayout] 完成";
    } catch (const std::exception& e) {
        qCritical() << "[setupComplexLayout] 异常:" << e.what();
    } catch (...) {
        qCritical() << "[setupComplexLayout] 未知异常";
    }
}

void PlotWindow::onViewTypeChanged(int index)
{
    m_complexViewType = static_cast<ComplexViewType>(index);
    const auto snap = PlotDataHub::instance()->snapshot();
    if (!snap) {
        return;
    }
    updatePlotDataFromSnapshot(snap);
}

void PlotWindow::onLegendClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event)
{
    Q_UNUSED(legend);
    Q_UNUSED(event);
    
    qDebug() << "[PlotWindow::onLegendClick] Signal triggered";
    
    if (!item) {
        qDebug() << "[PlotWindow::onLegendClick] Item is null";
        return;
    }
    
    qDebug() << "[PlotWindow::onLegendClick] Item type:" << item->metaObject()->className();
    
    // 尝试转换为可绘图的图例项
    QCPPlottableLegendItem* plotItem = qobject_cast<QCPPlottableLegendItem*>(item);
    if (!plotItem) {
        qDebug() << "[PlotWindow::onLegendClick] Item is not a QCPPlottableLegendItem";
        return;
    }
    
    // 获取对应的可绘图对象
    QCPAbstractPlottable* plottable = plotItem->plottable();
    if (!plottable) {
        qDebug() << "[PlotWindow::onLegendClick] No plottable associated with legend item";
        return;
    }
    
    qDebug() << "[PlotWindow::onLegendClick] Plottable name:" << plottable->name() << "type:" << plottable->metaObject()->className();
    
    // 切换可见性
    bool visible = !plottable->visible();
    plottable->setVisible(visible);
    qDebug() << "[PlotWindow::onLegendClick] Toggled visibility of" << plottable->name() << "to" << visible;
    
    // 更新图例项文本颜色以反映可见性
    const bool dark = isDarkThemeActive();
    if (visible) {
        plotItem->setTextColor(dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
    } else {
        plotItem->setTextColor(dark ? QColor(130, 136, 146) : QColor(130, 130, 130));
    }
    
    // 触发重绘
    m_plot->replot(QCustomPlot::rpQueuedReplot);
    qDebug() << "[PlotWindow::onLegendClick] Replot queued";
}

void PlotWindow::onLegendDoubleClick(QCPLegend* legend, QCPAbstractLegendItem* item, QMouseEvent* event)
{
    Q_UNUSED(legend);
    Q_UNUSED(item);
    Q_UNUSED(event);
    
    qDebug() << "Legend double clicked";
    // 双击可以切换所有曲线的可见性，但目前先不实现
}

void PlotWindow::onCriticalFrame(const FrameData& frame)
{
    // 报警视觉提示：背景变红，2秒恢复
    m_plot->setBackground(QColor(255, 204, 204));
    QTimer::singleShot(2000, [this]() {
        onThemeChanged();
    });

    // 打印报警日志
    QString alarmMsg;
    if (frame.detectMode == FrameData::MultiChannelReal) {
        alarmMsg = QString("【报警】帧%1：幅值/相位模式 通道数%2").arg(frame.frameId).arg(frame.channelCount);
    } else if (frame.detectMode == FrameData::MultiChannelComplex) {
        alarmMsg = QString("【报警】帧%1：复数模式 通道数%2").arg(frame.frameId).arg(frame.channelCount);
    } else {
        alarmMsg = QString("【报警】帧%1：Legacy模式（已弃用）").arg(frame.frameId);
    }
    
    qCritical() << alarmMsg;
}

void PlotWindow::onThemeChanged()
{
    applyThemeToPlot(m_plot, isDarkThemeActive());
    if (m_complexTopLegend) {
        const bool dark = isDarkThemeActive();
        m_complexTopLegend->setBrush(QBrush(dark ? QColor(42, 46, 52, 220) : QColor(255, 255, 255, 220)));
        m_complexTopLegend->setBorderPen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
        m_complexTopLegend->setTextColor(dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
    }
    if (m_plot) {
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}
