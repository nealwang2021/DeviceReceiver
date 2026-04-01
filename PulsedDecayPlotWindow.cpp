/*
 * 脉冲分段：见头文件注释。后续若 device_data.proto / FrameData 增加 pulse_seq、sample_in_pulse
 * 等字段，应优先用协议判定新脉冲，手动/时间间隙作为回退。
 */
#include "PulsedDecayPlotWindow.h"
#include "qcustomplot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QListWidget>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QSet>
#include <QtGlobal>
#include <QBrush>
#include <QPen>
#include <cmath>
#include <algorithm>
#include <limits>

namespace {
static QColor colorForCurveId(int id)
{
    const int h = (id * 47) % 360;
    return QColor::fromHsv(h, 200, 220);
}
} // namespace

PulsedDecayPlotWindow::PulsedDecayPlotWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle("脉冲涡流衰减曲线");
    resize(1000, 640);
    rebuildPlotUi();
    m_replotThrottle.start();
}

PulsedDecayPlotWindow::~PulsedDecayPlotWindow()
{
    if (m_mockTimer) {
        m_mockTimer->stop();
    }
}

void PulsedDecayPlotWindow::rebuildPlotUi()
{
    QWidget* root = new QWidget(this);
    QHBoxLayout* outer = new QHBoxLayout(root);
    outer->setContentsMargins(6, 6, 6, 6);

    m_decayPlot = new QCustomPlot(root);
    m_decayPlot->setMinimumWidth(480);
    m_decayPlot->xAxis->setLabel("时间 (ms, 相对脉冲起点)");
    m_decayPlot->yAxis->setLabel("幅值");
    m_decayPlot->legend->setVisible(true);
    m_decayPlot->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignTop | Qt::AlignRight);

    QWidget* side = new QWidget(root);
    QVBoxLayout* sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(0, 0, 0, 0);

    QGroupBox* ctrl = new QGroupBox("曲线与脉冲", side);
    QGridLayout* g = new QGridLayout(ctrl);
    m_channelSpin = new QSpinBox(ctrl);
    m_channelSpin->setRange(0, 255);
    m_channelSpin->setValue(0);
    m_maxCurvesSpin = new QSpinBox(ctrl);
    m_maxCurvesSpin->setRange(1, 32);
    m_maxCurvesSpin->setValue(8);
    m_autoGapCheck = new QCheckBox("自动新脉冲(时间间隙)", ctrl);
    m_gapMsSpin = new QDoubleSpinBox(ctrl);
    m_gapMsSpin->setRange(1.0, 60000.0);
    m_gapMsSpin->setValue(200.0);
    m_gapMsSpin->setSuffix(" ms");
    m_gapMsSpin->setEnabled(false);
    m_nextPulseBtn = new QPushButton("下一段脉冲", ctrl);
    m_delBtn = new QPushButton("删除选中曲线", ctrl);
    m_refBtn = new QPushButton("生成参考线(多选平均)", ctrl);
    m_clearRefBtn = new QPushButton("清除参考线", ctrl);
    m_mockCheck = new QCheckBox("模拟数据", ctrl);

    int r = 0;
    g->addWidget(new QLabel("通道索引:"), r, 0);
    g->addWidget(m_channelSpin, r, 1);
    ++r;
    g->addWidget(new QLabel("最大曲线数:"), r, 0);
    g->addWidget(m_maxCurvesSpin, r, 1);
    ++r;
    g->addWidget(m_autoGapCheck, r, 0, 1, 2);
    ++r;
    g->addWidget(new QLabel("间隙阈值:"), r, 0);
    g->addWidget(m_gapMsSpin, r, 1);
    ++r;
    g->addWidget(m_nextPulseBtn, r, 0, 1, 2);
    ++r;
    g->addWidget(m_delBtn, r, 0, 1, 2);
    ++r;
    g->addWidget(m_refBtn, r, 0, 1, 2);
    ++r;
    g->addWidget(m_clearRefBtn, r, 0, 1, 2);
    ++r;
    g->addWidget(m_mockCheck, r, 0, 1, 2);

    m_curveList = new QListWidget(side);
    m_curveList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_curveList->setMinimumWidth(220);

    m_statusLabel = new QLabel("就绪", side);
    m_statusLabel->setWordWrap(true);

    sideLay->addWidget(ctrl);
    sideLay->addWidget(new QLabel("曲线列表:", side));
    sideLay->addWidget(m_curveList, 1);
    sideLay->addWidget(m_statusLabel);

    outer->addWidget(m_decayPlot, 1);
    outer->addWidget(side);

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setContentsMargins(0, 0, 0, 0);
    main->addWidget(root);

    connect(m_nextPulseBtn, &QPushButton::clicked, this, &PulsedDecayPlotWindow::onNextPulseClicked);
    connect(m_delBtn, &QPushButton::clicked, this, &PulsedDecayPlotWindow::onDeleteSelectedClicked);
    connect(m_refBtn, &QPushButton::clicked, this, &PulsedDecayPlotWindow::onBuildReferenceClicked);
    connect(m_clearRefBtn, &QPushButton::clicked, this, &PulsedDecayPlotWindow::onClearReferenceClicked);
    connect(m_curveList, &QListWidget::itemSelectionChanged, this, &PulsedDecayPlotWindow::onListSelectionChanged);
    connect(m_mockCheck, &QCheckBox::toggled, this, &PulsedDecayPlotWindow::onMockToggled);
    connect(m_maxCurvesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &PulsedDecayPlotWindow::onMaxCurvesChanged);
    connect(m_autoGapCheck, &QCheckBox::toggled, this, &PulsedDecayPlotWindow::onAutoGapToggled);

    m_mockTimer = new QTimer(this);
    m_mockTimer->setInterval(40);
    connect(m_mockTimer, &QTimer::timeout, this, &PulsedDecayPlotWindow::onMockTick);
}

