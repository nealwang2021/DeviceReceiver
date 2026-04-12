#include "HeatMapPlotWindow.h"
#include "qcustomplot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QWidget>
#include <QFileDialog>
#include <QDebug>
#include <QtGlobal>
#include <cmath>
#include <algorithm>
#include <utility>

namespace {
// grpc_test_server.py 默认 --mode complex：通道0 为 hypot(re,im)，模长多在约 0.65~1.35；色标略宽于理论值以便 jet 色谱分层。
// gRPC 帧率由应用配置 Receiver/MockDataIntervalMs 传入 Subscribe（默认 100ms，勿误设 1000）。
// 若使用 --mode real，comp0 幅值约 6~12，请在「强度色标」中改为约 5~12。
static bool minMaxFinite(const QVector<double>& data, double* minV, double* maxV)
{
    bool any = false;
    for (double v : data) {
        if (!std::isfinite(v)) {
            continue;
        }
        if (!any) {
            *minV = *maxV = v;
            any = true;
        } else {
            *minV = std::min(*minV, v);
            *maxV = std::max(*maxV, v);
        }
    }
    return any;
}
} // namespace

HeatMapPlotWindow::HeatMapPlotWindow(QWidget *parent)
    : PlotWindowBase(parent)
    , m_gridWidth(101)
    , m_gridHeight(101)
    , m_dataMin(0.0)
    , m_dataMax(100.0)
    , m_displayMin(0.55)
    , m_displayMax(1.45)
{
    setWindowTitle("热力图");
    resize(1200, 600);

    // 创建主布局
    QWidget* centralWidget = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(5);

    // 顶部控制栏
    QGroupBox* controlGroup = new QGroupBox("控制面板", this);
    QHBoxLayout* controlLayout = new QHBoxLayout(controlGroup);

    // 网格尺寸控制
    QGroupBox* sizeGroup = new QGroupBox("网格尺寸", this);
    QFormLayout* sizeLayout = new QFormLayout(sizeGroup);
    
    m_widthSpinBox = new QSpinBox(this);
    m_widthSpinBox->setMinimum(32);
    m_widthSpinBox->setMaximum(2048);
    m_widthSpinBox->setValue(101);
    m_widthSpinBox->setSingleStep(1);
    
    m_heightSpinBox = new QSpinBox(this);
    m_heightSpinBox->setMinimum(32);
    m_heightSpinBox->setMaximum(512);
    m_heightSpinBox->setValue(101);
    m_heightSpinBox->setSingleStep(1);
    
    sizeLayout->addRow("宽度:", m_widthSpinBox);
    sizeLayout->addRow("高度:", m_heightSpinBox);
    
    // 数据范围控制
    QGroupBox* rangeGroup = new QGroupBox("强度色标 (通道0，complex 模长常用 0.55~1.45)", this);
    QFormLayout* rangeLayout = new QFormLayout(rangeGroup);
    
    m_minSpinBox = new QDoubleSpinBox(this);
    m_minSpinBox->setMinimum(-1000.0);
    m_minSpinBox->setMaximum(10000.0);
    m_minSpinBox->setDecimals(3);
    m_minSpinBox->setValue(0.55);
    m_minSpinBox->setSingleStep(0.05);
    
    m_maxSpinBox = new QDoubleSpinBox(this);
    m_maxSpinBox->setMinimum(-1000.0);
    m_maxSpinBox->setMaximum(10000.0);
    m_maxSpinBox->setDecimals(3);
    m_maxSpinBox->setValue(1.45);
    m_maxSpinBox->setSingleStep(0.05);
    
    rangeLayout->addRow("最小值:", m_minSpinBox);
    rangeLayout->addRow("最大值:", m_maxSpinBox);

    QGroupBox* stageGroup = new QGroupBox("台位范围 (mm)", this);
    QFormLayout* stageLayout = new QFormLayout(stageGroup);
    m_autoStageRangeCheck = new QCheckBox("自动扩展 XY 范围", this);
    m_autoStageRangeCheck->setChecked(false);
    m_stageXMinSpin = new QDoubleSpinBox(this);
    m_stageXMinSpin->setRange(-1e6, 1e6);
    m_stageXMinSpin->setDecimals(3);
    m_stageXMinSpin->setValue(0.0);
    m_stageXMaxSpin = new QDoubleSpinBox(this);
    m_stageXMaxSpin->setRange(-1e6, 1e6);
    m_stageXMaxSpin->setDecimals(3);
    m_stageXMaxSpin->setValue(1000.0);
    m_stageYMinSpin = new QDoubleSpinBox(this);
    m_stageYMinSpin->setRange(-1e6, 1e6);
    m_stageYMinSpin->setDecimals(3);
    m_stageYMinSpin->setValue(0.0);
    m_stageYMaxSpin = new QDoubleSpinBox(this);
    m_stageYMaxSpin->setRange(-1e6, 1e6);
    m_stageYMaxSpin->setDecimals(3);
    m_stageYMaxSpin->setValue(1000.0);
    QWidget* xRangeRow = new QWidget(this);
    QHBoxLayout* xRangeLayout = new QHBoxLayout(xRangeRow);
    xRangeLayout->setContentsMargins(0, 0, 0, 0);
    xRangeLayout->addWidget(m_stageXMinSpin);
    xRangeLayout->addWidget(m_stageXMaxSpin);
    QWidget* yRangeRow = new QWidget(this);
    QHBoxLayout* yRangeLayout = new QHBoxLayout(yRangeRow);
    yRangeLayout->setContentsMargins(0, 0, 0, 0);
    yRangeLayout->addWidget(m_stageYMinSpin);
    yRangeLayout->addWidget(m_stageYMaxSpin);
    stageLayout->addRow(m_autoStageRangeCheck);
    stageLayout->addRow("X 范围 (mm):", xRangeRow);
    stageLayout->addRow("Y 范围 (mm):", yRangeRow);
    m_clearHeatMapButton = new QPushButton("清除热力图", this);
    stageLayout->addRow(m_clearHeatMapButton);
    m_stageXMinSpin->setEnabled(true);
    m_stageXMaxSpin->setEnabled(true);
    m_stageYMinSpin->setEnabled(true);
    m_stageYMaxSpin->setEnabled(true);
    
    // 动作按钮
    m_startButton = new QPushButton("启动模拟", this);
    m_stopButton = new QPushButton("停止模拟", this);
    m_exportButton = new QPushButton("导出图像", this);
    
    m_stopButton->setEnabled(false);
    
    controlLayout->addWidget(sizeGroup);
    controlLayout->addWidget(rangeGroup);
    controlLayout->addWidget(stageGroup);
    controlLayout->addWidget(m_startButton);
    controlLayout->addWidget(m_stopButton);
    controlLayout->addWidget(m_exportButton);
    controlLayout->addStretch();

    mainLayout->addWidget(controlGroup);

    // 绘图区域
    m_plot = new QCustomPlot(this);
    mainLayout->addWidget(m_plot, 1);

    // 统计信息标签
    m_statsLabel = new QLabel("热力图已初始化", this);
    mainLayout->addWidget(m_statsLabel);

    setLayout(new QVBoxLayout(this));
    layout()->addWidget(centralWidget);

    // 初始化热力图
    initHeatMap();
    
    // 初始化数据
    m_gridData.resize(m_gridWidth * m_gridHeight);
    std::fill(m_gridData.begin(), m_gridData.end(), 50.0);
    
    // 生成初始的模拟数据并显示
    generateMockData();
    updateHeatMapDisplay();

    // 连接信号槽
    connect(m_widthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), 
            this, &HeatMapPlotWindow::onGridWidthChanged);
    connect(m_heightSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onGridHeightChanged);
    connect(m_minSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onDataMinChanged);
    connect(m_maxSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onDataMaxChanged);
    connect(m_startButton, &QPushButton::clicked, this, &HeatMapPlotWindow::onStartMockDataClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &HeatMapPlotWindow::onStopMockDataClicked);
    connect(m_exportButton, &QPushButton::clicked, this, &HeatMapPlotWindow::onExportClicked);
    connect(m_autoStageRangeCheck, &QCheckBox::toggled, this, &HeatMapPlotWindow::onAutoStageRangeToggled);
    connect(m_stageXMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onStageRangeSpinChanged);
    connect(m_stageXMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onStageRangeSpinChanged);
    connect(m_stageYMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onStageRangeSpinChanged);
    connect(m_stageYMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HeatMapPlotWindow::onStageRangeSpinChanged);
    connect(m_clearHeatMapButton, &QPushButton::clicked, this, &HeatMapPlotWindow::onClearHeatMapClicked);

    m_replotCoalesceTimer = new QTimer(this);
    m_replotCoalesceTimer->setSingleShot(true);
    connect(m_replotCoalesceTimer, &QTimer::timeout, this, [this]() {
        m_replotThrottle.restart();
        refreshLiveHeatMapPlot();
    });
    m_replotThrottle.start();

    // 定时器用于更新模拟数据
    m_mockDataTimer = new QTimer(this);
    connect(m_mockDataTimer, &QTimer::timeout, this, [this]() {
        if (m_useMockData) {
            generateMockData();
            updateHeatMapDisplay();
        }
    });
    m_mockDataTimer->setInterval(100);
}

