#include "PlotWindow.h"
#include "PlotDataHub.h"
#include "AppConfig.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QButtonGroup>
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
            if (m_viewTypeCombo) m_viewTypeCombo->setVisible(false);
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
    if (m_viewTypeCombo) m_viewTypeCombo->setVisible(false);

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
        m_mfTbPlot2->yAxis->setLabel(QStringLiteral("时间 (ms)"));
        m_mfTbPlot2->yAxis->setRangeReversed(true);

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

        // 阻抗图控件行
        // 自适应/默认
        auto* scaleRow = new QWidget(impCol);
        auto* scaleLayout = new QHBoxLayout(scaleRow);
        scaleLayout->setContentsMargins(0, 0, 0, 0);
        m_mfAdaptiveRadio = new QRadioButton(QStringLiteral("自适应"), scaleRow);
        m_mfDefaultRadio = new QRadioButton(QStringLiteral("默认 (-1000~1000)"), scaleRow);
        m_mfDefaultRadio->setChecked(true);
        auto* scaleGroup = new QButtonGroup(scaleRow);
        scaleGroup->addButton(m_mfAdaptiveRadio);
        scaleGroup->addButton(m_mfDefaultRadio);
        connect(scaleGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked),
                this, [this](QAbstractButton*) { applyImpedanceAxisMode(); });
        scaleLayout->addWidget(m_mfAdaptiveRadio);
        scaleLayout->addWidget(m_mfDefaultRadio);
        scaleLayout->addSpacing(12);
        auto* impTypeLabel = new QLabel(QStringLiteral("阻抗:"), scaleRow);
        scaleLayout->addWidget(impTypeLabel);
        m_mfRawRadio = new QRadioButton(QStringLiteral("原始"), scaleRow);
        m_mfRawRadio->setChecked(true);
        m_mfNormRadio = new QRadioButton(QStringLiteral("归一化"), scaleRow);
        auto* impTypeGroup = new QButtonGroup(scaleRow);
        impTypeGroup->addButton(m_mfRawRadio);
        impTypeGroup->addButton(m_mfNormRadio);
        connect(impTypeGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked),
                this, [this](QAbstractButton* btn) {
                    m_mfUseNormalized = (btn == m_mfNormRadio);
                    auto snap = PlotDataHub::instance()->snapshot();
                    if (snap) updateMultiFreqPlots(snap);
                    if (m_mfImpedancePlot) m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
                });
        scaleLayout->addWidget(m_mfRawRadio);
        scaleLayout->addWidget(m_mfNormRadio);
        scaleLayout->addStretch();
        impLayout->addWidget(scaleRow);

        // 频率勾选
        auto* freqRow = new QWidget(impCol);
        auto* freqRowLayout = new QHBoxLayout(freqRow);
        freqRowLayout->setContentsMargins(0, 0, 0, 0);
        freqRowLayout->addWidget(new QLabel(QStringLiteral("频率:"), freqRow));
        m_mfFreqCheckArea = new QScrollArea(freqRow);
        m_mfFreqCheckArea->setFixedHeight(32);
        m_mfFreqCheckArea->setFrameShape(QFrame::NoFrame);
        m_mfFreqCheckArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_mfFreqCheckContainer = new QWidget();
        m_mfFreqCheckLayout = new QHBoxLayout(m_mfFreqCheckContainer);
        m_mfFreqCheckLayout->setContentsMargins(0, 0, 0, 0);
        m_mfFreqCheckLayout->setSpacing(4);
        m_mfFreqCheckArea->setWidget(m_mfFreqCheckContainer);
        freqRowLayout->addWidget(m_mfFreqCheckArea, 1);
        impLayout->addWidget(freqRow);

        // 曲线保留时间
        auto* retentionRow = new QWidget(impCol);
        auto* retentionLayout = new QHBoxLayout(retentionRow);
        retentionLayout->setContentsMargins(0, 0, 0, 0);
        retentionLayout->addWidget(new QLabel(QStringLiteral("曲线保留:"), retentionRow));
        m_mfRetentionSpin = new QDoubleSpinBox(retentionRow);
        m_mfRetentionSpin->setRange(0.1, 60.0);
        m_mfRetentionSpin->setDecimals(1);
        m_mfRetentionSpin->setSingleStep(1.0);
        m_mfRetentionSpin->setValue(3.0);
        m_mfRetentionSpin->setSuffix(QStringLiteral(" s"));
        connect(m_mfRetentionSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double v) {
                    m_mfRetentionSecs = v;
                    auto snap = PlotDataHub::instance()->snapshot();
                    if (snap) { updateMultiFreqPlots(snap); m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot); }
                });
        retentionLayout->addWidget(m_mfRetentionSpin);
        retentionLayout->addStretch();
        impLayout->addWidget(retentionRow);

        // 圆边框
        auto* circleRow = new QWidget(impCol);
        auto* circleLayout = new QHBoxLayout(circleRow);
        circleLayout->setContentsMargins(0, 0, 0, 0);
        circleLayout->addWidget(new QLabel(QStringLiteral("圆边界 R:"), circleRow));
        m_mfCircleRadiusSpin = new QDoubleSpinBox(circleRow);
        m_mfCircleRadiusSpin->setRange(0, 100000);
        m_mfCircleRadiusSpin->setDecimals(1);
        m_mfCircleRadiusSpin->setSingleStep(10);
        m_mfCircleRadiusSpin->setValue(500);
        connect(m_mfCircleRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { updateCircleBoundary(); m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot); });
        circleLayout->addWidget(m_mfCircleRadiusSpin);
        m_mfCircleShowCheck = new QCheckBox(QStringLiteral("显示"), circleRow);
        m_mfCircleShowCheck->setChecked(false);
        connect(m_mfCircleShowCheck, &QCheckBox::toggled, this, &PlotWindow::onMfCircleToggled);
        circleLayout->addWidget(m_mfCircleShowCheck);
        circleLayout->addStretch();
        impLayout->addWidget(circleRow);

        // 圆边框 ellipse（默认隐藏）
        m_mfCircleItem = new QCPItemEllipse(m_mfImpedancePlot);
        m_mfCircleItem->setPen(QPen(QColor(220, 60, 60), 1, Qt::DashLine));
        m_mfCircleItem->setBrush(Qt::NoBrush);
        m_mfCircleItem->setVisible(false);
        updateCircleBoundary();

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
    // 阻抗图：清空 QCPCurve（用 clearPlottables 而非 clearGraphs）
    if (m_mfImpedancePlot) {
        m_mfImpedancePlot->clearPlottables();
        m_mfImpedanceCurves.clear();
        // 重新创建圆（clearPlottables 会删除）
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

        // 阻抗图：QCPCurve 轨迹 — X=实部，Y=虚部
        {
            auto* curve = new QCPCurve(m_mfImpedancePlot->xAxis, m_mfImpedancePlot->yAxis);
            curve->setPen(QPen(color, 1.5));
            curve->setName(QStringLiteral("f%1").arg(freqNum));
            m_mfImpedanceCurves.append(curve);
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
}

void PlotWindow::updateMultiFreqPlots(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    const int nPoints = snapshot->mfFreqPointCount;
    if (nPoints <= 0) return;

    const int n = snapshot->timeMs.size();
    if (n <= 0) return;

    // 滑动时间窗裁剪
    const double latest = snapshot->timeMs.last();
    const double windowMs = 10000.0;
    const double windowStart = latest - windowMs;
    int startIdx = 0;
    for (; startIdx < n && snapshot->timeMs[startIdx] < windowStart; ++startIdx) {}
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
    m_mfTbPlot1->yAxis->setRange(windowStart / 1000.0, latest / 1000.0);
    m_mfTbPlot1->xAxis->rescale();

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
    m_mfTbPlot2->yAxis->setRange(windowStart / 1000.0, latest / 1000.0);
    m_mfTbPlot2->xAxis->rescale();

    // 阻抗图：QCPCurve 完整轨迹 — key=索引, X=实部, Y=虚部
    const QVector<QVector<double>>& impX =
        m_mfUseNormalized ? snapshot->mfNormImpedanceReal : snapshot->mfImpedanceReal;
    const QVector<QVector<double>>& impY =
        m_mfUseNormalized ? snapshot->mfNormImpedanceImag : snapshot->mfImpedanceImag;
    // 更新轴标签
    if (m_mfImpedancePlot) {
        m_mfImpedancePlot->xAxis->setLabel(m_mfUseNormalized
            ? QStringLiteral("归一化阻抗实部") : QStringLiteral("阻抗实部 (Ω)"));
        m_mfImpedancePlot->yAxis->setLabel(m_mfUseNormalized
            ? QStringLiteral("归一化阻抗虚部") : QStringLiteral("阻抗虚部 (Ω)"));
    }

    // 阻抗曲线保留时间窗口
    const double retentionSecs = m_mfRetentionSecs * 1000.0; // 转为 ms
    const double cutoffMs = latest - retentionSecs;
    int impStartIdx = 0;
    for (; impStartIdx < n && snapshot->timeMs[impStartIdx] < cutoffMs; ++impStartIdx) {}

    for (int i = 0; i < nPoints && i < m_mfImpedanceCurves.size(); ++i) {
        if (i >= impX.size() || i >= impY.size()) continue;
        QCPCurve* curve = m_mfImpedanceCurves[i];
        if (!curve->visible()) continue;

        const auto& realVec = impX[i];
        const auto& imagVec = impY[i];
        const int pts = qMin(realVec.size(), imagVec.size());
        const int useCount = qMax(0, pts - impStartIdx);
        QVector<double> t(useCount), x(useCount), y(useCount);
        for (int j = 0; j < useCount; ++j) {
            t[j] = j;
            x[j] = realVec[impStartIdx + j];
            y[j] = imagVec[impStartIdx + j];
        }
        curve->setData(t, x, y, true);
    }

    // 自适应模式
    if (m_mfAdaptiveRadio && m_mfAdaptiveRadio->isChecked()) {
        m_mfImpedancePlot->rescaleAxes();
    }
}

void PlotWindow::applyImpedanceAxisMode()
{
    if (!m_mfImpedancePlot) return;
    if (m_mfDefaultRadio && m_mfDefaultRadio->isChecked()) {
        m_mfImpedancePlot->xAxis->setRange(-1000, 1000);
        m_mfImpedancePlot->yAxis->setRange(-1000, 1000);
    } else {
        m_mfImpedancePlot->rescaleAxes();
    }
    m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
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
    if (!m_mfImpedancePlot) return;
    for (int i = 0; i < m_mfFreqChecks.size() && i < m_mfImpedanceCurves.size(); ++i) {
        m_mfImpedanceCurves[i]->setVisible(m_mfFreqChecks[i]->isChecked());
    }
    m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotWindow::onMfCircleToggled()
{
    if (!m_mfCircleItem || !m_mfCircleShowCheck) return;
    m_mfCircleItem->setVisible(m_mfCircleShowCheck->isChecked());
    if (m_mfImpedancePlot) m_mfImpedancePlot->replot(QCustomPlot::rpQueuedReplot);
}
