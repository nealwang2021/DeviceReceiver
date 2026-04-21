#include "HistoryOverviewWindow.h"

#include "HistoryDataProvider.h"
#include <QDebug>
#include "SelectionState.h"
#include "qcustomplot.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
/// X 轴为 QCPAxisTickerDateTime：坐标系为「自 epoch 起的秒」（可带小数毫秒），内部逻辑仍用毫秒 qint64。
constexpr double kMsToPlotXSec = 1.0 / 1000.0;
inline double msToPlotX(qint64 ms) { return static_cast<double>(ms) * kMsToPlotXSec; }
inline qint64 plotXToMs(double xSec)
{
    if (!std::isfinite(xSec)) {
        return 0;
    }
    return qRound64(xSec * 1000.0);
}

/// QCPAxis::coordToPixel 在 range.size()==0 时会除零；单点时间戳或 Live 退化时会出现，拖动必崩。
inline void setOverviewXAxisRangeMs(QCustomPlot* plot, qint64 startMs, qint64 endMs)
{
    if (!plot || !plot->xAxis) {
        return;
    }
    qint64 loMs = startMs;
    qint64 hiMs = endMs;
    if (hiMs <= loMs) {
        hiMs = loMs + 1;
    }
    double lo = msToPlotX(loMs);
    double hi = msToPlotX(hiMs);
    constexpr double kMinSpanSec = 1e-3; // 1ms，保证秒坐标轴非退化
    if (hi - lo < kMinSpanSec) {
        hi = lo + kMinSpanSec;
    }
    plot->xAxis->setRange(lo, hi);
}

void updateOverviewXAxisDateTimeFormat(QCustomPlot* plot, qint64 spanMs)
{
    if (!plot || !plot->xAxis) {
        return;
    }
    auto dt = qSharedPointerDynamicCast<QCPAxisTickerDateTime>(plot->xAxis->ticker());
    if (!dt) {
        return;
    }
    if (spanMs > 86400000LL) {
        dt->setDateTimeFormat(QStringLiteral("yyyy-MM-dd  HH:mm:ss.zzz"));
        dt->setTickCount(6);
    } else {
        dt->setDateTimeFormat(QStringLiteral("HH:mm:ss.zzz"));
        dt->setTickCount(7);
    }
}

constexpr int kOverviewTargetBuckets = 800;
constexpr int kEdgeHandlePixels = 8;  // 左右拉伸判定像素宽度
constexpr qint64 kDefaultInitialWindowMs = 60 * 1000; // 默认 brush 宽度：60 秒
/// Live 下全表包络 SQL 的最小间隔（避免随数据变长每秒全表 GROUP BY 拖慢主线程）
constexpr qint64 kLiveFullEnvelopeIntervalMs = 60 * 1000;
/// Live 下仅更新时间边界 + 尾部跟随的间隔
constexpr int kLiveRefreshIntervalMs = 5000;
/// 会话实时 + Live 时总览与包络 SQL 仅覆盖最近 wall-clock 时间窗
constexpr qint64 kRealtimeLiveWindowMs = 3600LL * 1000LL;
constexpr qint64 kLargeRowCountWarnThreshold = 5000000LL;
/// Live 尾部跟随：与 SelectionState 差异小于此则不重复 emit，减轻阵列/热力图压力
constexpr qint64 kLiveTailCommitEpsilonMs = 2;
/// 判定 brush 右端「贴齐数据尾部」时的毫秒容差（像素反算量化）
constexpr qint64 kAlignRightSlackMs = 2;
}

