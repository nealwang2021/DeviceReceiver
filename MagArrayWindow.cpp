#include "MagArrayWindow.h"
#include "AppConfig.h"
#include "qcustomplot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QRadioButton>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QScrollArea>
#include <QButtonGroup>
#include <QFrame>
#include <QDebug>
#include <QtGlobal>
#include <cmath>
#include <algorithm>

// ============================================================================
// 构造 / 析构
// ============================================================================

MagArrayWindow::MagArrayWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("漏磁检测"));
    resize(1600, 900);
    buildUi();
}

MagArrayWindow::~MagArrayWindow() = default;

// ============================================================================
// UI 构建
// ============================================================================

void MagArrayWindow::buildUi()
{
    // --- 根布局 ---
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    // --- 顶部控制栏 ---
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setSpacing(6);

    QLabel* framesLabel = new QLabel(QStringLiteral("最大帧:"), this);
    m_maxFramesSpin = new QSpinBox(this);
    m_maxFramesSpin->setRange(100, 10000);
    m_maxFramesSpin->setValue(2000);
    m_maxFramesSpin->setSingleStep(100);
    m_maxFramesSpin->setToolTip(QStringLiteral("波形图和热力图最大显示帧数"));

    controlLayout->addWidget(framesLabel);
    controlLayout->addWidget(m_maxFramesSpin);
    controlLayout->addSpacing(8);

    QLabel* axisLabel = new QLabel(QStringLiteral("显示轴:"), this);
    for (int a = 0; a < 3; ++a) {
        m_axisChecks[a] = new QCheckBox(QString("XYZ"[a]), this);
        m_axisChecks[a]->setChecked(true);
        controlLayout->addWidget(m_axisChecks[a]);
    }
    controlLayout->addSpacing(12);

    QLabel* modeLabel = new QLabel(QStringLiteral("热力图X轴:"), this);
    m_timeModeBtn = new QRadioButton(QStringLiteral("时间"), this);
    m_posModeBtn = new QRadioButton(QStringLiteral("台位"), this);
    m_timeModeBtn->setChecked(true);
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_timeModeBtn);
    modeGroup->addButton(m_posModeBtn);
    modeGroup->setExclusive(true);

    QLabel* rangeLabel = new QLabel(QStringLiteral("色标:"), this);
    QLabel* minLabel = new QLabel(QStringLiteral("min:"), this);
    m_colorMinSpin = new QDoubleSpinBox(this);
    m_colorMinSpin->setRange(-10000.0, 10000.0);
    m_colorMinSpin->setDecimals(1);
    m_colorMinSpin->setValue(0.0);
    m_colorMinSpin->setSingleStep(5.0);
    QLabel* maxLabel = new QLabel(QStringLiteral("max:"), this);
    m_colorMaxSpin = new QDoubleSpinBox(this);
    m_colorMaxSpin->setRange(-10000.0, 10000.0);
    m_colorMaxSpin->setDecimals(1);
    m_colorMaxSpin->setValue(100.0);
    m_colorMaxSpin->setSingleStep(5.0);

    m_stageStatusLabel = new QLabel(QStringLiteral("三轴台: 未连接"), this);
    m_stageStatusLabel->setStyleSheet(QStringLiteral("color: #888888;"));
    m_statsLabel = new QLabel(QStringLiteral("就绪"), this);

    controlLayout->addWidget(modeLabel);
    controlLayout->addWidget(m_timeModeBtn);
    controlLayout->addWidget(m_posModeBtn);
    controlLayout->addSpacing(12);
    controlLayout->addWidget(rangeLabel);
    controlLayout->addWidget(minLabel);
    controlLayout->addWidget(m_colorMinSpin);
    controlLayout->addWidget(maxLabel);
    controlLayout->addWidget(m_colorMaxSpin);
    controlLayout->addStretch();
    controlLayout->addWidget(m_stageStatusLabel);
    controlLayout->addWidget(m_statsLabel);

    rootLayout->addLayout(controlLayout);

    // --- 中央分隔器（波形图 | 热力图） ---
    m_splitter = new QSplitter(Qt::Horizontal, this);

    // ---- 左侧：波形图 ----
    m_waveformPlot = new QCustomPlot(m_splitter);
    PlotWindowBase::applyConfiguredOpenGl(m_waveformPlot);
    m_waveformPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_waveformPlot->plotLayout()->setRowSpacing(10);
    m_waveformPlot->plotLayout()->setColumnSpacing(0);

    m_waveformScrollArea = new QScrollArea(m_splitter);
    m_waveformScrollArea->setWidget(m_waveformPlot);
    m_waveformScrollArea->setWidgetResizable(true);
    m_waveformScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_waveformScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_waveformScrollArea->setFrameShape(QFrame::NoFrame);

    // ---- 右侧：热力图 ----
    m_heatmapPlot = new QCustomPlot(m_splitter);
    PlotWindowBase::applyConfiguredOpenGl(m_heatmapPlot);
    m_heatmapPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    m_heatmapScrollArea = new QScrollArea(m_splitter);
    m_heatmapScrollArea->setWidget(m_heatmapPlot);
    m_heatmapScrollArea->setWidgetResizable(true);
    m_heatmapScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_heatmapScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_heatmapScrollArea->setFrameShape(QFrame::NoFrame);

    m_splitter->addWidget(m_waveformScrollArea);
    m_splitter->addWidget(m_heatmapScrollArea);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);

    rootLayout->addWidget(m_splitter, 1);
    setLayout(rootLayout);

    // --- 初始化波形图 ---
    rebuildWaveformGraphs(m_waveformChannelCount);

    // --- 初始化热力图 ---
    const bool dark = isDarkThemeActive();
    m_heatmapPlot->setBackground(QBrush(dark ? QColor(24, 26, 30) : QColor(255, 255, 255)));
    m_heatmapPlot->plotLayout()->clear();

    // 初始化 ring buffers
    for (int axis = 0; axis < 3; ++axis) {
        m_heatmapData[axis].resize(kHeatmapRows * kHeatmapCols);
        std::fill(m_heatmapData[axis].begin(), m_heatmapData[axis].end(), qQNaN());
    }
    m_heatmapTimeCol.fill(qQNaN(), kHeatmapCols);
    m_heatmapPosCol.fill(qQNaN(), kHeatmapCols);

    const char* axisLabels[3] = {"X 轴", "Y 轴", "Z 轴"};

    for (int axis = 0; axis < 3; ++axis) {
        const bool isLast = (axis == 2);
        QCPAxisRect* axisRect = new QCPAxisRect(m_heatmapPlot);
        m_heatmapPlot->plotLayout()->addElement(axis, 0, axisRect);
        m_heatmapAxisRects[axis] = axisRect;

        axisRect->setAutoMargins(QCP::msNone);
        axisRect->setMargins(isLast ? QMargins(48, 0, 8, 18) : QMargins(48, 0, 8, 0));

        QCPColorMap* colorMap = new QCPColorMap(axisRect->axis(QCPAxis::atBottom),
                                                 axisRect->axis(QCPAxis::atLeft));
        m_heatmapColorMaps[axis] = colorMap;

        colorMap->data()->setSize(kHeatmapCols, kHeatmapRows);
        colorMap->data()->setRange(QCPRange(0, kHeatmapCols - 1), QCPRange(0, kHeatmapRows - 1));
        colorMap->setDataRange(QCPRange(m_colorRangeMin, m_colorRangeMax));

        QCPColorGradient gradient;
        gradient.loadPreset(QCPColorGradient::gpJet);
        colorMap->setGradient(gradient);
        colorMap->setInterpolate(false);

        QCPColorScale* colorScale = new QCPColorScale(m_heatmapPlot);
        m_heatmapPlot->plotLayout()->addElement(axis, 1, colorScale);
        colorMap->setColorScale(colorScale);
        m_heatmapColorScales[axis] = colorScale;

        colorScale->setLabel(QStringLiteral("%1 强度").arg(QString::fromLatin1(axisLabels[axis])));
        colorScale->axis()->setLabelColor(dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
        colorScale->axis()->setTickLabelColor(dark ? QColor(200, 208, 220) : QColor(70, 78, 90));

        // Axis labels
        axisRect->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("传感器"));
        axisRect->axis(QCPAxis::atLeft)->setLabel(QStringLiteral("%1").arg(QString::fromLatin1(axisLabels[axis])));
        axisRect->axis(QCPAxis::atBottom)->setRange(0, kHeatmapCols - 1);
        axisRect->axis(QCPAxis::atLeft)->setRange(0, kHeatmapRows - 1);
        axisRect->axis(QCPAxis::atLeft)->setNumberFormat("f");
        axisRect->axis(QCPAxis::atLeft)->setNumberPrecision(0);
        axisRect->axis(QCPAxis::atLeft)->setSubTicks(false);

        // 仅最后一个轴显示底部标签（共享X轴）
        if (!isLast) {
            axisRect->axis(QCPAxis::atBottom)->setVisible(false);
        }

        // Fill with NaN
        for (int col = 0; col < kHeatmapCols; ++col) {
            for (int row = 0; row < kHeatmapRows; ++row) {
                colorMap->data()->setCell(col, row, qQNaN());
            }
        }
    }

    m_heatmapPlot->plotLayout()->setRowSpacing(10);
    m_heatmapPlot->plotLayout()->setColumnSpacing(0);

    m_heatmapPlot->plotLayout()->setRowSpacing(10);
    m_heatmapPlot->plotLayout()->setColumnSpacing(0);
    const int perRowH = 40;
    const int minHeatmapH = qMax(200, 3 * perRowH + 40);
    m_heatmapPlot->setMinimumHeight(minHeatmapH);

    // --- 信号连接 ---
    connect(m_timeModeBtn, &QRadioButton::toggled, this, &MagArrayWindow::onHeatmapAxisModeChanged);
    connect(m_posModeBtn, &QRadioButton::toggled, this, &MagArrayWindow::onHeatmapAxisModeChanged);
    connect(m_colorMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MagArrayWindow::onColorRangeChanged);
    connect(m_colorMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MagArrayWindow::onColorRangeChanged);

    onThemeChanged();
    m_replotThrottle.start();
}

