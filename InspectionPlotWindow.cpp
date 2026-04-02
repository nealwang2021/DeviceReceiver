#include "InspectionPlotWindow.h"
#include "PlotDataHub.h"
#include "AppConfig.h"
#include "qcustomplot.h"

#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QFrame>
#include <QPainter>
#include <QPixmap>
#include <QDebug>
#include <cmath>
#include <algorithm>

namespace {
constexpr int kDefaultImpedanceRange = 1000;
constexpr int kMaxChannelsPerGroup   = 256;
constexpr int kMinChannelsPerGroup   = 1;
constexpr double kTimeWindowMs       = 10000.0;

const QColor kPlotBg("#FFFFFF");
const QColor kGridMajor("#E0E0E0");
const QColor kGridMinor("#F0F0F0");
const QColor kAxisLine("#999999");
const QColor kAxisLabel("#555555");
} // namespace

// ============================================================================
// Helpers
// ============================================================================

QColor InspectionPlotWindow::colorForChannel(int ch)
{
    const int h = (ch * 47) % 360;
    return QColor::fromHsv(h, 200, 200);
}

QPixmap InspectionPlotWindow::lineStyleIcon(Qt::PenStyle style, const QColor& color, int w, int h)
{
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2.0, style));
    p.drawLine(2, h / 2, w - 2, h / 2);
    return pm;
}

// ============================================================================
// Construction
// ============================================================================

InspectionPlotWindow::InspectionPlotWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("检测分析"));
    resize(1400, 700);

    if (AppConfig* cfg = AppConfig::instance())
        m_channelsPerGroup = qBound(kMinChannelsPerGroup,
                                    cfg->inspectionChannelsPerGroup(),
                                    kMaxChannelsPerGroup);
    rebuildUi();
    m_replotThrottle.start();
}

InspectionPlotWindow::~InspectionPlotWindow() = default;

void InspectionPlotWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this]() { applyTimeBaseModeLayout(); });
}

// ============================================================================
// UI Construction — split into focused helpers
// ============================================================================

void InspectionPlotWindow::rebuildUi()
{
    // --- root layout ---
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(2);

    m_topBar = buildTopBar();
    rootLayout->addWidget(m_topBar);

    m_legendBar = buildLegendBar();
    rootLayout->addWidget(m_legendBar);

    // --- three-column splitter ---
    m_plotSplitter = new QSplitter(Qt::Horizontal, this);
    m_plotSplitter->setHandleWidth(3);
    m_plotSplitter->setChildrenCollapsible(false);

    m_timeCol1 = buildTimeBaseColumn(m_tbPlot1);
    m_tbPlot1->xAxis->setLabel(QStringLiteral("时间 (ms)"));
    m_tbPlot1->yAxis->setLabel(QStringLiteral("数值"));

    m_timeCol2 = buildTimeBaseColumn(m_tbPlot2);
    m_tbPlot2->xAxis->setLabel(QStringLiteral("时间 (ms)"));
    m_tbPlot2->yAxis->setLabel(QStringLiteral("数值"));

    m_impedanceCol = buildImpedanceColumn();

    m_plotSplitter->addWidget(m_timeCol1);
    m_plotSplitter->addWidget(m_timeCol2);
    m_plotSplitter->addWidget(m_impedanceCol);
    m_plotSplitter->setStretchFactor(0, 1);
    m_plotSplitter->setStretchFactor(1, 1);
    m_plotSplitter->setStretchFactor(2, 2);

    rootLayout->addWidget(m_plotSplitter, 1);
    setLayout(rootLayout);

    // circle boundary item (hidden initially)
    m_circleItem = new QCPItemEllipse(m_impedancePlot);
    m_circleItem->setVisible(false);
    m_circleItem->setPen(QPen(Qt::red, 2, Qt::DashLine));
    updateCircleBoundary();

    setupConnections();
    applyTimeBaseModeLayout();
}

// ---- Top bar: group selector + per-group channel checkboxes ----

