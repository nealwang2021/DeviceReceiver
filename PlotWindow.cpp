#include "PlotWindow.h"
#include "PlotDataHub.h"
#include "AppConfig.h"
#include "SqlHistoryQuery.h"
#include "HistoryDataProvider.h"
#include "SelectionState.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QButtonGroup>
#include <QtConcurrent>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <limits>
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
    m_viewTypeLabel = new QLabel(QStringLiteral("视图:"), ctrlWidget);
    qDebug() << "created viewLabel" << m_viewTypeLabel;
    m_viewTypeCombo = new QComboBox(ctrlWidget);
    qDebug() << "created viewTypeCombo" << m_viewTypeCombo;
    m_viewTypeCombo->addItem("实部/虚部");
    m_viewTypeCombo->addItem("幅值/相位");
    m_viewTypeCombo->setVisible(false);
    m_viewTypeLabel->setVisible(false);
    ctrlLayout->addWidget(m_viewTypeLabel);
    ctrlLayout->addWidget(m_viewTypeCombo);
    ctrlLayout->addSpacing(12);  // 视图选择器与操作按钮分离，避免清屏紧贴 combo
    auto* clearBtn = new QPushButton(QStringLiteral("清屏"), ctrlWidget);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_clearTimeMs = QDateTime::currentMSecsSinceEpoch();
        auto clearPlotData = [](QCustomPlot* p) {
            if (!p) return;
            for (int i = 0; i < p->graphCount(); ++i)
                p->graph(i)->data()->clear();
            p->replot(QCustomPlot::rpQueuedReplot);
        };
        clearPlotData(m_plot);
        clearPlotData(m_mfTbPlot1);
        clearPlotData(m_mfTbPlot2);
        clearPlotData(m_mfImpedancePlot);
    });
    ctrlLayout->addWidget(clearBtn);
    ctrlLayout->addStretch();

    // 不再使用外部QListWidget控制通道显示隐藏，改用QCustomPlot图例交互
    // m_channelList已移除，不再使用

    // 初始化绘图控件
    m_plot = new QCustomPlot(this);
    PlotWindowBase::applyConfiguredOpenGl(m_plot);
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

    // 监听 SelectionState 以实现 review 模式切换
    auto* sel = SelectionState::instance();
    connect(sel, &SelectionState::selectionChanged,
            this, &PlotWindow::onSelectionChanged);
    // 启动时同步一次（如果已处于 Review 模式则初始化）
    if (sel->hasRange()) {
        m_reviewStartMs = sel->startMs();
        m_reviewEndMs = sel->endMs();
        m_reviewMode = (sel->mode() == SelectionState::Review);
    }

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

    // Review mode: render from cached m_reviewFrames instead of live snapshot
    if (m_reviewMode) {
        if (m_lastMode == FrameData::MultiFreqEddy && !m_reviewFrames.isEmpty()) {
            buildAndRenderReviewSnapshot();
        }
        // For non-MultiFreqEddy review mode, review loading not yet implemented; stay idle.
        return;
    }

    const FrameData::DetectionMode mode = snapshot->mode;
    const int ch = snapshot->channelCount;
    if (mode == FrameData::Legacy) {
        return;
    }
    if (mode != FrameData::MultiFreqEddy && ch <= 0) {
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
            m_viewTypeLabel->setVisible(false);
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
            m_viewTypeLabel->setVisible(true);
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
    } else if (mode == FrameData::MultiFreqEddy) {
        const int nPoints = snapshot->mfFreqPointCount;
        if (nPoints <= 0) return;

        // 切换到多频布局
        if (m_lastMode != mode) {
            if (m_plot) {
                if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
                    root->removeWidget(m_plot);
                }
                m_plot->setVisible(false);
            }
            if (m_viewTypeCombo) { m_viewTypeCombo->setVisible(false); m_viewTypeLabel->setVisible(false); }
        }
        if (m_lastMode != mode || m_currentChannelCount != nPoints) {
            m_currentChannelCount = nPoints;
            setupMultiFreqLayout(nPoints);
        }

        updateMultiFreqPlots(snapshot);
        // return early, skip standard axis range logic below
        m_lastMode = mode;
        m_mfTbPlot1->replot(QCustomPlot::rpQueuedReplot);
        m_mfTbPlot2->replot(QCustomPlot::rpQueuedReplot);
        m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
        return;
    } else {
        // 切换回标准单图布局
        if (m_mfSplitter) m_mfSplitter->setVisible(false);
        if (m_plot) {
            m_plot->setVisible(true);
            // 将 m_plot 重新加回布局（之前 removeWidget 移除了）
            if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
                if (root->indexOf(m_plot) < 0) {
                    root->insertWidget(1, m_plot, 1);  // 恢复到 ctrlWidget 之下
                }
            }
        }
        if (m_viewTypeCombo && mode == FrameData::MultiChannelComplex) {
            m_viewTypeCombo->setVisible(true);
            m_viewTypeLabel->setVisible(true);
        }
    }

    const double latestTime = snapshot->timeMs.last();
    if (mode == FrameData::MultiChannelComplex || mode == FrameData::MultiFreqEddy) {
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

// ---- Time base column factory (Y-reversed, time flows downward) ----
QWidget* PlotWindow::buildTimeBaseColumn(QCustomPlot*& plotOut)
{
    auto* col = new QWidget(m_mfSplitter);
    auto* layout = new QVBoxLayout(col);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    plotOut = new QCustomPlot(col);
    PlotWindowBase::applyConfiguredOpenGl(plotOut);
    styleMultiFreqPlot(plotOut);
    // X轴=数值，Y轴=时间（反转，自上而下）
    plotOut->yAxis->setRangeReversed(true);
    plotOut->yAxis->setTickLabelRotation(90);  // 纵轴刻度纵向显示，节省水平空间
    auto* dateTicker = new QCPAxisTickerDateTime;
    dateTicker->setDateTimeFormat(QStringLiteral("hh:mm:ss"));
    plotOut->yAxis->setTicker(QSharedPointer<QCPAxisTicker>(dateTicker));
    plotOut->axisRect()->setAutoMargins(QCP::msAll);
    plotOut->axisRect()->setMinimumMargins(QMargins(0, 0, 0, 0));
    plotOut->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    plotOut->legend->setVisible(false);
    plotOut->xAxis->grid()->setVisible(true);
    plotOut->yAxis->grid()->setVisible(true);

    layout->addWidget(plotOut, 1);
    return col;
}

// ---- MultiFreq layout: 3-column QSplitter ----
void PlotWindow::setupMultiFreqLayout(int freqPointCount)
{
    // 隐藏标准单图
    if (m_plot) m_plot->setVisible(false);
    if (m_viewTypeCombo) { m_viewTypeCombo->setVisible(false); m_viewTypeLabel->setVisible(false); }

    const bool firstTime = (m_mfSplitter == nullptr);

    if (firstTime) {
        // 三列 splitter
        m_mfSplitter = new QSplitter(Qt::Horizontal, this);
        m_mfSplitter->setHandleWidth(3);
        m_mfSplitter->setChildrenCollapsible(false);

        // 时基图1 + 时基图2
        QWidget* col1 = buildTimeBaseColumn(m_mfTbPlot1);
        QWidget* col2 = buildTimeBaseColumn(m_mfTbPlot2);
        m_mfTbPlot1->xAxis->setLabel(QStringLiteral("幅值 / 相位"));
        m_mfTbPlot1->yAxis->setLabel(QStringLiteral("时间 (ms)"));
        m_mfTbPlot1->yAxis->setRangeReversed(true);
        m_mfTbPlot2->xAxis->setLabel(QStringLiteral("实部 / 虚部"));
        m_mfTbPlot2->yAxis->setVisible(false);  // 与时基图1共享时间轴

        // 时基图1 控制栏：勾选实线(幅值) / 虚线(相位) 显隐
        {
            auto* bar = new QWidget(col1);
            auto* barLayout = new QHBoxLayout(bar);
            barLayout->setContentsMargins(0, 2, 0, 4);
            barLayout->setSpacing(8);
            m_mfTb1AmpCheck = new QCheckBox(QStringLiteral("幅值"), bar);
            m_mfTb1AmpCheck->setChecked(true);
            m_mfTb1PhaseCheck = new QCheckBox(QStringLiteral("相位"), bar);
            m_mfTb1PhaseCheck->setChecked(true);
            barLayout->addWidget(m_mfTb1AmpCheck);
            barLayout->addWidget(m_mfTb1PhaseCheck);
            barLayout->addStretch();
            qobject_cast<QVBoxLayout*>(col1->layout())->insertWidget(0, bar);
        }
        // 时基图2 控制栏：勾选实线(实部) / 虚线(虚部) 显隐
        {
            auto* bar = new QWidget(col2);
            auto* barLayout = new QHBoxLayout(bar);
            barLayout->setContentsMargins(0, 2, 0, 4);
            barLayout->setSpacing(8);
            m_mfTb2RealCheck = new QCheckBox(QStringLiteral("实部"), bar);
            m_mfTb2RealCheck->setChecked(true);
            m_mfTb2ImagCheck = new QCheckBox(QStringLiteral("虚部"), bar);
            m_mfTb2ImagCheck->setChecked(true);
            barLayout->addWidget(m_mfTb2RealCheck);
            barLayout->addWidget(m_mfTb2ImagCheck);
            barLayout->addStretch();
            qobject_cast<QVBoxLayout*>(col2->layout())->insertWidget(0, bar);
        }

        // 阻抗图列
        auto* impCol = new QWidget(m_mfSplitter);
        auto* impLayout = new QVBoxLayout(impCol);
        impLayout->setContentsMargins(2, 2, 2, 2);
        impLayout->setSpacing(4);

        m_mfImpedancePlot = new QCustomPlot(impCol);
        PlotWindowBase::applyConfiguredOpenGl(m_mfImpedancePlot);
        styleMultiFreqPlot(m_mfImpedancePlot);
        m_mfImpedancePlot->xAxis->setLabel(QStringLiteral("阻抗实部 (Ω)"));
        m_mfImpedancePlot->yAxis->setLabel(QStringLiteral("阻抗虚部 (Ω)"));
        m_mfImpedancePlot->xAxis->setRange(-1000, 1000);
        m_mfImpedancePlot->yAxis->setRange(-1000, 1000);
        m_mfImpedancePlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        m_mfImpedancePlot->legend->setVisible(true);
        m_mfImpedancePlot->legend->setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        m_mfImpedancePlot->legend->setSelectableParts(QCPLegend::spItems);
        m_mfImpedancePlot->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignRight | Qt::AlignTop);
        impLayout->addWidget(m_mfImpedancePlot, 1);

        // 第一行：全部配置控件（自适应/默认 + 阻抗类型 + 曲线保留 + 圆边界）
        auto* row1 = new QWidget(impCol);
        auto* row1Layout = new QHBoxLayout(row1);
        row1Layout->setContentsMargins(0, 0, 0, 0);
        row1Layout->setSpacing(6);
        m_mfAdaptiveRadio = new QRadioButton(QStringLiteral("自适应"), row1);
        m_mfDefaultRadio = new QRadioButton(QStringLiteral("默认 (-1000~1000)"), row1);
        m_mfDefaultRadio->setChecked(true);
        auto* scaleGroup = new QButtonGroup(row1);
        scaleGroup->addButton(m_mfAdaptiveRadio);
        scaleGroup->addButton(m_mfDefaultRadio);
        connect(scaleGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked),
                this, [this](QAbstractButton*) { applyImpedanceAxisMode(); });
        row1Layout->addWidget(m_mfAdaptiveRadio);
        row1Layout->addWidget(m_mfDefaultRadio);
        row1Layout->addSpacing(8);
        row1Layout->addWidget(new QLabel(QStringLiteral("阻抗:"), row1));
        m_mfRawRadio = new QRadioButton(QStringLiteral("原始"), row1);
        m_mfRawRadio->setChecked(true);
        m_mfNormRadio = new QRadioButton(QStringLiteral("归一化"), row1);
        auto* impTypeGroup = new QButtonGroup(row1);
        impTypeGroup->addButton(m_mfRawRadio);
        impTypeGroup->addButton(m_mfNormRadio);
        connect(impTypeGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked),
                this, [this](QAbstractButton* btn) {
                    m_mfUseNormalized = (btn == m_mfNormRadio);
                    auto snap = PlotDataHub::instance()->snapshot();
                    if (snap) updateMultiFreqPlots(snap);
                    if (m_mfImpedancePlot) m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
                });
        row1Layout->addWidget(m_mfRawRadio);
        row1Layout->addWidget(m_mfNormRadio);
        row1Layout->addSpacing(8);
        row1Layout->addWidget(new QLabel(QStringLiteral("曲线保留:"), row1));
        m_mfRetentionSpin = new QDoubleSpinBox(row1);
        m_mfRetentionSpin->setRange(0.1, 60.0);
        m_mfRetentionSpin->setDecimals(1);
        m_mfRetentionSpin->setSingleStep(1.0);
        m_mfRetentionSpin->setValue(3.0);
        m_mfRetentionSpin->setSuffix(QStringLiteral(" s"));
        m_mfRetentionSpin->setFixedWidth(80);
        connect(m_mfRetentionSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double v) {
                    m_mfRetentionSecs = v;
                    auto snap = PlotDataHub::instance()->snapshot();
                    if (snap) { updateMultiFreqPlots(snap); m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot); }
                });
        row1Layout->addWidget(m_mfRetentionSpin);
        row1Layout->addWidget(new QLabel(QStringLiteral("圆 R:"), row1));
        m_mfCircleRadiusSpin = new QDoubleSpinBox(row1);
        m_mfCircleRadiusSpin->setRange(0, 100000);
        m_mfCircleRadiusSpin->setDecimals(3);
        m_mfCircleRadiusSpin->setSingleStep(10);
        m_mfCircleRadiusSpin->setValue(500);
        m_mfCircleRadiusSpin->setFixedWidth(80);
        connect(m_mfCircleRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { updateCircleBoundary(); m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot); });
        row1Layout->addWidget(m_mfCircleRadiusSpin);
        m_mfCircleShowCheck = new QCheckBox(QStringLiteral("显示"), row1);
        m_mfCircleShowCheck->setChecked(false);
        connect(m_mfCircleShowCheck, &QCheckBox::toggled, this, &PlotWindow::onMfCircleToggled);
        row1Layout->addWidget(m_mfCircleShowCheck);
        row1Layout->addStretch();
        impLayout->addWidget(row1);

        // 第二行：频率勾选（整行）
        auto* freqRow = new QWidget(impCol);
        auto* freqRowLayout = new QHBoxLayout(freqRow);
        freqRowLayout->setContentsMargins(0, 0, 0, 0);
        freqRowLayout->setSpacing(4);
        freqRowLayout->addWidget(new QLabel(QStringLiteral("频率:"), freqRow));
        m_mfFreqCheckArea = new QScrollArea(freqRow);
        m_mfFreqCheckArea->setWidgetResizable(true);
        m_mfFreqCheckArea->setFixedHeight(28);
        m_mfFreqCheckArea->setFrameShape(QFrame::NoFrame);
        m_mfFreqCheckArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_mfFreqCheckContainer = new QWidget();
        m_mfFreqCheckLayout = new QHBoxLayout(m_mfFreqCheckContainer);
        m_mfFreqCheckLayout->setContentsMargins(0, 0, 0, 0);
        m_mfFreqCheckLayout->setSpacing(4);
        m_mfFreqCheckArea->setWidget(m_mfFreqCheckContainer);
        freqRowLayout->addWidget(m_mfFreqCheckArea, 1);
        impLayout->addWidget(freqRow);

        // 圆边框 ellipse（默认隐藏）
        m_mfCircleItem = new QCPItemEllipse(m_mfImpedancePlot);
        m_mfCircleItem->setPen(QPen(QColor(220, 60, 60), 1, Qt::DashLine));
        m_mfCircleItem->setBrush(Qt::NoBrush);
        m_mfCircleItem->setVisible(false);
        updateCircleBoundary();

        // 时基线型显隐勾选框连接
        auto tbLineToggled = [this]() {
            applyMultiFreqLineVisibility();
            if (m_mfTbPlot1) { m_mfTbPlot1->xAxis->rescale(true); m_mfTbPlot1->replot(QCustomPlot::rpQueuedReplot); }
            if (m_mfTbPlot2) { m_mfTbPlot2->xAxis->rescale(true); m_mfTbPlot2->replot(QCustomPlot::rpQueuedReplot); }
        };
        connect(m_mfTb1AmpCheck, &QCheckBox::toggled, this, tbLineToggled);
        connect(m_mfTb1PhaseCheck, &QCheckBox::toggled, this, tbLineToggled);
        connect(m_mfTb2RealCheck, &QCheckBox::toggled, this, tbLineToggled);
        connect(m_mfTb2ImagCheck, &QCheckBox::toggled, this, tbLineToggled);

        col1->setMinimumWidth(180);
        col2->setMinimumWidth(180);
        m_mfSplitter->addWidget(col1);
        m_mfSplitter->addWidget(col2);
        m_mfSplitter->addWidget(impCol);
        m_mfSplitter->setStretchFactor(0, 1);
        m_mfSplitter->setStretchFactor(1, 1);
        m_mfSplitter->setStretchFactor(2, 1);
        // 等分初始宽度
        const int w = m_mfSplitter->width();
        if (w > 0) m_mfSplitter->setSizes({w / 3, w / 3, w / 3});

        // 插入到根布局
        if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
            root->addWidget(m_mfSplitter, 1);
        }
    }

    rebuildMultiFreqGraphs(freqPointCount);
    m_mfSplitter->setVisible(true);
    if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
        root->activate();
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
    // 多频涡流三列
    if (m_mfTbPlot1) { styleMultiFreqPlot(m_mfTbPlot1); m_mfTbPlot1->replot(QCustomPlot::rpQueuedReplot); }
    if (m_mfTbPlot2) { styleMultiFreqPlot(m_mfTbPlot2); m_mfTbPlot2->replot(QCustomPlot::rpQueuedReplot); }
    if (m_mfImpedancePlot) { styleMultiFreqPlot(m_mfImpedancePlot); m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot); }
}