HistoryOverviewWindow::HistoryOverviewWindow(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("historyOverviewWindow"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部控制栏
    auto* controlRow = new QHBoxLayout();
    controlRow->setSpacing(6);
    auto* compLabel = new QLabel(QStringLiteral("分量:"), this);
    m_componentCombo = new QComboBox(this);
    m_componentCombo->addItem(QStringLiteral("幅值"));
    m_componentCombo->addItem(QStringLiteral("相位"));
    m_componentCombo->addItem(QStringLiteral("实部"));
    m_componentCombo->addItem(QStringLiteral("虚部"));
    m_componentCombo->setCurrentIndex(0);
    m_componentCombo->setToolTip(QStringLiteral("总览包络使用的分量"));

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);

    m_backToLiveButton = new QPushButton(QStringLiteral("回到实时"), this);
    m_backToLiveButton->setToolTip(QStringLiteral("将范围对齐到数据尾部并切换到 Live 模式"));

    m_openOfflineDbButton = new QPushButton(QStringLiteral("打开离线库…"), this);
    m_openOfflineDbButton->setToolTip(QStringLiteral("打开外部 device 会话 SQLite（只读）"));

    m_reopenSessionDbButton = new QPushButton(QStringLiteral("回到会话库"), this);
    m_reopenSessionDbButton->setToolTip(QStringLiteral("切换回当前运行会话的实时记录库"));

    m_dbLabel = new QLabel(QStringLiteral("DB: 未连接"), this);
    m_boundsLabel = new QLabel(QStringLiteral("历史: -"), this);
    m_statusLabel = new QLabel(QStringLiteral("范围: -"), this);

    controlRow->addWidget(compLabel);
    controlRow->addWidget(m_componentCombo);
    controlRow->addWidget(m_refreshButton);
    controlRow->addWidget(m_backToLiveButton);
    controlRow->addWidget(m_openOfflineDbButton);
    controlRow->addWidget(m_reopenSessionDbButton);
    controlRow->addSpacing(12);
    controlRow->addWidget(m_dbLabel);
    controlRow->addWidget(m_boundsLabel);
    controlRow->addWidget(m_statusLabel);
    controlRow->addStretch();

    mainLayout->addLayout(controlRow);

    // QCustomPlot
    m_plot = new QCustomPlot(this);
#ifdef QCUSTOMPLOT_USE_OPENGL
    m_plot->setOpenGl(true);
#endif
    m_plot->setMinimumHeight(120);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_plot->setInteractions(QCP::Interactions{});
    m_plot->xAxis->setLabel(QStringLiteral("时间 (本地)"));
    m_plot->xAxis->setTickLabels(true);
    {
        QSharedPointer<QCPAxisTickerDateTime> dateTicker(new QCPAxisTickerDateTime);
        dateTicker->setDateTimeFormat(QStringLiteral("HH:mm:ss.zzz"));
        dateTicker->setTickCount(7);
        m_plot->xAxis->setTicker(dateTicker);
    }
    m_plot->yAxis->setLabel(QString());
    m_plot->yAxis->setTickLabels(true);
    m_plot->axisRect()->setupFullAxesBox(true);
    m_plot->axisRect()->setRangeZoom(Qt::Horizontal);
    m_plot->axisRect()->setRangeDrag(Qt::Horizontal);

    m_maxGraph = m_plot->addGraph();
    m_minGraph = m_plot->addGraph();
    QPen envPen(QColor(120, 170, 240));
    envPen.setWidthF(1.2);
    m_maxGraph->setPen(envPen);
    m_minGraph->setPen(envPen);
    m_maxGraph->setBrush(QBrush(QColor(120, 170, 240, 80)));
    m_maxGraph->setChannelFillGraph(m_minGraph); // 上下 graph 之间填充

    // 选区矩形
    m_rangeRect = new QCPItemRect(m_plot);
    m_rangeRect->setLayer(QStringLiteral("overlay"));
    m_rangeRect->topLeft->setType(QCPItemPosition::ptPlotCoords);
    m_rangeRect->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    QPen rectPen(QColor(255, 170, 60));
    rectPen.setWidthF(1.2);
    m_rangeRect->setPen(rectPen);
    m_rangeRect->setBrush(QBrush(QColor(255, 170, 60, 60)));
    m_rangeRect->setVisible(false);

    m_leftHandle = new QCPItemStraightLine(m_plot);
    m_leftHandle->setLayer(QStringLiteral("overlay"));
    m_leftHandle->setPen(QPen(QColor(255, 140, 40), 2.0));
    m_leftHandle->setVisible(false);

    m_rightHandle = new QCPItemStraightLine(m_plot);
    m_rightHandle->setLayer(QStringLiteral("overlay"));
    m_rightHandle->setPen(QPen(QColor(255, 140, 40), 2.0));
    m_rightHandle->setVisible(false);

    mainLayout->addWidget(m_plot, 1);
    setLayout(mainLayout);

    // Live 模式定时刷新：默认仅轻量 queryTimeBounds + 尾部跟随；全表包络 SQL 见 kLiveFullEnvelopeIntervalMs。
    // Review 模式不自动刷新，避免打断人工回放。
    m_liveRefreshTimer = new QTimer(this);
    m_liveRefreshTimer->setInterval(kLiveRefreshIntervalMs);
    connect(m_liveRefreshTimer, &QTimer::timeout, this, &HistoryOverviewWindow::liveLightweightTick);
    m_liveRefreshTimer->start();

    // 事件过滤：处理 brush 交互
    m_plot->installEventFilter(this);
    m_plot->setMouseTracking(true);

    // 信号连接
    connect(m_componentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HistoryOverviewWindow::onComponentChanged);
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refreshOverview(true);
    });
    connect(m_backToLiveButton, &QPushButton::clicked,
            this, &HistoryOverviewWindow::jumpToLive);
    connect(m_openOfflineDbButton, &QPushButton::clicked,
            this, &HistoryOverviewWindow::onOpenOfflineDatabaseClicked);
    connect(m_reopenSessionDbButton, &QPushButton::clicked,
            this, &HistoryOverviewWindow::onReopenSessionDatabaseClicked);

    auto* hdp = HistoryDataProvider::instance();
    connect(hdp, &HistoryDataProvider::databaseOpened,
            this, &HistoryOverviewWindow::onDatabaseOpened);
    connect(hdp, &HistoryDataProvider::databaseClosed,
            this, &HistoryOverviewWindow::onDatabaseClosed);

    auto* sel = SelectionState::instance();
    connect(sel, &SelectionState::selectionChanged,
            this, &HistoryOverviewWindow::onSelectionChanged);

    // 与 onDatabaseOpened 共用一次 queued 刷新，避免启动双次全量 SQL。
    scheduleRefreshOverview();

    updateStatusLabels();
    updateRefreshButtonToolTip();
}