void PulsedDecayPlotWindow::onAutoGapToggled(bool on)
{
    m_gapMsSpin->setEnabled(on);
}

void PulsedDecayPlotWindow::onMaxCurvesChanged(int v)
{
    Q_UNUSED(v);
    enforceMaxCurves();
    syncListWidget();
    scheduleReplot();
}

bool PulsedDecayPlotWindow::scalarFromFrame(const FrameData& frame, int ch, double* out) const
{
    if (!out) {
        return false;
    }
    if (frame.detectMode == FrameData::Legacy) {
        return false;
    }
    if (frame.channelCount < 1 || ch < 0 || ch >= frame.channelCount) {
        return false;
    }
    if (ch >= frame.channels_comp0.size()) {
        return false;
    }
    if (frame.detectMode == FrameData::MultiChannelReal) {
        *out = frame.channels_comp0[ch];
        return true;
    }
    if (frame.detectMode == FrameData::MultiChannelComplex) {
        if (ch >= frame.channels_comp1.size()) {
            return false;
        }
        *out = std::hypot(frame.channels_comp0[ch], frame.channels_comp1[ch]);
        return true;
    }
    return false;
}

void PulsedDecayPlotWindow::startNewPulseAt(qint64 pulseStartMs)
{
    const int maxN = m_maxCurvesSpin ? m_maxCurvesSpin->value() : 8;
    while (m_curves.size() >= maxN) {
        removeCurveAt(0);
    }

    DecayCurveEntry c;
    c.id = m_nextCurveId++;
    c.pulseStartMs = pulseStartMs;
    c.graph = m_decayPlot->addGraph();
    c.graph->setName(QString("脉冲 #%1").arg(c.id));
    c.graph->setPen(QPen(colorForCurveId(c.id), 1.5));

    m_curves.append(c);
    bumpRefGraphToFront();
    syncListWidget();
    m_statusLabel->setText(QString("新脉冲 #%1 起点 t=%2 ms").arg(c.id).arg(pulseStartMs));
}

void PulsedDecayPlotWindow::enforceMaxCurves()
{
    const int maxN = m_maxCurvesSpin ? m_maxCurvesSpin->value() : 8;
    while (m_curves.size() > maxN) {
        removeCurveAt(0);
    }
}

void PulsedDecayPlotWindow::removeCurveAt(int index)
{
    if (index < 0 || index >= m_curves.size()) {
        return;
    }
    DecayCurveEntry& c = m_curves[index];
    if (c.graph) {
        m_decayPlot->removeGraph(c.graph);
        c.graph = nullptr;
    }
    m_curves.remove(index);
    bumpRefGraphToFront();
}

void PulsedDecayPlotWindow::syncListWidget()
{
    QSet<int> wasSel;
    for (QListWidgetItem* wi : m_curveList->selectedItems()) {
        wasSel.insert(wi->data(Qt::UserRole).toInt());
    }
    m_curveList->blockSignals(true);
    m_curveList->clear();
    for (int i = 0; i < m_curves.size(); ++i) {
        const DecayCurveEntry& c = m_curves[i];
        auto* it = new QListWidgetItem(
            QString("脉冲 #%1 | %2 点 | t0=%3")
                .arg(c.id)
                .arg(c.tRelMs.size())
                .arg(c.pulseStartMs));
        it->setData(Qt::UserRole, c.id);
        m_curveList->addItem(it);
    }
    for (int i = 0; i < m_curveList->count(); ++i) {
        QListWidgetItem* it = m_curveList->item(i);
        if (wasSel.contains(it->data(Qt::UserRole).toInt())) {
            it->setSelected(true);
        }
    }
    m_curveList->blockSignals(false);
}

