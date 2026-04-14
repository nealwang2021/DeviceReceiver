#include "ArrayPlotWindow.h"
#include "AppConfig.h"
#include "FrameData.h"
#include "qcustomplot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QSignalBlocker>
#include <QTimer>
#include <QtGlobal>
#include <set>
#include <cmath>
#include <algorithm>
#include <limits>

namespace {

constexpr int kArrayRenderPointCap = 3000;

QVector<int> buildSampleIndices(int totalPoints, int targetPoints)
{
    QVector<int> indices;
    if (totalPoints <= 0 || targetPoints <= 0) {
        return indices;
    }

    if (totalPoints <= targetPoints) {
        indices.reserve(totalPoints);
        for (int i = 0; i < totalPoints; ++i) {
            indices.append(i);
        }
        return indices;
    }

    indices.reserve(targetPoints);
    const double step = static_cast<double>(totalPoints - 1) / static_cast<double>(targetPoints - 1);
    int lastIndex = -1;
    for (int i = 0; i < targetPoints; ++i) {
        int idx = static_cast<int>(std::lround(i * step));
        idx = qBound(0, idx, totalPoints - 1);
        if (idx != lastIndex) {
            indices.append(idx);
            lastIndex = idx;
        }
    }
    if (indices.isEmpty() || indices.last() != totalPoints - 1) {
        indices.append(totalPoints - 1);
    }
    return indices;
}

QVector<double> sampleByIndices(const QVector<double>& source, const QVector<int>& indices)
{
    QVector<double> sampled;
    sampled.reserve(indices.size());
    for (int idx : indices) {
        if (idx >= 0 && idx < source.size()) {
            sampled.append(source.at(idx));
        }
    }
    return sampled;
}

bool perfLogEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("DEVICE_RECEIVER_PERF_LOG") > 0;
    return enabled;
}

} // namespace