HistoryOverviewWindow::~HistoryOverviewWindow() = default;

void HistoryOverviewWindow::scheduleRefreshOverview()
{
    if (m_refreshOverviewQueued) {
        return;
    }
    m_refreshOverviewQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_refreshOverviewQueued = false;
        refreshOverview(true);
    });
}

void HistoryOverviewWindow::syncLiveTailToSelectionState(bool forceCommit)
{
    if (!m_hasBounds) {
        return;
    }
    auto* sel = SelectionState::instance();
    // 轻量 tick 仅在已是 Live 时对齐尾部；「回到实时」按钮 force=true 时需从 Review 拉回 Live。
    if (!forceCommit && sel->mode() != SelectionState::Live) {
        return;
    }

    const qint64 span = plotEnvelopeSpanMs();
    qint64 width = m_selEndMs - m_selStartMs;
    if (width <= 0) {
        width = std::min<qint64>(kDefaultInitialWindowMs, std::max<qint64>(1, span));
    }
    const qint64 end = plotEnvelopeEndMs();
    const qint64 envStart = plotEnvelopeStartMs();
    const qint64 start = std::max(envStart, end - width);

    if (!forceCommit && sel->hasRange()
        && qAbs(sel->startMs() - start) <= kLiveTailCommitEpsilonMs
        && qAbs(sel->endMs() - end) <= kLiveTailCommitEpsilonMs) {
        m_selStartMs = sel->startMs();
        m_selEndMs = sel->endMs();
        applyRangeToItems();
        m_plot->replot(QCustomPlot::rpQueuedReplot);
        return;
    }

    m_selStartMs = start;
    m_selEndMs = end;
    commitRangeToSelectionState(false);
    applyRangeToItems();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void HistoryOverviewWindow::scheduleEnvelopeRebuild()
{
    if (m_envelopeRebuildPending) {
        return;
    }
    m_envelopeRebuildPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_envelopeRebuildPending = false;
        if (!m_hasBounds) {
            return;
        }
        rebuildEnvelope();
        m_lastFullEnvelopeWallMs = QDateTime::currentMSecsSinceEpoch();
    });
}

void HistoryOverviewWindow::updateRefreshButtonToolTip()
{
    if (!m_refreshButton) {
        return;
    }
    auto* hdp = HistoryDataProvider::instance();
    auto* sel = SelectionState::instance();
    if (hdp->sourceMode() == HistoryDataProvider::HistorySourceMode::SessionRealtime
        && sel->mode() == SelectionState::Live) {
        m_refreshButton->setToolTip(QStringLiteral("按当前分量重算总览包络（会话实时 + Live 下为最近 1 小时）"));
    } else {
        m_refreshButton->setToolTip(QStringLiteral("按当前分量重算总览包络（当前模式下的全时间范围）"));
    }
}

qint64 HistoryOverviewWindow::plotEnvelopeStartMs() const
{
    auto* hdp = HistoryDataProvider::instance();
    auto* sel = SelectionState::instance();
    if (hdp->sourceMode() == HistoryDataProvider::HistorySourceMode::SessionRealtime
        && sel->mode() == SelectionState::Live) {
        return std::max(m_dataMinMs, m_dataMaxMs - kRealtimeLiveWindowMs);
    }
    return m_dataMinMs;
}