void PulsedDecayPlotWindow::updateCurveStyles()
{
    QList<QListWidgetItem*> sel = m_curveList->selectedItems();
    QSet<int> selectedIds;
    for (QListWidgetItem* it : sel) {
        selectedIds.insert(it->data(Qt::UserRole).toInt());
    }
    const bool oneSel = (sel.size() == 1);

    for (DecayCurveEntry& c : m_curves) {
        if (!c.graph) {
            continue;
        }
        const bool focus = oneSel && selectedIds.contains(c.id);
        QPen pen = c.graph->pen();
        pen.setWidthF(focus ? 2.8 : 1.5);
        c.graph->setPen(pen);
    }
}

void PulsedDecayPlotWindow::applyCurveDataToGraph(DecayCurveEntry& c)
{
    if (!c.graph) {
        return;
    }
    c.graph->setData(c.tRelMs, c.y);
}

void PulsedDecayPlotWindow::scheduleReplot()
{
    if (!m_decayPlot) {
        return;
    }
    if (m_replotThrottle.elapsed() >= m_replotMinMs) {
        m_replotThrottle.restart();
        m_decayPlot->replot();
    }
}

void PulsedDecayPlotWindow::bumpRefGraphToFront()
{
    if (!m_decayPlot || !m_refGraph || m_refT.isEmpty()) {
        return;
    }
    const QPen p = m_refGraph->pen();
    const QString name = m_refGraph->name();
    m_decayPlot->removeGraph(m_refGraph);
    m_refGraph = m_decayPlot->addGraph();
    m_refGraph->setPen(p);
    m_refGraph->setName(name);
    m_refGraph->setData(m_refT, m_refY);
}

void PulsedDecayPlotWindow::processFrame(const FrameData& frame)
{
    const int ch = m_channelSpin ? m_channelSpin->value() : 0;
    double y = 0.0;
    if (!scalarFromFrame(frame, ch, &y)) {
        return;
    }

    const qint64 ts = frame.timestamp;

    if (m_autoGapCheck && m_autoGapCheck->isChecked() && m_lastFrameTs >= 0) {
        const double gap = static_cast<double>(ts - m_lastFrameTs);
        if (gap > (m_gapMsSpin ? m_gapMsSpin->value() : 200.0)) {
            m_startNewPulseNextFrame = true;
        }
    }

    if (m_startNewPulseNextFrame || m_curves.isEmpty()) {
        startNewPulseAt(ts);
        m_startNewPulseNextFrame = false;
    }

    DecayCurveEntry& cur = m_curves.last();
    const double tRel = static_cast<double>(ts - cur.pulseStartMs);
    cur.tRelMs.append(tRel);
    cur.y.append(y);
    applyCurveDataToGraph(cur);

    m_lastFrameTs = ts;
    bumpRefGraphToFront();
    syncListWidget();
    updateCurveStyles();
    scheduleReplot();
}

void PulsedDecayPlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (m_useMockData) {
        return;
    }
    for (const FrameData& f : frames) {
        processFrame(f);
    }
}

void PulsedDecayPlotWindow::onCriticalFrame(const FrameData& frame)
{
    Q_UNUSED(frame);
}

void PulsedDecayPlotWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    Q_UNUSED(snapshot);
    // 衰减曲线由逐帧 FrameData 构建；PlotSnapshot 为通道时间序列，形状不同，此处不更新。
}

void PulsedDecayPlotWindow::onNextPulseClicked()
{
    m_startNewPulseNextFrame = true;
    m_statusLabel->setText("已请求：下一帧开始新脉冲曲线");
}

void PulsedDecayPlotWindow::onDeleteSelectedClicked()
{
    QList<QListWidgetItem*> sel = m_curveList->selectedItems();
    if (sel.isEmpty()) {
        return;
    }
    QVector<int> ids;
    ids.reserve(sel.size());
    for (QListWidgetItem* it : sel) {
        ids.append(it->data(Qt::UserRole).toInt());
    }
    std::sort(ids.begin(), ids.end());
    for (int i = m_curves.size() - 1; i >= 0; --i) {
        if (ids.contains(m_curves[i].id)) {
            removeCurveAt(i);
        }
    }
    syncListWidget();
    updateCurveStyles();
    m_decayPlot->replot();
    m_statusLabel->setText("已删除选中曲线");
}