QWidget* InspectionPlotWindow::buildTopBar()
{
    QWidget* bar = new QWidget(this);
    bar->setObjectName("inspectionTopBar");
    QHBoxLayout* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(4, 2, 4, 2);
    lay->setSpacing(6);

    lay->addWidget(new QLabel(QStringLiteral("组:")));
    m_groupCombo = new QComboBox(bar);
    m_groupCombo->setMinimumWidth(120);
    lay->addWidget(m_groupCombo);

    lay->addWidget(new QLabel(QStringLiteral("每组通道:")));
    m_chPerGroupSpin = new QSpinBox(bar);
    m_chPerGroupSpin->setRange(kMinChannelsPerGroup, kMaxChannelsPerGroup);
    m_chPerGroupSpin->setValue(m_channelsPerGroup);
    lay->addWidget(m_chPerGroupSpin);

    m_channelCheckArea = new QScrollArea(bar);
    m_channelCheckArea->setWidgetResizable(true);
    m_channelCheckArea->setFixedHeight(32);
    m_channelCheckArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_channelCheckArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_channelCheckArea->setFrameShape(QFrame::NoFrame);
    m_channelCheckContainer = new QWidget();
    m_channelCheckLayout = new QHBoxLayout(m_channelCheckContainer);
    m_channelCheckLayout->setContentsMargins(0, 0, 0, 0);
    m_channelCheckLayout->setSpacing(4);
    m_channelCheckArea->setWidget(m_channelCheckContainer);
    lay->addWidget(m_channelCheckArea, 1);

    return bar;
}

// ---- Legend bar: 幅值 ── | 相位 ··· | 实部 ── | 虚部 ··· ----

QWidget* InspectionPlotWindow::buildLegendBar()
{
    QWidget* bar = new QWidget(this);
    bar->setObjectName("inspectionLegendBar");
    QHBoxLayout* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(4, 0, 4, 0);
    lay->setSpacing(4);

    auto addLegendItem = [&](const QString& text, Qt::PenStyle style,
                             const QColor& iconColor) -> QCheckBox* {
        QLabel* icon = new QLabel(bar);
        icon->setPixmap(lineStyleIcon(style, iconColor, 32, 14));
        icon->setFixedSize(36, 18);
        lay->addWidget(icon);

        QCheckBox* cb = new QCheckBox(text, bar);
        cb->setChecked(true);
        lay->addWidget(cb);

        return cb;
    };

    m_showMagCheck   = addLegendItem(QStringLiteral("幅值"), Qt::SolidLine, QColor("#E8A735"));
    m_showPhaseCheck = addLegendItem(QStringLiteral("相位"), Qt::DotLine,   QColor("#333333"));
    m_showRealCheck  = addLegendItem(QStringLiteral("实部"), Qt::SolidLine, QColor("#E8A735"));
    m_showImagCheck  = addLegendItem(QStringLiteral("虚部"), Qt::DotLine,   QColor("#333333"));

    lay->addStretch();
    return bar;
}

// ---- Time-base column (reusable for both plot1 and plot2) ----

QWidget* InspectionPlotWindow::buildTimeBaseColumn(QCustomPlot*& plotOut)
{
    QWidget* col = new QWidget(m_plotSplitter);
    QVBoxLayout* lay = new QVBoxLayout(col);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(0);

    plotOut = new QCustomPlot(col);
    stylePlot(plotOut);
    plotOut->legend->setVisible(false);
    plotOut->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    lay->addWidget(plotOut, 1);

    return col;
}

// ---- Impedance column: plot + mode + freq checks + circle ----