// ========== 多频涡流辅助方法 ==========

void PlotWindow::rebuildMultiFreqGraphs(int freqPointCount)
{
    // 时基图：清空 QCPGraph
    for (QCustomPlot* p : {m_mfTbPlot1, m_mfTbPlot2}) {
        if (p) p->clearGraphs();
    }
    // 阻抗图：清空圆滑曲线
    if (m_mfImpedancePlot) {
        m_mfImpedancePlot->clearGraphs();
        m_mfImpedanceCurves.clear();
        // 重新创建圆（clearGraphs 会删除 QCPItemEllipse）
        m_mfCircleItem = new QCPItemEllipse(m_mfImpedancePlot);
        m_mfCircleItem->setPen(QPen(QColor(220, 60, 60), 1, Qt::DashLine));
        m_mfCircleItem->setBrush(Qt::NoBrush);
        m_mfCircleItem->setVisible(m_mfCircleShowCheck ? m_mfCircleShowCheck->isChecked() : false);
        updateCircleBoundary();
    }

    // 清除旧的频率勾选
    for (auto* cb : m_mfFreqChecks) {
        if (m_mfFreqCheckLayout) m_mfFreqCheckLayout->removeWidget(cb);
        cb->deleteLater();
    }
    m_mfFreqChecks.clear();

    for (int i = 0; i < freqPointCount; ++i) {
        const QColor color = QColor::fromHsv((i * 47) % 360, 200, 200);
        const int freqNum = i + 1;

        // 时基图1：幅值(实线) + 相位(虚线) — Y轴=时间，X轴=数值
        {
            auto* gA = m_mfTbPlot1->addGraph(m_mfTbPlot1->yAxis, m_mfTbPlot1->xAxis);
            gA->setPen(QPen(color, 1.5));
            gA->setName(QStringLiteral("f%1 幅值").arg(freqNum));

            auto* gB = m_mfTbPlot1->addGraph(m_mfTbPlot1->yAxis, m_mfTbPlot1->xAxis);
            gB->setPen(QPen(color.lighter(130), 1.0, Qt::DashLine));
            gB->setName(QStringLiteral("f%1 相位").arg(freqNum));
        }

        // 时基图2：实部(实线) + 虚部(虚线) — Y轴=时间，X轴=数值
        {
            auto* gA = m_mfTbPlot2->addGraph(m_mfTbPlot2->yAxis, m_mfTbPlot2->xAxis);
            gA->setPen(QPen(color, 1.5));
            gA->setName(QStringLiteral("f%1 实部").arg(freqNum));

            auto* gB = m_mfTbPlot2->addGraph(m_mfTbPlot2->yAxis, m_mfTbPlot2->xAxis);
            gB->setPen(QPen(color.lighter(130), 1.0, Qt::DashLine));
            gB->setName(QStringLiteral("f%1 虚部").arg(freqNum));
        }

        // 阻抗图：平滑曲线 — X=实部，Y=虚部
        {
            auto* g = m_mfImpedancePlot->addGraph(
                m_mfImpedancePlot->xAxis, m_mfImpedancePlot->yAxis);
            g->setPen(QPen(color, 1.5));
            g->setSmooth(1);  // 圆滑贝塞尔曲线
            g->setScatterStyle(QCPScatterStyle::ssNone);
            g->setName(QStringLiteral("f%1").arg(freqNum));
            m_mfImpedanceCurves.append(g);
        }

        // 频率勾选（默认全选）
        auto* cb = new QCheckBox(QStringLiteral("f%1").arg(freqNum), m_mfFreqCheckContainer);
        cb->setChecked(true);
        connect(cb, &QCheckBox::toggled, this, &PlotWindow::onMfFreqCheckToggled);
        m_mfFreqChecks.append(cb);
        if (m_mfFreqCheckLayout) {
            m_mfFreqCheckLayout->addWidget(cb);
        }
    }
    // 强制容器更新布局，确保 QScrollArea 内可见
    if (m_mfFreqCheckContainer)
        m_mfFreqCheckContainer->adjustSize();

    // 应用线型显隐（复选框状态可能在重建之前已被用户切换）
    applyMultiFreqLineVisibility();
}