qint64 HistoryOverviewWindow::plotEnvelopeEndMs() const
{
    return m_dataMaxMs;
}

qint64 HistoryOverviewWindow::plotEnvelopeSpanMs() const
{
    return std::max<qint64>(1LL, plotEnvelopeEndMs() - plotEnvelopeStartMs());
}

void HistoryOverviewWindow::onOpenOfflineDatabaseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("打开离线 SQLite"),
        QString(),
        QStringLiteral("SQLite 数据库 (*.db);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    auto* hdp = HistoryDataProvider::instance();
    if (!hdp->openOfflineDatabase(path)) {
        QMessageBox::warning(
            this,
            QStringLiteral("打开失败"),
            QStringLiteral("无法以只读方式打开数据库：\n%1").arg(path));
        return;
    }
    qint64 rowCount = 0;
    if (hdp->queryAlignedFrameRowCount(&rowCount) && rowCount >= kLargeRowCountWarnThreshold) {
        QMessageBox::information(
            this,
            QStringLiteral("数据量较大"),
            QStringLiteral("aligned_frames 约 %1 百万行。总览按时间桶聚合、热力图列数会抽样，"
                           "避免一次绘制过多像素；缩小 brush 时间范围可提高细节。")
                .arg(QString::number(static_cast<double>(rowCount) / 1e6, 'f', 1)));
    }
    refreshOverview(true);
    m_prevSelModeForEnvelope = static_cast<int>(SelectionState::instance()->mode());
    updateRefreshButtonToolTip();
}

void HistoryOverviewWindow::onReopenSessionDatabaseClicked()
{
    auto* hdp = HistoryDataProvider::instance();
    if (hdp->sessionDatabasePath().isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("无会话库"),
            QStringLiteral("当前没有记录会话数据库路径（可能记录器未启动）。"));
        return;
    }
    if (!QFileInfo::exists(hdp->sessionDatabasePath())) {
        QMessageBox::warning(
            this,
            QStringLiteral("文件不存在"),
            QStringLiteral("会话库文件不存在：\n%1").arg(hdp->sessionDatabasePath()));
        return;
    }
    if (!hdp->reopenSessionDatabase()) {
        QMessageBox::warning(
            this,
            QStringLiteral("打开失败"),
            QStringLiteral("无法打开会话库：\n%1").arg(hdp->sessionDatabasePath()));
        return;
    }
    refreshOverview(true);
    m_prevSelModeForEnvelope = static_cast<int>(SelectionState::instance()->mode());
    updateRefreshButtonToolTip();
}

void HistoryOverviewWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_plot) {
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void HistoryOverviewWindow::onComponentChanged(int index)
{
    switch (index) {
    case 1: m_component = SqlHistoryQuery::Component::Phase; break;
    case 2: m_component = SqlHistoryQuery::Component::Real; break;
    case 3: m_component = SqlHistoryQuery::Component::Imag; break;
    case 0:
    default: m_component = SqlHistoryQuery::Component::Amplitude; break;
    }
    scheduleEnvelopeRebuild();
    updateRefreshButtonToolTip();
}

void HistoryOverviewWindow::onDatabaseOpened(const QString& /*path*/)
{
    scheduleRefreshOverview();
}

void HistoryOverviewWindow::onDatabaseClosed()
{
    if (m_dbLabel) {
        m_dbLabel->setText(QStringLiteral("DB: 未连接"));
    }
    m_dbFallbackMismatchSession = false;
    m_hasBounds = false;
    m_dataMinMs = 0;
    m_dataMaxMs = 0;
    m_lastFullEnvelopeWallMs = 0;
    if (m_maxGraph) m_maxGraph->data()->clear();
    if (m_minGraph) m_minGraph->data()->clear();
    if (m_rangeRect) m_rangeRect->setVisible(false);
    if (m_leftHandle) m_leftHandle->setVisible(false);
    if (m_rightHandle) m_rightHandle->setVisible(false);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
    updateStatusLabels();
}

void HistoryOverviewWindow::onSelectionChanged(qint64 startMs, qint64 endMs, int mode)
{
    if (m_dragMode != DragMode::None) {
        return; // 自己在拖动，不被自己回写打断
    }
    m_selStartMs = startMs;
    m_selEndMs = endMs;
    if (m_prevSelModeForEnvelope != mode) {
        m_prevSelModeForEnvelope = mode;
        if (m_hasBounds) {
            scheduleEnvelopeRebuild();
        }
    }
    snapRangeToBounds();
    applyRangeToItems();
    updateStatusLabels();
    updateRefreshButtonToolTip();
}

bool HistoryOverviewWindow::ensureDatabaseAndBounds()
{
    auto* hdp = HistoryDataProvider::instance();

    if (hdp->sourceMode() == HistoryDataProvider::HistorySourceMode::OfflineExternal) {
        if (!hdp->isDatabaseOpen()) {
            if (m_dbLabel) {
                m_dbLabel->setText(QStringLiteral("DB: 未连接"));
            }
            onDatabaseClosed();
            return false;
        }
    } else {
        if (!hdp->isDatabaseOpen()) {
            const QString sessionPath = hdp->sessionDatabasePath();
            if (!sessionPath.isEmpty() && QFileInfo::exists(sessionPath)) {
                hdp->setSourceMode(HistoryDataProvider::HistorySourceMode::SessionRealtime);
                hdp->openDatabase(sessionPath);
            }
        }
        if (!hdp->isDatabaseOpen()) {
            // 兜底：会话路径无效时，尝试自动发现当前目录 data 下最新会话 DB。
            const QString dataDirPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
            QDir dataDir(dataDirPath);
            const QFileInfoList dbFiles = dataDir.entryInfoList(
                QStringList() << QStringLiteral("device_realtime_*.db"),
                QDir::Files,
                QDir::Time);
            if (!dbFiles.isEmpty()) {
                hdp->setSourceMode(HistoryDataProvider::HistorySourceMode::SessionRealtime);
                hdp->openDatabase(dbFiles.first().absoluteFilePath());
            }
        }
        if (!hdp->isDatabaseOpen()) {
            if (m_dbLabel) {
                m_dbLabel->setText(QStringLiteral("DB: 未连接"));
            }
            onDatabaseClosed();
            return false;
        }
    }

    m_dbFallbackMismatchSession = false;
    if (hdp->sourceMode() == HistoryDataProvider::HistorySourceMode::SessionRealtime) {
        const QString sessionPath = hdp->sessionDatabasePath();
        const QString curPath = hdp->currentDatabasePath();
        if (!sessionPath.isEmpty()) {
            if (!QFileInfo::exists(sessionPath)) {
                m_dbFallbackMismatchSession = true;
            } else {
                const QString c = QFileInfo(curPath).canonicalFilePath();
                const QString s = QFileInfo(sessionPath).canonicalFilePath();
                if (!c.isEmpty() && !s.isEmpty()) {
                    m_dbFallbackMismatchSession = (c != s);
                } else {
                    m_dbFallbackMismatchSession = QFileInfo(curPath).absoluteFilePath()
                        != QFileInfo(sessionPath).absoluteFilePath();
                }
            }
            if (m_dbFallbackMismatchSession) {
                qWarning() << "[HistoryOverview] 当前只读库与会话记录路径不一致。会话:"
                           << sessionPath << "当前:" << curPath;
            }
        }
    }

    qint64 minMs = 0, maxMs = 0;
    if (!hdp->queryTimeBoundsFast(minMs, maxMs)) {
        onDatabaseClosed();
        return false;
    }
    m_dataMinMs = minMs;
    m_dataMaxMs = maxMs;
    m_hasBounds = true;

    if (m_dbLabel) {
        const QString dbName = QFileInfo(hdp->currentDatabasePath()).fileName();
        QString prefix;
        if (hdp->sourceMode() == HistoryDataProvider::HistorySourceMode::OfflineExternal) {
            prefix = QStringLiteral("离线");
        } else if (m_dbFallbackMismatchSession) {
            prefix = QStringLiteral("会话·兜底");
        } else {
            prefix = QStringLiteral("会话");
        }
        m_dbLabel->setText(dbName.isEmpty()
            ? QStringLiteral("DB: 已连接 (%1)").arg(prefix)
            : QStringLiteral("DB: %1 (%2)").arg(dbName, prefix));
    }
    return true;
}

void HistoryOverviewWindow::liveLightweightTick()
{
    auto* sel = SelectionState::instance();
    if (sel->mode() != SelectionState::Live) {
        return;
    }
    if (!ensureDatabaseAndBounds()) {
        return;
    }

    if (!sel->hasRange()) {
        const qint64 span = plotEnvelopeSpanMs();
        const qint64 width = std::min<qint64>(kDefaultInitialWindowMs, std::max<qint64>(1, span));
        const qint64 end = m_dataMaxMs;
        const qint64 envStart = plotEnvelopeStartMs();
        const qint64 start = std::max(envStart, end - width);
        m_selStartMs = start;
        m_selEndMs = end;
        sel->setRangeAndMode(start, end, SelectionState::Live);
    } else {
        m_selStartMs = sel->startMs();
        m_selEndMs = sel->endMs();
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool needFull = (m_lastFullEnvelopeWallMs == 0)
        || (now - m_lastFullEnvelopeWallMs >= kLiveFullEnvelopeIntervalMs);

    if (needFull) {
        rebuildEnvelope();
        m_lastFullEnvelopeWallMs = now;
        syncLiveTailToSelectionState(false);
    } else {
        setOverviewXAxisRangeMs(m_plot, plotEnvelopeStartMs(), plotEnvelopeEndMs());
        updateOverviewXAxisDateTimeFormat(m_plot, plotEnvelopeSpanMs());
        syncLiveTailToSelectionState(false);
    }
    updateStatusLabels();
}

void HistoryOverviewWindow::refreshOverview(bool fullEnvelope)
{
    if (!ensureDatabaseAndBounds()) {
        return;
    }

    // 若 SelectionState 还没有范围，初始化一个默认尾部窗口
    auto* sel = SelectionState::instance();
    if (!sel->hasRange()) {
        const qint64 span = plotEnvelopeSpanMs();
        const qint64 width = std::min<qint64>(kDefaultInitialWindowMs, std::max<qint64>(1, span));
        const qint64 end = m_dataMaxMs;
        const qint64 envStart = plotEnvelopeStartMs();
        const qint64 start = std::max(envStart, end - width);
        m_selStartMs = start;
        m_selEndMs = end;
        sel->setRangeAndMode(start, end, SelectionState::Live);
    } else {
        m_selStartMs = sel->startMs();
        m_selEndMs = sel->endMs();
    }

    if (fullEnvelope) {
        rebuildEnvelope();
        m_lastFullEnvelopeWallMs = QDateTime::currentMSecsSinceEpoch();
    } else {
        setOverviewXAxisRangeMs(m_plot, plotEnvelopeStartMs(), plotEnvelopeEndMs());
        updateOverviewXAxisDateTimeFormat(m_plot, plotEnvelopeSpanMs());
        applyRangeToItems();
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
    updateStatusLabels();
    updateRefreshButtonToolTip();
}

void HistoryOverviewWindow::rebuildEnvelope()
{
    if (!m_hasBounds || !m_plot || !m_maxGraph || !m_minGraph) {
        return;
    }
    auto* hdp = HistoryDataProvider::instance();
    const qint64 start = plotEnvelopeStartMs();
    const qint64 end = plotEnvelopeEndMs();
    if (end <= start) {
        return;
    }
    const qint64 bucket = HistoryDataProvider::suggestBucketMs(start, end, kOverviewTargetBuckets);

    QVector<SqlHistoryQuery::BucketRow> rows;
    if (!hdp->queryOverviewEnvelope(start, end, bucket, m_component, &rows)) {
        return;
    }

    QVector<double> keys;
    QVector<double> maxs;
    QVector<double> mins;
    keys.reserve(rows.size());
    maxs.reserve(rows.size());
    mins.reserve(rows.size());
    double yMin = std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();
    for (const auto& r : rows) {
        if (!r.hasData) continue;
        keys.append(msToPlotX(r.bucketStartMs));
        maxs.append(r.maxValue);
        mins.append(r.minValue);
        yMin = std::min(yMin, r.minValue);
        yMax = std::max(yMax, r.maxValue);
    }

    m_maxGraph->setData(keys, maxs, true);
    m_minGraph->setData(keys, mins, true);

    setOverviewXAxisRangeMs(m_plot, start, end);
    updateOverviewXAxisDateTimeFormat(m_plot, std::max<qint64>(1LL, end - start));
    if (std::isfinite(yMin) && std::isfinite(yMax) && yMax > yMin) {
        const double pad = (yMax - yMin) * 0.08;
        m_plot->yAxis->setRange(yMin - pad, yMax + pad);
    } else if (std::isfinite(yMin)) {
        m_plot->yAxis->setRange(yMin - 1.0, yMin + 1.0);
    }
    applyRangeToItems();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void HistoryOverviewWindow::applyRangeToItems()
{
    if (!m_rangeRect || !m_leftHandle || !m_rightHandle || !m_plot) {
        return;
    }
    if (!m_hasBounds || m_selEndMs <= m_selStartMs) {
        m_rangeRect->setVisible(false);
        m_leftHandle->setVisible(false);
        m_rightHandle->setVisible(false);
        return;
    }
    const double yLo = m_plot->yAxis->range().lower;
    const double yHi = m_plot->yAxis->range().upper;
    m_rangeRect->topLeft->setCoords(msToPlotX(m_selStartMs), yHi);
    m_rangeRect->bottomRight->setCoords(msToPlotX(m_selEndMs), yLo);
    m_rangeRect->setVisible(true);

    m_leftHandle->point1->setCoords(msToPlotX(m_selStartMs), yLo);
    m_leftHandle->point2->setCoords(msToPlotX(m_selStartMs), yHi);
    m_leftHandle->setVisible(true);

    m_rightHandle->point1->setCoords(msToPlotX(m_selEndMs), yLo);
    m_rightHandle->point2->setCoords(msToPlotX(m_selEndMs), yHi);
    m_rightHandle->setVisible(true);
}

void HistoryOverviewWindow::updateStatusLabels()
{
    if (m_hasBounds) {
        const auto fmt = [](qint64 ms) {
            return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss.zzz"));
        };
        const qint64 envLo = plotEnvelopeStartMs();
        const qint64 envHi = plotEnvelopeEndMs();
        if (envLo != m_dataMinMs || envHi != m_dataMaxMs) {
            m_boundsLabel->setText(QStringLiteral("全库: %1 ~ %2 | 总览: %3 ~ %4")
                .arg(fmt(m_dataMinMs)).arg(fmt(m_dataMaxMs)).arg(fmt(envLo)).arg(fmt(envHi)));
        } else {
            m_boundsLabel->setText(QStringLiteral("历史: %1 ~ %2")
                .arg(fmt(m_dataMinMs)).arg(fmt(m_dataMaxMs)));
        }
        if (m_selEndMs > m_selStartMs) {
            const qint64 widthMs = m_selEndMs - m_selStartMs;
            const QString widthText = widthMs >= 60000
                ? QStringLiteral("%1m%2s").arg(widthMs / 60000).arg((widthMs / 1000) % 60)
                : QStringLiteral("%1ms").arg(widthMs);
            const auto* sel = SelectionState::instance();
            const QString modeText = (sel->mode() == SelectionState::Live)
                ? QStringLiteral("Live")
                : QStringLiteral("Review");
            m_statusLabel->setText(QStringLiteral("范围: %1 ~ %2 (%3, %4)")
                .arg(fmt(m_selStartMs)).arg(fmt(m_selEndMs)).arg(widthText).arg(modeText));
        } else {
            m_statusLabel->setText(QStringLiteral("范围: -"));
        }
    } else {
        m_boundsLabel->setText(QStringLiteral("历史: 无数据"));
        m_statusLabel->setText(QStringLiteral("范围: -"));
    }
}

void HistoryOverviewWindow::commitRangeToSelectionState(bool asReview)
{
    auto* sel = SelectionState::instance();
    const SelectionState::Mode mode = asReview ? SelectionState::Review : SelectionState::Live;
    sel->setRangeAndMode(m_selStartMs, m_selEndMs, mode);
    updateStatusLabels();
}

HistoryOverviewWindow::DragMode HistoryOverviewWindow::hitTest(double timeX) const
{
    if (m_selEndMs <= m_selStartMs) {
        return DragMode::Creating;
    }
    const QCPRange xr = m_plot->xAxis->range();
    if (!std::isfinite(xr.lower) || !std::isfinite(xr.upper) || !(xr.upper > xr.lower)) {
        return DragMode::Creating;
    }
    if (!std::isfinite(timeX)) {
        return DragMode::Creating;
    }
    const double leftPx = m_plot->xAxis->coordToPixel(msToPlotX(m_selStartMs));
    const double rightPx = m_plot->xAxis->coordToPixel(msToPlotX(m_selEndMs));
    const double hitPx = m_plot->xAxis->coordToPixel(timeX);
    if (std::abs(hitPx - leftPx) <= kEdgeHandlePixels) {
        return DragMode::ResizingLeft;
    }
    if (std::abs(hitPx - rightPx) <= kEdgeHandlePixels) {
        return DragMode::ResizingRight;
    }
    if (hitPx > leftPx && hitPx < rightPx) {
        return DragMode::Moving;
    }
    return DragMode::Creating;
}

void HistoryOverviewWindow::snapRangeToBounds()
{
    if (!m_hasBounds) return;
    const qint64 lo = plotEnvelopeStartMs();
    const qint64 hi = plotEnvelopeEndMs();
    if (m_selStartMs < lo) m_selStartMs = lo;
    if (m_selEndMs > hi) m_selEndMs = hi;
    if (m_selEndMs <= m_selStartMs) {
        m_selEndMs = std::min(hi, m_selStartMs + 1);
    }
}

void HistoryOverviewWindow::jumpToLive()
{
    syncLiveTailToSelectionState(true);
}

bool HistoryOverviewWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_plot || !m_hasBounds) {
        return QWidget::eventFilter(watched, event);
    }
    const QEvent::Type t = event->type();
    if (t == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton) {
            return QWidget::eventFilter(watched, event);
        }
        double tx = m_plot->xAxis->pixelToCoord(me->pos().x());
        if (!std::isfinite(tx)) {
            event->accept();
            return true;
        }
        const DragMode hit = hitTest(tx);
        m_dragMode = hit;
        m_dragAnchorTime = tx;
        m_dragAnchorStart = m_selStartMs;
        m_dragAnchorEnd = m_selEndMs;
        if (hit == DragMode::Creating) {
            m_selStartMs = plotXToMs(tx);
            m_selEndMs = m_selStartMs;
        }
        event->accept();
        return true;
    }
    if (t == QEvent::MouseMove) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (m_dragMode == DragMode::None) {
            return QWidget::eventFilter(watched, event);
        }
        double tx = m_plot->xAxis->pixelToCoord(me->pos().x());
        if (!std::isfinite(tx)) {
            event->accept();
            return true;
        }
        switch (m_dragMode) {
        case DragMode::Creating: {
            qint64 a = plotXToMs(m_dragAnchorTime);
            qint64 b = plotXToMs(tx);
            if (b < a) std::swap(a, b);
            m_selStartMs = a;
            m_selEndMs = b;
            break;
        }
        case DragMode::Moving: {
            const qint64 width = m_dragAnchorEnd - m_dragAnchorStart;
            if (width <= 0) {
                break;
            }
            const qint64 delta = plotXToMs(tx) - plotXToMs(m_dragAnchorTime);
            qint64 newStart = m_dragAnchorStart + delta;
            qint64 newEnd = m_dragAnchorEnd + delta;
            const qint64 lo = plotEnvelopeStartMs();
            const qint64 hi = plotEnvelopeEndMs();
            if (newStart < lo) {
                newStart = lo;
                newEnd = newStart + width;
            }
            if (newEnd > hi) {
                newEnd = hi;
                newStart = newEnd - width;
            }
            m_selStartMs = newStart;
            m_selEndMs = newEnd;
            break;
        }
        case DragMode::ResizingLeft: {
            m_selStartMs = plotXToMs(tx);
            if (m_selStartMs >= m_selEndMs) m_selStartMs = m_selEndMs - 1;
            break;
        }
        case DragMode::ResizingRight: {
            m_selEndMs = plotXToMs(tx);
            if (m_selEndMs <= m_selStartMs) m_selEndMs = m_selStartMs + 1;
            break;
        }
        default:
            break;
        }
        snapRangeToBounds();
        applyRangeToItems();
        m_plot->replot(QCustomPlot::rpQueuedReplot);
        event->accept();
        return true;
    }
    if (t == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton || m_dragMode == DragMode::None) {
            return QWidget::eventFilter(watched, event);
        }
        // 创建模式下若几乎没有拖动，视为点击：以当前宽度跳转到该点
        if (m_dragMode == DragMode::Creating && std::abs(m_selEndMs - m_selStartMs) < 5) {
            const qint64 width = std::max<qint64>(1, (m_dragAnchorEnd - m_dragAnchorStart) > 0
                ? (m_dragAnchorEnd - m_dragAnchorStart)
                : kDefaultInitialWindowMs);
            const qint64 center = m_selStartMs;
            m_selStartMs = center - width / 2;
            m_selEndMs = m_selStartMs + width;
        }
        snapRangeToBounds();
        m_dragMode = DragMode::None;
        // 是否对齐到右端 → Live，否则 Review（与全局数据尾部比较，留毫秒容差）
        const bool alignedToRight = (m_selEndMs >= m_dataMaxMs - kAlignRightSlackMs);
        commitRangeToSelectionState(!alignedToRight);
        applyRangeToItems();
        m_plot->replot(QCustomPlot::rpQueuedReplot);
        updateRefreshButtonToolTip();
        event->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