QWidget* InspectionPlotWindow::buildImpedanceColumn()
{
    QWidget* col = new QWidget(m_plotSplitter);
    QVBoxLayout* lay = new QVBoxLayout(col);
    lay->setContentsMargins(4, 2, 4, 4);
    lay->setSpacing(4);

    // --- impedance plot ---
    m_impedancePlot = new QCustomPlot(col);
    stylePlot(m_impedancePlot);
    m_impedancePlot->xAxis->setLabel(QStringLiteral("实部"));
    m_impedancePlot->yAxis->setLabel(QStringLiteral("虚部"));
    m_impedancePlot->xAxis->setRange(-kDefaultImpedanceRange, kDefaultImpedanceRange);
    m_impedancePlot->yAxis->setRange(-kDefaultImpedanceRange, kDefaultImpedanceRange);
    m_impedancePlot->legend->setVisible(true);
    m_impedancePlot->legend->setFont(QFont("Microsoft YaHei", 8));
    m_impedancePlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectLegend);
    m_impedancePlot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    lay->addWidget(m_impedancePlot, 1);

    // --- mode radio buttons ---
    QWidget* modeRow = new QWidget(col);
    QHBoxLayout* modeLay = new QHBoxLayout(modeRow);
    modeLay->setContentsMargins(0, 2, 0, 0);
    modeLay->setSpacing(8);
    m_adaptiveRadio = new QRadioButton(QStringLiteral("自适应"), modeRow);
    m_defaultRadio  = new QRadioButton(QStringLiteral("默认 (-1000~1000)"), modeRow);
    m_defaultRadio->setChecked(true);
    QButtonGroup* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_adaptiveRadio);
    modeGroup->addButton(m_defaultRadio);
    modeLay->addWidget(m_adaptiveRadio);
    modeLay->addWidget(m_defaultRadio);
    modeLay->addStretch();
    lay->addWidget(modeRow);

    // --- frequency / channel checkboxes ---
    QLabel* freqLabel = new QLabel(QStringLiteral("频率/通道:"), col);
    freqLabel->setStyleSheet("font-size: 9pt; color: #555;");
    lay->addWidget(freqLabel);

    m_freqCheckArea = new QScrollArea(col);
    m_freqCheckArea->setWidgetResizable(true);
    m_freqCheckArea->setFixedHeight(32);
    m_freqCheckArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_freqCheckArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_freqCheckArea->setFrameShape(QFrame::NoFrame);
    m_freqCheckContainer = new QWidget();
    m_freqCheckLayout = new QHBoxLayout(m_freqCheckContainer);
    m_freqCheckLayout->setContentsMargins(0, 0, 0, 0);
    m_freqCheckLayout->setSpacing(4);
    m_freqCheckArea->setWidget(m_freqCheckContainer);
    lay->addWidget(m_freqCheckArea);

    // --- circle boundary ---
    QWidget* circleRow = new QWidget(col);
    QHBoxLayout* circleLay = new QHBoxLayout(circleRow);
    circleLay->setContentsMargins(0, 2, 0, 0);
    circleLay->setSpacing(6);
    circleLay->addWidget(new QLabel(QStringLiteral("圆边界 R:")));
    m_circleRadiusSpin = new QDoubleSpinBox(circleRow);
    m_circleRadiusSpin->setRange(0.0, 100000.0);
    m_circleRadiusSpin->setValue(500.0);
    m_circleRadiusSpin->setDecimals(1);
    m_circleRadiusSpin->setSingleStep(10.0);
    circleLay->addWidget(m_circleRadiusSpin);
    m_circleShowCheck = new QCheckBox(QStringLiteral("显示"), circleRow);
    m_circleShowCheck->setChecked(false);
    circleLay->addWidget(m_circleShowCheck);
    circleLay->addStretch();
    lay->addWidget(circleRow);

    // --- status ---
    m_statusLabel = new QLabel(col);
    m_statusLabel->setStyleSheet("color: #999; font-size: 9px;");
    lay->addWidget(m_statusLabel);

    return col;
}

// ---- Unified plot styling ----