// ============================================================================
// 波形图重建
// ============================================================================

void MagArrayWindow::rebuildWaveformGraphs(int channelCount)
{
    if (!m_waveformPlot) return;

    m_waveformPlot->clearGraphs();
    m_waveformPlot->clearItems();
    m_waveformPlot->plotLayout()->clear();

    const bool dark = isDarkThemeActive();
    m_waveformPlot->setBackground(QBrush(dark ? QColor(24, 26, 30) : QColor(255, 255, 255)));

    const QColor rowBg[3] = {
        dark ? QColor(36, 40, 46) : QColor(248, 251, 255),
        dark ? QColor(31, 35, 41) : QColor(241, 246, 252),
        dark ? QColor(36, 40, 46) : QColor(248, 251, 255),
    };
    const QColor gridBottom = dark ? QColor(74, 82, 94) : QColor(210, 220, 232);
    const QColor gridLeft   = dark ? QColor(64, 72, 84) : QColor(198, 208, 220);
    const QColor tickColor  = dark ? QColor(200, 208, 220) : QColor(70, 78, 90);
    const QColor labelColor = dark ? QColor(222, 228, 236) : QColor(50, 58, 70);
    const QColor axisColor  = dark ? QColor(152, 162, 176) : QColor(138, 148, 160);

    constexpr int kSensorsPerAxis = 20;
    constexpr int kAxisCount = 3;
    const char* axisLabels[kAxisCount] = {"X 轴", "Y 轴", "Z 轴"};

    m_waveformAxisRects.clear();
    m_waveformChannelCount = qBound(0, channelCount, kSensorsPerAxis * kAxisCount);

    // 3 个轴矩形，共享时间轴，每轴 20 通道叠加显示（不同颜色区分）
    for (int axis = 0; axis < kAxisCount; ++axis) {
        const bool isLast = (axis == kAxisCount - 1);
        QCPAxisRect* axisRect = new QCPAxisRect(m_waveformPlot);
        m_waveformPlot->plotLayout()->addElement(axis, 0, axisRect);
        m_waveformAxisRects.append(axisRect);

        axisRect->setAutoMargins(QCP::msNone);
        axisRect->setMargins(isLast ? QMargins(48, 0, 8, 18) : QMargins(48, 0, 8, 0));
        axisRect->setBackground(rowBg[axis]);

        // Axis labels
        axisRect->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("时间 (ms)"));
        axisRect->axis(QCPAxis::atLeft)->setLabel(QString::fromLatin1(axisLabels[axis]));
        axisRect->axis(QCPAxis::atBottom)->setTickLabelColor(tickColor);
        axisRect->axis(QCPAxis::atLeft)->setTickLabelColor(tickColor);
        axisRect->axis(QCPAxis::atLeft)->setLabelColor(labelColor);
        axisRect->axis(QCPAxis::atBottom)->setLabelColor(labelColor);
        axisRect->axis(QCPAxis::atLeft)->setBasePen(QPen(axisColor));
        axisRect->axis(QCPAxis::atBottom)->setBasePen(QPen(axisColor));
        axisRect->axis(QCPAxis::atLeft)->setNumberFormat("f");
        axisRect->axis(QCPAxis::atLeft)->setNumberPrecision(0);

        // Grids
        axisRect->axis(QCPAxis::atBottom)->grid()->setVisible(true);
        axisRect->axis(QCPAxis::atBottom)->grid()->setPen(QPen(gridBottom, 1, Qt::DotLine));
        axisRect->axis(QCPAxis::atLeft)->grid()->setVisible(true);
        axisRect->axis(QCPAxis::atLeft)->grid()->setPen(QPen(gridLeft, 1, Qt::DotLine));

        // 20 channels per axis, overlaid
        for (int sensorIdx = 0; sensorIdx < kSensorsPerAxis; ++sensorIdx) {
            const QColor color = QColor::fromHsv((sensorIdx * 36) % 360, 200, 200);
            m_waveformPlot->addGraph(axisRect->axis(QCPAxis::atBottom),
                                     axisRect->axis(QCPAxis::atLeft));
            m_waveformPlot->graph()->setPen(QPen(color, 1));
            m_waveformPlot->graph()->setAntialiased(false);
        }

        // 仅最后一个轴显示时间标签，前两个隐藏（共享时间轴）
        if (!isLast) {
            axisRect->axis(QCPAxis::atBottom)->setVisible(false);
        }
    }

    // Set minimum height: 3 axes × ~150px each
    m_waveformPlot->setMinimumHeight(450);
    m_waveformPlot->replot(QCustomPlot::rpQueuedReplot);
}

