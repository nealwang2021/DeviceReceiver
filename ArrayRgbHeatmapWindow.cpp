#include "ArrayRgbHeatmapWindow.h"

#include "AppConfig.h"
#include "BusyOverlay.h"
#include "HistoryDataProvider.h"
#include "ReviewLoadCoordinator.h"
#include "SelectionState.h"
#include "SqlHistoryQuery.h"
#include "qcustomplot.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kDisplayChannels = 40;
constexpr qint64 kRealtimeLiveWindowMs = 3600LL * 1000LL;
/// 热力图横向像素列数硬顶，避免超大 visible 列表时 QImage/QPainter 过慢
constexpr int kHeatmapMaxVisibleColumns = 12000;
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

bool perfLogEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("DEVICE_RECEIVER_PERF_LOG") > 0;
    return enabled;
}

} // namespace

ArrayRgbHeatmapWindow::ArrayRgbHeatmapWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("阵列热力图"));
    resize(1400, 900);
    initUi();

    auto* sel = SelectionState::instance();
    connect(sel, &SelectionState::selectionChanged,
            this, &ArrayRgbHeatmapWindow::onSelectionChanged);
    if (sel->hasRange()) {
        m_reviewStartMs = sel->startMs();
        m_reviewEndMs = sel->endMs();
        m_reviewMode = (sel->mode() == SelectionState::Review);
    }

    // 加载蒙版：覆盖整个窗口，仅响应阵列热力图自己的任务。
    m_busyOverlay = new BusyOverlay(this);
    m_busyOverlay->setMessage(QStringLiteral("正在加载历史数据..."));
    connect(ReviewLoadCoordinator::instance(),
            &ReviewLoadCoordinator::busyChanged,
            this,
            [this](bool /*busy*/, const QStringList& descriptors) {
                if (!m_busyOverlay) return;
                bool selfBusy = false;
                for (const QString& d : descriptors) {
                    if (d.startsWith(QStringLiteral("阵列热力图"))) {
                        selfBusy = true;
                        break;
                    }
                }
                if (selfBusy) {
                    m_busyOverlay->showOverlay();
                } else {
                    m_busyOverlay->hideOverlay();
                }
            });

    if (m_reviewMode) {
        // 窗口在已处于 review 状态时打开：尝试一次 DB 加载；DB 未就绪也不报错，
        // 后续 selectionChanged 信号会再次触发 loadReviewFromDb。
        loadReviewFromDb();
    }
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
        PlotWindowBase::applyConfiguredOpenGl(plot);
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
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    connect(m_rebuildTimer, &QTimer::timeout, this, [this]() {
        m_rebuildThrottle.restart();
        m_rebuildPending = false;
        rebuildPlots();
    });
    m_rebuildThrottle.start();
    m_perfLogTimer.start();
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
    if (m_frames.size() > cap) {
        m_frames.remove(0, m_frames.size() - cap);
    }
    trimFramesToRealtimeLiveWindow();
}

QString ArrayRgbHeatmapWindow::currentXAxisLabel() const
{
    return (m_xAxisMode == XAxisMode::TimeSeconds)
        ? QStringLiteral("相对时间(s)")
        : QStringLiteral("帧号");
}

double ArrayRgbHeatmapWindow::currentXAxisMax() const
{
    const QVector<int> visible = collectVisibleFrameIndices();
    if (visible.isEmpty()) {
        return 1.0;
    }

    const QVector<FrameRecord>& src = sourceFramesForRender();
    if (m_xAxisMode == XAxisMode::TimeSeconds) {
        const qint64 t0 = src.at(visible.first()).timestampMs;
        const qint64 t1 = src.at(visible.last()).timestampMs;
        const double duration = (t1 - t0) / 1000.0;
        return (duration > 0.0) ? duration : 1.0;
    }

    return qMax(1.0, static_cast<double>(visible.size() - 1));
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
    if (m_frames.size() > cap) {
        m_frames.remove(0, m_frames.size() - cap);
    }
    trimFramesToRealtimeLiveWindow();

    return true;
}