void PlotWindow::updateMultiFreqPlots(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    const int nPoints = snapshot->mfFreqPointCount;
    if (nPoints <= 0) return;

    const int n = snapshot->timeMs.size();
    if (n <= 0) return;

    // 滑动时间窗裁剪：Live 模式只显示最近 10 秒；Review 模式显示完整范围
    const double latest = snapshot->timeMs.last();
    const double plotStartMs = m_reviewMode ? snapshot->timeMs.first() : qMax(latest - 10000.0, static_cast<double>(m_clearTimeMs));
    int startIdx = 0;
    for (; startIdx < n && snapshot->timeMs[startIdx] < plotStartMs; ++startIdx) {}
    const int count = n - startIdx;

    // 时间值转换为秒（Unix epoch），配合 QCPAxisTickerDateTime 显示 HH:MM:SS
    QVector<double> timeRel(count);
    for (int i = 0; i < count; ++i) {
        timeRel[i] = snapshot->timeMs[startIdx + i] / 1000.0;
    }

    // 时基图1：幅值(实线) + 相位(虚线) — graph(value, time)
    for (int i = 0; i < nPoints && i < snapshot->mfImpedanceMag.size(); ++i) {
        const int idxA = i * 2;
        const int idxB = idxA + 1;
        const auto& magVec = snapshot->mfImpedanceMag[i];
        const auto& phaseVec = snapshot->mfImpedancePhase[i];
        QVector<double> magSlice(count), phaseSlice(count);
        for (int j = 0; j < count; ++j) {
            const int src = startIdx + j;
            magSlice[j] = (src < magVec.size()) ? magVec[src] : qQNaN();
            phaseSlice[j] = (src < phaseVec.size()) ? phaseVec[src] : qQNaN();
        }
        if (idxA < m_mfTbPlot1->graphCount()) {
            m_mfTbPlot1->graph(idxA)->setData(timeRel, magSlice, true);
        }
        if (idxB < m_mfTbPlot1->graphCount()) {
            m_mfTbPlot1->graph(idxB)->setData(timeRel, phaseSlice, true);
        }
    }
    m_mfTbPlot1->yAxis->setRange(plotStartMs / 1000.0, latest / 1000.0);
    {
        // 时基图1 X轴：仅按可见频点 rescale（NaN 不影响 QCustomPlot range finder）
        double xMin = std::numeric_limits<double>::max();
        double xMax = std::numeric_limits<double>::lowest();
        bool   any  = false;
        for (int i = 0; i < nPoints; ++i) {
            const bool vis = (i < m_mfFreqChecks.size()) ? m_mfFreqChecks[i]->isChecked() : true;
            if (!vis) continue;
            auto scan = [&](const QVector<QVector<double>>& arrs, int idx) {
                if (idx >= arrs.size()) return;
                for (int j = startIdx; j < arrs[idx].size() && j < n; ++j) {
                    const double v = arrs[idx][j];
                    if (std::isfinite(v)) { xMin = qMin(xMin, v); xMax = qMax(xMax, v); any = true; }
                }
            };
            const bool showAmp   = m_mfTb1AmpCheck   ? m_mfTb1AmpCheck->isChecked()   : true;
            const bool showPhase = m_mfTb1PhaseCheck ? m_mfTb1PhaseCheck->isChecked() : true;
            if (showAmp)   scan(snapshot->mfImpedanceMag, i);
            if (showPhase) scan(snapshot->mfImpedancePhase, i);
        }
        if (any) {
            const double margin = qMax((xMax - xMin) * 0.05, 1e-9);
            m_mfTbPlot1->xAxis->setRange(xMin - margin, xMax + margin);
        } else {
            m_mfTbPlot1->xAxis->rescale(true);
        }
    }

    // 时基图2：实部(实线) + 虚部(虚线)
    for (int i = 0; i < nPoints && i < snapshot->mfImpedanceReal.size(); ++i) {
        const int idxA = i * 2;
        const int idxB = idxA + 1;
        const auto& realVec = snapshot->mfImpedanceReal[i];
        const auto& imagVec = snapshot->mfImpedanceImag[i];
        QVector<double> realSlice(count), imagSlice(count);
        for (int j = 0; j < count; ++j) {
            const int src = startIdx + j;
            realSlice[j] = (src < realVec.size()) ? realVec[src] : qQNaN();
            imagSlice[j] = (src < imagVec.size()) ? imagVec[src] : qQNaN();
        }
        if (idxA < m_mfTbPlot2->graphCount()) {
            m_mfTbPlot2->graph(idxA)->setData(timeRel, realSlice, true);
        }
        if (idxB < m_mfTbPlot2->graphCount()) {
            m_mfTbPlot2->graph(idxB)->setData(timeRel, imagSlice, true);
        }
    }
    m_mfTbPlot2->yAxis->setRange(plotStartMs / 1000.0, latest / 1000.0);
    {
        // 时基图2 X轴：仅按可见频点 rescale
        double xMin = std::numeric_limits<double>::max();
        double xMax = std::numeric_limits<double>::lowest();
        bool   any  = false;
        for (int i = 0; i < nPoints; ++i) {
            const bool vis = (i < m_mfFreqChecks.size()) ? m_mfFreqChecks[i]->isChecked() : true;
            if (!vis) continue;
            auto scan = [&](const QVector<QVector<double>>& arrs, int idx) {
                if (idx >= arrs.size()) return;
                for (int j = startIdx; j < arrs[idx].size() && j < n; ++j) {
                    const double v = arrs[idx][j];
                    if (std::isfinite(v)) { xMin = qMin(xMin, v); xMax = qMax(xMax, v); any = true; }
                }
            };
            const bool showReal = m_mfTb2RealCheck ? m_mfTb2RealCheck->isChecked() : true;
            const bool showImag = m_mfTb2ImagCheck ? m_mfTb2ImagCheck->isChecked() : true;
            if (showReal) scan(snapshot->mfImpedanceReal, i);
            if (showImag) scan(snapshot->mfImpedanceImag, i);
        }
        if (any) {
            const double margin = qMax((xMax - xMin) * 0.05, 1e-9);
            m_mfTbPlot2->xAxis->setRange(xMin - margin, xMax + margin);
        } else {
            m_mfTbPlot2->xAxis->rescale(true);
        }
    }

    // 阻抗图：平滑曲线 — key=索引, X=实部, Y=虚部
    const QVector<QVector<double>>& impX =
        m_mfUseNormalized ? snapshot->mfNormImpedanceReal : snapshot->mfImpedanceReal;
    const QVector<QVector<double>>& impY =
        m_mfUseNormalized ? snapshot->mfNormImpedanceImag : snapshot->mfImpedanceImag;
    if (m_mfImpedancePlot) {
        m_mfImpedancePlot->xAxis->setLabel(m_mfUseNormalized
            ? QStringLiteral("归一化阻抗实部") : QStringLiteral("阻抗实部 (Ω)"));
        m_mfImpedancePlot->yAxis->setLabel(m_mfUseNormalized
            ? QStringLiteral("归一化阻抗虚部") : QStringLiteral("阻抗虚部 (Ω)"));
    }

    const double retentionSecs = m_reviewMode ? 0.0 : m_mfRetentionSecs * 1000.0;
    const double impCutoffMs = latest - retentionSecs;
    int impStartIdx = 0;
    if (!m_reviewMode) {
        for (; impStartIdx < n && snapshot->timeMs[impStartIdx] < impCutoffMs; ++impStartIdx) {}
    }

    for (int i = 0; i < nPoints && i < m_mfImpedanceCurves.size(); ++i) {
        if (i >= impX.size() || i >= impY.size()) continue;
        QCPGraph* g = m_mfImpedanceCurves[i];
        if (!g->visible()) continue;

        const auto& realVec = impX[i];
        const auto& imagVec = impY[i];
        const int pts = qMin(realVec.size(), imagVec.size());
        const int useCount = qMax(0, pts - impStartIdx);
        QVector<double> x(useCount), y(useCount);
        for (int j = 0; j < useCount; ++j) {
            x[j] = realVec[impStartIdx + j];
            y[j] = imagVec[impStartIdx + j];
        }
        g->setData(x, y, true);
    }

    // 自适应/默认模式委托给 applyImpedanceAxisMode（仅计算可见频点）
    applyImpedanceAxisMode();
}

