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
#include <QColorDialog>
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

    // --- 顶部控制栏（单行，分组布局，方便扩充） ---
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setSpacing(4);

    AppConfig* cfg = AppConfig::instance();

    // 组1：最大帧数
    QLabel* framesLabel = new QLabel(QStringLiteral("最大帧:"), this);
    m_maxFramesSpin = new QSpinBox(this);
    m_maxFramesSpin->setRange(100, 10000);
    m_maxFramesSpin->setValue(cfg->magArrayMaxFrames());
    m_maxFramesSpin->setSingleStep(100);
    m_maxFramesSpin->setToolTip(QStringLiteral("波形图和热力图最大显示帧数"));
    m_maxFramesSpin->setMaximumWidth(72);
    controlLayout->addWidget(framesLabel);
    controlLayout->addWidget(m_maxFramesSpin);
    connect(m_maxFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            [](int v) { AppConfig::instance()->setMagArrayMaxFrames(v); });
    controlLayout->addSpacing(10);

    // 组2：显示轴选择
    QLabel* axisLabel = new QLabel(QStringLiteral("显示轴:"), this);
    controlLayout->addWidget(axisLabel);
    const bool axisDefaults[3] = {cfg->magArrayAxisXVisible(),
                                  cfg->magArrayAxisYVisible(),
                                  cfg->magArrayAxisZVisible()};
    for (int a = 0; a < 3; ++a) {
        m_axisChecks[a] = new QCheckBox(QString("XYZ"[a]), this);
        m_axisChecks[a]->setChecked(axisDefaults[a]);
        controlLayout->addWidget(m_axisChecks[a]);
        connect(m_axisChecks[a], &QCheckBox::toggled, this, [a](bool v) {
            switch (a) {
            case 0: AppConfig::instance()->setMagArrayAxisXVisible(v); break;
            case 1: AppConfig::instance()->setMagArrayAxisYVisible(v); break;
            case 2: AppConfig::instance()->setMagArrayAxisZVisible(v); break;
            }
        });
    }
    controlLayout->addSpacing(10);

    // 组3：热力图X轴模式
    QLabel* modeLabel = new QLabel(QStringLiteral("热图X轴:"), this);
    m_timeModeBtn = new QRadioButton(QStringLiteral("时间"), this);
    m_posModeBtn = new QRadioButton(QStringLiteral("台位"), this);
    m_timeModeBtn->setChecked(cfg->magArrayHeatmapXAxisMode() == 0);
    m_posModeBtn->setChecked(cfg->magArrayHeatmapXAxisMode() == 1);
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_timeModeBtn);
    modeGroup->addButton(m_posModeBtn);
    modeGroup->setExclusive(true);
    controlLayout->addWidget(modeLabel);
    controlLayout->addWidget(m_timeModeBtn);
    controlLayout->addWidget(m_posModeBtn);
    connect(m_timeModeBtn, &QRadioButton::toggled, [](bool checked) {
        if (checked) AppConfig::instance()->setMagArrayHeatmapXAxisMode(0);
    });
    connect(m_posModeBtn, &QRadioButton::toggled, [](bool checked) {
        if (checked) AppConfig::instance()->setMagArrayHeatmapXAxisMode(1);
    });
    controlLayout->addSpacing(10);

    // 组4：色标数值范围
    QLabel* rangeLabel = new QLabel(QStringLiteral("色标:"), this);
    QLabel* minLabel = new QLabel(QStringLiteral("min"), this);
    m_colorMinSpin = new QDoubleSpinBox(this);
    m_colorMinSpin->setRange(-10000.0, 10000.0);
    m_colorMinSpin->setDecimals(1);
    m_colorMinSpin->setValue(cfg->magArrayColorDataMin());
    m_colorMinSpin->setSingleStep(5.0);
    m_colorMinSpin->setMaximumWidth(66);
    QLabel* maxLabel = new QLabel(QStringLiteral("max"), this);
    m_colorMaxSpin = new QDoubleSpinBox(this);
    m_colorMaxSpin->setRange(-10000.0, 10000.0);
    m_colorMaxSpin->setDecimals(1);
    m_colorMaxSpin->setValue(cfg->magArrayColorDataMax());
    m_colorMaxSpin->setSingleStep(5.0);
    m_colorMaxSpin->setMaximumWidth(66);
    controlLayout->addWidget(rangeLabel);
    controlLayout->addWidget(minLabel);
    controlLayout->addWidget(m_colorMinSpin);
    connect(m_colorMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [](double v) { AppConfig::instance()->setMagArrayColorDataMin(v); });
    // min: color button
    m_colorMinBtn = new QPushButton(this);
    m_colorMinBtn->setFixedWidth(20);
    m_colorMinBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_colorMinBtn->setToolTip(QStringLiteral("色标最小值对应颜色"));
    m_colorMinBtn->setCursor(Qt::PointingHandCursor);
    controlLayout->addWidget(m_colorMinBtn);
    // max: spinbox + 颜色按钮
    controlLayout->addWidget(maxLabel);
    controlLayout->addWidget(m_colorMaxSpin);
    connect(m_colorMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [](double v) { AppConfig::instance()->setMagArrayColorDataMax(v); });
    m_colorMaxBtn = new QPushButton(this);
    m_colorMaxBtn->setFixedWidth(20);
    m_colorMaxBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_colorMaxBtn->setToolTip(QStringLiteral("色标最大值对应颜色"));
    m_colorMaxBtn->setCursor(Qt::PointingHandCursor);
    controlLayout->addWidget(m_colorMaxBtn);
    controlLayout->addSpacing(10);

    // Load gradient colors from config
    m_gradientColorMin = QColor(cfg->magArrayGradientColorMin());
    m_gradientColorMax = QColor(cfg->magArrayGradientColorMax());

    // 右侧：清屏 + 拉伸 + 状态
    auto* clearBtn = new QPushButton(QStringLiteral("清屏"), this);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_clearTimeMs = QDateTime::currentMSecsSinceEpoch();
        // 仅清数据，保留 graph 结构
        if (m_waveformPlot) {
            for (int i = 0; i < m_waveformPlot->graphCount(); ++i)
                m_waveformPlot->graph(i)->data()->clear();
            m_waveformPlot->replot(QCustomPlot::rpQueuedReplot);
        }
        // 清空热力图环形缓冲区
        for (int a = 0; a < 3; ++a) {
            for (int r = 0; r < kHeatmapRows; ++r)
                for (int c = 0; c < kHeatmapCols; ++c)
                    m_heatmapData[a][r * kHeatmapCols + c] = qQNaN();
        }
        m_heatmapWriteCol = 0;
        m_liveFrameCount = 0;
        m_lastFrameId = 0;
    });
    controlLayout->addWidget(clearBtn);
    controlLayout->addStretch();
    m_stageStatusLabel = new QLabel(QStringLiteral("三轴台: 未连接"), this);
    m_stageStatusLabel->setStyleSheet(QStringLiteral("color: #888888;"));
    m_statsLabel = new QLabel(QStringLiteral("就绪"), this);
    controlLayout->addWidget(m_stageStatusLabel);
    controlLayout->addWidget(m_statsLabel);

    rootLayout->addLayout(controlLayout);

    // --- 中央分隔器（波形图 | 热力图） ---
    m_splitter = new QSplitter(Qt::Horizontal, this);

    // ---- 左侧：波形图 ----
    m_waveformPlot = new QCustomPlot(m_splitter);
    PlotWindowBase::applyConfiguredOpenGl(m_waveformPlot);
    m_waveformPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_waveformPlot->setMinimumWidth(300);
    m_waveformPlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
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
    m_heatmapPlot->setMinimumWidth(300);
    m_heatmapPlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

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
    m_heatmapPlot->setBackground(QBrush(dark ? QColor(24, 24, 24) : QColor(255, 255, 255)));
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
        axisRect->setBackground(QBrush(dark ? QColor(24, 24, 24) : QColor(255, 255, 255)));

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
        // 热力图通过色块表达数据，不需网格线覆盖
        axisRect->axis(QCPAxis::atBottom)->grid()->setVisible(false);
        axisRect->axis(QCPAxis::atLeft)->grid()->setVisible(false);

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
    m_heatmapPlot->plotLayout()->setColumnStretchFactor(0, 4);
    m_heatmapPlot->plotLayout()->setColumnStretchFactor(1, 1);
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
    // 颜色选择按钮
    auto updateBtnStyle = [](QPushButton* btn, const QColor& c) {
        btn->setStyleSheet(QStringLiteral("background-color:%1; border:1px solid #888; border-radius:3px;").arg(c.name()));
    };
    updateBtnStyle(m_colorMinBtn, m_gradientColorMin);
    updateBtnStyle(m_colorMaxBtn, m_gradientColorMax);
    connect(m_colorMinBtn, &QPushButton::clicked, this, [this, updateBtnStyle]() {
        const QColor c = QColorDialog::getColor(m_gradientColorMin, this, QStringLiteral("选择最小值颜色"));
        if (c.isValid()) {
            m_gradientColorMin = c;
            updateBtnStyle(m_colorMinBtn, c);
            updateHeatmapGradient();
            AppConfig::instance()->setMagArrayGradientColorMin(c.name());
        }
    });
    connect(m_colorMaxBtn, &QPushButton::clicked, this, [this, updateBtnStyle]() {
        const QColor c = QColorDialog::getColor(m_gradientColorMax, this, QStringLiteral("选择最大值颜色"));
        if (c.isValid()) {
            m_gradientColorMax = c;
            updateBtnStyle(m_colorMaxBtn, c);
            updateHeatmapGradient();
            AppConfig::instance()->setMagArrayGradientColorMax(c.name());
        }
    });
    // 初始应用渐变色
    updateHeatmapGradient();

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
    m_waveformPlot->plotLayout()->setColumnStretchFactor(0, 1); // 横向填满
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
    int startIdx = qMax(0, totalFrames - kMaxDisplayFrames);
    // 清屏截断：跳过 m_clearTimeMs 之前的数据
    if (m_clearTimeMs > 0) {
        while (startIdx < totalFrames && fullTime[startIdx] < static_cast<double>(m_clearTimeMs))
            ++startIdx;
    }
    const int displayFrames = totalFrames - startIdx;

    QVector<double> timeVec(displayFrames);
    for (int i = 0; i < displayFrames; ++i) timeVec[i] = fullTime[startIdx + i];

    for (int i = 0; i < effectiveCh; ++i) {
        // graph[i] = axisIdx * 20 + sensorIdx
        // realAmp 排列: ch = sensorIdx * 3 + axisIdx（proto 顺序: S0_X, S0_Y, S0_Z, S1_X, ...）
        const int sensorIdx = i % 20;
        const int axisIdx   = i / 20;
        const int dataIdx   = sensorIdx * 3 + axisIdx;
        if (dataIdx >= snapshot->realAmp.size()) continue;
        if (axisIdx < 3 && m_axisChecks[axisIdx] && !m_axisChecks[axisIdx]->isChecked()) {
            m_waveformPlot->graph(i)->setData(QVector<double>(), QVector<double>(), true);
            continue;
        }
        const QVector<double>& fullVals = snapshot->realAmp[dataIdx];
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

        // 清空隐藏轴的热力图旧数据
        for (int a = 0; a < 3; ++a) {
            if (curMask & (1 << a)) continue;
            for (int r = 0; r < kHeatmapRows; ++r)
                for (int c = 0; c < kHeatmapCols; ++c)
                    m_heatmapData[a][r * kHeatmapCols + c] = qQNaN();
        }

        auto* wl = m_waveformPlot->plotLayout();
        auto* hl = m_heatmapPlot->plotLayout();
        wl->setRowSpacing(0);
        hl->setRowSpacing(0);

        int lastVis = -1;
        for (int a = 2; a >= 0; --a)
            if (curMask & (1 << a)) { lastVis = a; break; }

        for (int a = 0; a < 3; ++a) {
            const bool vis = (curMask & (1 << a)) != 0;
            const int bottomMargin = (vis && a == lastVis) ? 18 : 0;
            // Waveform
            if (a < m_waveformAxisRects.size() && m_waveformAxisRects[a]) {
                auto* r = m_waveformAxisRects[a];
                r->setVisible(vis);
                r->setMaximumSize(vis ? QSize(9999, 9999) : QSize(9999, 0));
                r->setMinimumSize(vis ? QSize(50, 50) : QSize(0, 0));
                r->setMargins(vis ? QMargins(48, 5, 8, bottomMargin) : QMargins(0,0,0,0));
                r->setMinimumMargins(vis ? QMargins(48, 5, 8, bottomMargin) : QMargins(0,0,0,0));
            }
            wl->setRowStretchFactor(a, vis ? 1 : 0);
            // Heatmap
            if (m_heatmapAxisRects[a]) {
                auto* r = m_heatmapAxisRects[a];
                r->setVisible(vis);
                r->setMaximumSize(vis ? QSize(9999, 9999) : QSize(9999, 0));
                r->setMinimumSize(vis ? QSize(50, 50) : QSize(0, 0));
                r->setMargins(vis ? QMargins(48, 5, 8, bottomMargin) : QMargins(0,0,0,0));
                r->setMinimumMargins(vis ? QMargins(48, 5, 8, bottomMargin) : QMargins(0,0,0,0));
            }
            if (m_heatmapColorScales[a]) {
                m_heatmapColorScales[a]->setVisible(vis);
                if (!vis) {
                    m_heatmapColorScales[a]->setMinimumSize(0, 0);
                    m_heatmapColorScales[a]->setMaximumSize(9999, 0);
                } else {
                    m_heatmapColorScales[a]->setMaximumSize(9999, 9999);
                }
            }
            hl->setRowStretchFactor(a, vis ? 1 : 0);
        }

        // 动态调整最小高度：根据可见轴数缩放
        int visCount = 0;
        for (int a = 0; a < 3; ++a)
            if (curMask & (1 << a)) ++visCount;
        m_waveformPlot->setMinimumHeight(visCount * 150);
        const int perRowH = 40;
        m_heatmapPlot->setMinimumHeight(qMax(80, visCount * perRowH + 40));
    }

    // 颜色刻度显隐
    for (int a = 0; a < 3; ++a) {
        if (m_heatmapColorScales[a])
            m_heatmapColorScales[a]->setVisible((curMask & (1 << a)) != 0);
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
        m_heatmapData[0][idx] = (m_axisChecks[0] && m_axisChecks[0]->isChecked()) ? sr.xMean : qQNaN();
        m_heatmapData[1][idx] = (m_axisChecks[1] && m_axisChecks[1]->isChecked()) ? sr.yMean : qQNaN();
        m_heatmapData[2][idx] = (m_axisChecks[2] && m_axisChecks[2]->isChecked()) ? sr.zMean : qQNaN();
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
                m_heatmapAxisRects[axis]->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("时间列"));
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