void ArrayRgbHeatmapWindow::clearFrames()
{
    if (m_rebuildTimer) {
        m_rebuildTimer->stop();
    }
    m_rebuildPending = false;
    m_frames.clear();
    m_reviewFrames.clear();
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
    const QVector<int> visible = collectVisibleFrameIndices();
    const int width = qMax(1, visible.size());
    const int height = qMax(1, m_channelCount);
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(24, 24, 24));

    const QVector<FrameRecord>& src = sourceFramesForRender();
    for (int x = 0; x < visible.size(); ++x) {
        const int frameIdx = visible.at(x);
        if (frameIdx < 0 || frameIdx >= src.size()) continue;
        const FrameRecord& record = src[frameIdx];
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

QVector<int> ArrayRgbHeatmapWindow::collectVisibleFrameIndices() const
{
    const QVector<FrameRecord>& src = sourceFramesForRender();
    QVector<int> out;
    out.reserve(src.size());

    // Review 模式：m_reviewFrames 已经是按 SelectionState 范围加载并抽稀过的，直接全选；
    // Live 模式：使用全部 m_frames（早期版本支持过 review 时按时间过滤 m_frames，但 m_frames
    // 仅在 live 流时有效，无法回放更早历史，故现已移除该过滤路径）。
    for (int i = 0; i < src.size(); ++i) {
        out.append(i);
    }

    if (out.size() > kHeatmapMaxVisibleColumns) {
        const int stride = (out.size() + kHeatmapMaxVisibleColumns - 1) / kHeatmapMaxVisibleColumns;
        QVector<int> sub;
        sub.reserve((out.size() + stride - 1) / stride);
        for (int i = 0; i < out.size(); i += stride) {
            sub.append(out.at(i));
        }
        return sub;
    }
    return out;
}

const QVector<ArrayRgbHeatmapWindow::FrameRecord>& ArrayRgbHeatmapWindow::sourceFramesForRender() const
{
    return m_reviewMode ? m_reviewFrames : m_frames;
}

void ArrayRgbHeatmapWindow::loadReviewFromDb()
{
    m_reviewFrames.clear();
    if (m_reviewEndMs <= m_reviewStartMs) {
        return;
    }
    auto* hdp = HistoryDataProvider::instance();
    if (!hdp || !hdp->isDatabaseOpen()) {
        return;
    }
    const QString dbPath = hdp->currentDatabasePath();
    if (dbPath.isEmpty()) {
        return;
    }

    const int channelCount = qBound(1, m_channelCount, displayChannelCount());
    const qint64 startMs = m_reviewStartMs;
    const qint64 endMs = m_reviewEndMs;

    const quint64 epoch = ++m_reviewEpoch;
    auto* coord = ReviewLoadCoordinator::instance();
    const QString taskKey = QStringLiteral("ArrayRgbHeatmap/%1").arg(epoch);
    coord->beginTask(taskKey, QStringLiteral("阵列热力图加载历史"));

    QPointer<ArrayRgbHeatmapWindow> self(this);
    QtConcurrent::run([self, epoch, taskKey, dbPath, startMs, endMs, channelCount]() {
        ReviewQueryResult result;
        result.startMs = startMs;
        result.endMs = endMs;
        result.channelCount = channelCount;

        SqlHistoryQuery query;
        if (!query.open(dbPath)) {
            qWarning() << "[ArrayRgbHeatmap review] 打开 DB 只读连接失败:" << dbPath;
            QMetaObject::invokeMethod(qApp,
                [self, epoch, taskKey, r = std::move(result)]() mutable {
                    if (self) self->onReviewQueryFinished(epoch, std::move(r));
                    ReviewLoadCoordinator::instance()->endTask(taskKey);
                },
                Qt::QueuedConnection);
            return;
        }

        constexpr int kReviewMaxFrames = kHeatmapMaxVisibleColumns;
        qint64 estimated = query.estimateRowCount(startMs, endMs);
        if (estimated < 0) estimated = 0;
        int stride = 1;
        if (estimated > kReviewMaxFrames) {
            stride = static_cast<int>((estimated + kReviewMaxFrames - 1) / kReviewMaxFrames);
            stride = qMax(1, stride);
        }
        result.stride = stride;

        constexpr int kChunkSize = 2000;
        qint64 cursorTs = startMs - 1;
        qint64 cursorRowId = std::numeric_limits<qint64>::min();
        qint64 seenRows = 0;
        QString err;

        while (result.frames.size() < kReviewMaxFrames) {
            QVector<SqlHistoryQuery::AlignedFrameRow> rows;
            if (!query.fetchRawChunk(startMs, endMs,
                                     cursorTs, cursorRowId,
                                     kChunkSize, &rows, &err)) {
                qWarning() << "[ArrayRgbHeatmap review] fetchRawChunk 失败:" << err;
                break;
            }
            if (rows.isEmpty()) break;

            for (const SqlHistoryQuery::AlignedFrameRow& row : rows) {
                const qint64 idx = seenRows++;
                cursorTs = row.timestampMs;
                cursorRowId = row.rowId;
                if (stride > 1 && (idx % stride) != 0) continue;

                FrameRecord record;
                record.sequence = row.frameSequence;
                record.timestampMs = row.timestampMs;
                record.amp = makeNaNVector(channelCount);
                record.phase = makeNaNVector(channelCount);
                record.x = makeNaNVector(channelCount);
                record.y = makeNaNVector(channelCount);
                for (int pos = 0; pos < channelCount && pos < SqlHistoryQuery::kAlignedChannelCount; ++pos) {
                    if (row.amp[pos].isValid() && !row.amp[pos].isNull()) {
                        record.amp[pos] = row.amp[pos].toDouble();
                    }
                    if (row.phase[pos].isValid() && !row.phase[pos].isNull()) {
                        record.phase[pos] = row.phase[pos].toDouble();
                    }
                    if (row.x[pos].isValid() && !row.x[pos].isNull()) {
                        record.x[pos] = row.x[pos].toDouble();
                    }
                    if (row.y[pos].isValid() && !row.y[pos].isNull()) {
                        record.y[pos] = row.y[pos].toDouble();
                    }
                }
                result.frames.append(std::move(record));
                if (result.frames.size() >= kReviewMaxFrames) break;
            }
            if (rows.size() < kChunkSize) break;
        }
        result.ok = true;

        QMetaObject::invokeMethod(qApp,
            [self, epoch, taskKey, r = std::move(result)]() mutable {
                if (self) self->onReviewQueryFinished(epoch, std::move(r));
                ReviewLoadCoordinator::instance()->endTask(taskKey);
            },
            Qt::QueuedConnection);
    });
}

void ArrayRgbHeatmapWindow::onReviewQueryFinished(quint64 epoch, ReviewQueryResult result)
{
    if (epoch != m_reviewEpoch) {
        return;
    }
    if (!m_reviewMode) {
        return;
    }

    m_reviewFrames = std::move(result.frames);
    if (result.channelCount > 0) {
        m_channelCount = qBound(1, result.channelCount, displayChannelCount());
    }

    if (m_statusLabel) {
        if (!result.ok) {
            m_statusLabel->setText(QStringLiteral("状态：回放加载失败"));
        } else {
            m_statusLabel->setText(QStringLiteral("状态：回放 [%1, %2] 共 %3 帧（步长 %4）")
                                       .arg(result.startMs)
                                       .arg(result.endMs)
                                       .arg(m_reviewFrames.size())
                                       .arg(result.stride));
        }
    }
    scheduleRebuild();
}

void ArrayRgbHeatmapWindow::trimFramesToRealtimeLiveWindow()
{
    if (m_reviewMode) {
        return;
    }
    if (HistoryDataProvider::instance()->sourceMode() != HistoryDataProvider::HistorySourceMode::SessionRealtime) {
        return;
    }
    if (m_frames.isEmpty()) {
        return;
    }
    const qint64 lastTs = m_frames.last().timestampMs;
    const qint64 cutoff = lastTs - kRealtimeLiveWindowMs;
    while (!m_frames.isEmpty() && m_frames.first().timestampMs < cutoff) {
        m_frames.removeFirst();
    }
}

void ArrayRgbHeatmapWindow::onSelectionChanged(qint64 startMs, qint64 endMs, int mode)
{
    const bool nowReview = (mode == 1);
    m_reviewStartMs = startMs;
    m_reviewEndMs = endMs;
    const bool wasReview = m_reviewMode;
    m_reviewMode = nowReview;

    if (nowReview) {
        // 切到 review：异步从 DB 拉取；先清空 review 缓冲并主动重绘一次（即出现"空白等待"），
        // worker 完成后 onReviewQueryFinished 会再次 scheduleRebuild。
        loadReviewFromDb();
        scheduleRebuild();
    } else if (wasReview) {
        // 切回 live：丢弃 review 缓冲，下一帧/快照到达后用 m_frames 重绘。
        m_reviewFrames.clear();
        scheduleRebuild();
    }
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
    QElapsedTimer timer;
    timer.start();

    const QImage ampPhase = buildHeatmapImage(true);
    const QImage realImag = buildHeatmapImage(false);
    configurePlot(m_ampPhasePlot, m_ampPhasePixmap, ampPhase, currentXAxisLabel());
    configurePlot(m_realImagPlot, m_realImagPixmap, realImag, currentXAxisLabel());

    if (m_statusLabel) {
        const QVector<FrameRecord>& src = sourceFramesForRender();
        if (src.isEmpty()) {
            if (m_reviewMode) {
                m_statusLabel->setText(QStringLiteral("状态：回放范围内无数据"));
            } else {
                m_statusLabel->setText(QStringLiteral("状态：等待数据"));
            }
        } else {
            const FrameRecord& last = src.last();
            const QString prefix = m_reviewMode ? QStringLiteral("回放") : QStringLiteral("帧");
            m_statusLabel->setText(QStringLiteral("状态：%1 %2 | 缓冲 %3 帧 | 通道 %4 | 轴: %5")
                                   .arg(prefix)
                                   .arg(last.sequence)
                                   .arg(src.size())
                                   .arg(m_channelCount)
                                   .arg(currentXAxisLabel()));
        }
    }

    m_perfRebuildCount += 1;
    m_perfRebuildCostMs += timer.elapsed();
    if (perfLogEnabled() && m_perfLogTimer.elapsed() >= 5000) {
        const double avgMs = (m_perfRebuildCount > 0)
            ? static_cast<double>(m_perfRebuildCostMs) / static_cast<double>(m_perfRebuildCount)
            : 0.0;
        qInfo().nospace()
            << "[Perf][ArrayRgbHeatmapWindow] avgRebuildMs=" << QString::number(avgMs, 'f', 2)
            << " bufferedFrames=" << m_frames.size()
            << " channels=" << m_channelCount;
        m_perfLogTimer.restart();
        m_perfRebuildCount = 0;
        m_perfRebuildCostMs = 0;
    }
}

void ArrayRgbHeatmapWindow::scheduleRebuild()
{
    if (!m_rebuildTimer) {
        rebuildPlots();
        return;
    }
    if (m_rebuildTimer->isActive()) {
        m_rebuildPending = true;
        return;
    }
    const qint64 elapsed = m_rebuildThrottle.elapsed();
    if (elapsed >= m_rebuildMinIntervalMs) {
        m_rebuildThrottle.restart();
        m_rebuildPending = false;
        rebuildPlots();
        return;
    }
    m_rebuildPending = true;
    const int waitMs = qMax(1, m_rebuildMinIntervalMs - static_cast<int>(elapsed));
    m_rebuildTimer->start(waitMs);
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

    // Review 模式下保留 live 缓冲（便于切回 live 后立刻有数据），但不触发重绘以免覆盖回放视图。
    if (changed && !m_reviewMode) {
        scheduleRebuild();
    }
}

void ArrayRgbHeatmapWindow::onCriticalFrame(const FrameData& frame)
{
    const bool added = appendFrame(frame);
    if (added && !m_reviewMode) {
        scheduleRebuild();
    }
}

void ArrayRgbHeatmapWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot)
{
    if (!m_frames.isEmpty()) {
        return;
    }
    loadSnapshot(snapshot);
    if (!m_frames.isEmpty() && !m_reviewMode) {
        scheduleRebuild();
    }
}

void ArrayRgbHeatmapWindow::onXAxisModeChanged(int index)
{
    m_xAxisMode = (index == 1) ? XAxisMode::TimeSeconds : XAxisMode::FrameNumber;
    scheduleRebuild();
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