void InspectionPlotWindow::stylePlot(QCustomPlot* plot)
{
    plot->setBackground(kPlotBg);
    plot->setNotAntialiasedElements(QCP::aeAll);
    plot->setNoAntialiasingOnDrag(true);

    for (auto* axis : {plot->xAxis, plot->yAxis}) {
        axis->setBasePen(QPen(kAxisLine, 1));
        axis->setTickPen(QPen(kAxisLine, 1));
        axis->setSubTickPen(QPen(kAxisLine, 1));
        axis->setLabelColor(kAxisLabel);
        axis->setTickLabelColor(kAxisLabel);
        axis->grid()->setPen(QPen(kGridMajor, 1, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(kGridMinor, 1, Qt::DotLine));
        axis->grid()->setSubGridVisible(true);
    }

    for (auto* axis : {plot->xAxis2, plot->yAxis2}) {
        axis->setVisible(true);
        axis->setTicks(false);
        axis->setTickLabels(false);
        axis->setBasePen(QPen(kAxisLine, 1));
    }
}

// ---- Signal/Slot connections (centralized) ----

void InspectionPlotWindow::setupConnections()
{
    connect(m_groupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &InspectionPlotWindow::onGroupComboChanged);
    connect(m_chPerGroupSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &InspectionPlotWindow::onChannelsPerGroupChanged);
    connect(m_adaptiveRadio, &QRadioButton::toggled,
            this, &InspectionPlotWindow::onImpedanceModeChanged);
    connect(m_circleRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &InspectionPlotWindow::onCircleRadiusChanged);
    connect(m_circleShowCheck, &QCheckBox::toggled,
            this, &InspectionPlotWindow::onCircleShowToggled);
    connect(m_showMagCheck, &QCheckBox::toggled,
            this, &InspectionPlotWindow::onComponentCheckToggled);
    connect(m_showPhaseCheck, &QCheckBox::toggled,
            this, &InspectionPlotWindow::onComponentCheckToggled);
    connect(m_showRealCheck, &QCheckBox::toggled,
            this, &InspectionPlotWindow::onComponentCheckToggled);
    connect(m_showImagCheck, &QCheckBox::toggled,
            this, &InspectionPlotWindow::onComponentCheckToggled);
}

// ============================================================================
// Layout switching (complex vs real mode)
// ============================================================================

void InspectionPlotWindow::applyTimeBaseModeLayout()
{
    if (!m_plotSplitter || !m_timeCol2 || !m_legendBar)
        return;

    const bool complex = (m_lastMode == FrameData::MultiChannelComplex);
    m_timeCol2->setVisible(complex);
    m_legendBar->setVisible(complex);

    m_plotSplitter->setStretchFactor(0, 1);
    m_plotSplitter->setStretchFactor(1, complex ? 1 : 0);
    m_plotSplitter->setStretchFactor(2, 2);

    const int w = qMax(400, m_plotSplitter->width());
    QList<int> sizes;
    if (complex) {
        sizes << w / 4 << w / 4 << w / 2;
    } else {
        const int t = w / 3;
        sizes << t << 0 << (2 * t);
    }
    m_plotSplitter->setSizes(sizes);
}

void InspectionPlotWindow::onComponentCheckToggled()
{
    auto snap = PlotDataHub::instance()->snapshot();
    if (snap) {
        updateTimeBasePlots(snap);
        scheduleReplot();
    }
}

// ============================================================================
// Slots: PlotWindowBase overrides
// ============================================================================

void InspectionPlotWindow::onDataUpdated(const QVector<FrameData>& /*frames*/)
{
}

void InspectionPlotWindow::onCriticalFrame(const FrameData& /*frame*/)
{
}

void InspectionPlotWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!snapshot || snapshot->version == m_lastSnapshotVersion)
        return;
    m_lastSnapshotVersion = snapshot->version;

    const auto mode = snapshot->mode;
    const int ch = snapshot->channelCount;

    if (mode == FrameData::Legacy || ch <= 0)
        return;

    const bool structureChanged = (mode != m_lastMode || ch != m_lastChannelCount);
    if (structureChanged) {
        m_lastMode = mode;
        m_lastChannelCount = ch;

        rebuildGroupCombo(ch);
        rebuildChannelChecks();
        rebuildTimeBaseGraphs();

        rebuildFreqChecks(ch);
        rebuildImpedanceCurves(ch);

        applyTimeBaseModeLayout();
    }

    updateTimeBasePlots(snapshot);
    updateImpedancePlane(snapshot);
    scheduleReplot();
}

// ============================================================================
// Group / channel management
// ============================================================================

void InspectionPlotWindow::rebuildGroupCombo(int totalChannels)
{
    QSignalBlocker block(m_groupCombo);
    m_groupCombo->clear();

    const int g = m_channelsPerGroup;
    const int numGroups = (totalChannels + g - 1) / g;
    for (int i = 0; i < numGroups; ++i) {
        const int first = i * g + 1;
        const int last  = qMin((i + 1) * g, totalChannels);
        m_groupCombo->addItem(
            QStringLiteral("组%1 (Ch%2-%3)").arg(i + 1).arg(first).arg(last));
    }
    if (numGroups > 0)
        m_groupCombo->setCurrentIndex(0);
}

void InspectionPlotWindow::rebuildChannelChecks()
{
    qDeleteAll(m_channelChecks);
    m_channelChecks.clear();

    while (m_channelCheckLayout->count() > 0) {
        QLayoutItem* item = m_channelCheckLayout->takeAt(0);
        delete item;
    }

    const int groupIdx = m_groupCombo->currentIndex();
    if (groupIdx < 0)
        return;

    const int firstCh = groupIdx * m_channelsPerGroup;
    const int lastCh  = qMin(firstCh + m_channelsPerGroup, m_lastChannelCount);

    for (int ch = firstCh; ch < lastCh; ++ch) {
        QCheckBox* cb = new QCheckBox(
            QStringLiteral("Ch%1").arg(ch + 1), m_channelCheckContainer);
        cb->setChecked(true);
        connect(cb, &QCheckBox::toggled,
                this, &InspectionPlotWindow::onChannelCheckToggled);
        m_channelCheckLayout->addWidget(cb);
        m_channelChecks.append(cb);
    }
    m_channelCheckLayout->addStretch();
}

