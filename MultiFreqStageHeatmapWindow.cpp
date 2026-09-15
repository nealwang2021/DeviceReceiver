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
#include <QSettings>
#include <cmath>
#include <algorithm>

MultiFreqStageHeatmapWindow::MultiFreqStageHeatmapWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("多频台位热力图"));
    resize(750, 750);

    // 从配置加载步距
    {
        QSettings settings(AppConfig::defaultConfigFilePath(), QSettings::IniFormat);
        m_step = settings.value("MultiFreqStageHeatmap/GridStep", 1.0).toDouble();
        if (m_step <= 0.01) m_step = 1.0;
        m_ampMin = settings.value("MultiFreqStageHeatmap/AmpMin", 0.0001).toDouble();
        m_ampMax = settings.value("MultiFreqStageHeatmap/AmpMax", 1.0).toDouble();
        if (m_ampMax <= m_ampMin) m_ampMax = m_ampMin + 0.0001;
    }

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

    // ---- QCustomPlot + QCPColorScale ----
    m_plot = new QCustomPlot(this);
    PlotWindowBase::applyConfiguredOpenGl(m_plot);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 色标条
    m_colorScale = new QCPColorScale(m_plot);
    m_colorScale->setType(QCPAxis::atRight);
    m_colorScale->axis()->setLabel(QStringLiteral("阻抗幅值 (log10)"));
    m_plot->plotLayout()->addElement(0, 1, m_colorScale);

    root->addWidget(m_plot, 1);

    initPlot();
}

MultiFreqStageHeatmapWindow::~MultiFreqStageHeatmapWindow() = default;

void MultiFreqStageHeatmapWindow::initPlot()
{
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // QCPItemPixmap — 固定网格 QImage，原点在左上角
    m_pixmapItem = new QCPItemPixmap(m_plot);
    m_pixmapItem->topLeft->setType(QCPItemPosition::ptPlotCoords);
    m_pixmapItem->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    m_pixmapItem->topLeft->setAxes(m_plot->xAxis, m_plot->yAxis);
    m_pixmapItem->bottomRight->setAxes(m_plot->xAxis, m_plot->yAxis);
    m_pixmapItem->setScaled(true, Qt::IgnoreAspectRatio, Qt::FastTransformation);

    // 初始 1×1 空白图，收到首个台位数据后扩展
    const int w = static_cast<int>((m_xMax - m_xMin) / m_step) + 1;
    const int h = static_cast<int>((m_yMax - m_yMin) / m_step) + 1;
    QImage img(w, h, QImage::Format_ARGB32);
    const QColor bg = isDarkThemeActive() ? QColor(24, 24, 24) : QColor(240, 240, 240);
    img.fill(bg);
    m_pixmapItem->setPixmap(QPixmap::fromImage(img));
    m_pixmapItem->topLeft->setCoords(m_xMin, m_yMin);
    m_pixmapItem->bottomRight->setCoords(m_xMax, m_yMax);

    m_plot->xAxis->setLabel(QStringLiteral("台位 X (mm)"));
    m_plot->yAxis->setLabel(QStringLiteral("台位 Y (mm)"));
    m_plot->xAxis->setRange(m_xMin - 2, m_xMax + 2);
    m_plot->yAxis->setRange(m_yMin - 2, m_yMax + 2);
    m_plot->yAxis->setRangeReversed(true);  // Y 轴反转: 原点在左上角

    // 色标条：使用 gpSpectrum 梯度（Hue→Saturation→Value），幅值范围绑定
    updateColorScale();

    onThemeChanged();
}

void MultiFreqStageHeatmapWindow::updateColorScale()
{
    if (!m_colorScale) return;
    QCPColorGradient grad(QCPColorGradient::gpSpectrum);
    grad.setLevelCount(256);
    m_colorScale->setGradient(grad);
    m_colorScale->setDataRange(QCPRange(std::log10(m_ampMin), std::log10(m_ampMax)));
}