void PlotWindow::applyImpedanceAxisMode()
{
    if (!m_mfImpedancePlot) return;
    if (m_mfDefaultRadio && m_mfDefaultRadio->isChecked()) {
        m_mfImpedancePlot->xAxis->setRange(-1000, 1000);
        m_mfImpedancePlot->yAxis->setRange(-1000, 1000);
    } else {
        // 自适应：仅根据可见频点的阻抗曲线计算范围
        double xMin = std::numeric_limits<double>::max();
        double xMax = std::numeric_limits<double>::lowest();
        double yMin = std::numeric_limits<double>::max();
        double yMax = std::numeric_limits<double>::lowest();
        bool any = false;
        for (int i = 0; i < m_mfImpedanceCurves.size(); ++i) {
            QCPGraph* g = m_mfImpedanceCurves[i];
            if (!g || !g->visible()) continue;
            auto dataPtr = g->data();
            if (!dataPtr || dataPtr->isEmpty()) continue;
            for (auto it = dataPtr->constBegin(); it != dataPtr->constEnd(); ++it) {
                const double k = it->key;
                const double v = it->value;
                if (std::isfinite(k) && std::isfinite(v)) {
                    xMin = qMin(xMin, k); xMax = qMax(xMax, k);
                    yMin = qMin(yMin, v); yMax = qMax(yMax, v);
                    any = true;
                }
            }
        }
        if (any) {
            const double xCenter = (xMin + xMax) / 2.0;
            const double yCenter = (yMin + yMax) / 2.0;
            const double half = qMax(xMax - xMin, yMax - yMin) * 0.6;
            const double margin = qMax(half, 1.0);
            m_mfImpedancePlot->xAxis->setRange(xCenter - margin, xCenter + margin);
            m_mfImpedancePlot->yAxis->setRange(yCenter - margin * 1.2, yCenter + margin * 1.2);
        } else {
            m_mfImpedancePlot->xAxis->setRange(-1000, 1000);
            m_mfImpedancePlot->yAxis->setRange(-1000, 1000);
        }
    }
    m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotWindow::applyMultiFreqLineVisibility()
{
    const bool showAmp   = m_mfTb1AmpCheck   ? m_mfTb1AmpCheck->isChecked()   : true;
    const bool showPhase = m_mfTb1PhaseCheck ? m_mfTb1PhaseCheck->isChecked() : true;
    const bool showReal  = m_mfTb2RealCheck  ? m_mfTb2RealCheck->isChecked()  : true;
    const bool showImag  = m_mfTb2ImagCheck  ? m_mfTb2ImagCheck->isChecked()  : true;
    const int  nFreq     = m_mfFreqChecks.size();

    if (m_mfTbPlot1) {
        for (int i = 0; i < m_mfTbPlot1->graphCount(); ++i) {
            // 偶数索引 = 幅值(实线)，奇数索引 = 相位(虚线)
            // 频率勾选 AND 线型勾选 同时满足才可见
            const int  freqIdx = i / 2;
            const bool freqVis = (freqIdx < nFreq) ? m_mfFreqChecks[freqIdx]->isChecked() : true;
            const bool lineVis = (i % 2 == 0) ? showAmp : showPhase;
            m_mfTbPlot1->graph(i)->setVisible(freqVis && lineVis);
        }
    }
    if (m_mfTbPlot2) {
        for (int i = 0; i < m_mfTbPlot2->graphCount(); ++i) {
            // 偶数索引 = 实部(实线)，奇数索引 = 虚部(虚线)
            const int  freqIdx = i / 2;
            const bool freqVis = (freqIdx < nFreq) ? m_mfFreqChecks[freqIdx]->isChecked() : true;
            const bool lineVis = (i % 2 == 0) ? showReal : showImag;
            m_mfTbPlot2->graph(i)->setVisible(freqVis && lineVis);
        }
    }
}

void PlotWindow::updateCircleBoundary()
{
    if (!m_mfCircleItem || !m_mfCircleRadiusSpin) return;
    const double r = m_mfCircleRadiusSpin->value();
    m_mfCircleItem->topLeft->setCoords(-r, r);
    m_mfCircleItem->bottomRight->setCoords(r, -r);
}

void PlotWindow::styleMultiFreqPlot(QCustomPlot* p)
{
    if (!p) return;
    applyThemeToPlot(p, isDarkThemeActive());
}

void PlotWindow::onMfFreqCheckToggled()
{
    const int nFreq = m_mfFreqChecks.size();

    // 1) 阻抗图曲线显隐（仅受频率勾选控制，无独立的线型复选框）
    for (int i = 0; i < nFreq && i < m_mfImpedanceCurves.size(); ++i) {
        m_mfImpedanceCurves[i]->setVisible(m_mfFreqChecks[i]->isChecked());
    }

    // 2) 时基图：统一显隐逻辑（频率勾选 AND 线型勾选）
    applyMultiFreqLineVisibility();

    // 3) 重绘三列
    if (m_mfImpedancePlot) m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
    if (m_mfTbPlot1)       m_mfTbPlot1->replot(QCustomPlot::rpQueuedReplot);
    if (m_mfTbPlot2)       m_mfTbPlot2->replot(QCustomPlot::rpQueuedReplot);

    // 4) 自适应模式下按可见频点重新调整坐标轴
    applyImpedanceAxisMode();
    if (m_mfTbPlot1) m_mfTbPlot1->xAxis->rescale(true);
    if (m_mfTbPlot2) m_mfTbPlot2->xAxis->rescale(true);
}

void PlotWindow::onMfCircleToggled()
{
    if (!m_mfCircleItem || !m_mfCircleShowCheck) return;
    m_mfCircleItem->setVisible(m_mfCircleShowCheck->isChecked());
    if (m_mfImpedancePlot) m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
}

// ========== Review mode ==========

void PlotWindow::onSelectionChanged(qint64 startMs, qint64 endMs, int mode)
{
    m_reviewStartMs = startMs;
    m_reviewEndMs = endMs;
    const bool nowReview = (mode == SelectionState::Review);

    if (nowReview) {
        qDebug() << "[PlotWindow] onSelectionChanged Review mode, startMs=" << startMs << "endMs=" << endMs
                 << "m_lastMode=" << static_cast<int>(m_lastMode);
        // 始终探测 DB 中 multifreq_frames 是否有数据
        // 不信任 m_lastMode（可能为 Legacy 或来自其他会话的陈旧值）
        bool hasMultiFreqData = false;
        {
            auto* hdp = HistoryDataProvider::instance();
            qDebug() << "[PlotWindow] probing DB: hdp=" << (hdp ? "ok" : "null")
                     << "isOpen=" << (hdp ? hdp->isDatabaseOpen() : false);
            if (hdp && hdp->isDatabaseOpen()) {
                const auto rows = hdp->fetchMultiFreqRawChunk(startMs, endMs, startMs - 1,
                    std::numeric_limits<qint64>::min(), 1);
                hasMultiFreqData = !rows.isEmpty();
                qDebug() << "[PlotWindow] DB probe result: rows.size=" << rows.size()
                         << "hasMultiFreqData=" << hasMultiFreqData;
            }
        }
        if (hasMultiFreqData) {
            qDebug() << "[PlotWindow] calling loadMultiFreqReviewFromDb";
            m_reviewLoadCanceled.storeRelaxed(1); // 取消上一次仍在跑的异步加载
            m_reviewMode = true;  // 必须在异步加载前设置，与 MagArray/PulseEddy 一致
            loadMultiFreqReviewFromDb();
        } else {
            qDebug() << "[PlotWindow] no MultiFreq data detected, skipping review load";
        }
        // For other modes, review loading not yet implemented
    } else {
        // Return to live mode
        m_reviewLoadCanceled.storeRelaxed(1); // 取消正在运行的异步加载
        m_reviewFrames.clear();
        m_reviewMode = false;
        // Force re-render from current live snapshot
        auto snap = PlotDataHub::instance()->snapshot();
        if (snap) {
            m_lastSnapshotVersion = 0; // force update even if version unchanged
            updatePlotDataFromSnapshot(snap);
        }
    }
}

void PlotWindow::loadMultiFreqReviewFromDb()
{
    auto* hdp = HistoryDataProvider::instance();
    if (!hdp || !hdp->isDatabaseOpen()) {
        qDebug() << "[PlotWindow::loadReview] hdp null or not open";
        return;
    }
    qDebug() << "[PlotWindow::loadReview] start, path=" << hdp->currentDatabasePath();

    m_reviewFrames.clear();
    m_reviewLoadCanceled.storeRelaxed(0); // 新加载开始，清除取消标志

    const qint64 startMs = m_reviewStartMs;
    const qint64 endMs = m_reviewEndMs;
    const quint64 epoch = ++m_reviewEpoch;

    // 更新窗口标题提示加载中
    m_loadingTitle = windowTitle();
    setWindowTitle(m_loadingTitle + QStringLiteral(" — 加载中..."));

    // 异步加载（QPointer 防止窗口销毁后回调崩溃）
    QPointer<PlotWindow> self(this);
    QtConcurrent::run([self, startMs, endMs, epoch]() {
        // 创建独立的 SqlHistoryQuery（不同的连接名，避免与主连接冲突）
        SqlHistoryQuery query;
        const QString dbPath = HistoryDataProvider::instance()->currentDatabasePath();
        qDebug() << "[PlotWindow::loadReview] bg thread: dbPath=" << dbPath;
        if (dbPath.isEmpty()) return;
        if (!query.open(dbPath)) {
            qDebug() << "[PlotWindow::loadReview] bg thread: open failed";
            return;
        }
        qDebug() << "[PlotWindow::loadReview] bg thread: DB opened, querying...";

        const qint64 totalRows = query.estimateMultiFreqRowCount(startMs, endMs);
        qDebug() << "[PlotWindow::loadReview] bg: totalRows estimate=" << totalRows
                 << "startMs=" << startMs << "endMs=" << endMs;
        const int maxFrames = 5000;
        const int stride = qMax(1, static_cast<int>(totalRows / maxFrames));

        QMap<qint64, QVector<SqlHistoryQuery::MultiFreqFrameRow>> frameGroups;
        QSet<int> allFactors;  // 从全部行收集，不受 stride 采样影响，保证频率曲线数量稳定
        QSet<qint64> seenFrames; // 跟踪已见过的 frameIndex，用于按帧采样
        qint64 lastTs = startMs - 1;
        qint64 lastRowId = std::numeric_limits<qint64>::min();
        // stride 应用于帧而非行：每帧含多个频率因子行，按行采样会导致帧数据不完整
        const int avgFreqPerFrame = qMax(1, static_cast<int>(allFactors.size() > 0 ? allFactors.size() : 4));
        const qint64 estFrameCount = totalRows / avgFreqPerFrame;
        const int frameStride = qMax(1, static_cast<int>(estFrameCount / maxFrames));
        int frameSeq = 0;

        while (true) {
            // 检查取消标志（用户拖动了新范围或切换回 Live）
            if (self && self->m_reviewLoadCanceled.loadRelaxed()) {
                qDebug() << "[PlotWindow::loadReview] bg: cancelled";
                return;
            }
            const auto rows = query.fetchMultiFreqRawChunk(startMs, endMs, lastTs, lastRowId, 500);
            if (rows.isEmpty()) break;

            for (const auto& r : rows) {
                allFactors.insert(r.frequencyFactor);  // 全量收集，保证 f1-f4 完整体现

                // 按帧采样：首次见到新 frameIndex 时判断是否采样该帧
                if (!seenFrames.contains(r.frameIndex)) {
                    seenFrames.insert(r.frameIndex);
                    if (frameSeq++ % frameStride == 0) {
                        frameGroups[r.frameIndex] = {}; // 标记该帧需要收集完整数据
                    }
                }
                // 被选中的帧收集其所有频率因子行
                if (frameGroups.contains(r.frameIndex)) {
                    frameGroups[r.frameIndex].append(r);
                }
            }
            lastTs = rows.last().timestampMs;
            lastRowId = rows.last().rowId;
        }

        // 构建完成后再次检查取消
        if (self && self->m_reviewLoadCanceled.loadRelaxed()) {
            qDebug() << "[PlotWindow::loadReview] bg: cancelled after build";
            return;
        }

        // 按 frameIndex 排序组装
        QVector<FrameRecord> results;
        for (auto it = frameGroups.constBegin(); it != frameGroups.constEnd(); ++it) {
            const auto& group = it.value();
            FrameRecord rec;
            rec.timestampMs = group.first().timestampMs;
            rec.sequence = group.first().frameIndex;

            for (const auto& mfRow : group) {
                MultiFreqPointResult pt;
                pt.frequencyFactor = mfRow.frequencyFactor;
                pt.frequencyHz = mfRow.frequencyHz;
                pt.impedanceReal_raw = mfRow.impedanceReal;
                pt.impedanceImag_raw = mfRow.impedanceImag;
                pt.impedanceMagnitude = mfRow.impedanceMagnitude;
                pt.impedancePhaseDeg = mfRow.impedancePhaseDeg;
                pt.normalizedImpedanceReal = mfRow.normImpedanceReal;
                pt.normalizedImpedanceImag = mfRow.normImpedanceImag;
                pt.voltageMagnitude = mfRow.voltageMag;
                pt.currentMagnitude = mfRow.currentMag;
                pt.valid = mfRow.valid;
                rec.mfFreqPoints.append(pt);
            }
            results.append(rec);
        }

        // 回主线程
        qDebug() << "[PlotWindow::loadReview] bg: built" << results.size() << "frames, posting to main thread";
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, epoch, results = std::move(results), allFactors = std::move(allFactors)]() {
            if (!self || epoch != self->m_reviewEpoch || !self->m_reviewMode) {
                qDebug() << "[PlotWindow::loadReview] main: stale, destroyed, or no longer in review";
                return;
            }
            qDebug() << "[PlotWindow::loadReview] main: applying" << results.size() << "frames, m_lastMode=" << static_cast<int>(self->m_lastMode);
            self->m_reviewFrames = results;
            self->m_reviewAllFactors = allFactors;
            self->m_reviewMode = true;
            // 恢复窗口标题
            self->setWindowTitle(self->m_loadingTitle.isEmpty() ? self->windowTitle() : self->m_loadingTitle);
            // 不检查 m_lastMode：数据已确认是多频类型才进入此流程
            self->buildAndRenderReviewSnapshot();
        }, Qt::QueuedConnection);
    });
}