void InspectionPlotWindow::onGroupComboChanged(int /*index*/)
{
    rebuildChannelChecks();
    rebuildTimeBaseGraphs();

    auto snap = PlotDataHub::instance()->snapshot();
    if (snap) {
        updateTimeBasePlots(snap);
        scheduleReplot();
    }
}

void InspectionPlotWindow::onChannelsPerGroupChanged(int value)
{
    m_channelsPerGroup = qBound(kMinChannelsPerGroup, value, kMaxChannelsPerGroup);

    if (AppConfig* cfg = AppConfig::instance())
        cfg->setInspectionChannelsPerGroup(m_channelsPerGroup);

    rebuildGroupCombo(m_lastChannelCount);
    rebuildChannelChecks();
    rebuildTimeBaseGraphs();

    auto snap = PlotDataHub::instance()->snapshot();
    if (snap) {
        updateTimeBasePlots(snap);
        scheduleReplot();
    }
}

void InspectionPlotWindow::onChannelCheckToggled()
{
    auto snap = PlotDataHub::instance()->snapshot();
    if (snap) {
        updateTimeBasePlots(snap);
        scheduleReplot();
    }
}

// ============================================================================
// Time-base graphs rebuild
// ============================================================================

void InspectionPlotWindow::rebuildTimeBaseGraphs()
{
    m_tbPlot1->clearGraphs();
    m_tbPlot2->clearGraphs();

    const int groupIdx = m_groupCombo->currentIndex();
    if (groupIdx < 0 || m_lastChannelCount <= 0)
        return;

    const int firstCh = groupIdx * m_channelsPerGroup;
    const int lastCh  = qMin(firstCh + m_channelsPerGroup, m_lastChannelCount);

    if (m_lastMode == FrameData::MultiChannelReal) {
        m_tbPlot1->yAxis->setLabel(QStringLiteral("幅值"));
        for (int ch = firstCh; ch < lastCh; ++ch) {
            QCPGraph* g = m_tbPlot1->addGraph();
            g->setPen(QPen(colorForChannel(ch), 1.5));
            g->setName(QStringLiteral("Ch%1").arg(ch + 1));
        }
    } else if (m_lastMode == FrameData::MultiChannelComplex) {
        m_tbPlot1->yAxis->setLabel(QStringLiteral("幅值 / 相位"));
        for (int ch = firstCh; ch < lastCh; ++ch) {
            QColor c = colorForChannel(ch);
            QCPGraph* gMag = m_tbPlot1->addGraph();
            gMag->setPen(QPen(c, 1.5, Qt::SolidLine));
            gMag->setName(QStringLiteral("Ch%1 幅值").arg(ch + 1));

            QCPGraph* gPhase = m_tbPlot1->addGraph();
            gPhase->setPen(QPen(c, 1.0, Qt::DashLine));
            gPhase->setName(QStringLiteral("Ch%1 相位").arg(ch + 1));
        }

        m_tbPlot2->yAxis->setLabel(QStringLiteral("实部 / 虚部"));
        for (int ch = firstCh; ch < lastCh; ++ch) {
            QColor c = colorForChannel(ch);
            QCPGraph* gRe = m_tbPlot2->addGraph();
            gRe->setPen(QPen(c, 1.5, Qt::SolidLine));
            gRe->setName(QStringLiteral("Ch%1 实部").arg(ch + 1));

            QCPGraph* gIm = m_tbPlot2->addGraph();
            gIm->setPen(QPen(c, 1.0, Qt::DashLine));
            gIm->setName(QStringLiteral("Ch%1 虚部").arg(ch + 1));
        }
    }
}

// ============================================================================
// Time-base data update
// ============================================================================

