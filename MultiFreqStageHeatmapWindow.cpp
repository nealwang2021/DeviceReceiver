#include "MultiFreqStageHeatmapWindow.h"
#include "AppConfig.h"
#include "qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <cmath>
#include <algorithm>

MultiFreqStageHeatmapWindow::MultiFreqStageHeatmapWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("多频台位热力图"));
    resize(700, 700);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // ---- 顶栏 ----
    QWidget* topBar = new QWidget(this);
    QHBoxLayout* topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(4, 2, 4, 2);
    topLay->setSpacing(8);

    topLay->addWidget(new QLabel(QStringLiteral("频点:"), topBar));
    m_freqCombo = new QComboBox(topBar);
    m_freqCombo->setMinimumWidth(60);
    connect(m_freqCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() {
                // 切换频点时换对应图片显示，数据各自保留
                const int idx = m_freqCombo->currentIndex();
                if (m_pixmapItem && idx >= 0 && idx < m_freqImages.size())
                    m_pixmapItem->setPixmap(QPixmap::fromImage(m_freqImages[idx]));
                rebuildHeatmap();
            });
    topLay->addWidget(m_freqCombo);

    topLay->addSpacing(12);
    topLay->addWidget(new QLabel(QStringLiteral("归一化范围:"), topBar));
    m_ampMinSpin = new QDoubleSpinBox(topBar);
    m_ampMinSpin->setRange(0.00001, 1000.0);
    m_ampMinSpin->setDecimals(5);
    m_ampMinSpin->setSingleStep(0.001);
    m_ampMinSpin->setValue(m_ampMin);
    m_ampMinSpin->setFixedWidth(90);
    connect(m_ampMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MultiFreqStageHeatmapWindow::onAmpRangeChanged);
    topLay->addWidget(m_ampMinSpin);

    topLay->addWidget(new QLabel(QStringLiteral("~"), topBar));
    m_ampMaxSpin = new QDoubleSpinBox(topBar);
    m_ampMaxSpin->setRange(0.001, 1000.0);
    m_ampMaxSpin->setDecimals(5);
    m_ampMaxSpin->setSingleStep(0.1);
    m_ampMaxSpin->setValue(m_ampMax);
    m_ampMaxSpin->setFixedWidth(90);
    connect(m_ampMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MultiFreqStageHeatmapWindow::onAmpRangeChanged);
    topLay->addWidget(m_ampMaxSpin);

    topLay->addSpacing(12);
    m_exportBtn = new QPushButton(QStringLiteral("导出PNG"), topBar);
    connect(m_exportBtn, &QPushButton::clicked, this, &MultiFreqStageHeatmapWindow::onExportClicked);
    topLay->addWidget(m_exportBtn);

    topLay->addStretch();
    m_pointCountLabel = new QLabel(QStringLiteral("已扫: 0"), topBar);
    topLay->addWidget(m_pointCountLabel);
    root->addWidget(topBar);

    // ---- QCustomPlot ----
    m_plot = new QCustomPlot(this);
    PlotWindowBase::applyConfiguredOpenGl(m_plot);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(m_plot, 1);

    initPlot();
}

MultiFreqStageHeatmapWindow::~MultiFreqStageHeatmapWindow() = default;

void MultiFreqStageHeatmapWindow::initPlot()
{
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // QCPItemPixmap — 固定网格 QImage
    m_pixmapItem = new QCPItemPixmap(m_plot);
    m_pixmapItem->topLeft->setType(QCPItemPosition::ptPlotCoords);
    m_pixmapItem->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    m_pixmapItem->topLeft->setAxes(m_plot->xAxis, m_plot->yAxis);
    m_pixmapItem->bottomRight->setAxes(m_plot->xAxis, m_plot->yAxis);
    // 左上角为 (0,0) 原点
    m_pixmapItem->topLeft->setCoords(m_xMin, m_yMin);
    m_pixmapItem->bottomRight->setCoords(m_xMax, m_yMax);
    m_pixmapItem->setScaled(true, Qt::IgnoreAspectRatio, Qt::FastTransformation);

    // 初始空白图（freqImages 在首次收到数据时按频点数分配）
    const int w = static_cast<int>((m_xMax - m_xMin) / m_step) + 1;
    const int h = static_cast<int>((m_yMax - m_yMin) / m_step) + 1;
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(24, 24, 24));
    m_pixmapItem->setPixmap(QPixmap::fromImage(img));

    m_plot->xAxis->setLabel(QStringLiteral("台位 X (mm)"));
    m_plot->yAxis->setLabel(QStringLiteral("台位 Y (mm)"));
    m_plot->xAxis->setRange(m_xMin - 2, m_xMax + 2);
    m_plot->yAxis->setRange(m_yMin - 2, m_yMax + 2);
    m_plot->yAxis->setRangeReversed(true);  // Y 轴反转: 原点在左上角

    onThemeChanged();
}