// ============================================================================
// 波形图：从快照更新（onPlotSnapshotUpdated）
// ============================================================================

void MagArrayWindow::updateWaveformFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!snapshot || !m_waveformPlot) return;
    if (snapshot->timeMs.isEmpty()) return;

    const int ch = qBound(1, snapshot->channelCount, 200);
    if (ch <= 0 || snapshot->realAmp.size() < ch) return;

    if (snapshot->mode != m_lastMode || ch != m_lastSnapshotChannelCount) {
        m_lastMode = snapshot->mode;
        m_lastSnapshotChannelCount = ch;
        rebuildWaveformGraphs(ch);
    }

    const int kMaxDisplayFrames = m_maxFramesSpin ? m_maxFramesSpin->value() : 2000;
    const int effectiveCh = qMin(ch, m_waveformPlot->graphCount());
    const QVector<double>& fullTime = snapshot->timeMs;
    const int totalFrames = fullTime.size();
    const int startIdx = qMax(0, totalFrames - kMaxDisplayFrames);
    const int displayFrames = totalFrames - startIdx;

    QVector<double> timeVec(displayFrames);
    for (int i = 0; i < displayFrames; ++i) timeVec[i] = fullTime[startIdx + i];

    for (int i = 0; i < effectiveCh; ++i) {
        if (i >= snapshot->realAmp.size()) break;
        const QVector<double>& fullVals = snapshot->realAmp[i];
        if (fullVals.size() < displayFrames) continue;
        QVector<double> vals(displayFrames);
        for (int j = 0; j < displayFrames; ++j) vals[j] = fullVals[startIdx + j];
        m_waveformPlot->graph(i)->setData(timeVec, vals, true);
        m_waveformPlot->graph(i)->rescaleValueAxis(false);
    }

    // Rebuild layouts so hidden axes don't take space
    const int oldMask = m_lastAxisMask;
    int curMask = 0;
    for (int a = 0; a < 3; ++a)
        if (m_axisChecks[a] && m_axisChecks[a]->isChecked()) curMask |= (1 << a);

    if (curMask != oldMask) {
        m_lastAxisMask = curMask;

        // Waveform layout rebuild
        {
            auto* wl = m_waveformPlot->plotLayout();
            const int n = wl->elementCount();
            QVector<QCPLayoutElement*> saved;
            for (int i = 0; i < n; ++i) saved.append(wl->elementAt(0));
            for (auto* el : saved) wl->take(el);

            int row = 0;
            for (int a = 0; a < 3; ++a) {
                if (!(curMask & (1 << a))) {
                    if (a < m_waveformAxisRects.size() && m_waveformAxisRects[a])
                        m_waveformAxisRects[a]->setVisible(false);
                    continue;
                }
                if (a < m_waveformAxisRects.size() && m_waveformAxisRects[a]) {
                    m_waveformAxisRects[a]->setVisible(true);
                    wl->addElement(row++, 0, m_waveformAxisRects[a]);
                }
            }
        }

        // Heatmap layout rebuild
        {
            auto* hl = m_heatmapPlot->plotLayout();
            const int n = hl->elementCount();
            QVector<QCPLayoutElement*> saved;
            for (int i = 0; i < n; ++i) saved.append(hl->elementAt(0));
            for (auto* el : saved) hl->take(el);

            int row = 0;
            for (int a = 0; a < 3; ++a) {
                if (!(curMask & (1 << a))) {
                    if (m_heatmapAxisRects[a]) m_heatmapAxisRects[a]->setVisible(false);
                    continue;
                }
                if (m_heatmapAxisRects[a]) {
                    hl->addElement(row, 0, m_heatmapAxisRects[a]);
                }
                if (m_heatmapColorScales[a])
                    hl->addElement(row, 1, m_heatmapColorScales[a]);
                ++row;
            }
        }
    }

    // Graph visibility + bottom axis for last visible
    for (int a = 0; a < 3; ++a) {
        const bool vis = (curMask & (1 << a)) != 0;
        for (int s = 0; s < 20; ++s) {
            int gi = a * 20 + s;
            if (gi < m_waveformPlot->graphCount())
                m_waveformPlot->graph(gi)->setVisible(vis);
        }
        if (m_heatmapColorScales[a])
            m_heatmapColorScales[a]->setVisible(vis);
    }
    // Bottom axis only on last visible
    int lastVis = -1;
    for (int a = 2; a >= 0; --a) if (curMask & (1 << a)) { lastVis = a; break; }
    for (int a = 0; a < 3; ++a) {
        if (a < m_waveformAxisRects.size() && m_waveformAxisRects[a])
            m_waveformAxisRects[a]->axis(QCPAxis::atBottom)->setVisible(a == lastVis);
        if (m_heatmapAxisRects[a])
            m_heatmapAxisRects[a]->axis(QCPAxis::atBottom)->setVisible(a == lastVis);
    }

    // Set X axis range on all visible axis rects
    if (!timeVec.isEmpty()) {
        const double lower = timeVec.first();
        const double upper = timeVec.last() + 1.0;
        for (auto* axisRect : m_waveformAxisRects) {
            if (axisRect->visible())
                axisRect->axis(QCPAxis::atBottom)->setRange(lower, upper);
        }
    }

    m_waveformPlot->replot(QCustomPlot::rpQueuedReplot);
}