void MagArrayWindow::updateHeatmapGradient()
{
    QCPColorGradient gradient;
    gradient.setColorStopAt(0.0, m_gradientColorMin);
    gradient.setColorStopAt(1.0, m_gradientColorMax);
    gradient.setNanHandling(QCPColorGradient::nhTransparent);  // NaN 单元格透明，透出 axis rect 背景色

    for (int axis = 0; axis < 3; ++axis) {
        if (m_heatmapColorMaps[axis]) {
            m_heatmapColorMaps[axis]->setGradient(gradient);
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
        m_heatmapPlot->setBackground(QBrush(dark ? QColor(24, 24, 24) : QColor(255, 255, 255)));
        // 热力图轴矩形背景与阵列热力图保持一致
        const QBrush hmRectBg(dark ? QColor(24, 24, 24) : QColor(255, 255, 255));
        for (int axis = 0; axis < 3; ++axis) {
            if (m_heatmapAxisRects[axis]) {
                m_heatmapAxisRects[axis]->setBackground(hmRectBg);
                // 热力图色块自表达结构，不叠加网格线
                m_heatmapAxisRects[axis]->axis(QCPAxis::atBottom)->grid()->setVisible(false);
                m_heatmapAxisRects[axis]->axis(QCPAxis::atLeft)->grid()->setVisible(false);
            }
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
