#include "ArrayRgbHeatmapWindow.h"

#include "AppConfig.h"
#include "qcustomplot.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kDisplayChannels = 40;
constexpr double kAmpMin = 0.05;
constexpr double kAmpMax = 0.3;
constexpr double kVerySmallValue = 1e-12;

double clamp01(double v)
{
    return qBound(0.0, v, 1.0);
}

QVector<double> makeNaNVector(int count)
{
    QVector<double> values;
    values.resize(count);
    values.fill(std::numeric_limits<double>::quiet_NaN());
    return values;
}

} // namespace

ArrayRgbHeatmapWindow::ArrayRgbHeatmapWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("阵列热力图"));
    resize(1400, 900);
    initUi();
}

ArrayRgbHeatmapWindow::~ArrayRgbHeatmapWindow() = default;

void ArrayRgbHeatmapWindow::initUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    auto* controlGroup = new QGroupBox(QStringLiteral("热力图控制"), this);
    auto* controlLayout = new QHBoxLayout(controlGroup);
    controlLayout->setContentsMargins(8, 6, 8, 6);

    auto* xAxisLabel = new QLabel(QStringLiteral("横轴:"), this);
    m_xAxisModeCombo = new QComboBox(this);
    m_xAxisModeCombo->addItem(QStringLiteral("帧号"));
    m_xAxisModeCombo->addItem(QStringLiteral("时间(s)"));
    m_xAxisModeCombo->setCurrentIndex(0);

    m_clearButton = new QPushButton(QStringLiteral("清空"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出PNG"), this);

    controlLayout->addWidget(xAxisLabel);
    controlLayout->addWidget(m_xAxisModeCombo);
    controlLayout->addWidget(m_clearButton);
    controlLayout->addWidget(m_exportButton);
    controlLayout->addStretch();

    auto createPlotGroup = [this](const QString& title, QCustomPlot*& plot, QCPItemPixmap*& item) {
        auto* group = new QGroupBox(title, this);
        auto* layout = new QVBoxLayout(group);
        layout->setContentsMargins(6, 6, 6, 6);

        plot = new QCustomPlot(group);
        plot->setMinimumHeight(300);
        plot->axisRect()->setupFullAxesBox(true);
        plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        plot->xAxis->setLabel(QStringLiteral("帧号"));
        plot->yAxis->setLabel(QStringLiteral("显示位置"));
        plot->xAxis->setNumberFormat("f");
        plot->xAxis->setNumberPrecision(0);
        plot->yAxis->setNumberFormat("f");
        plot->yAxis->setNumberPrecision(0);
        plot->xAxis->setRange(0, 1);
        plot->yAxis->setRange(0, kDisplayChannels - 1);

        item = new QCPItemPixmap(plot);
        item->topLeft->setType(QCPItemPosition::ptPlotCoords);
        item->bottomRight->setType(QCPItemPosition::ptPlotCoords);
        item->topLeft->setAxes(plot->xAxis, plot->yAxis);
        item->bottomRight->setAxes(plot->xAxis, plot->yAxis);
        item->setPen(Qt::NoPen);
        item->setScaled(true, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        item->topLeft->setCoords(0, kDisplayChannels - 1);
        item->bottomRight->setCoords(1, 0);

        layout->addWidget(plot);
        return group;
    };

    rootLayout->addWidget(controlGroup);
    rootLayout->addWidget(createPlotGroup(QStringLiteral("幅值 + 相位"), m_ampPhasePlot, m_ampPhasePixmap), 1);
    rootLayout->addWidget(createPlotGroup(QStringLiteral("实部 + 虚部"), m_realImagPlot, m_realImagPixmap), 1);

    m_statusLabel = new QLabel(QStringLiteral("状态：等待数据"), this);
    rootLayout->addWidget(m_statusLabel);

    connect(m_xAxisModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ArrayRgbHeatmapWindow::onXAxisModeChanged);
    connect(m_clearButton, &QPushButton::clicked,
            this, &ArrayRgbHeatmapWindow::onClearClicked);
    connect(m_exportButton, &QPushButton::clicked,
            this, &ArrayRgbHeatmapWindow::onExportClicked);

    m_channelCount = displayChannelCount();
    onThemeChanged();
}

int ArrayRgbHeatmapWindow::displayChannelCount() const
{
    return kDisplayChannels;
}

int ArrayRgbHeatmapWindow::maximumBufferedFrames() const
{
    int maxFrames = 200;
    if (auto* config = AppConfig::instance()) {
        maxFrames = qBound(50, config->maxPlotPoints(), 2000);
    }
    return maxFrames;
}

void ArrayRgbHeatmapWindow::loadSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!snapshot || snapshot->timeMs.isEmpty() || snapshot->channelCount <= 0) {
        return;
    }

    clearFrames();
    m_channelCount = qMin(displayChannelCount(), snapshot->channelCount);

    const int frameCount = snapshot->timeMs.size();
    for (int fi = 0; fi < frameCount; ++fi) {
        FrameRecord record;
        record.sequence = fi + 1;
        record.timestampMs = static_cast<qint64>(snapshot->timeMs.at(fi));
        record.amp = makeNaNVector(m_channelCount);
        record.phase = makeNaNVector(m_channelCount);
        record.x = makeNaNVector(m_channelCount);
        record.y = makeNaNVector(m_channelCount);

        if (snapshot->mode == FrameData::MultiChannelReal) {
            for (int ch = 0; ch < m_channelCount && ch < snapshot->realAmp.size(); ++ch) {
                if (fi < snapshot->realAmp[ch].size()) {
                    record.amp[ch] = snapshot->realAmp[ch].at(fi);
                }
            }
        } else if (snapshot->mode == FrameData::MultiChannelComplex) {
            for (int ch = 0; ch < m_channelCount && ch < snapshot->complexMag.size(); ++ch) {
                if (fi < snapshot->complexMag[ch].size()) {
                    record.amp[ch] = snapshot->complexMag[ch].at(fi);
                }
                if (ch < snapshot->complexPhase.size() && fi < snapshot->complexPhase[ch].size()) {
                    record.phase[ch] = snapshot->complexPhase[ch].at(fi) * 180.0 / M_PI;
                }
                if (ch < snapshot->complexReal.size() && fi < snapshot->complexReal[ch].size()) {
                    record.x[ch] = snapshot->complexReal[ch].at(fi);
                }
                if (ch < snapshot->complexImag.size() && fi < snapshot->complexImag[ch].size()) {
                    record.y[ch] = snapshot->complexImag[ch].at(fi);
                }
            }
        }

        m_frames.append(record);
        m_lastSequence = record.sequence;
        m_lastTimestamp = record.timestampMs;
    }

    const int cap = maximumBufferedFrames();
    while (m_frames.size() > cap) {
        m_frames.removeFirst();
    }
}