// ============================================================================
// 热力图：从单帧更新（onDataUpdated）
// ============================================================================

void MagArrayWindow::updateHeatmapFromFrame(const FrameData& frame)
{
    if (!m_heatmapPlot) return;
    if (frame.detectMode != FrameData::MagArray) return;
    if (frame.magSensorResults.size() < kHeatmapRows) return;

    // Write to the next ring buffer column
    const int col = m_heatmapWriteCol % kHeatmapCols;

    for (int sensor = 0; sensor < kHeatmapRows; ++sensor) {
        const MagSensorResult& sr = frame.magSensorResults[sensor];
        const int row = sr.sensorIndex;
        if (row < 0 || row >= kHeatmapRows) continue;

        const int idx = row * kHeatmapCols + col;
        m_heatmapData[0][idx] = sr.xMean;
        m_heatmapData[1][idx] = sr.yMean;
        m_heatmapData[2][idx] = sr.zMean;
    }

    // Record the time column value
    m_heatmapTimeCol[col] = static_cast<double>(frame.timestamp);

    // Record position column value if available
    if (frame.hasStagePose) {
        m_heatmapPosCol[col] = frame.stageXMm;
        m_stageStatusLabel->setText(QStringLiteral("三轴台: ●已连接 X=%1 Y=%2 Z=%3")
                                    .arg(frame.stageXMm, 0, 'f', 1)
                                    .arg(frame.stageYMm, 0, 'f', 1)
                                    .arg(frame.stageZMm, 0, 'f', 1));
        m_stageStatusLabel->setStyleSheet(QStringLiteral("color: #00aa00;"));
    } else {
        m_stageStatusLabel->setText(QStringLiteral("三轴台: 未连接"));
        m_stageStatusLabel->setStyleSheet(QStringLiteral("color: #888888;"));
    }

    m_heatmapWriteCol++;
    m_liveFrameCount++;

    // Update color maps from ring buffer
    for (int axis = 0; axis < 3; ++axis) {
        QCPColorMap* cmap = m_heatmapColorMaps[axis];
        if (!cmap) continue;

        for (int c = 0; c < kHeatmapCols; ++c) {
            for (int r = 0; r < kHeatmapRows; ++r) {
                cmap->data()->setCell(c, r, m_heatmapData[axis][r * kHeatmapCols + c]);
            }
        }
    }

    // Update axis labels for time/pos mode
    if (m_usePosMode) {
        for (int axis = 0; axis < 3; ++axis) {
            if (m_heatmapAxisRects[axis]) {
                m_heatmapAxisRects[axis]->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("台位 X (mm)"));
            }
        }
    } else {
        for (int axis = 0; axis < 3; ++axis) {
            if (m_heatmapAxisRects[axis]) {
                m_heatmapAxisRects[axis]->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("时间列 (最新→旧)"));
            }
        }
    }

    // Throttled replot
    const qint64 elapsed = m_replotThrottle.elapsed();
    if (elapsed >= m_replotMinMs) {
        m_replotThrottle.restart();
        m_heatmapPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}