HeatMapPlotWindow::~HeatMapPlotWindow()
{
    if (m_mockDataTimer) {
        m_mockDataTimer->stop();
    }
}

void HeatMapPlotWindow::initUI()
{
    // UI 初始化由构造函数完成
}

void HeatMapPlotWindow::initHeatMap()
{
    if (!m_plot) return;

    const bool dark = isDarkThemeActive();

    m_plot->clearPlottables();
    m_plot->clearItems();

    // 创建颜色映射
    m_colorMap = new QCPColorMap(m_plot->xAxis, m_plot->yAxis);
    
    // 设置颜色映射的数据大小和范围（通过 data() 对象）
    m_colorMap->data()->setSize(m_gridWidth, m_gridHeight);
    m_colorMap->data()->setRange(QCPRange(0, m_gridWidth), QCPRange(0, m_gridHeight));
    
    // 先用宽泛的范围，后续会根据实际数据调整
    m_colorMap->setDataRange(QCPRange(0, 100));
    
    // 创建颜色标尺
    m_colorScale = new QCPColorScale(m_plot);
    m_plot->plotLayout()->addElement(0, 1, m_colorScale);
    m_colorMap->setColorScale(m_colorScale);
    
    // 设置色带（浅黄→深橙→红）
    setColorGradient();
    
    // 配置轴
    m_plot->xAxis->setLabel("网格 X");
    m_plot->yAxis->setLabel("网格 Y");
    m_plot->xAxis->setRange(0, m_gridWidth);
    m_plot->yAxis->setRange(0, m_gridHeight);
    m_plot->xAxis->setNumberFormat("f");
    m_plot->yAxis->setNumberFormat("f");
    m_plot->xAxis->setNumberPrecision(1);
    m_plot->yAxis->setNumberPrecision(1);
    
    // 配置颜色标尺
    m_colorScale->setLabel("信号强度");
    m_colorScale->axis()->setLabel("强度");
    m_colorScale->axis()->setLabelColor(dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
    m_colorScale->axis()->setTickLabelColor(dark ? QColor(200, 208, 220) : QColor(70, 78, 90));
    m_colorScale->axis()->setBasePen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
    m_colorScale->axis()->setTickPen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
    m_colorScale->axis()->setSubTickPen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
    
    const double fillVal = m_useMockData ? 50.0 : m_displayMin;
    for (int x = 0; x < m_gridWidth; ++x) {
        for (int y = 0; y < m_gridHeight; ++y) {
            m_colorMap->data()->setCell(x, y, fillVal);
        }
    }
    
    onThemeChanged();
    m_plot->replot();
}

void HeatMapPlotWindow::setColorGradient()
{
    if (!m_colorMap) return;

    // jet：蓝→青→绿→黄→红，幅值分层优于黄-红单色带（与固定窄色标配合）
    QCPColorGradient gradient;
    gradient.loadPreset(QCPColorGradient::gpJet);
    m_colorMap->setGradient(gradient);
}

void HeatMapPlotWindow::onThemeChanged()
{
    applyThemeToPlot(m_plot, isDarkThemeActive());
    if (m_colorScale) {
        const bool dark = isDarkThemeActive();
        m_colorScale->axis()->setLabelColor(dark ? QColor(222, 228, 236) : QColor(50, 58, 70));
        m_colorScale->axis()->setTickLabelColor(dark ? QColor(200, 208, 220) : QColor(70, 78, 90));
        m_colorScale->axis()->setBasePen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
        m_colorScale->axis()->setTickPen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
        m_colorScale->axis()->setSubTickPen(QPen(dark ? QColor(152, 162, 176) : QColor(138, 148, 160), 1));
    }
    if (m_plot) {
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void HeatMapPlotWindow::updateHeatMapDisplay()
{
    if (!m_colorMap || !m_colorMap->data()) return;

    // 检查数据有效性
    if (m_gridData.isEmpty()) {
        m_colorMap->setDataRange(QCPRange(0, 100));
        m_plot->replot();
        return;
    }

    double gridMin = 0.0;
    double gridMax = 100.0;
    if (!minMaxFinite(m_gridData, &gridMin, &gridMax)) {
        m_colorMap->setDataRange(QCPRange(0, 100));
        m_plot->replot();
        return;
    }

    double colorLo = gridMin;
    double colorHi = gridMax;
    if (m_useMockData) {
        if (std::fabs(colorHi - colorLo) < 0.01) {
            colorLo -= 2.0;
            colorHi += 2.0;
        }
    } else {
        colorLo = m_displayMin;
        colorHi = m_displayMax;
        if (std::fabs(colorHi - colorLo) < 1e-300) {
            colorLo -= 1.0;
            colorHi += 1.0;
        }
    }
    m_colorMap->setDataRange(QCPRange(colorLo, colorHi));
    
    for (int x = 0; x < m_gridWidth; ++x) {
        for (int y = 0; y < m_gridHeight; ++y) {
            int idx = y * m_gridWidth + x;
            if (idx >= 0 && idx < m_gridData.size()) {
                double val = m_gridData.at(idx);
                if (std::isfinite(val)) {
                    val = std::max(colorLo - 10.0, std::min(colorHi + 10.0, val));
                }
                m_colorMap->data()->setCell(x, y, val);
            }
        }
    }

    // 更新统计信息
    double avgVal = 0.0;
    int finiteCount = 0;
    for (double val : m_gridData) {
        if (std::isfinite(val)) {
            avgVal += val;
            ++finiteCount;
        }
    }
    if (finiteCount > 0) {
        avgVal /= static_cast<double>(finiteCount);
    }

    m_statsLabel->setText(QString("网格: %1×%2 | 最小: %3 | 最大: %4 | 平均: %5 | 帧数: %6")
                         .arg(m_gridWidth)
                         .arg(m_gridHeight)
                         .arg(gridMin, 0, 'f', 2)
                         .arg(gridMax, 0, 'f', 2)
                         .arg(avgVal, 0, 'f', 2)
                         .arg(m_frameCount));

    if (m_useMockData) {
        m_colorMap->data()->setRange(QCPRange(0, m_gridWidth), QCPRange(0, m_gridHeight));
        m_plot->xAxis->setRange(0, m_gridWidth);
        m_plot->yAxis->setRange(0, m_gridHeight);
        m_plot->xAxis->setLabel("网格 X (索引)");
        m_plot->yAxis->setLabel("网格 Y (索引)");
    }

    // 重新绘制
    m_plot->replot();
}

void HeatMapPlotWindow::generateMockData()
{
    m_frameCount++;
    
    // 生成模拟数据：多个动态热点，强对比
    for (int x = 0; x < m_gridWidth; ++x) {
        for (int y = 0; y < m_gridHeight; ++y) {
            int idx = y * m_gridWidth + x;
            
            // 低背景值
            double value = 10.0;
            
            // 热点1：快速移动的主热点
            double centerX = 256.0 + 256.0 * std::sin(m_frameCount * 0.03);
            double centerY = 64.0 + 40.0 * std::cos(m_frameCount * 0.04);
            double dist1 = std::hypot(x - centerX, y - centerY);
            value += 90.0 * std::exp(-dist1 * dist1 / 10000.0);
            
            // 热点2：左上固定热点（较强）
            double dist2 = std::hypot(x - 100.0, y - 30.0);
            value += 75.0 * std::exp(-dist2 * dist2 / 8000.0);
            
            // 热点3：右下固定热点（较强）
            double dist3 = std::hypot(x - 900.0, y - 100.0);
            value += 70.0 * std::exp(-dist3 * dist3 / 7000.0);
            
            // 热点4：缓慢移动的中等热点
            double hotspot4X = 500.0 + 150.0 * std::sin(m_frameCount * 0.01);
            double hotspot4Y = 64.0 + 20.0 * std::cos(m_frameCount * 0.015);
            double dist4 = std::hypot(x - hotspot4X, y - hotspot4Y);
            value += 60.0 * std::exp(-dist4 * dist4 / 6000.0);
            
            // 添加周期性波纹效果以增加视觉动感
            double ripple = 8.0 * std::sin((std::hypot(x - 512, y - 64) - m_frameCount * 2.0) / 50.0);
            if (ripple > 0) value += ripple;
            
            // 约束值在0-100范围内
            m_gridData[idx] = std::max(0.0, std::min(100.0, value));
        }
    }
}

void HeatMapPlotWindow::setGridSize(int width, int height)
{
    if (width < 32 || height < 32) return;
    
    m_gridWidth = width;
    m_gridHeight = height;
    m_gridData.resize(m_gridWidth * m_gridHeight);
    m_stageBoundsValid = false;
    if (m_useMockData) {
        std::fill(m_gridData.begin(), m_gridData.end(), 50.0);
    } else {
        m_gridData.fill(m_displayMin);
    }
    
    initHeatMap();
    updateHeatMapDisplay();
}

void HeatMapPlotWindow::applyGrid(const QVector<double>& values, int width, int height)
{
    if (values.size() != width * height) {
        qWarning() << "applyGrid: 数据大小不匹配" << values.size() << "!=" << width * height;
        return;
    }
    
    m_gridWidth = width;
    m_gridHeight = height;
    m_gridData = values;
    
    initHeatMap();
    updateHeatMapDisplay();
}

bool HeatMapPlotWindow::tryScalarValueForHeatmap(const FrameData& frame, double* out) const
{
    if (!out) {
        return false;
    }
    if (frame.detectMode == FrameData::Legacy) {
        return false;
    }
    if (frame.channelCount < 1 || frame.channels_comp0.isEmpty()) {
        return false;
    }
    if (frame.detectMode == FrameData::MultiChannelReal) {
        *out = frame.channels_comp0[0];
        return true;
    }
    if (frame.detectMode == FrameData::MultiChannelComplex) {
        if (frame.channels_comp1.size() < 1) {
            return false;
        }
        *out = std::hypot(frame.channels_comp0[0], frame.channels_comp1[0]);
        return true;
    }
    return false;
}

void HeatMapPlotWindow::updateStageBoundsFromPoint(double vx, double vy)
{
    if (!m_stageBoundsValid) {
        m_stageXMinMm = m_stageXMaxMm = vx;
        m_stageYMinMm = m_stageYMaxMm = vy;
        m_stageBoundsValid = true;
    } else {
        m_stageXMinMm = std::min(m_stageXMinMm, vx);
        m_stageXMaxMm = std::max(m_stageXMaxMm, vx);
        m_stageYMinMm = std::min(m_stageYMinMm, vy);
        m_stageYMaxMm = std::max(m_stageYMaxMm, vy);
    }
}

void HeatMapPlotWindow::mapMmToCell(double vx, double vy, int* ix, int* iy) const
{
    double xmin = 0.0;
    double xmax = 1.0;
    double ymin = 0.0;
    double ymax = 1.0;
    if (m_autoStageRangeMm) {
        xmin = m_stageXMinMm;
        xmax = m_stageXMaxMm;
        ymin = m_stageYMinMm;
        ymax = m_stageYMaxMm;
    } else {
        xmin = m_stageXMinSpin->value();
        xmax = m_stageXMaxSpin->value();
        ymin = m_stageYMinSpin->value();
        ymax = m_stageYMaxSpin->value();
    }
    const double sx = qMax(xmax - xmin, 1e-9);
    const double sy = qMax(ymax - ymin, 1e-9);
    const double tx = (vx - xmin) / sx;
    const double ty = (vy - ymin) / sy;
    *ix = qBound(0, static_cast<int>(std::floor(tx * static_cast<double>(m_gridWidth))), m_gridWidth - 1);
    *iy = qBound(0, static_cast<int>(std::floor(ty * static_cast<double>(m_gridHeight))), m_gridHeight - 1);
}

void HeatMapPlotWindow::applyStageAxisRange()
{
    if (!m_colorMap || !m_colorMap->data()) {
        return;
    }
    double x0 = 0.0;
    double x1 = 1.0;
    double y0 = 0.0;
    double y1 = 1.0;
    if (m_autoStageRangeMm) {
        if (!m_stageBoundsValid) {
            return;
        }
        x0 = m_stageXMinMm;
        x1 = m_stageXMaxMm;
        y0 = m_stageYMinMm;
        y1 = m_stageYMaxMm;
    } else {
        x0 = m_stageXMinSpin->value();
        x1 = m_stageXMaxSpin->value();
        y0 = m_stageYMinSpin->value();
        y1 = m_stageYMaxSpin->value();
    }
    auto padAxis = [](double a, double b) {
        const double s = b - a;
        if (s < 1e-9) {
            return std::pair<double, double>(a - 0.5, b + 0.5);
        }
        const double p = s * 0.02;
        return std::pair<double, double>(a - p, b + p);
    };
    const std::pair<double, double> rx = padAxis(x0, x1);
    const std::pair<double, double> ry = padAxis(y0, y1);
    m_colorMap->data()->setRange(QCPRange(rx.first, rx.second), QCPRange(ry.first, ry.second));
    m_plot->xAxis->setRange(rx.first, rx.second);
    m_plot->yAxis->setRange(ry.first, ry.second);
}

void HeatMapPlotWindow::refreshLiveHeatMapPlot()
{
    if (!m_colorMap || !m_colorMap->data()) {
        return;
    }
    double lo = m_displayMin;
    double hi = m_displayMax;
    if (std::fabs(hi - lo) < 1e-300) {
        lo -= 1.0;
        hi += 1.0;
    }
    m_colorMap->setDataRange(QCPRange(lo, hi));
    m_plot->xAxis->setLabel("台位 X (mm)");
    m_plot->yAxis->setLabel("台位 Y (mm)");
    applyStageAxisRange();
    QString obsPart;
    if (m_liveScalarRangeValid) {
        obsPart = QStringLiteral(" | 实测[%1,%2]")
                      .arg(m_liveScalarMin, 0, 'f', 3)
                      .arg(m_liveScalarMax, 0, 'f', 3);
    }
    m_statsLabel->setText(QString("台位热力图 | 通道0 | 色标[%1,%2]%3 | 帧:%4 | 网格%5×%6")
                              .arg(lo, 0, 'f', 3)
                              .arg(hi, 0, 'f', 3)
                              .arg(obsPart)
                              .arg(m_liveFrameCount)
                              .arg(m_gridWidth)
                              .arg(m_gridHeight));
    m_plot->replot();
}

void HeatMapPlotWindow::scheduleThrottledReplot()
{
    if (m_replotCoalesceTimer->isActive()) {
        return;
    }
    const qint64 elapsed = m_replotThrottle.elapsed();
    if (elapsed >= m_replotMinIntervalMs) {
        m_replotThrottle.restart();
        refreshLiveHeatMapPlot();
        return;
    }
    const int wait = qMax(1, m_replotMinIntervalMs - static_cast<int>(elapsed));
    m_replotCoalesceTimer->start(wait);
}

void HeatMapPlotWindow::clearHeatMapData()
{
    const double bg = m_displayMin;
    m_gridData.fill(bg);
    m_stageBoundsValid = false;
    m_liveScalarRangeValid = false;
    m_liveFrameCount = 0;
    if (m_colorMap && m_colorMap->data()) {
        for (int x = 0; x < m_gridWidth; ++x) {
            for (int y = 0; y < m_gridHeight; ++y) {
                m_colorMap->data()->setCell(x, y, bg);
            }
        }
    }
    m_replotThrottle.restart();
    refreshLiveHeatMapPlot();
}

void HeatMapPlotWindow::updateFromFrame(const FrameData& frame)
{
    if (!m_colorMap || !frame.hasStagePose) {
        return;
    }
    double val = 0.0;
    if (!tryScalarValueForHeatmap(frame, &val)) {
        return;
    }
    const double vx = frame.stageXMm;
    const double vy = frame.stageYMm;
    if (m_autoStageRangeMm) {
        updateStageBoundsFromPoint(vx, vy);
    }
    int ix = 0;
    int iy = 0;
    mapMmToCell(vx, vy, &ix, &iy);
    const int idx = iy * m_gridWidth + ix;
    if (idx < 0 || idx >= m_gridData.size()) {
        return;
    }
    m_gridData[idx] = val;
    m_colorMap->data()->setCell(ix, iy, val);
    if (!m_liveScalarRangeValid) {
        m_liveScalarMin = m_liveScalarMax = val;
        m_liveScalarRangeValid = true;
    } else {
        m_liveScalarMin = std::min(m_liveScalarMin, val);
        m_liveScalarMax = std::max(m_liveScalarMax, val);
    }
    ++m_liveFrameCount;
    scheduleThrottledReplot();
}

void HeatMapPlotWindow::exportImage(const QString& filename)
{
    if (!m_plot) return;
    
    bool success = m_plot->savePng(filename, 1200, 600, 1.0, 100);
    if (success) {
        m_statsLabel->setText(QString("已导出到: %1").arg(filename));
        qInfo() << "热力图已导出:" << filename;
    } else {
        qWarning() << "导出失败:" << filename;
    }
}

void HeatMapPlotWindow::onGridWidthChanged(int width)
{
    setGridSize(width, m_gridHeight);
}

void HeatMapPlotWindow::onGridHeightChanged(int height)
{
    setGridSize(m_gridWidth, height);
}

void HeatMapPlotWindow::onDataMinChanged(double min)
{
    m_displayMin = min;
    if (!m_colorMap) {
        return;
    }
    if (m_useMockData) {
        m_colorMap->setDataRange(QCPRange(m_displayMin, m_displayMax));
        m_plot->replot();
    } else {
        refreshLiveHeatMapPlot();
    }
}

void HeatMapPlotWindow::onDataMaxChanged(double max)
{
    m_displayMax = max;
    if (!m_colorMap) {
        return;
    }
    if (m_useMockData) {
        m_colorMap->setDataRange(QCPRange(m_displayMin, m_displayMax));
        m_plot->replot();
    } else {
        refreshLiveHeatMapPlot();
    }
}

void HeatMapPlotWindow::onAutoStageRangeToggled(bool checked)
{
    m_autoStageRangeMm = checked;
    m_stageXMinSpin->setEnabled(!checked);
    m_stageXMaxSpin->setEnabled(!checked);
    m_stageYMinSpin->setEnabled(!checked);
    m_stageYMaxSpin->setEnabled(!checked);
    if (m_colorMap && !m_useMockData) {
        refreshLiveHeatMapPlot();
    }
}

void HeatMapPlotWindow::onStageRangeSpinChanged()
{
    if (!m_autoStageRangeMm && m_colorMap && !m_useMockData) {
        refreshLiveHeatMapPlot();
    }
}

void HeatMapPlotWindow::onClearHeatMapClicked()
{
    clearHeatMapData();
}

void HeatMapPlotWindow::onStartMockDataClicked()
{
    m_useMockData = true;
    m_frameCount = 0;
    m_liveFrameCount = 0;
    m_liveScalarRangeValid = false;
    m_mockDataTimer->setInterval(50);  // 50ms 更新一次（20 FPS）
    m_mockDataTimer->start();
    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_statsLabel->setText("模拟数据已启动 - 更新中...");
}

void HeatMapPlotWindow::onStopMockDataClicked()
{
    m_useMockData = false;
    m_mockDataTimer->stop();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_statsLabel->setText("模拟数据已停止");
}

void HeatMapPlotWindow::onExportClicked()
{
    QString filename = QFileDialog::getSaveFileName(
        this, "导出热力图", "", "PNG 图像 (*.png);;PNG 图像 (*.png)");
    
    if (!filename.isEmpty()) {
        exportImage(filename);
    }
}

void HeatMapPlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (m_useMockData) {
        return;
    }
    for (const auto& frame : frames) {
        updateFromFrame(frame);
    }
}

void HeatMapPlotWindow::onCriticalFrame(const FrameData& frame)
{
    QString alarmMsg;
    if (frame.detectMode == FrameData::MultiChannelReal) {
        alarmMsg = QString("报警！帧ID:%1 幅值/相位模式 通道数:%2")
            .arg(frame.frameId)
            .arg(frame.channelCount);
    } else if (frame.detectMode == FrameData::MultiChannelComplex) {
        alarmMsg = QString("报警！帧ID:%1 复数模式 通道数:%2")
            .arg(frame.frameId)
            .arg(frame.channelCount);
    } else {
        alarmMsg = QString("报警！帧ID:%1 Legacy模式（已弃用）")
            .arg(frame.frameId);
    }
    
    m_statsLabel->setText(alarmMsg);
}

void HeatMapPlotWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    Q_UNUSED(snapshot);
    // 热力图依赖 FrameData 中的台位字段；PlotSnapshot 不含台位，故不从快照更新。
}

