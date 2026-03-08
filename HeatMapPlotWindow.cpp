#include "HeatMapPlotWindow.h"
#include "qcustomplot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QDebug>
#include <cmath>
#include <algorithm>

HeatMapPlotWindow::HeatMapPlotWindow(QWidget *parent)
    : PlotWindow(parent)
    , m_gridWidth(1024)
    , m_gridHeight(128)
    , m_dataMin(0.0)
    , m_dataMax(100.0)
    , m_displayMin(0.0)
    , m_displayMax(100.0)
{
    setWindowTitle("热力图");
    resize(1200, 600);

    // 清空从 PlotWindow 继承的布局
    QLayout* existingLayout = layout();
    if (existingLayout) {
        QLayoutItem* item;
        while ((item = existingLayout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }

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
    m_widthSpinBox->setMinimum(64);
    m_widthSpinBox->setMaximum(2048);
    m_widthSpinBox->setValue(1024);
    m_widthSpinBox->setSingleStep(128);
    
    m_heightSpinBox = new QSpinBox(this);
    m_heightSpinBox->setMinimum(32);
    m_heightSpinBox->setMaximum(512);
    m_heightSpinBox->setValue(128);
    m_heightSpinBox->setSingleStep(16);
    
    sizeLayout->addRow("宽度:", m_widthSpinBox);
    sizeLayout->addRow("高度:", m_heightSpinBox);
    
    // 数据范围控制
    QGroupBox* rangeGroup = new QGroupBox("数据范围", this);
    QFormLayout* rangeLayout = new QFormLayout(rangeGroup);
    
    m_minSpinBox = new QDoubleSpinBox(this);
    m_minSpinBox->setMinimum(-1000.0);
    m_minSpinBox->setMaximum(1000.0);
    m_minSpinBox->setValue(0.0);
    m_minSpinBox->setSingleStep(1.0);
    
    m_maxSpinBox = new QDoubleSpinBox(this);
    m_maxSpinBox->setMinimum(-1000.0);
    m_maxSpinBox->setMaximum(1000.0);
    m_maxSpinBox->setValue(100.0);
    m_maxSpinBox->setSingleStep(1.0);
    
    rangeLayout->addRow("最小值:", m_minSpinBox);
    rangeLayout->addRow("最大值:", m_maxSpinBox);
    
    // 动作按钮
    m_startButton = new QPushButton("启动模拟", this);
    m_stopButton = new QPushButton("停止模拟", this);
    m_exportButton = new QPushButton("导出图像", this);
    
    m_stopButton->setEnabled(false);
    
    controlLayout->addWidget(sizeGroup);
    controlLayout->addWidget(rangeGroup);
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
    m_plot->xAxis->setLabel("X 坐标");
    m_plot->yAxis->setLabel("Y 坐标");
    m_plot->xAxis->setRange(0, m_gridWidth);
    m_plot->yAxis->setRange(0, m_gridHeight);
    m_plot->xAxis->setNumberFormat("f");
    m_plot->yAxis->setNumberFormat("f");
    
    // 配置颜色标尺
    m_colorScale->setLabel("信号强度");
    m_colorScale->axis()->setLabel("强度");
    
    // 初始化网格数据到 QCPColorMap
    // 填充初始数据（所有值为 50）
    for (int x = 0; x < m_gridWidth; ++x) {
        for (int y = 0; y < m_gridHeight; ++y) {
            m_colorMap->data()->setCell(x, y, 50.0);
        }
    }
    
    m_plot->replot();
}

void HeatMapPlotWindow::setColorGradient()
{
    if (!m_colorMap) return;

    // 定义颜色梯度：浅黄→深橙→红
    QCPColorGradient gradient;
    gradient.clearColorStops();
    
    // 浅黄 (#FFFCB0)
    gradient.setColorStopAt(0.0, QColor(255, 252, 176));
    // 橙色 (#FFA500)
    gradient.setColorStopAt(0.5, QColor(255, 165, 0));
    // 深橙 (#FF8C00)
    gradient.setColorStopAt(0.75, QColor(255, 140, 0));
    // 红色 (#FF0000)
    gradient.setColorStopAt(1.0, QColor(255, 0, 0));
    
    m_colorMap->setGradient(gradient);
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

    // 计算实际数据范围
    double minVal = *std::min_element(m_gridData.begin(), m_gridData.end());
    double maxVal = *std::max_element(m_gridData.begin(), m_gridData.end());
    
    // 确保有足够的范围差异以显示颜色梯度
    if (std::fabs(maxVal - minVal) < 0.01) {
        minVal -= 2.0;
        maxVal += 2.0;
    }
    
    // 设置数据范围（必须在更新数据前或后设置，但很关键）
    m_colorMap->setDataRange(QCPRange(minVal, maxVal));
    
    // 逐个更新网格数据到 QCPColorMap
    for (int x = 0; x < m_gridWidth; ++x) {
        for (int y = 0; y < m_gridHeight; ++y) {
            int idx = y * m_gridWidth + x;  // row-major order: y * width + x
            if (idx >= 0 && idx < m_gridData.size()) {
                double val = m_gridData.at(idx);
                // 确保值在合理范围内
                val = std::max(minVal - 10.0, std::min(maxVal + 10.0, val));
                m_colorMap->data()->setCell(x, y, val);
            }
        }
    }

    // 更新统计信息
    double avgVal = 0.0;
    for (double val : m_gridData) {
        avgVal += val;
    }
    avgVal /= m_gridData.size();

    m_statsLabel->setText(QString("网格: %1×%2 | 最小: %3 | 最大: %4 | 平均: %5 | 帧数: %6")
                         .arg(m_gridWidth)
                         .arg(m_gridHeight)
                         .arg(minVal, 0, 'f', 2)
                         .arg(maxVal, 0, 'f', 2)
                         .arg(avgVal, 0, 'f', 2)
                         .arg(m_frameCount));

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
    if (width < 64 || height < 32) return;
    
    m_gridWidth = width;
    m_gridHeight = height;
    m_gridData.resize(m_gridWidth * m_gridHeight);
    std::fill(m_gridData.begin(), m_gridData.end(), 50.0);
    
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

void HeatMapPlotWindow::updateFromFrame(const FrameData& frame)
{
    // 占位：未来实现如何从 FrameData 更新热力图
    // 例如：将通道数据映射到热力图网格
    Q_UNUSED(frame);
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
    if (m_colorMap) {
        m_colorMap->setDataRange(QCPRange(m_displayMin, m_displayMax));
        m_plot->replot();
    }
}

void HeatMapPlotWindow::onDataMaxChanged(double max)
{
    m_displayMax = max;
    if (m_colorMap) {
        m_colorMap->setDataRange(QCPRange(m_displayMin, m_displayMax));
        m_plot->replot();
    }
}

void HeatMapPlotWindow::onStartMockDataClicked()
{
    m_useMockData = true;
    m_frameCount = 0;
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
    // 暂未实现 FrameData 的绑定，仅保留占位接口
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