void MultiFreqStageHeatmapWindow::onThemeChanged()
{
    applyThemeToPlot(m_plot, isDarkThemeActive());
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void MultiFreqStageHeatmapWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (frames.isEmpty()) return;

    const FrameData* latest = nullptr;
    for (int i = frames.size() - 1; i >= 0; --i) {
        if (frames[i].detectMode == FrameData::MultiFreqEddy) {
            latest = &frames[i];
            break;
        }
    }
    if (!latest) return;

    const FrameData& f = *latest;
    const int nPoints = f.mfFreqPoints.size();
    if (nPoints <= 0) return;

    if (nPoints != m_freqPointCount) {
        m_freqPointCount = nPoints;
        m_freqCombo->blockSignals(true);
        m_freqCombo->clear();
        for (int i = 0; i < nPoints; ++i)
            m_freqCombo->addItem(QStringLiteral("f%1").arg(i + 1));
        m_freqCombo->blockSignals(false);
        // 按频点数分配独立 QImage
        const int w = static_cast<int>((m_xMax - m_xMin) / m_step) + 1;
        const int h = static_cast<int>((m_yMax - m_yMin) / m_step) + 1;
        m_freqImages.resize(nPoints);
        for (int i = 0; i < nPoints; ++i) {
            m_freqImages[i] = QImage(w, h, QImage::Format_ARGB32);
            m_freqImages[i].fill(QColor(24, 24, 24));
        }
        if (!m_freqImages.isEmpty())
            m_pixmapItem->setPixmap(QPixmap::fromImage(m_freqImages[0]));
    }

    if (!f.hasStagePose) return;

    const int sel = m_freqCombo->currentIndex();
    if (sel < 0 || sel >= nPoints || sel >= m_freqImages.size()) return;

    const MultiFreqPointResult& pt = f.mfFreqPoints[sel];
    if (!pt.valid) return;

    // 网格坐标
    const double x = f.stageXMm;
    const double y = f.stageYMm;
    const int ix = static_cast<int>((x - m_xMin) / m_step + 0.5);
    const int iy = static_cast<int>((y - m_yMin) / m_step + 0.5);

    // 写入对应频点的独立 QImage
    if (!m_pixmapItem) return;
    QImage& img = m_freqImages[sel];
    const int iw = img.width();
    const int ih = img.height();

    if (ix < 0 || ix >= iw || iy < 0 || iy >= ih) return;

    // 颜色：phase→hue, amp→brightness
    const double phaseNorm = std::fmod(pt.impedancePhaseDeg, 360.0);
    const double hue = (phaseNorm < 0 ? phaseNorm + 360.0 : phaseNorm) / 360.0;
    const double logAmp = std::log10(std::max(pt.impedanceMagnitude, kVerySmallValue));
    const double minLog = std::log10(m_ampMin);
    const double maxLog = std::log10(m_ampMax);
    double v = (logAmp - minLog) / (maxLog - minLog);
    v = (v < 0.0) ? 0.0 : (v > 1.0) ? 1.0 : v;
    img.setPixelColor(ix, iy, QColor::fromHsvF(hue, 1.0, v));

    // 当前选中频点才刷新显示
    if (m_freqCombo->currentIndex() == sel)
        m_pixmapItem->setPixmap(QPixmap::fromImage(img));

    m_plot->replot(QCustomPlot::rpQueuedReplot);

    // 已扫统计
    static int cnt = 0;
    ++cnt;
    if (cnt % 20 == 1) {
        int filled = 0;
        for (int xi = 0; xi < iw; ++xi)
            for (int yi = 0; yi < ih; ++yi)
                if (img.pixelColor(xi, yi) != QColor(24, 24, 24))
                    ++filled;
        m_pointCountLabel->setText(QStringLiteral("已扫: %1 / %2").arg(filled).arg(iw * ih));
    }
}

void MultiFreqStageHeatmapWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>&) {}
void MultiFreqStageHeatmapWindow::onCriticalFrame(const FrameData&) {}

void MultiFreqStageHeatmapWindow::rebuildHeatmap()
{
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

QColor MultiFreqStageHeatmapWindow::colorFromAmpPhase(double amp, double phaseDeg) const
{
    if (!std::isfinite(amp) || !std::isfinite(phaseDeg))
        return QColor(24, 24, 24);
    double phaseNorm = std::fmod(phaseDeg, 360.0);
    if (phaseNorm < 0.0) phaseNorm += 360.0;
    const double minLogAmp = std::log10(m_ampMin);
    const double maxLogAmp = std::log10(m_ampMax);
    double logAmp = std::log10(std::max(amp, kVerySmallValue));
    logAmp = qBound(minLogAmp, logAmp, maxLogAmp);
    const double val = (logAmp - minLogAmp) / (maxLogAmp - minLogAmp);
    return QColor::fromHsvF(phaseNorm / 360.0, 1.0, val);
}

double MultiFreqStageHeatmapWindow::clamp01(double v)
{
    return (v < 0.0) ? 0.0 : (v > 1.0) ? 1.0 : v;
}

void MultiFreqStageHeatmapWindow::onAmpRangeChanged()
{
    m_ampMin = m_ampMinSpin->value();
    m_ampMax = m_ampMaxSpin->value();
    if (m_ampMax <= m_ampMin) m_ampMax = m_ampMin + 0.0001;
}

void MultiFreqStageHeatmapWindow::onExportClicked()
{
    const QString defaultName = QStringLiteral("multifreq_stage_heatmap_%1.png")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString startDir = AppConfig::instance()
        ? AppConfig::instance()->defaultExportDirectory()
        : QDir::currentPath();
    const QString filePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出热力图"), QDir(startDir).filePath(defaultName),
        QStringLiteral("PNG 图片 (*.png)"));
    if (filePath.isEmpty()) return;
    const QPixmap shot = grab();
    if (!shot.save(filePath, "PNG"))
        qWarning() << "[MultiFreqStageHeatmapWindow] 导出失败:" << filePath;
}
