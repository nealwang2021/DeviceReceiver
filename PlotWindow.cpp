#include "PlotWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QDebug>
#include <cmath>

PlotWindow::PlotWindow(QWidget *parent) : PlotWindowBase(parent)
{
    qDebug() << "PlotWindow constructor begin";
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
    m_refreshTimer->setInterval(50);
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

    // 图例配置 - 启用交互功能
    m_plot->legend->setVisible(true);
    m_plot->legend->setFont(QFont("Microsoft YaHei", 9));
    m_plot->legend->setSelectableParts(QCPLegend::spItems); // 允许选择图例项
    // 图例位置默认在右上角，由QCustomPlot自动管理
    
    // 启用QCustomPlot交互功能
    m_plot->setInteractions(QCP::iSelectLegend | QCP::iSelectPlottables | QCP::iRangeDrag | QCP::iRangeZoom);

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
    qDebug() << "updatePlotData called with" << frames.size() << "frames, m_plot=" << m_plot;
    if (!m_plot) {
        qCritical() << "m_plot is null, aborting update";
        return;
    }

    // 根据帧的 detectMode 选择绘图策略（兼容 legacy 与 多通道实数）
    int idx = 0;
    for (const auto& frame : frames) {
        qDebug() << "processing frame" << idx << "id" << frame.frameId
                 << "mode" << frame.detectMode << "count" << frame.channelCount;
        idx++;
        if (frame.detectMode == FrameData::Legacy) {
            // legacy 行为：温度/湿度
            if (m_viewTypeCombo) m_viewTypeCombo->setVisible(false);
            m_xTime.append(frame.timestamp);
            m_yTemp.append(frame.temperature);
            m_yHumidity.append(frame.humidity);
            qDebug() << "legacy append done";
        } else if (frame.detectMode == FrameData::MultiChannelReal) {
            // 如果之前是 complex 模式，需要恢复简单布局
            if (m_lastMode == FrameData::MultiChannelComplex) {
                qDebug() << "mode switch complex->real, clearing axisRects";
                m_axisRects.clear(); // remove stale pointers
                if (m_plot && m_plot->plotLayout()) {
                    qDebug() << "clearing old layout for real mode";
                    m_plot->plotLayout()->clear();
                    m_plot->clearGraphs();
                    m_plot->plotLayout()->addElement(0,0,new QCPAxisRect(m_plot));
                    initPlot();
                    qDebug() << "simple layout initialized";
                }
                // reset channel state since layout changed
                m_currentChannelCount = 0;
                m_xTime.clear();
                m_yTemp.clear();
                m_yHumidity.clear();
                m_channelData.clear();
                m_channelData2.clear();
                if (m_viewTypeCombo) m_viewTypeCombo->setVisible(false);
            }
            // 单个多通道信号
            int ch = static_cast<int>(frame.channelCount);
            qDebug() << "entered real branch ch=" << ch;
            if (ch < 0 || ch > 200) {
                qWarning() << "Unreasonable channel count" << ch << "clamped";
                ch = qBound(0, ch, 200);
            }

            // 首次到达或通道数变化时，重建图形并清空旧数据
            if (m_currentChannelCount != ch) {
                qDebug() << "channel count changed from" << m_currentChannelCount << "to" << ch << ", rebuilding graphs";
                m_currentChannelCount = ch;
                // 清空现有数据，因为通道数改变了
                m_xTime.clear();
                m_yTemp.clear();
                m_yHumidity.clear();
                m_channelData.clear();
                m_channelData.resize(ch);
                m_plot->clearGraphs();
                for (int i = 0; i < ch; ++i) {
                    m_plot->addGraph();
                    QColor color = QColor::fromHsv((i * 36) % 360, 200, 200);
                    m_plot->graph(i)->setPen(QPen(color, 2));
                    m_plot->graph(i)->setName(QString("Ch%1").arg(i + 1));
                }
                // 不再需要同步通道列表，使用图例交互
            }

            m_xTime.append(frame.timestamp);
            for (int i = 0; i < ch; ++i) {
                double v = (i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN();
                m_channelData[i].append(v);
            }
            qDebug() << "real append done";
        } else if (frame.detectMode == FrameData::MultiChannelComplex) {
            // 切换到复数模式时或通道数变化时重建复杂布局
            int ch = static_cast<int>(frame.channelCount);
            if (ch < 0 || ch > 200) {
                qWarning() << "Unreasonable channel count" << ch << "clamped";
                ch = qBound(0, ch, 200);
            }
            if (m_lastMode != frame.detectMode || m_currentChannelCount != ch) {
                // 清空现有数据，因为模式改变了
                m_xTime.clear();
                m_yTemp.clear();
                m_yHumidity.clear();
                m_currentChannelCount = ch;
                setupComplexLayout(ch);
                if (m_viewTypeCombo) m_viewTypeCombo->setVisible(true);
            }

            // 时间轴
            m_xTime.append(frame.timestamp);
            // 计算数据显示数组
            QVector<double> topVals(ch);
            QVector<double> bottomVals(ch);
            for (int i = 0; i < ch; ++i) {
                double re = (i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN();
                double im = (i < frame.channels_comp1.size()) ? frame.channels_comp1.at(i) : qQNaN();
                if (m_complexViewType == RealImag) {
                    topVals[i] = re;
                    bottomVals[i] = im;
                } else {
                    topVals[i] = std::hypot(re, im);
                    bottomVals[i] = std::atan2(im, re);
                }
            }
            // 添加数据并保持点数限制
            for (int i = 0; i < ch; ++i) {
                m_channelData[i].append(topVals[i]);
                m_channelData2[i].append(bottomVals[i]);
            }
            qDebug() << "complex append done";
        }

        // 更新上一个模式以避免在同一批帧中重复重建布局
        m_lastMode = frame.detectMode;
    }

    // 记录最新模式以便后续判断
    if (!frames.isEmpty()) {
        // m_lastMode 已在循环中设置
    }
    qDebug() << "updatePlotData finished, lastMode=" << m_lastMode;

    // 限制最大点数，避免卡顿（对通道数据与 legacy 数据分别处理）
    if (m_xTime.size() > MAX_PLOT_POINTS) {
        int removeCount = m_xTime.size() - MAX_PLOT_POINTS;
        m_xTime.remove(0, removeCount);
        if (!m_yTemp.isEmpty()) m_yTemp.remove(0, removeCount);
        if (!m_yHumidity.isEmpty()) m_yHumidity.remove(0, removeCount);
        for (auto &vec : m_channelData) {
            if (vec.size() > removeCount)
                vec.remove(0, removeCount);
            else
                vec.clear();
        }
        for (auto &vec : m_channelData2) {
            if (vec.size() > removeCount)
                vec.remove(0, removeCount);
            else
                vec.clear();
        }
    }

    // 更新曲线数据
    if (m_lastMode == FrameData::MultiChannelComplex && m_currentChannelCount > 0) {
        // complex mode: graphs are arranged top-channels then bottom-channels
        for (int i = 0; i < m_currentChannelCount; ++i) {
            int topIdx = i;
            int bottomIdx = m_currentChannelCount + i;
            if (topIdx < m_plot->graphCount())
                m_plot->graph(topIdx)->setData(m_xTime, m_channelData.at(i));
            if (bottomIdx < m_plot->graphCount())
                m_plot->graph(bottomIdx)->setData(m_xTime, m_channelData2.at(i));
        }
    } else if (m_currentChannelCount > 0 && !m_channelData.isEmpty()) {
        for (int i = 0; i < m_currentChannelCount; ++i) {
            if (i < m_plot->graphCount())
                m_plot->graph(i)->setData(m_xTime, m_channelData.at(i));
        }
    } else {
        // legacy 回退
        if (m_plot->graphCount() < 2) {
            // 保证至少有两个曲线用于 legacy
            m_plot->clearGraphs();
            m_plot->addGraph(); m_plot->graph(0)->setPen(QPen(Qt::red, 2)); m_plot->graph(0)->setName("温度(℃)");
            m_plot->addGraph(); m_plot->graph(1)->setPen(QPen(Qt::blue, 2)); m_plot->graph(1)->setName("湿度(%RH)");
        }
        m_plot->graph(0)->setData(m_xTime, m_yTemp);
        m_plot->graph(1)->setData(m_xTime, m_yHumidity);
    }

    // 自动调整X轴（显示最近10秒）
    if (!m_xTime.isEmpty()) {
        qint64 latestTime = m_xTime.last();
        if (m_lastMode == FrameData::MultiChannelComplex) {
            for (auto rect : m_axisRects) {
                if (!rect) continue;
                if (auto axis = rect->axis(QCPAxis::atBottom)) {
                    axis->setRange(latestTime - 10000, latestTime);
                }
            }
        } else {
            if (m_plot && m_plot->xAxis) {
                m_plot->xAxis->setRange(latestTime - 10000, latestTime);
            }
        }
    }

    // 高效重绘（排队重绘，避免UI阻塞）
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
            gTop->setPen(QPen(color, 2));
            gBottom->setPen(QPen(color, 2));
            gTop->setName(QString("Ch%1(R)").arg(i + 1));
            gBottom->setName(QString("Ch%1(I)").arg(i + 1));
            qDebug() << "[setupComplexLayout] 通道" << i << "图形创建完成";
        }
        qDebug() << "[setupComplexLayout] 所有通道曲线创建完成";

        // Legend: 为复杂模式配置图例交互
        qDebug() << "[setupComplexLayout] 配置图例...";
        m_plot->legend->setVisible(true);
        m_plot->legend->setFont(QFont("Microsoft YaHei", 9));
        m_plot->legend->setSelectableParts(QCPLegend::spItems); // 允许选择图例项
        qDebug() << "[setupComplexLayout] 图例配置完成";

        // ensure channel data containers have proper size
        qDebug() << "[setupComplexLayout] 调整数据容器大小...";
        m_channelData.clear();
        m_channelData.resize(channelCount);
        m_channelData2.clear();
        m_channelData2.resize(channelCount);
        // 预分配每通道数据以避免反复重分配
        for (auto &vec : m_channelData) {
            vec.reserve(MAX_PLOT_POINTS);
        }
        for (auto &vec : m_channelData2) {
            vec.reserve(MAX_PLOT_POINTS);
        }
        qDebug() << "[setupComplexLayout] 数据容器已调整，大小=" << channelCount;
        
        qDebug() << "[setupComplexLayout] 完成";
    } catch (const std::exception& e) {
        qCritical() << "[setupComplexLayout] 异常:" << e.what();
    } catch (...) {
        qCritical() << "[setupComplexLayout] 未知异常";
    }
}

void PlotWindow::onViewTypeChanged(int index)
{
    m_complexViewType = static_cast<ComplexViewType>(index);
    // 清空已有数据，以便下一次 updatePlotData 重新计算
    for (auto &vec : m_channelData) vec.clear();
    for (auto &vec : m_channelData2) vec.clear();
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
    if (visible) {
        plotItem->setTextColor(Qt::black);
    } else {
        plotItem->setTextColor(Qt::gray);
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
        m_plot->setBackground(Qt::white);
    });

    // 打印报警日志
    qCritical() << QString("【报警】帧%1：温度%2℃ 超过阈值！").arg(frame.frameId).arg(frame.temperature, 0, 'f', 1);
}