QString ArrayRgbHeatmapWindow::currentXAxisLabel() const
{
    return (m_xAxisMode == XAxisMode::TimeSeconds)
        ? QStringLiteral("相对时间(s)")
        : QStringLiteral("帧号");
}

double ArrayRgbHeatmapWindow::currentXAxisMax() const
{
    if (m_frames.isEmpty()) {
        return 1.0;
    }

    if (m_xAxisMode == XAxisMode::TimeSeconds) {
        const double duration = (m_frames.last().timestampMs - m_frames.first().timestampMs) / 1000.0;
        return (duration > 0.0) ? duration : 1.0;
    }

    return qMax(1.0, static_cast<double>(m_frames.size() - 1));
}

bool ArrayRgbHeatmapWindow::isNewFrame(const FrameData& frame) const
{
    if (m_lastSequence >= 0 && frame.sequence > 0 && frame.sequence <= static_cast<quint64>(m_lastSequence)) {
        return false;
    }
    if (m_lastTimestamp >= 0 && frame.timestamp > 0 && frame.timestamp <= m_lastTimestamp && frame.sequence == 0) {
        return false;
    }
    return true;
}

bool ArrayRgbHeatmapWindow::appendFrame(const FrameData& frame)
{
    if (!isNewFrame(frame)) {
        return false;
    }

    FrameRecord record;
    record.sequence = static_cast<qint64>(frame.sequence);
    record.timestampMs = frame.timestamp;
    record.amp = makeNaNVector(m_channelCount);
    record.phase = makeNaNVector(m_channelCount);
    record.x = makeNaNVector(m_channelCount);
    record.y = makeNaNVector(m_channelCount);

    const int sampleCount = std::min({
        m_channelCount,
        frame.channels_amp.size(),
        frame.channels_phase.size(),
        frame.channels_x.size(),
        frame.channels_y.size(),
        frame.channels_display_index.size() > 0 ? frame.channels_display_index.size() : m_channelCount
    });

    for (int i = 0; i < sampleCount; ++i) {
        int pos = i;
        if (i < frame.channels_display_index.size()) {
            pos = frame.channels_display_index[i];
        }
        if (pos < 0 || pos >= m_channelCount) {
            pos = i;
        }

        if (i < frame.channels_amp.size()) {
            record.amp[pos] = frame.channels_amp[i];
        }
        if (i < frame.channels_phase.size()) {
            record.phase[pos] = frame.channels_phase[i];
        }
        if (i < frame.channels_x.size()) {
            record.x[pos] = frame.channels_x[i];
        }
        if (i < frame.channels_y.size()) {
            record.y[pos] = frame.channels_y[i];
        }
    }

    m_frames.append(record);
    m_lastSequence = record.sequence;
    m_lastTimestamp = record.timestampMs;

    const int cap = maximumBufferedFrames();
    while (m_frames.size() > cap) {
        m_frames.removeFirst();
    }

    return true;
}

