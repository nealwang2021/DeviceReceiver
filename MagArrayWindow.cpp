#include "MagArrayWindow.h"
#include "PlotDataHub.h"
#include "AppConfig.h"
#include "qcustomplot.h"

#include <QVBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QDebug>

// ============================================================================
// 构造 / 析构
// ============================================================================

MagArrayWindow::MagArrayWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("漏磁检测"));
    resize(1000, 600);
    setupUi();
}

MagArrayWindow::~MagArrayWindow() = default;

// ============================================================================
// UI 初始化
// ============================================================================

void MagArrayWindow::setupUi()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);

    // 热力图占位：后续实现
    m_heatmapPlot = new QCustomPlot(splitter);
    m_heatmapPlot->xAxis->setLabel(QStringLiteral("传感器"));
    m_heatmapPlot->yAxis->setLabel(QStringLiteral("XYZ 轴"));
    m_heatmapPlot->xAxis->setRange(0, 20);
    m_heatmapPlot->yAxis->setRange(0, 3);
    splitter->addWidget(m_heatmapPlot);

    // 时序图占位：后续实现
    m_timeSeriesPlot = new QCustomPlot(splitter);
    m_timeSeriesPlot->xAxis->setLabel(QStringLiteral("时间"));
    m_timeSeriesPlot->yAxis->setLabel(QStringLiteral("幅值"));
    splitter->addWidget(m_timeSeriesPlot);

    rootLayout->addWidget(splitter, 1);
    setLayout(rootLayout);
}

// ============================================================================
// PlotWindowBase 接口实现
// ============================================================================

void MagArrayWindow::onDataUpdated(const QVector<FrameData>& /*frames*/)
{
    // 暂未实现
}

void MagArrayWindow::onCriticalFrame(const FrameData& /*frame*/)
{
    // 暂未实现
}

void MagArrayWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& /*snapshot*/)
{
    // 暂未实现
}