// ============================================================================
// PlotWindowBase 接口
// ============================================================================

void MagArrayWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (frames.isEmpty()) return;

    // For heatmap, use only the last frame (or the latest MagArray frame)
    bool updated = false;
    for (int i = frames.size() - 1; i >= 0; --i) {
        const FrameData& f = frames[i];
        if (f.detectMode != FrameData::MagArray) continue;
        if (f.frameId <= m_lastFrameId) continue; // skip duplicates

        m_lastFrameId = f.frameId;
        updateHeatmapFromFrame(f);
        updated = true;
        break; // only process the latest MagArray frame per batch
    }
    Q_UNUSED(updated);

    if (m_statsLabel) {
        m_statsLabel->setText(QStringLiteral("帧数: %1 | 列: %2").arg(m_liveFrameCount).arg(m_heatmapWriteCol));
    }
}

void MagArrayWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!snapshot) return;

    // Only handle MagArray and MultiChannelReal modes (both use realAmp)
    if (snapshot->mode == FrameData::MagArray
        || snapshot->mode == FrameData::MultiChannelReal) {
        updateWaveformFromSnapshot(snapshot);
    }
}

void MagArrayWindow::onCriticalFrame(const FrameData& frame)
{
    if (m_statsLabel) {
        QString alarmMsg;
        if (frame.detectMode == FrameData::MagArray) {
            alarmMsg = QStringLiteral("警报！帧ID:%1 漏磁检测模式 通道数:%2")
                           .arg(frame.frameId)
                           .arg(frame.channelCount);
        } else {
            alarmMsg = QStringLiteral("警报！帧ID:%1 模式:%2")
                           .arg(frame.frameId)
                           .arg(static_cast<int>(frame.detectMode));
        }
        m_statsLabel->setText(alarmMsg);
    }
}