void PlotWindow::buildAndRenderReviewSnapshot()
{
    // 优先使用 DB 全量扫描的频率因子集合，保证拖动范围时曲线数量稳定
    // 回退：从采样帧中收集（re-render 路径）
    QSet<int> factors;
    if (!m_reviewAllFactors.isEmpty()) {
        factors = m_reviewAllFactors;
    } else {
        for (const auto& rec : m_reviewFrames) {
            for (const auto& pt : rec.mfFreqPoints) {
                factors.insert(pt.frequencyFactor);
            }
        }
    }
    QList<int> sortedFactors = factors.values();
    std::sort(sortedFactors.begin(), sortedFactors.end());
    const int nPoints = sortedFactors.size();
    if (nPoints <= 0) return;

    // 构建临时 PlotSnapshot
    QSharedPointer<PlotSnapshot> snap = QSharedPointer<PlotSnapshot>::create();
    snap->mode = FrameData::MultiFreqEddy;
    snap->mfFreqPointCount = nPoints;
    snap->mfFreqFactors = sortedFactors.toVector();
    snap->mfImpedanceReal.resize(nPoints);
    snap->mfImpedanceImag.resize(nPoints);
    snap->mfImpedanceMag.resize(nPoints);
    snap->mfImpedancePhase.resize(nPoints);
    snap->mfNormImpedanceReal.resize(nPoints);
    snap->mfNormImpedanceImag.resize(nPoints);

    for (const auto& rec : m_reviewFrames) {
        snap->timeMs.append(static_cast<double>(rec.timestampMs));
        for (int i = 0; i < nPoints; ++i) {
            int factor = sortedFactors[i];
            bool found = false;
            for (const auto& pt : rec.mfFreqPoints) {
                if (pt.frequencyFactor == factor) {
                    snap->mfImpedanceReal[i].append(pt.impedanceReal_raw);
                    snap->mfImpedanceImag[i].append(pt.impedanceImag_raw);
                    snap->mfImpedanceMag[i].append(pt.impedanceMagnitude);
                    snap->mfImpedancePhase[i].append(pt.impedancePhaseDeg);
                    snap->mfNormImpedanceReal[i].append(pt.normalizedImpedanceReal);
                    snap->mfNormImpedanceImag[i].append(pt.normalizedImpedanceImag);
                    found = true;
                    break;
                }
            }
            if (!found) {
                snap->mfImpedanceReal[i].append(qQNaN());
                snap->mfImpedanceImag[i].append(qQNaN());
                snap->mfImpedanceMag[i].append(qQNaN());
                snap->mfImpedancePhase[i].append(qQNaN());
                snap->mfNormImpedanceReal[i].append(qQNaN());
                snap->mfNormImpedanceImag[i].append(qQNaN());
            }
        }
    }

    // 确保多频布局已建立（与 updatePlotDataFromSnapshot 中 MultiFreqEddy 分支一致）
    if (m_lastMode != FrameData::MultiFreqEddy) {
        if (m_plot) {
            if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
                root->removeWidget(m_plot);
            }
            m_plot->setVisible(false);
        }
        if (m_viewTypeCombo) { m_viewTypeCombo->setVisible(false); m_viewTypeLabel->setVisible(false); }
    }
    if (m_lastMode != FrameData::MultiFreqEddy || m_currentChannelCount != nPoints) {
        m_currentChannelCount = nPoints;
        setupMultiFreqLayout(nPoints);
    }

    m_lastMode = FrameData::MultiFreqEddy;

    // 重用现有渲染逻辑
    updateMultiFreqPlots(snap);
    if (m_mfTbPlot1) m_mfTbPlot1->replot(QCustomPlot::rpQueuedReplot);
    if (m_mfTbPlot2) m_mfTbPlot2->replot(QCustomPlot::rpQueuedReplot);
    if (m_mfImpedancePlot) m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
}