void MultiFreqStageHeatmapWindow::rebuildGridAndImages(double xMin, double xMax, double yMin, double yMax)
{
    const int w = static_cast<int>((xMax - xMin) / m_step) + 1;
    const int h = static_cast<int>((yMax - yMin) / m_step) + 1;
    qDebug() << "[MultiFreqStageHeatmap] rebuildGrid:" << m_xMin << m_xMax << m_yMin << m_yMax
             << "->" << xMin << xMax << yMin << yMax
             << "oldFilled=" << m_filledSet.size();
    if (w <= 0 || h <= 0) return;
    const QColor bg = isDarkThemeActive() ? QColor(24, 24, 24) : QColor(240, 240, 240);

    // 偏移量：旧网格原点在新网格中的像素位置
    const int xOff = static_cast<int>((m_xMin - xMin) / m_step + 0.5);
    const int yOff = static_cast<int>((m_yMin - yMin) / m_step + 0.5);

    // 从 m_filledSet 精确定位已扫像素，避免颜色比对（主题切换后背景色不同）
    QSet<QPair<int,int>> newFilledSet;
    QVector<QImage> oldImgs = m_freqImages;  // 拷贝旧图像数据

    for (int i = 0; i < m_freqImages.size(); ++i) {
        m_freqImages[i] = QImage(w, h, QImage::Format_ARGB32);
        m_freqImages[i].fill(bg);
    }

    for (const auto& cell : m_filledSet) {
        const int ox = cell.first;
        const int oy = cell.second;
        const int nx = ox + xOff;
        const int ny = oy + yOff;
        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
            for (int i = 0; i < m_freqImages.size() && i < oldImgs.size(); ++i) {
                if (ox < oldImgs[i].width() && oy < oldImgs[i].height())
                    m_freqImages[i].setPixelColor(nx, ny, oldImgs[i].pixelColor(ox, oy));
            }
            newFilledSet.insert({nx, ny});
        }
    }

    m_xMin = xMin; m_xMax = xMax;
    m_yMin = yMin; m_yMax = yMax;

    m_filledSet = std::move(newFilledSet);
    m_filledCount = m_filledSet.size();

    if (m_pixmapItem) {
        m_pixmapItem->topLeft->setCoords(m_xMin, m_yMin);
        m_pixmapItem->bottomRight->setCoords(m_xMax, m_yMax);
    }
    m_plot->xAxis->setRange(m_xMin - 2, m_xMax + 2);
    m_plot->yAxis->setRange(m_yMin - 2, m_yMax + 2);

    m_pointCountLabel->setText(QStringLiteral("已扫: %1").arg(m_filledCount));
}