double PulsedDecayPlotWindow::interpY(const QVector<double>& t, const QVector<double>& y, double tt)
{
    if (t.isEmpty() || t.size() != y.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (tt <= t.first()) {
        return y.first();
    }
    if (tt >= t.last()) {
        return y.last();
    }
    for (int i = 1; i < t.size(); ++i) {
        if (tt <= t[i]) {
            const double t0 = t[i - 1];
            const double t1 = t[i];
            const double u = (tt - t0) / (t1 - t0 + 1e-300);
            return y[i - 1] + u * (y[i] - y[i - 1]);
        }
    }
    return y.last();
}

void PulsedDecayPlotWindow::buildReferenceFromSelection()
{
    QList<QListWidgetItem*> sel = m_curveList->selectedItems();
    if (sel.isEmpty()) {
        m_statusLabel->setText("请先在列表中多选至少一条曲线");
        return;
    }

    QVector<const DecayCurveEntry*> chosen;
    chosen.reserve(sel.size());
    for (QListWidgetItem* it : sel) {
        const int id = it->data(Qt::UserRole).toInt();
        for (const DecayCurveEntry& c : m_curves) {
            if (c.id == id && !c.tRelMs.isEmpty()) {
                chosen.append(&c);
                break;
            }
        }
    }
    if (chosen.isEmpty()) {
        return;
    }

    double tMax = 0.0;
    for (const DecayCurveEntry* c : chosen) {
        tMax = qMax(tMax, c->tRelMs.last());
    }

    double dt = 1.0;
    const DecayCurveEntry* first = chosen.first();
    if (first->tRelMs.size() >= 2) {
        double sum = 0.0;
        int cnt = 0;
        for (int i = 1; i < first->tRelMs.size(); ++i) {
            sum += first->tRelMs[i] - first->tRelMs[i - 1];
            ++cnt;
        }
        if (cnt > 0) {
            dt = qMax(0.5, sum / cnt);
        }
    }

    const int n = qBound(2, static_cast<int>(std::ceil(tMax / dt)) + 1, 200000);
    m_refT.resize(n);
    m_refY.resize(n);
    for (int i = 0; i < n; ++i) {
        const double tt = qMin(static_cast<double>(i) * dt, tMax);
        m_refT[i] = tt;
        double sumY = 0.0;
        int cnt = 0;
        for (const DecayCurveEntry* c : chosen) {
            const double yy = interpY(c->tRelMs, c->y, tt);
            if (std::isfinite(yy)) {
                sumY += yy;
                ++cnt;
            }
        }
        m_refY[i] = cnt > 0 ? sumY / cnt : 0.0;
    }

    if (!m_refGraph) {
        m_refGraph = m_decayPlot->addGraph();
        m_refGraph->setName("参考线(平均)");
        QPen rp(QColor(255, 0, 180));
        rp.setStyle(Qt::DashLine);
        rp.setWidthF(2.5);
        m_refGraph->setPen(rp);
    }
    m_refGraph->setData(m_refT, m_refY);
    m_decayPlot->replot();
    m_statusLabel->setText(QString("已生成参考线：%1 条曲线平均，%2 点").arg(chosen.size()).arg(n));
}

void PulsedDecayPlotWindow::onBuildReferenceClicked()
{
    buildReferenceFromSelection();
}

void PulsedDecayPlotWindow::onClearReferenceClicked()
{
    if (m_refGraph) {
        m_decayPlot->removeGraph(m_refGraph);
        m_refGraph = nullptr;
    }
    m_refT.clear();
    m_refY.clear();
    m_decayPlot->replot();
    m_statusLabel->setText("已清除参考线");
}

void PulsedDecayPlotWindow::onListSelectionChanged()
{
    updateCurveStyles();
    scheduleReplot();
}

void PulsedDecayPlotWindow::onMockToggled(bool on)
{
    m_useMockData = on;
    if (on) {
        m_lastFrameTs = -1;
        m_mockT = 0.0;
        m_mockTimer->start();
        m_statusLabel->setText("模拟数据：指数衰减 + 噪声；可用「下一段脉冲」切换曲线");
    } else {
        m_mockTimer->stop();
    }
}

void PulsedDecayPlotWindow::onMockTick()
{
    FrameData f;
    f.timestamp = static_cast<int64_t>(QDateTime::currentMSecsSinceEpoch());
    f.frameId = static_cast<uint16_t>(m_nextCurveId & 0xFFFF);
    f.detectMode = FrameData::MultiChannelReal;
    f.channelCount = 1;
    const double amp = 5.0 * std::exp(-m_mockT / 80.0) + 0.05 * (static_cast<double>((f.timestamp / 3) % 17) - 8.0);
    f.channels_comp0.append(amp);
    f.channels_comp1.append(0.0);
    m_mockT += 40.0;
    if (m_mockT > 400.0) {
        m_mockT = 0.0;
        m_startNewPulseNextFrame = true;
    }
    processFrame(f);
}