// ============================================================================
// 控件回调
// ============================================================================

void MagArrayWindow::onColorRangeChanged()
{
    m_colorRangeMin = m_colorMinSpin->value();
    m_colorRangeMax = m_colorMaxSpin->value();

    if (m_colorRangeMax <= m_colorRangeMin) {
        m_colorRangeMax = m_colorRangeMin + 1.0;
    }

    for (int axis = 0; axis < 3; ++axis) {
        if (m_heatmapColorMaps[axis]) {
            m_heatmapColorMaps[axis]->setDataRange(QCPRange(m_colorRangeMin, m_colorRangeMax));
        }
        if (m_heatmapColorScales[axis]) {
            m_heatmapColorScales[axis]->axis()->setRange(m_colorRangeMin, m_colorRangeMax);
        }
    }

    if (m_heatmapPlot) {
        m_heatmapPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void MagArrayWindow::onHeatmapAxisModeChanged()
{
    m_usePosMode = m_posModeBtn->isChecked();

    // Update heatmap X axis labels
    for (int axis = 0; axis < 3; ++axis) {
        if (m_heatmapAxisRects[axis]) {
            m_heatmapAxisRects[axis]->axis(QCPAxis::atBottom)->setLabel(
                m_usePosMode ? QStringLiteral("台位 X (mm)") : QStringLiteral("时间列 (最新→旧)"));
        }
    }

    if (m_heatmapPlot) {
        m_heatmapPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}

// ============================================================================
// 主题
// ============================================================================

void MagArrayWindow::onThemeChanged()
{
    const bool dark = isDarkThemeActive();

    // Waveform plot
    if (m_waveformPlot) {
        m_waveformPlot->setBackground(QBrush(dark ? QColor(24, 26, 30) : QColor(255, 255, 255)));
    }
    applyThemeToPlot(m_waveformPlot, dark);
    applyThemeToPlot(m_heatmapPlot, dark);

    if (m_heatmapPlot) {
        m_heatmapPlot->setBackground(QBrush(dark ? QColor(24, 26, 30) : QColor(255, 255, 255)));
        for (int axis = 0; axis < 3; ++axis) {
            if (m_heatmapColorScales[axis]) {
                m_heatmapColorScales[axis]->axis()->setLabelColor(
                    dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
                m_heatmapColorScales[axis]->axis()->setTickLabelColor(
                    dark ? QColor(200, 208, 220) : QColor(70, 78, 90));
            }
        }
    }

    if (m_waveformPlot) {
        m_waveformPlot->replot(QCustomPlot::rpQueuedReplot);
    }
    if (m_heatmapPlot) {
        m_heatmapPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}