void MultiFreqStageHeatmapWindow::onThemeChanged()
{
    applyThemeToPlot(m_plot, isDarkThemeActive());
    // 重填背景
    const QColor bg = isDarkThemeActive() ? QColor(24, 24, 24) : QColor(240, 240, 240);
    for (int y = 0; y < m_freqImages.size(); ++y) {
        auto& img = m_freqImages[y];
        for (int x = 0; x < img.width(); ++x) {
            for (int yy = 0; yy < img.height(); ++yy) {
                if (img.pixelColor(x, yy) == QColor(24, 24, 24) ||
                    img.pixelColor(x, yy) == QColor(240, 240, 240)) {
                    img.setPixelColor(x, yy, bg);
                }
            }
        }
    }
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

    // 频点数变化时扩展 combo 和图像数组，保留已有扫描数据
    if (nPoints != m_freqPointCount) {
        qDebug() << "[MultiFreqStageHeatmap] freqPointCount changed:" << m_freqPointCount << "->" << nPoints
                 << "frameId=" << f.frameId << "hasStagePose=" << f.hasStagePose
                 << "filled=" << m_filledSet.size();
        const int oldPoints = m_freqPointCount;
        m_freqPointCount = nPoints;
        m_freqCombo->blockSignals(true);
        m_freqCombo->clear();
        for (int i = 0; i < nPoints; ++i)
            m_freqCombo->addItem(QStringLiteral("f%1").arg(i + 1));
        m_freqCombo->blockSignals(false);

        const int w = static_cast<int>((m_xMax - m_xMin) / m_step) + 1;
        const int h = static_cast<int>((m_yMax - m_yMin) / m_step) + 1;
        const QColor bg = isDarkThemeActive() ? QColor(24, 24, 24) : QColor(240, 240, 240);

        // 保留旧频点图像，新增频点创建空白图
        QVector<QImage> oldImgs = m_freqImages;
        m_freqImages.resize(nPoints);
        for (int i = 0; i < nPoints; ++i) {
            if (i < oldImgs.size()) {
                // 已有频点：保留（可能尺寸不匹配则重建）
                if (oldImgs[i].width() == w && oldImgs[i].height() == h) {
                    m_freqImages[i] = oldImgs[i];
                } else {
                    m_freqImages[i] = QImage(w, h, QImage::Format_ARGB32);
                    m_freqImages[i].fill(bg);
                }
            } else {
                // 新增频点：从 m_filledSet 回填已有扫查数据
                m_freqImages[i] = QImage(w, h, QImage::Format_ARGB32);
                m_freqImages[i].fill(bg);
                for (const auto& cell : m_filledSet) {
                    const int ix = cell.first, iy = cell.second;
                    if (ix >= 0 && ix < w && iy >= 0 && iy < h && !oldImgs.isEmpty()) {
                        m_freqImages[i].setPixelColor(ix, iy, oldImgs[0].pixelColor(ix, iy));
                    }
                }
            }
        }

        if (!m_freqImages.isEmpty()) {
            const int sel = qBound(0, m_freqCombo->currentIndex(), m_freqImages.size() - 1);
            m_pixmapItem->setPixmap(QPixmap::fromImage(m_freqImages[sel]));
        }

        // 不清空 m_filledSet — 扫描位置数据仍然有效
    }

    if (!f.hasStagePose) return;

    const int sel = m_freqCombo->currentIndex();
    if (sel < 0 || sel >= nPoints || sel >= m_freqImages.size()) return;

    const MultiFreqPointResult& pt = f.mfFreqPoints[sel];
    if (!pt.valid) return;

    const double x = f.stageXMm;
    const double y = f.stageYMm;

    // 自适应网格范围：首次收到有效数据时，以 (0,0) 为左上角起点
    if (!m_hasData) {
        m_hasData = true;
        const double margin = 50.0;
        // 始终从 0 原点开始，仅正向扩展到台位 + margin
        const double initX = qMax(x + margin, m_xMax);
        const double initY = qMax(y + margin, m_yMax);
        rebuildGridAndImages(0.0, initX, 0.0, initY);
        if (m_pixmapItem && !m_freqImages.isEmpty()) {
            const int sel = qBound(0, m_freqCombo->currentIndex(), m_freqImages.size() - 1);
            m_pixmapItem->setPixmap(QPixmap::fromImage(m_freqImages[sel]));
        }
    }

    // 超出当前网格时自动扩展（仅正向扩展，始终保持 (0,0) 为左上角原点）
    bool needRebuild = false;
    double nxMin = m_xMin, nxMax = m_xMax, nyMin = m_yMin, nyMax = m_yMax;
    const double expandMargin = 20.0;
    if (x > m_xMax) { nxMax = x + expandMargin; needRebuild = true; }
    if (x < 0)     { nxMin = x - expandMargin; needRebuild = true; } // 台位走到负半轴时才扩展负向
    if (y > m_yMax) { nyMax = y + expandMargin; needRebuild = true; }
    if (y < 0)     { nyMin = y - expandMargin; needRebuild = true; }
    if (needRebuild) {
        qDebug() << "[MultiFreqStageHeatmap] expand bounds: stage=(" << x << "," << y << ")"
                 << "oldBounds=[" << m_xMin << m_xMax << m_yMin << m_yMax << "]"
                 << "frameId=" << f.frameId << "filledBefore=" << m_filledSet.size();
        rebuildGridAndImages(nxMin, nxMax, nyMin, nyMax);
        if (m_pixmapItem && !m_freqImages.isEmpty()) {
            const int sel = qBound(0, m_freqCombo->currentIndex(), m_freqImages.size() - 1);
            m_pixmapItem->setPixmap(QPixmap::fromImage(m_freqImages[sel]));
        }
    }

    const int ix = static_cast<int>((x - m_xMin) / m_step + 0.5);
    const int iy = static_cast<int>((y - m_yMin) / m_step + 0.5);

    if (!m_pixmapItem) return;
    QImage& img = m_freqImages[sel];
    const int iw = img.width();
    const int ih = img.height();

    if (ix < 0 || ix >= iw || iy < 0 || iy >= ih) return;

    // 颜色映射
    img.setPixelColor(ix, iy, colorFromAmpPhase(pt.impedanceMagnitude, pt.impedancePhaseDeg));

    // 当前选中频点才刷新显示
    if (m_freqCombo->currentIndex() == sel)
        m_pixmapItem->setPixmap(QPixmap::fromImage(img));

    m_plot->replot(QCustomPlot::rpQueuedReplot);

    // 增量统计：记录新扫过的格子
    const QPair<int,int> cell(ix, iy);
    if (!m_filledSet.contains(cell)) {
        m_filledSet.insert(cell);
        ++m_filledCount;
        m_pointCountLabel->setText(QStringLiteral("已扫: %1").arg(m_filledCount));
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
    if (!std::isfinite(amp) || !std::isfinite(phaseDeg)) {
        return isDarkThemeActive() ? QColor(24, 24, 24) : QColor(240, 240, 240);
    }

    // 相位 → 色相 (0~1)
    double phaseNorm = std::fmod(phaseDeg, 360.0);
    if (phaseNorm < 0.0) phaseNorm += 360.0;
    const double hue = phaseNorm / 360.0;

    // 幅值 → 亮度 (log10 归一化)
    const double logAmp = std::log10(std::max(amp, kVerySmallValue));
    const double minLog = std::log10(m_ampMin);
    const double maxLog = std::log10(m_ampMax);
    double v = (logAmp - minLog) / (maxLog - minLog);
    v = clamp01(v);

    // 色阶：饱和度固定 1.0，亮度随幅值变化
    return QColor::fromHsvF(hue, 1.0, 0.2 + v * 0.8);
}

double MultiFreqStageHeatmapWindow::clamp01(double v)
{
    return (v < 0.0) ? 0.0 : (v > 1.0) ? 1.0 : v;
}

void MultiFreqStageHeatmapWindow::onAmpRangeChanged()
{
    m_ampMin = m_ampMinSpin->value();
    m_ampMax = m_ampMaxSpin->value();
    if (m_ampMax <= m_ampMin) {
        m_ampMax = m_ampMin + 0.0001;
        m_ampMaxSpin->blockSignals(true);
        m_ampMaxSpin->setValue(m_ampMax);
        m_ampMaxSpin->blockSignals(false);
    }
    updateColorScale();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
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