void InspectionPlotWindow::updateTimeBasePlots(const QSharedPointer<const PlotSnapshot>& snap)
{
    if (!snap || snap->timeMs.isEmpty())
        return;

    const int groupIdx = m_groupCombo->currentIndex();
    if (groupIdx < 0)
        return;

    const int firstCh = groupIdx * m_channelsPerGroup;
    const int chCount  = qMin(m_channelsPerGroup, m_lastChannelCount - firstCh);
    if (chCount <= 0)
        return;

    if (m_lastMode == FrameData::MultiChannelReal) {
        for (int i = 0; i < chCount && i < m_tbPlot1->graphCount(); ++i) {
            const int ch = firstCh + i;
            const bool visible = (i < m_channelChecks.size()) ? m_channelChecks[i]->isChecked() : true;
            m_tbPlot1->graph(i)->setVisible(visible);
            if (visible && ch < snap->realAmp.size())
                m_tbPlot1->graph(i)->setData(snap->timeMs, snap->realAmp[ch], true);
        }
    } else if (m_lastMode == FrameData::MultiChannelComplex) {
        const bool showMag = m_showMagCheck   && m_showMagCheck->isChecked();
        const bool showPh  = m_showPhaseCheck && m_showPhaseCheck->isChecked();
        const bool showRe  = m_showRealCheck  && m_showRealCheck->isChecked();
        const bool showIm  = m_showImagCheck  && m_showImagCheck->isChecked();

        for (int i = 0; i < chCount; ++i) {
            const int ch = firstCh + i;
            const bool channelVis = (i < m_channelChecks.size()) ? m_channelChecks[i]->isChecked() : true;
            const int gMag   = i * 2;
            const int gPhase = i * 2 + 1;
            if (gMag >= m_tbPlot1->graphCount() || gPhase >= m_tbPlot1->graphCount())
                break;
            m_tbPlot1->graph(gMag)->setVisible(channelVis && showMag);
            m_tbPlot1->graph(gPhase)->setVisible(channelVis && showPh);
            if (channelVis) {
                if (ch < snap->complexMag.size())
                    m_tbPlot1->graph(gMag)->setData(snap->timeMs, snap->complexMag[ch], true);
                if (ch < snap->complexPhase.size())
                    m_tbPlot1->graph(gPhase)->setData(snap->timeMs, snap->complexPhase[ch], true);
            }
        }

        for (int i = 0; i < chCount; ++i) {
            const int ch = firstCh + i;
            const bool channelVis = (i < m_channelChecks.size()) ? m_channelChecks[i]->isChecked() : true;
            const int gRe = i * 2;
            const int gIm = i * 2 + 1;
            if (gRe >= m_tbPlot2->graphCount() || gIm >= m_tbPlot2->graphCount())
                break;
            m_tbPlot2->graph(gRe)->setVisible(channelVis && showRe);
            m_tbPlot2->graph(gIm)->setVisible(channelVis && showIm);
            if (channelVis) {
                if (ch < snap->complexReal.size())
                    m_tbPlot2->graph(gRe)->setData(snap->timeMs, snap->complexReal[ch], true);
                if (ch < snap->complexImag.size())
                    m_tbPlot2->graph(gIm)->setData(snap->timeMs, snap->complexImag[ch], true);
            }
        }
    }

    const double latestTime = snap->timeMs.last();
    m_tbPlot1->xAxis->setRange(latestTime - kTimeWindowMs, latestTime);
    if (m_timeCol2 && m_timeCol2->isVisible())
        m_tbPlot2->xAxis->setRange(latestTime - kTimeWindowMs, latestTime);
}

// ============================================================================
// Impedance plane
// ============================================================================

void InspectionPlotWindow::rebuildFreqChecks(int totalChannels)
{
    qDeleteAll(m_freqChecks);
    m_freqChecks.clear();

    while (m_freqCheckLayout->count() > 0) {
        QLayoutItem* item = m_freqCheckLayout->takeAt(0);
        delete item;
    }

    for (int ch = 0; ch < totalChannels; ++ch) {
        QCheckBox* cb = new QCheckBox(
            QStringLiteral("Ch%1").arg(ch + 1), m_freqCheckContainer);
        cb->setChecked(ch < 4);
        connect(cb, &QCheckBox::toggled,
                this, &InspectionPlotWindow::onFreqCheckToggled);
        m_freqCheckLayout->addWidget(cb);
        m_freqChecks.append(cb);
    }
    m_freqCheckLayout->addStretch();
}