void ArrayRgbHeatmapWindow::clearFrames()
{
    m_frames.clear();
    m_lastSequence = -1;
    m_lastTimestamp = -1;
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("状态：已清空"));
    }

    if (m_ampPhasePixmap) {
        m_ampPhasePixmap->setPixmap(QPixmap());
    }
    if (m_realImagPixmap) {
        m_realImagPixmap->setPixmap(QPixmap());
    }
    if (m_ampPhasePlot) {
        m_ampPhasePlot->replot(QCustomPlot::rpQueuedReplot);
    }
    if (m_realImagPlot) {
        m_realImagPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}

QColor ArrayRgbHeatmapWindow::colorFromAmpPhase(double amp, double phaseDeg) const
{
    if (!std::isfinite(amp) || !std::isfinite(phaseDeg)) {
        return QColor(24, 24, 24);
    }

    double phaseNorm = std::fmod(phaseDeg, 360.0);
    if (phaseNorm < 0.0) {
        phaseNorm += 360.0;
    }

    const double minLogAmp = std::log10(kAmpMin);
    const double maxLogAmp = std::log10(kAmpMax);
    double logAmp = std::log10(std::max(amp, kVerySmallValue));
    logAmp = qBound(minLogAmp, logAmp, maxLogAmp);
    const double value = clamp01((logAmp - minLogAmp) / (maxLogAmp - minLogAmp));

    return QColor::fromHsvF(phaseNorm / 360.0, 1.0, value);
}

QImage ArrayRgbHeatmapWindow::buildHeatmapImage(bool useAmpPhase) const
{
    const int width = qMax(1, m_frames.size());
    const int height = qMax(1, m_channelCount);
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(24, 24, 24));

    for (int x = 0; x < m_frames.size(); ++x) {
        const FrameRecord& record = m_frames[x];
        for (int pos = 0; pos < m_channelCount; ++pos) {
            const int row = (m_channelCount - 1) - pos;
            QColor color(24, 24, 24);

            if (useAmpPhase) {
                if (pos < record.amp.size() && pos < record.phase.size()) {
                    color = colorFromAmpPhase(record.amp[pos], record.phase[pos]);
                }
            } else {
                if (pos < record.x.size() && pos < record.y.size()) {
                    const double xVal = record.x[pos];
                    const double yVal = record.y[pos];
                    const double amp = std::hypot(xVal, yVal);
                    const double phase = std::atan2(yVal, xVal) * 180.0 / M_PI;
                    color = colorFromAmpPhase(amp, phase);
                }
            }

            image.setPixelColor(x, row, color);
        }
    }

    return image;
}