ArrayPlotWindow::ArrayPlotWindow(QWidget *parent)
    : PlotWindowBase(parent)
{
    if (AppConfig* config = AppConfig::instance()) {
        m_maxDataPoints = qMax(100, config->maxPlotPoints());
    }

    setWindowTitle(QStringLiteral("多通道阵列图窗口"));
    resize(1200, 700);

    // 创建新的主布局
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(3, 3, 3, 3);
    mainLayout->setSpacing(3);
    
    // 创建控制按钮面板
    QHBoxLayout *controlLayout = new QHBoxLayout();
    m_exportButton = new QPushButton(QStringLiteral("导出图像"), this);
    QLabel *componentLabel = new QLabel(QStringLiteral("分量:"));
    QLabel *rowLabelLabel = new QLabel(QStringLiteral("行标识:"));
    QLabel *densityLabel = new QLabel(QStringLiteral("密度:"));
    QLabel *yAxisLabel = new QLabel(QStringLiteral("Y轴:"));
    m_componentCombo = new QComboBox(this);
    m_componentCombo->addItem(QStringLiteral("幅值"));
    m_componentCombo->addItem(QStringLiteral("相位"));
    m_componentCombo->addItem(QStringLiteral("实部"));
    m_componentCombo->addItem(QStringLiteral("虚部"));
    m_componentCombo->setToolTip(QStringLiteral("复数模式下选择每行波形显示分量"));
    m_rowLabelCombo = new QComboBox(this);
    m_rowLabelCombo->addItem(QStringLiteral("通道号"));
    m_rowLabelCombo->addItem(QStringLiteral("显示索引"));
    m_rowLabelCombo->addItem(QStringLiteral("来源通道"));
    m_rowLabelCombo->setToolTip(QStringLiteral("切换每行 Y 轴标签语义"));
    m_densityCombo = new QComboBox(this);
    m_densityCombo->addItem(QStringLiteral("紧凑"));
    m_densityCombo->addItem(QStringLiteral("标准"));
    m_densityCombo->addItem(QStringLiteral("宽松"));
    m_densityCombo->setCurrentIndex(0);
    m_densityCombo->setToolTip(QStringLiteral("调整每通道显示高度"));
    m_yAxisCombo = new QComboBox(this);
    m_yAxisCombo->addItem(QStringLiteral("自动"));
    m_yAxisCombo->addItem(QStringLiteral("固定"));
    m_yAxisCombo->setCurrentIndex(0);
    m_yAxisCombo->setToolTip(QStringLiteral("自动自适应或锁定当前统一范围"));
    m_statsLabel = new QLabel(QStringLiteral("状态：就绪"));
    controlLayout->setSpacing(3);
    
    controlLayout->addWidget(componentLabel);
    controlLayout->addWidget(m_componentCombo);
    controlLayout->addWidget(rowLabelLabel);
    controlLayout->addWidget(m_rowLabelCombo);
    controlLayout->addWidget(densityLabel);
    controlLayout->addWidget(m_densityCombo);
    controlLayout->addWidget(yAxisLabel);
    controlLayout->addWidget(m_yAxisCombo);
    controlLayout->addWidget(m_exportButton);
    controlLayout->addWidget(m_statsLabel);
    controlLayout->addStretch();

    m_channelScrollArea = new QScrollArea(this);
    m_channelScrollArea->setWidgetResizable(true);
    m_channelScrollArea->setFrameShape(QFrame::NoFrame);
    m_channelScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_channelScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_channelScrollArea->setFixedHeight(30);

    m_channelSelectorWidget = new QWidget(m_channelScrollArea);
    m_channelSelectorWidget->setLayout(new QHBoxLayout());
    m_channelSelectorWidget->layout()->setContentsMargins(0, 0, 0, 0);
    m_channelSelectorWidget->layout()->setSpacing(3);
    m_channelScrollArea->setWidget(m_channelSelectorWidget);
    
    // 创建QCustomPlot实例
    m_plot = new QCustomPlot(this);
    m_plot->plotLayout()->setRowSpacing(1);

    m_plotScrollArea = new QScrollArea(this);
    m_plotScrollArea->setWidget(m_plot);
    m_plotScrollArea->setWidgetResizable(true);
    m_plotScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_plotScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_plotScrollArea->setFrameShape(QFrame::NoFrame);
    
    // 初始化数组图（默认8个通道）
    initArrayPlot();
    
    // 添加到主布局
    mainLayout->addLayout(controlLayout);
    mainLayout->addWidget(m_channelScrollArea);
    mainLayout->addWidget(m_plotScrollArea, 1);
    centralWidget->setLayout(mainLayout);
    
    // 创建主窗口布局并添加中心部件
    if (!this->layout()) {
        setLayout(new QVBoxLayout());
    }
    this->layout()->addWidget(centralWidget);
    
    // 设置定时器用于模拟数据更新
    m_mockDataTimer = new QTimer(this);
    connect(m_mockDataTimer, &QTimer::timeout, this, &ArrayPlotWindow::generateMockData);

    connect(m_componentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                switch (index) {
                case 1:
                    m_componentMode = ArrayComponent::Phase;
                    break;
                case 2:
                    m_componentMode = ArrayComponent::Real;
                    break;
                case 3:
                    m_componentMode = ArrayComponent::Imag;
                    break;
                case 0:
                default:
                    m_componentMode = ArrayComponent::Amplitude;
                    break;
                }

                if (m_cachedSnapshot) {
                    renderSnapshot(m_cachedSnapshot, true);
                }
            });

    connect(m_rowLabelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                switch (index) {
                case 1:
                    m_rowLabelMode = RowLabelMode::DisplayIndex;
                    break;
                case 2:
                    m_rowLabelMode = RowLabelMode::SourceChannel;
                    break;
                case 0:
                default:
                    m_rowLabelMode = RowLabelMode::ChannelIndex;
                    break;
                }

                if (m_cachedSnapshot) {
                    renderSnapshot(m_cachedSnapshot, true);
                } else {
                    for (int i = 0; i < m_channelAxisRects.size(); ++i) {
                        m_channelAxisRects[i]->axis(QCPAxis::atLeft)->setLabel(QStringLiteral("CH%1").arg(i + 1));
                    }
                    m_plot->replot(QCustomPlot::rpQueuedReplot);
                }
            });

    connect(m_densityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                switch (index) {
                case 1:
                    m_layoutDensity = LayoutDensity::Standard;
                    break;
                case 2:
                    m_layoutDensity = LayoutDensity::Comfortable;
                    break;
                case 0:
                default:
                    m_layoutDensity = LayoutDensity::Compact;
                    break;
                }
                initArrayPlot();
                if (m_cachedSnapshot) {
                    renderSnapshot(m_cachedSnapshot, true);
                }
            });

    connect(m_yAxisCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index == 1) {
                    m_yAxisMode = YAxisMode::Fixed;
                    m_yAxisRangeValid = false;
                } else {
                    m_yAxisMode = YAxisMode::Auto;
                }
                updateUnifiedYAxisRange();
                m_plot->replot(QCustomPlot::rpQueuedReplot);
            });

    connect(m_exportButton, &QPushButton::clicked,
            this, &ArrayPlotWindow::onExportClicked);

    m_perfLogTimer.start();
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

    const bool dark = isDarkThemeActive();
    m_plot->setBackground(QBrush(dark ? QColor(24, 26, 30) : QColor(255, 255, 255)));
    const QColor rowBgEven = dark ? QColor(36, 40, 46) : QColor(248, 251, 255);
    const QColor rowBgOdd = dark ? QColor(31, 35, 41) : QColor(241, 246, 252);
    const QColor gridBottom = dark ? QColor(74, 82, 94) : QColor(210, 220, 232);
    const QColor gridLeft = dark ? QColor(64, 72, 84) : QColor(198, 208, 220);
    const QColor tickColor = dark ? QColor(200, 208, 220) : QColor(70, 78, 90);
    const QColor labelColor = dark ? QColor(222, 228, 236) : QColor(50, 58, 70);
    const QColor axisColor = dark ? QColor(152, 162, 176) : QColor(138, 148, 160);
    
    // 清除已有的布局和图
    m_plot->clearPlottables();
    m_plot->clearItems();
    m_plot->plotLayout()->clear();
    
    // 设置默认通道数（若外部已指定通道数则保持）
    if (m_currentChannelCount <= 0) {
        m_currentChannelCount = 8;
    }

    QVector<bool> oldVisible = m_channelVisible;
    m_channelVisible.fill(true, m_currentChannelCount);
    for (int i = 0; i < std::min(oldVisible.size(), m_channelVisible.size()); ++i) {
        m_channelVisible[i] = oldVisible[i];
    }

    m_channelAxisRects.clear();
    m_timeAxis.clear();
    m_latestTime = 0.0;
    m_axisUpdateCounter = 0;
    m_channelDataMin.clear();
    m_channelDataMax.clear();
    
    // 创建通道轴矩形（垂直堆叠）
    for (int i = 0; i < m_currentChannelCount; i++) {
        QCPAxisRect* axisRect = new QCPAxisRect(m_plot, true);
        m_plot->plotLayout()->addElement(i, 0, axisRect);
        m_channelAxisRects.append(axisRect);

        axisRect->setMinimumMargins(QMargins(48, 3, 8, 3));
        axisRect->setAutoMargins(QCP::msLeft | QCP::msBottom);
        axisRect->setBackground((i % 2 == 0) ? rowBgEven : rowBgOdd);
        
        // 为每个轴矩形创建图
        QCPGraph* graph = m_plot->addGraph(axisRect->axis(QCPAxis::atBottom), axisRect->axis(QCPAxis::atLeft));
        
        // 设置线条颜色和样式
        QColor color = QColor::fromHsv((i * 36) % 360, 200, 200);
        graph->setPen(QPen(color, 2));
        graph->setVisible(i < m_channelVisible.size() ? m_channelVisible.at(i) : true);

        axisRect->axis(QCPAxis::atBottom)->grid()->setVisible(true);
        axisRect->axis(QCPAxis::atBottom)->grid()->setPen(QPen(gridBottom, 1, Qt::DotLine));
        axisRect->axis(QCPAxis::atLeft)->grid()->setVisible(true);
        axisRect->axis(QCPAxis::atLeft)->grid()->setPen(QPen(gridLeft, 1, Qt::DotLine));
        axisRect->axis(QCPAxis::atBottom)->setTickLabelColor(tickColor);
        axisRect->axis(QCPAxis::atLeft)->setTickLabelColor(tickColor);
        axisRect->axis(QCPAxis::atLeft)->setLabelColor(labelColor);
        axisRect->axis(QCPAxis::atLeft)->setBasePen(QPen(axisColor));
        axisRect->axis(QCPAxis::atBottom)->setBasePen(QPen(axisColor));
        axisRect->axis(QCPAxis::atLeft)->setTickPen(QPen(axisColor));
        axisRect->axis(QCPAxis::atBottom)->setTickPen(QPen(axisColor));
        axisRect->axis(QCPAxis::atLeft)->setSubTickPen(QPen(axisColor));
        axisRect->axis(QCPAxis::atBottom)->setSubTickPen(QPen(axisColor));
        axisRect->axis(QCPAxis::atLeft)->setNumberFormat("f");
        axisRect->axis(QCPAxis::atLeft)->setNumberPrecision(0);
        axisRect->axis(QCPAxis::atLeft)->setSubTicks(false);
        axisRect->axis(QCPAxis::atLeft)->setTickLabelPadding(1);
        QFont yTickFont = axisRect->axis(QCPAxis::atLeft)->tickLabelFont();
        const qreal baseSize = yTickFont.pointSizeF() > 0 ? yTickFont.pointSizeF() : 8.0;
        yTickFont.setPointSizeF(qMax(6.0, baseSize - 1.0));
        axisRect->axis(QCPAxis::atLeft)->setTickLabelFont(yTickFont);
        
        // 配置Y轴标签
        axisRect->axis(QCPAxis::atLeft)->setLabel(QStringLiteral("CH%1").arg(i + 1));
        
        // 隐藏底部X轴（除了最后一个）
        if (i < m_currentChannelCount - 1) {
            axisRect->axis(QCPAxis::atBottom)->setVisible(false);
        } else {
            axisRect->axis(QCPAxis::atBottom)->setLabel(QStringLiteral("t(ms)"));
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

    const int perRowHeight = perRowHeightForDensity();
    const int basePadding = (m_layoutDensity == LayoutDensity::Compact) ? 24 : 30;
    const int adaptiveFloor = qMax(150, perRowHeight * 2 + basePadding);
    const int minPlotHeight = qMax(adaptiveFloor, m_currentChannelCount * perRowHeight + basePadding);
    m_plot->setMinimumHeight(minPlotHeight);

    rebuildChannelSelector();
    applyChannelVisibility();
    
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ArrayPlotWindow::onThemeChanged()
{
    initArrayPlot();
    if (m_cachedSnapshot) {
        renderSnapshot(m_cachedSnapshot, true);
    } else if (m_plot) {
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void ArrayPlotWindow::generateMockData()
{
    if (!m_useMockData) return;
    
    // 追加新的数据点
    const double t = m_timeAxis.isEmpty() ? 0.0 : m_timeAxis.last() + 100.0;
    const double tSec = t / 1000.0;
    m_timeAxis.append(t);
    m_latestTime = t;
    
    // 生成虚拟数据
    for (int ch = 0; ch < m_currentChannelCount; ch++) {
        const double val = 10.0 + 3.0 * ch + 5.0 * std::sin(tSec * (0.5 + ch * 0.04) + (m_frameCount * 0.01));
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
        upper = lower + 100.0;
    }

    for (auto axisRect : m_channelAxisRects) {
        axisRect->axis(QCPAxis::atBottom)->setRange(lower, upper);
    }

    applyChannelVisibility();
    updateUnifiedYAxisRange();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ArrayPlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    Q_UNUSED(frames);
}

void ArrayPlotWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    renderSnapshot(snapshot, false);
}

void ArrayPlotWindow::rebuildChannelSelector()
{
    if (!m_channelSelectorWidget) {
        return;
    }

    auto* selectorLayout = qobject_cast<QHBoxLayout*>(m_channelSelectorWidget->layout());
    if (!selectorLayout) {
        selectorLayout = new QHBoxLayout(m_channelSelectorWidget);
        selectorLayout->setContentsMargins(0, 0, 0, 0);
        selectorLayout->setSpacing(4);
    }

    while (QLayoutItem* item = selectorLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    m_channelChecks.clear();
    for (int i = 0; i < m_currentChannelCount; ++i) {
        const bool checked = i < m_channelVisible.size() ? m_channelVisible.at(i) : true;
        auto* checkBox = new QCheckBox(QStringLiteral("CH%1").arg(i + 1), m_channelSelectorWidget);
        checkBox->setChecked(checked);
        selectorLayout->addWidget(checkBox);
        m_channelChecks.append(checkBox);

        connect(checkBox, &QCheckBox::toggled, this, [this, i](bool enabled) {
            if (i >= 0 && i < m_channelVisible.size()) {
                m_channelVisible[i] = enabled;
            }
            applyChannelVisibility();
            updateUnifiedYAxisRange();
            m_plot->replot(QCustomPlot::rpQueuedReplot);
        });
    }
    selectorLayout->addStretch();
}

void ArrayPlotWindow::exportImage(const QString& filename)
{
    if (!m_plot) {
        return;
    }

    const int exportWidth = qMax(1600, m_plot->width());
    const int exportHeight = qMax(
        900,
        qMax(m_plot->height(), m_currentChannelCount * perRowHeightForDensity() + 180));
    const bool ok = m_plot->savePng(filename, exportWidth, exportHeight, 1.0, 100);
    if (ok) {
        if (m_statsLabel) {
            m_statsLabel->setText(QStringLiteral("已导出: %1").arg(filename));
        }
        qInfo() << "阵列图已导出:" << filename << "size=" << exportWidth << "x" << exportHeight;
    } else {
        if (m_statsLabel) {
            m_statsLabel->setText(QStringLiteral("导出失败"));
        }
        qWarning() << "阵列图导出失败:" << filename;
    }
}

void ArrayPlotWindow::onExportClicked()
{
    QString defaultDir = QStringLiteral("exports");
    if (AppConfig* config = AppConfig::instance()) {
        if (!config->defaultExportDirectory().trimmed().isEmpty()) {
            defaultDir = config->defaultExportDirectory();
        }
    }

    QDir appDir(QCoreApplication::applicationDirPath());
    const QString exportDirPath = QDir::isAbsolutePath(defaultDir)
        ? defaultDir
        : appDir.filePath(defaultDir);
    QDir exportDir(exportDirPath);
    if (!exportDir.exists()) {
        exportDir.mkpath(QStringLiteral("."));
    }

    const QString defaultName = QStringLiteral("array_plot_%1.png")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    QString filename = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出阵列图"),
        exportDir.filePath(defaultName),
        QStringLiteral("PNG 图像 (*.png);;PNG 图像 (*.png)"));

    if (filename.isEmpty()) {
        return;
    }
    if (QFileInfo(filename).suffix().isEmpty()) {
        filename += QStringLiteral(".png");
    }

    exportImage(filename);
}

int ArrayPlotWindow::perRowHeightForDensity() const
{
    int baseHeight = 0;
    if (AppConfig* config = AppConfig::instance()) {
        const int configuredHeight = config->arrayPlotRowHeightPx();
        if (configuredHeight > 0) {
            baseHeight = configuredHeight;
        }
    }

    if (baseHeight <= 0) {
        switch (m_layoutDensity) {
        case LayoutDensity::Compact:
            return 36;
        case LayoutDensity::Comfortable:
            return 51;
        case LayoutDensity::Standard:
        default:
            return 43;
        }
    }

    double scale = 1.0;
    switch (m_layoutDensity) {
    case LayoutDensity::Compact:
        scale = 0.85;
        break;
    case LayoutDensity::Comfortable:
        scale = 1.20;
        break;
    case LayoutDensity::Standard:
    default:
        scale = 1.0;
        break;
    }

    return qBound(12, static_cast<int>(std::lround(baseHeight * scale)), 300);
}

void ArrayPlotWindow::updateUnifiedYAxisRange()
{
    if (!m_plot || m_channelAxisRects.isEmpty() || m_plot->graphCount() == 0) {
        return;
    }

    bool useFixedRange = (m_yAxisMode == YAxisMode::Fixed && m_yAxisRangeValid);

    bool hasData = false;
    double minY = 0.0;
    double maxY = 0.0;

    if (m_channelDataMin.size() == m_plot->graphCount() && m_channelDataMax.size() == m_plot->graphCount()) {
        for (int i = 0; i < m_plot->graphCount(); ++i) {
            if (i >= m_channelVisible.size() || !m_channelVisible.at(i)) {
                continue;
            }
            const double vMin = m_channelDataMin.at(i);
            const double vMax = m_channelDataMax.at(i);
            if (!std::isfinite(vMin) || !std::isfinite(vMax)) {
                continue;
            }
            if (!hasData) {
                minY = vMin;
                maxY = vMax;
                hasData = true;
            } else {
                minY = std::min(minY, vMin);
                maxY = std::max(maxY, vMax);
            }
        }
    }

    if (!hasData) {
        for (int i = 0; i < m_plot->graphCount(); ++i) {
            QCPGraph* graph = m_plot->graph(i);
            if (!graph || !graph->data()) {
                continue;
            }
            for (auto it = graph->data()->constBegin(); it != graph->data()->constEnd(); ++it) {
                const double value = it->value;
                if (!hasData) {
                    minY = maxY = value;
                    hasData = true;
                } else {
                    minY = std::min(minY, value);
                    maxY = std::max(maxY, value);
                }
            }
        }
    }

    double lower = 0.0;
    double upper = 0.0;
    if (useFixedRange) {
        lower = m_yAxisLower;
        upper = m_yAxisUpper;
    } else {
        if (!hasData) {
            return;
        }

        double padding = (maxY - minY) * 0.08;
        if (padding < 1.0) {
            padding = 1.0;
        }
        lower = minY - padding;
        upper = maxY + padding;

        if (m_yAxisMode == YAxisMode::Fixed) {
            m_yAxisLower = lower;
            m_yAxisUpper = upper;
            m_yAxisRangeValid = true;
            useFixedRange = true;
        }
    }

    for (auto axisRect : m_channelAxisRects) {
        QCPAxis* yAxis = axisRect->axis(QCPAxis::atLeft);
        yAxis->setSubTicks(false);
        yAxis->setRange(lower, upper);

        const int minInt = static_cast<int>(std::floor(lower));
        const int maxInt = static_cast<int>(std::ceil(upper));

        QVector<int> tickInts;
        tickInts.append(minInt);
        const bool hasZeroTick = (minInt <= 0 && maxInt >= 0 && minInt != 0 && maxInt != 0);
        if (hasZeroTick) {
            const double minPx = yAxis->coordToPixel(static_cast<double>(minInt));
            const double zeroPx = yAxis->coordToPixel(0.0);
            const double maxPx = yAxis->coordToPixel(static_cast<double>(maxInt));
            const double minPixelGap = 10.0;
            if (std::abs(zeroPx - minPx) >= minPixelGap && std::abs(maxPx - zeroPx) >= minPixelGap) {
                tickInts.append(0);
            }
        }
        if (maxInt != minInt) {
            tickInts.append(maxInt);
        }

        std::sort(tickInts.begin(), tickInts.end());
        tickInts.erase(std::unique(tickInts.begin(), tickInts.end()), tickInts.end());

        auto textTicker = QSharedPointer<QCPAxisTickerText>(new QCPAxisTickerText);
        for (int value : tickInts) {
            textTicker->addTick(static_cast<double>(value), QString::number(value));
        }
        yAxis->setTicker(textTicker);
    }
}

void ArrayPlotWindow::applyChannelVisibility()
{
    if (!m_plot || m_channelAxisRects.isEmpty()) {
        return;
    }

    int lastVisibleIndex = -1;
    for (int i = 0; i < m_channelVisible.size(); ++i) {
        if (m_channelVisible.at(i)) {
            lastVisibleIndex = i;
        }
    }
    if (lastVisibleIndex < 0) {
        lastVisibleIndex = m_channelAxisRects.size() - 1;
    }

    for (int i = 0; i < m_plot->graphCount(); ++i) {
        const bool visible = i < m_channelVisible.size() ? m_channelVisible.at(i) : true;
        m_plot->graph(i)->setVisible(visible);

        if (i < m_channelAxisRects.size()) {
            QCPAxisRect* axisRect = m_channelAxisRects.at(i);
            const bool showBottomAxis = (i == lastVisibleIndex);
            axisRect->axis(QCPAxis::atBottom)->setVisible(showBottomAxis);
            axisRect->axis(QCPAxis::atBottom)->setLabel(showBottomAxis ? QStringLiteral("t(ms)") : QString());
            axisRect->axis(QCPAxis::atTop)->setVisible(false);
            axisRect->axis(QCPAxis::atRight)->setVisible(false);
        }
    }

}

const QVector<QVector<double>>* ArrayPlotWindow::resolveSnapshotSource(const QSharedPointer<const PlotSnapshot>& snapshot) const
{
    if (!snapshot) {
        return nullptr;
    }

    if (snapshot->mode == FrameData::MultiChannelReal) {
        return &snapshot->realAmp;
    }

    if (snapshot->mode == FrameData::MultiChannelComplex) {
        switch (m_componentMode) {
        case ArrayComponent::Phase:
            return &snapshot->complexPhase;
        case ArrayComponent::Real:
            return &snapshot->complexReal;
        case ArrayComponent::Imag:
            return &snapshot->complexImag;
        case ArrayComponent::Amplitude:
        default:
            return &snapshot->complexMag;
        }
    }

    return nullptr;
}

void ArrayPlotWindow::renderSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot, bool forceRefresh)
{
    QElapsedTimer timer;
    timer.start();

    if (m_useMockData || !snapshot || !m_plot) {
        return;
    }

    if (!forceRefresh && snapshot->version == m_lastSnapshotVersion) {
        return;
    }

    m_cachedSnapshot = snapshot;
    m_lastSnapshotVersion = snapshot->version;

    const FrameData::DetectionMode mode = snapshot->mode;
    const int ch = qBound(1, snapshot->channelCount, 200);
    if (mode == FrameData::Legacy || ch <= 0 || snapshot->timeMs.isEmpty()) {
        return;
    }

    if (m_componentCombo) {
        if (mode == FrameData::MultiChannelReal) {
            QSignalBlocker blocker(m_componentCombo);
            m_componentCombo->setCurrentIndex(0);
            m_componentCombo->setEnabled(false);
            m_componentMode = ArrayComponent::Amplitude;
        } else {
            m_componentCombo->setEnabled(true);
        }
    }

    if (mode != m_lastMode || ch != m_currentChannelCount) {
        m_lastMode = mode;
        m_currentChannelCount = ch;
        initArrayPlot();
    }

    const QVector<QVector<double>>* source = resolveSnapshotSource(snapshot);
    if (!source) {
        return;
    }

    const QVector<double>* timeForRender = &snapshot->timeMs;
    QVector<int> sampledIndices;
    QVector<double> sampledTime;
    if (snapshot->timeMs.size() > kArrayRenderPointCap) {
        sampledIndices = buildSampleIndices(snapshot->timeMs.size(), kArrayRenderPointCap);
        sampledTime = sampleByIndices(snapshot->timeMs, sampledIndices);
        if (!sampledTime.isEmpty()) {
            timeForRender = &sampledTime;
        }
    }

    m_channelDataMin.fill(std::numeric_limits<double>::quiet_NaN(), m_plot->graphCount());
    m_channelDataMax.fill(std::numeric_limits<double>::quiet_NaN(), m_plot->graphCount());

    for (int i = 0; i < ch && i < m_plot->graphCount() && i < source->size(); ++i) {
        QVector<double> sampledValues;
        if (!sampledIndices.isEmpty()) {
            sampledValues = sampleByIndices(source->at(i), sampledIndices);
            m_plot->graph(i)->setData(*timeForRender, sampledValues, true);
        } else {
            m_plot->graph(i)->setData(*timeForRender, source->at(i), true);
            sampledValues = source->at(i);
        }

        double localMin = std::numeric_limits<double>::quiet_NaN();
        double localMax = std::numeric_limits<double>::quiet_NaN();
        for (double value : sampledValues) {
            if (!std::isfinite(value)) {
                continue;
            }
            if (!std::isfinite(localMin) || !std::isfinite(localMax)) {
                localMin = value;
                localMax = value;
            } else {
                localMin = std::min(localMin, value);
                localMax = std::max(localMax, value);
            }
        }
        if (i < m_channelDataMin.size()) {
            m_channelDataMin[i] = localMin;
            m_channelDataMax[i] = localMax;
        }

        QString yLabel;
        switch (m_rowLabelMode) {
        case RowLabelMode::DisplayIndex:
            if (i < snapshot->rowDisplayIndex.size()) {
                yLabel = QStringLiteral("D%1").arg(snapshot->rowDisplayIndex.at(i));
            }
            break;
        case RowLabelMode::SourceChannel:
            if (i < snapshot->rowSourceChannel.size()) {
                yLabel = QStringLiteral("SRC%1").arg(snapshot->rowSourceChannel.at(i));
            }
            break;
        case RowLabelMode::ChannelIndex:
        default:
            yLabel = QStringLiteral("CH%1").arg(i + 1);
            break;
        }

        if (yLabel.isEmpty()) {
            yLabel = QStringLiteral("CH%1").arg(i + 1);
        }
        if (i < m_channelAxisRects.size()) {
            m_channelAxisRects[i]->axis(QCPAxis::atLeft)->setLabel(yLabel);
        }
    }

    m_timeAxis = *timeForRender;
    m_latestTime = m_timeAxis.last();
    updateArrayData();

    const qint64 costMs = timer.elapsed();
    m_perfRenderCount += 1;
    m_perfRenderCostMs += costMs;
    if (perfLogEnabled() && m_perfLogTimer.elapsed() >= 5000) {
        const double avgMs = (m_perfRenderCount > 0)
            ? static_cast<double>(m_perfRenderCostMs) / static_cast<double>(m_perfRenderCount)
            : 0.0;
        qInfo().nospace()
            << "[Perf][ArrayPlotWindow] avgRenderMs=" << QString::number(avgMs, 'f', 2)
            << " channels=" << ch
            << " points=" << m_timeAxis.size();
        m_perfLogTimer.restart();
        m_perfRenderCount = 0;
        m_perfRenderCostMs = 0;
    }
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