void InspectionPlotWindow::rebuildImpedanceCurves(int totalChannels)
{
    m_impedanceCurves.clear();
    m_impedancePlot->clearPlottables();

    m_circleItem = new QCPItemEllipse(m_impedancePlot);
    m_circleItem->setPen(QPen(Qt::red, 2, Qt::DashLine));
    m_circleItem->setVisible(m_circleShowCheck->isChecked());
    updateCircleBoundary();

    for (int ch = 0; ch < totalChannels; ++ch) {
        QCPCurve* curve = new QCPCurve(m_impedancePlot->xAxis, m_impedancePlot->yAxis);
        curve->setPen(QPen(colorForChannel(ch), 1.5));
        curve->setName(QStringLiteral("Ch%1").arg(ch + 1));
        const bool visible = (ch < m_freqChecks.size()) ? m_freqChecks[ch]->isChecked() : false;
        curve->setVisible(visible);
        m_impedanceCurves.append(curve);
    }

    applyImpedanceAxisMode();
}

void InspectionPlotWindow::updateImpedancePlane(const QSharedPointer<const PlotSnapshot>& snap)
{
    if (!snap || m_lastMode != FrameData::MultiChannelComplex)
        return;

    const int n = snap->timeMs.size();

    for (int ch = 0; ch < m_impedanceCurves.size() && ch < snap->complexReal.size(); ++ch) {
        QCPCurve* curve = m_impedanceCurves[ch];
        if (!curve->visible())
            continue;

        const auto& re = snap->complexReal[ch];
        const auto& im = snap->complexImag[ch];
        const int pts = qMin(n, qMin(re.size(), im.size()));

        QVector<double> t(pts), x(pts), y(pts);
        for (int i = 0; i < pts; ++i) {
            t[i] = i;
            x[i] = re[i];
            y[i] = im[i];
        }
        curve->setData(t, x, y, true);
    }

    if (m_adaptiveRadio->isChecked())
        m_impedancePlot->rescaleAxes();

    m_statusLabel->setText(
        QStringLiteral("通道数: %1  数据点: %2").arg(m_lastChannelCount).arg(n));
}

void InspectionPlotWindow::onFreqCheckToggled()
{
    for (int ch = 0; ch < m_impedanceCurves.size() && ch < m_freqChecks.size(); ++ch)
        m_impedanceCurves[ch]->setVisible(m_freqChecks[ch]->isChecked());

    auto snap = PlotDataHub::instance()->snapshot();
    if (snap)
        updateImpedancePlane(snap);
    scheduleReplot();
}

void InspectionPlotWindow::onImpedanceModeChanged()
{
    applyImpedanceAxisMode();
    m_impedancePlot->replot(QCustomPlot::rpQueuedReplot);
}

void InspectionPlotWindow::applyImpedanceAxisMode()
{
    if (m_defaultRadio->isChecked()) {
        m_impedancePlot->xAxis->setRange(-kDefaultImpedanceRange, kDefaultImpedanceRange);
        m_impedancePlot->yAxis->setRange(-kDefaultImpedanceRange, kDefaultImpedanceRange);
    } else {
        m_impedancePlot->rescaleAxes();
    }
}

// ============================================================================
// Circle boundary
// ============================================================================

void InspectionPlotWindow::onCircleRadiusChanged(double /*radius*/)
{
    updateCircleBoundary();
    m_impedancePlot->replot(QCustomPlot::rpQueuedReplot);
}

void InspectionPlotWindow::onCircleShowToggled(bool show)
{
    m_circleItem->setVisible(show);
    m_impedancePlot->replot(QCustomPlot::rpQueuedReplot);
}

void InspectionPlotWindow::updateCircleBoundary()
{
    const double r = m_circleRadiusSpin->value();
    m_circleItem->topLeft->setCoords(-r, r);
    m_circleItem->bottomRight->setCoords(r, -r);
}

// ============================================================================
// Replot throttle
// ============================================================================

void InspectionPlotWindow::scheduleReplot()
{
    if (m_replotThrottle.elapsed() < m_replotMinMs)
        return;
    m_replotThrottle.restart();

    m_tbPlot1->replot(QCustomPlot::rpQueuedReplot);
    if (m_timeCol2 && m_timeCol2->isVisible())
        m_tbPlot2->replot(QCustomPlot::rpQueuedReplot);
    m_impedancePlot->replot(QCustomPlot::rpQueuedReplot);
}