void ArrayRgbHeatmapWindow::configurePlot(QCustomPlot* plot,
                                          QCPItemPixmap* item,
                                          const QImage& image,
                                          const QString& xAxisLabel) const
{
    if (!plot || !item) {
        return;
    }

    const QPixmap pixmap = QPixmap::fromImage(image);
    item->setPixmap(pixmap);
    item->setScaled(true, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    item->topLeft->setType(QCPItemPosition::ptPlotCoords);
    item->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    item->topLeft->setAxes(plot->xAxis, plot->yAxis);
    item->bottomRight->setAxes(plot->xAxis, plot->yAxis);

    const double xMax = currentXAxisMax();
    item->topLeft->setCoords(0.0, m_channelCount - 1.0);
    item->bottomRight->setCoords(xMax, 0.0);

    plot->xAxis->setLabel(xAxisLabel);
    plot->xAxis->setRange(0.0, xMax);
    plot->yAxis->setLabel(QStringLiteral("显示位置"));
    plot->yAxis->setRange(0.0, m_channelCount - 1.0);

    plot->replot(QCustomPlot::rpQueuedReplot);
}

void ArrayRgbHeatmapWindow::rebuildPlots()
{
    const QImage ampPhase = buildHeatmapImage(true);
    const QImage realImag = buildHeatmapImage(false);
    configurePlot(m_ampPhasePlot, m_ampPhasePixmap, ampPhase, currentXAxisLabel());
    configurePlot(m_realImagPlot, m_realImagPixmap, realImag, currentXAxisLabel());

    if (m_statusLabel) {
        if (m_frames.isEmpty()) {
            m_statusLabel->setText(QStringLiteral("状态：等待数据"));
        } else {
            const FrameRecord& last = m_frames.last();
            m_statusLabel->setText(QStringLiteral("状态：帧 %1 | 缓冲 %2 帧 | 通道 %3 | 轴: %4")
                                   .arg(last.sequence)
                                   .arg(m_frames.size())
                                   .arg(m_channelCount)
                                   .arg(currentXAxisLabel()));
        }
    }
}

void ArrayRgbHeatmapWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    bool changed = false;
    for (const FrameData& frame : frames) {
        if (m_frames.isEmpty()) {
            m_channelCount = displayChannelCount();
        }
        changed = appendFrame(frame) || changed;
    }

    if (changed) {
        rebuildPlots();
    }
}

void ArrayRgbHeatmapWindow::onCriticalFrame(const FrameData& frame)
{
    if (appendFrame(frame)) {
        rebuildPlots();
    }
}

void ArrayRgbHeatmapWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!m_frames.isEmpty()) {
        return;
    }
    loadSnapshot(snapshot);
    if (!m_frames.isEmpty()) {
        rebuildPlots();
    }
}

void ArrayRgbHeatmapWindow::onXAxisModeChanged(int index)
{
    m_xAxisMode = (index == 1) ? XAxisMode::TimeSeconds : XAxisMode::FrameNumber;
    rebuildPlots();
}

void ArrayRgbHeatmapWindow::onClearClicked()
{
    clearFrames();
}

void ArrayRgbHeatmapWindow::onExportClicked()
{
    const QString defaultName = QStringLiteral("array_rgb_heatmap_%1.png")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString startDir = AppConfig::instance()
        ? AppConfig::instance()->defaultExportDirectory()
        : QDir::currentPath();

    const QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出热力图"),
        QDir(startDir).filePath(defaultName),
        QStringLiteral("PNG 图片 (*.png)"));

    if (filePath.isEmpty()) {
        return;
    }

    const QPixmap shot = grab();
    if (!shot.save(filePath, "PNG")) {
        qWarning() << "ArrayRgbHeatmapWindow 导出失败:" << filePath;
    }
}
