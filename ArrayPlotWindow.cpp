#include "ArrayPlotWindow.h"
#include "FrameData.h"
#include "qcustomplot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <cmath>
#include <algorithm>

ArrayPlotWindow::ArrayPlotWindow(QWidget *parent)
    : PlotWindow(parent)
{
    setWindowTitle(QStringLiteral("多通道阵列图窗口"));
    resize(1200, 700);
    
    // 清除继承的布局
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
    
    // 创建新的主布局
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(5);
    
    // 创建控制按钮面板
    QHBoxLayout *controlLayout = new QHBoxLayout();
    QPushButton *startButton = new QPushButton(QStringLiteral("启动模拟"));
    QPushButton *stopButton = new QPushButton(QStringLiteral("停止模拟"));
    stopButton->setEnabled(false);
    m_statsLabel = new QLabel(QStringLiteral("状态：就绪"));
    
    controlLayout->addWidget(startButton);
    controlLayout->addWidget(stopButton);
    controlLayout->addWidget(m_statsLabel);
    controlLayout->addStretch();
    
    // 创建QCustomPlot实例
    m_plot = new QCustomPlot(this);
    
    // 初始化数组图（默认8个通道）
    initArrayPlot();
    
    // 添加到主布局
    mainLayout->addLayout(controlLayout);
    mainLayout->addWidget(m_plot, 1);
    centralWidget->setLayout(mainLayout);
    
    // 创建主窗口布局并添加中心部件
    if (!this->layout()) {
        setLayout(new QVBoxLayout());
    }
    this->layout()->addWidget(centralWidget);
    
    // 设置定时器用于模拟数据更新
    m_mockDataTimer = new QTimer(this);
    connect(m_mockDataTimer, &QTimer::timeout, this, &ArrayPlotWindow::generateMockData);
    
    // 按钮信号连接
    connect(startButton, &QPushButton::clicked, this, [this, startButton, stopButton]() {
        m_useMockData = true;
        m_frameCount = 0;
        m_mockDataTimer->start(100); // 每100ms更新一次
        m_statsLabel->setText(QStringLiteral("状态：模拟数据运行中..."));
        startButton->setEnabled(false);
        stopButton->setEnabled(true);
    });
    
    connect(stopButton, &QPushButton::clicked, this, [this, startButton, stopButton]() {
        m_useMockData = false;
        m_mockDataTimer->stop();
        m_statsLabel->setText(QStringLiteral("状态：已停止"));
        startButton->setEnabled(true);
        stopButton->setEnabled(false);
    });
}

ArrayPlotWindow::~ArrayPlotWindow()
{
    if (m_mockDataTimer) {
        m_mockDataTimer->stop();
        m_mockDataTimer->deleteLater();
    }
}

void ArrayPlotWindow::initArrayPlot()
{
    if (!m_plot) return;
    
    // 清除已有的布局和图
    m_plot->clearPlottables();
    m_plot->clearItems();
    m_plot->plotLayout()->clear();
    
    // 设置默认通道数（若外部已指定通道数则保持）
    if (m_currentChannelCount <= 0) {
        m_currentChannelCount = 8;
    }
    m_channelAxisRects.clear();
    m_timeAxis.clear();
    m_latestTime = 0.0;
    m_axisUpdateCounter = 0;
    
    // 创建通道轴矩形（垂直堆叠）
    for (int i = 0; i < m_currentChannelCount; i++) {
        QCPAxisRect* axisRect = new QCPAxisRect(m_plot, true);
        m_plot->plotLayout()->addElement(i, 0, axisRect);
        m_channelAxisRects.append(axisRect);
        
        // 为每个轴矩形创建图
        QCPGraph* graph = m_plot->addGraph(axisRect->axis(QCPAxis::atBottom), axisRect->axis(QCPAxis::atLeft));
        
        // 设置线条颜色和样式
        QColor color = QColor::fromHsv((i * 36) % 360, 200, 200);
        graph->setPen(QPen(color, 2));
        
        // 配置Y轴标签
        axisRect->axis(QCPAxis::atLeft)->setLabel(QStringLiteral("通道 %1").arg(i + 1));
        
        // 隐藏底部X轴（除了最后一个）
        if (i < m_currentChannelCount - 1) {
            axisRect->axis(QCPAxis::atBottom)->setVisible(false);
        } else {
            axisRect->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("时间 (s)"));
        }
    }
    
    // 同步所有X轴缩放和平移
    for (int i = 0; i < m_currentChannelCount - 1; i++) {
        QCPAxis* xAxisCurrent = m_channelAxisRects[i]->axis(QCPAxis::atBottom);
        QCPAxis* xAxisNext = m_channelAxisRects[i + 1]->axis(QCPAxis::atBottom);
        
        connect(xAxisCurrent, static_cast<void (QCPAxis::*)(const QCPRange &)>(&QCPAxis::rangeChanged),
                xAxisNext, [xAxisNext](const QCPRange &newRange) { xAxisNext->setRange(newRange); });
    }
    
    // 调整数据容器大小
    m_channelValues.resize(m_currentChannelCount);
    m_channelValues2.resize(m_currentChannelCount);
    
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ArrayPlotWindow::generateMockData()
{
    if (!m_useMockData) return;
    
    // 追加新的数据点
    const double t = m_timeAxis.isEmpty() ? 0.0 : m_timeAxis.last() + 0.1;
    m_timeAxis.append(t);
    m_latestTime = t;
    
    // 生成虚拟数据
    for (int ch = 0; ch < m_currentChannelCount; ch++) {
        const double val = 10.0 + 3.0 * ch + 5.0 * std::sin(t * (0.5 + ch * 0.04) + (m_frameCount * 0.01));
        if (ch < m_plot->graphCount()) {
            QCPGraph* graph = m_plot->graph(ch);
            graph->addData(t, val);
            if ((m_axisUpdateCounter % m_axisUpdateStride) == 0) {
                graph->rescaleValueAxis(false);
            }
        }
    }
    
    // 如果数据超过最大点数，按时间裁剪（与组合图一致）
    if (m_timeAxis.size() > m_maxDataPoints) {
        const int removeCount = m_timeAxis.size() - m_maxDataPoints;
        const double cutoffKey = m_timeAxis.at(removeCount);
        m_timeAxis.remove(0, removeCount);
        for (int i = 0; i < m_plot->graphCount(); ++i) {
            m_plot->graph(i)->data()->removeBefore(cutoffKey);
        }
    }
    
    ++m_axisUpdateCounter;
    m_frameCount++;
    updateArrayData();
}

