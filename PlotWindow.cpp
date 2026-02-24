#include "PlotWindow.h"
#include <QVBoxLayout>
#include <QDateTime>
#include <QDebug>

PlotWindow::PlotWindow(QWidget *parent) : QWidget(parent)
{
    // 窗口基础配置
    setWindowTitle("实时数据监控");
    resize(800, 600);

    // 初始化绘图控件
    m_plot = new QCustomPlot(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->addWidget(m_plot);
    setLayout(mainLayout);

    // 初始化绘图样式
    initPlot();

    // 保留定时器用于平滑动画（可选，可以移除或保留）
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(50);
    // 不再连接onRefreshTimer，数据由PlotWindowManager提供
    // m_refreshTimer->start(); // 暂时不启动，等待数据更新
}

PlotWindow::~PlotWindow()
{
    if (m_refreshTimer) {
        m_refreshTimer->stop();
    }
}

void PlotWindow::initPlot()
{
    // 添加温度/湿度曲线
    m_plot->addGraph();
    m_plot->graph(0)->setPen(QPen(Qt::red, 2));
    m_plot->graph(0)->setName("温度(℃)");

    m_plot->addGraph();
    m_plot->graph(1)->setPen(QPen(Qt::blue, 2));
    m_plot->graph(1)->setName("湿度(%RH)");

    // 坐标轴配置
    m_plot->xAxis->setLabel("时间(ms)");
    m_plot->yAxis->setLabel("数值");
    m_plot->yAxis->setRange(0, 100);
    m_plot->xAxis->setRange(0, 10000);

    // 图例配置
    m_plot->legend->setVisible(true);
    m_plot->legend->setFont(QFont("Microsoft YaHei", 9));
    // m_plot->legend->setPosition(QCPLegend::plTopRight); // 注释掉，使用默认位置

    // 样式优化
    m_plot->setBackground(Qt::white);
    //m_plot->setAntialiased(true);
    m_plot->xAxis->setTickLabelFont(QFont("Microsoft YaHei", 8));
    m_plot->yAxis->setTickLabelFont(QFont("Microsoft YaHei", 8));
}

void PlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (frames.isEmpty()) {
        return;
    }
    
    // 更新绘图数据
    updatePlotData(frames);
    
    // 如果定时器未启动，可以启动用于平滑动画
    if (m_refreshTimer && !m_refreshTimer->isActive()) {
        // 可选：启动定时器用于平滑重绘
        // m_refreshTimer->start();
    }
}

void PlotWindow::updatePlotData(const QVector<FrameData>& frames)
{
    // 更新本地绘图数据
    for (const auto& frame : frames) {
        m_xTime.append(frame.timestamp);
        m_yTemp.append(frame.temperature);
        m_yHumidity.append(frame.humidity);
    }

    // 限制最大点数，避免卡顿
    if (m_xTime.size() > MAX_PLOT_POINTS) {
        int removeCount = m_xTime.size() - MAX_PLOT_POINTS;
        m_xTime.remove(0, removeCount);
        m_yTemp.remove(0, removeCount);
        m_yHumidity.remove(0, removeCount);
    }

    // 更新曲线数据
    m_plot->graph(0)->setData(m_xTime, m_yTemp);
    m_plot->graph(1)->setData(m_xTime, m_yHumidity);

    // 自动调整X轴（显示最近10秒）
    if (!m_xTime.isEmpty()) {
        qint64 latestTime = m_xTime.last();
        m_plot->xAxis->setRange(latestTime - 10000, latestTime);
    }

    // 高效重绘（排队重绘，避免UI阻塞）
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotWindow::onCriticalFrame(const FrameData& frame)
{
    // 报警视觉提示：背景变红，2秒恢复
    m_plot->setBackground(QColor(255, 204, 204));
    QTimer::singleShot(2000, [this]() {
        m_plot->setBackground(Qt::white);
    });

    // 打印报警日志
    qCritical() << QString("【报警】帧%1：温度%2℃ 超过阈值！").arg(frame.frameId).arg(frame.temperature, 0, 'f', 1);
}