void ArrayPlotWindow::updateArrayData()
{
    if (!m_plot || m_channelAxisRects.isEmpty() || m_timeAxis.isEmpty()) return;

    const double lower = m_timeAxis.first();
    double upper = m_timeAxis.last();
    if (upper <= lower) {
        upper = lower + 1.0;
    }

    for (auto axisRect : m_channelAxisRects) {
        axisRect->axis(QCPAxis::atBottom)->setRange(lower, upper);
    }

    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ArrayPlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (frames.isEmpty() || m_useMockData) return;
    
    for (const FrameData& frame : frames) {
        // 检查模式变化
        if (frame.detectMode != m_lastMode || frame.channelCount != m_currentChannelCount) {
            // 模式或通道数变化，需要重建布局
            m_lastMode = frame.detectMode;
            m_currentChannelCount = qBound(1, static_cast<int>(frame.channelCount), 200);
            
            // 清空现有数据
            m_timeAxis.clear();
            m_channelValues.clear();
            m_channelValues2.clear();
            m_latestTime = 0.0;
            m_axisUpdateCounter = 0;
            
            // 重新初始化绘图
            initArrayPlot();
        }
        
        const double t = (frame.timestamp > 0)
            ? static_cast<double>(frame.timestamp)
            : (m_timeAxis.isEmpty() ? 0.0 : m_timeAxis.last() + 0.1);
        m_timeAxis.append(t);
        m_latestTime = t;
        
        // 根据模式处理数据
        if (frame.detectMode == FrameData::MultiChannelReal) {
            // 幅值/相位模式（阵列图当前展示 comp0 幅值分量）
            for (int ch = 0; ch < m_currentChannelCount; ch++) {
                double val = (ch < frame.channels_comp0.size()) ? frame.channels_comp0.at(ch) : qQNaN();
                if (ch < m_plot->graphCount()) {
                    QCPGraph* graph = m_plot->graph(ch);
                    graph->addData(t, val);
                    if ((m_axisUpdateCounter % m_axisUpdateStride) == 0) {
                        graph->rescaleValueAxis(false);
                    }
                }
            }
        } else if (frame.detectMode == FrameData::MultiChannelComplex) {
            // 复数模式
            for (int ch = 0; ch < m_currentChannelCount; ch++) {
                double re = (ch < frame.channels_comp0.size()) ? frame.channels_comp0.at(ch) : qQNaN();
                if (ch < m_plot->graphCount()) {
                    QCPGraph* graph = m_plot->graph(ch);
                    graph->addData(t, re);
                    if ((m_axisUpdateCounter % m_axisUpdateStride) == 0) {
                        graph->rescaleValueAxis(false);
                    }
                }
            }
        }
        
        // 如果数据超过最大点数，按时间裁剪
        if (m_timeAxis.size() > m_maxDataPoints) {
            const int removeCount = m_timeAxis.size() - m_maxDataPoints;
            const double cutoffKey = m_timeAxis.at(removeCount);
            m_timeAxis.remove(0, removeCount);
            for (int i = 0; i < m_plot->graphCount(); ++i) {
                m_plot->graph(i)->data()->removeBefore(cutoffKey);
            }
        }

        ++m_axisUpdateCounter;
    }
    
    updateArrayData();
}

void ArrayPlotWindow::onCriticalFrame(const FrameData& frame)
{
    QString alarmMsg;
    if (frame.detectMode == FrameData::MultiChannelReal) {
        alarmMsg = QStringLiteral("警报！帧ID:%1 幅值/相位模式 通道数:%2")
            .arg(frame.frameId)
            .arg(frame.channelCount);
    } else if (frame.detectMode == FrameData::MultiChannelComplex) {
        alarmMsg = QStringLiteral("警报！帧ID:%1 复数模式 通道数:%2")
            .arg(frame.frameId)
            .arg(frame.channelCount);
    } else {
        alarmMsg = QStringLiteral("警报！帧ID:%1 Legacy模式（已弃用）")
            .arg(frame.frameId);
    }
    
    m_statsLabel->setText(alarmMsg);
}