#include "PulseEddyPlotWindow.h"
#include "HistoryDataProvider.h"
#include "MainWindow.h"
#include "ApplicationController.h"
#include "SelectionState.h"
#include "qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrent>
#include <QPointer>

PulseEddyPlotWindow::PulseEddyPlotWindow(QWidget* parent)
    : PlotWindowBase(parent)
{
    setWindowTitle(QStringLiteral("脉冲涡流"));
    resize(900, 500);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // ---- 顶栏 ----
    QWidget* topBar = new QWidget(this);
    QVBoxLayout* topLayout = new QVBoxLayout(topBar);
    topLayout->setContentsMargins(4, 2, 4, 2);
    topLayout->setSpacing(2);

    // 第一行：参考线操作 + Y 轴范围 + 帧号
    QWidget* row1 = new QWidget(topBar);
    QHBoxLayout* row1Lay = new QHBoxLayout(row1);
    row1Lay->setContentsMargins(0, 0, 0, 0);
    row1Lay->setSpacing(6);

    m_startRefBtn = new QPushButton(QStringLiteral("开始参考线"), row1);
    m_clearRefBtn = new QPushButton(QStringLiteral("清除参考线"), row1);
    auto* clearBtn = new QPushButton(QStringLiteral("清屏"), row1);
    m_refStatusLabel = new QLabel(QStringLiteral("参考线: 未采集"), row1);
    m_refStatusLabel->setStyleSheet("color: #888;");

    row1Lay->addWidget(m_startRefBtn);
    row1Lay->addWidget(m_clearRefBtn);
    row1Lay->addWidget(clearBtn);
    row1Lay->addWidget(m_refStatusLabel);
    row1Lay->addSpacing(16);

    // Y 轴范围控制
    row1Lay->addWidget(new QLabel(QStringLiteral("Y轴:"), row1));
    m_yAutoRadio   = new QRadioButton(QStringLiteral("自适应"), row1);
    m_yManualRadio = new QRadioButton(QStringLiteral("手动"), row1);
    m_yAutoRadio->setChecked(true);
    auto* yGroup = new QButtonGroup(row1);
    yGroup->addButton(m_yAutoRadio);
    yGroup->addButton(m_yManualRadio);
    row1Lay->addWidget(m_yAutoRadio);
    row1Lay->addWidget(m_yManualRadio);

    m_yMinSpin = new QDoubleSpinBox(row1);
    m_yMinSpin->setRange(-1e6, 1e6);
    m_yMinSpin->setDecimals(3);
    m_yMinSpin->setValue(-10.0);
    m_yMinSpin->setFixedWidth(80);
    m_yMinSpin->setEnabled(false);
    row1Lay->addWidget(m_yMinSpin);
    row1Lay->addWidget(new QLabel(QStringLiteral("~"), row1));
    m_yMaxSpin = new QDoubleSpinBox(row1);
    m_yMaxSpin->setRange(-1e6, 1e6);
    m_yMaxSpin->setDecimals(3);
    m_yMaxSpin->setValue(10.0);
    m_yMaxSpin->setFixedWidth(80);
    m_yMaxSpin->setEnabled(false);
    row1Lay->addWidget(m_yMaxSpin);

    row1Lay->addStretch();
    m_frameLabel = new QLabel(QStringLiteral("帧#: 0"), row1);
    row1Lay->addWidget(m_frameLabel);
    topLayout->addWidget(row1);

    root->addWidget(topBar);

    // ---- QCustomPlot ----
    m_plot = new QCustomPlot(this);
    PlotWindowBase::applyConfiguredOpenGl(m_plot);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    initPlot();
    root->addWidget(m_plot, 1);

    // ---- 信号 ----
    connect(m_startRefBtn, &QPushButton::clicked, this, &PulseEddyPlotWindow::onStartReferenceClicked);
    connect(m_clearRefBtn, &QPushButton::clicked, this, &PulseEddyPlotWindow::onClearReferenceClicked);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_clearTimeMs = QDateTime::currentMSecsSinceEpoch();
        m_plot->graph(0)->data()->clear();
        m_plot->graph(1)->data()->clear();
        m_plot->graph(1)->setVisible(false);
        m_hasReference = false;
        m_lastFrameId = 0;
        updateReferenceStatus();
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    });
    connect(yGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked),
            this, [this](QAbstractButton* btn) {
                const bool manual = (btn == m_yManualRadio);
                m_yMinSpin->setEnabled(manual);
                m_yMaxSpin->setEnabled(manual);
                applyYAxisMode();
                m_plot->replot(QCustomPlot::rpQueuedReplot);
            });
    connect(m_yMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { if (m_yManualRadio->isChecked()) { applyYAxisMode(); m_plot->replot(QCustomPlot::rpQueuedReplot); } });
    connect(m_yMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { if (m_yManualRadio->isChecked()) { applyYAxisMode(); m_plot->replot(QCustomPlot::rpQueuedReplot); } });

    // 监听 SelectionState 以支持 Review 模式
    auto* sel = SelectionState::instance();
    connect(sel, &SelectionState::selectionChanged,
            this, &PulseEddyPlotWindow::onSelectionChanged);
    if (sel->hasRange()) {
        m_reviewStartMs = sel->startMs();
        m_reviewEndMs = sel->endMs();
        m_reviewMode = (sel->mode() == SelectionState::Review);
    }
}

PulseEddyPlotWindow::~PulseEddyPlotWindow() = default;

void PulseEddyPlotWindow::initPlot()
{
    m_plot->clearGraphs();

    // 脉冲曲线 (蓝)
    m_plot->addGraph();
    m_plot->graph(0)->setPen(QPen(QColor(40, 120, 220), 1));
    m_plot->graph(0)->setName(QStringLiteral("脉冲"));
    m_plot->graph(0)->setAntialiased(false);

    // 参考线 (红)
    m_plot->addGraph();
    m_plot->graph(1)->setPen(QPen(QColor(220, 60, 60), 1));
    m_plot->graph(1)->setName(QStringLiteral("参考线"));
    m_plot->graph(1)->setAntialiased(false);
    m_plot->graph(1)->setVisible(false);

    m_plot->xAxis->setLabel(QStringLiteral("采样点"));
    m_plot->yAxis->setLabel(QStringLiteral("幅值"));
    m_plot->legend->setVisible(true);
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    onThemeChanged();
}

void PulseEddyPlotWindow::onThemeChanged()
{
    applyThemeToPlot(m_plot, isDarkThemeActive());
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void PulseEddyPlotWindow::onDataUpdated(const QVector<FrameData>& frames)
{
    if (frames.isEmpty()) return;
    if (m_reviewMode) return; // Review 模式下不处理实时数据

    // 取最后一帧 PulseEddy
    const FrameData* latest = nullptr;
    for (int i = frames.size() - 1; i >= 0; --i) {
        if (frames[i].detectMode == FrameData::PulseEddy) {
            latest = &frames[i];
            break;
        }
    }
    if (!latest) return;
    if (latest->frameId <= m_lastFrameId) return;
    m_lastFrameId = latest->frameId;

    const FrameData& f = *latest;

    // 脉冲曲线
    const int nRaw = f.pulseRawValues.size();
    if (nRaw > 0) {
        QVector<double> x(nRaw);
        for (int i = 0; i < nRaw; ++i) x[i] = static_cast<double>(i);
        m_plot->graph(0)->setData(x, f.pulseRawValues, true);
    }

    // 参考线
    m_hasReference = f.hasPulseReference;
    if (m_hasReference && !f.pulseRefValues.isEmpty()) {
        const int nRef = f.pulseRefValues.size();
        QVector<double> x(nRef);
        for (int i = 0; i < nRef; ++i) x[i] = static_cast<double>(i);
        m_plot->graph(1)->setData(x, f.pulseRefValues, true);
        m_plot->graph(1)->setVisible(true);
    } else {
        m_plot->graph(1)->setVisible(false);
    }

    m_plot->xAxis->setRange(0, 65000);
    applyYAxisMode();
    m_plot->replot(QCustomPlot::rpQueuedReplot);

    updateReferenceStatus();
    m_frameLabel->setText(QStringLiteral("帧#: %1").arg(f.frameId));
}

void PulseEddyPlotWindow::onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& /*snapshot*/)
{
    // 脉冲涡流不通过 PlotSnapshot 渲染，直连 FrameData
}

void PulseEddyPlotWindow::onCriticalFrame(const FrameData& /*frame*/)
{
}

void PulseEddyPlotWindow::onStartReferenceClicked()
{
    // 通过 parent 链找到 MainWindow 发指令
    QWidget* w = parentWidget();
    while (w) {
        if (auto* mw = qobject_cast<MainWindow*>(w)) {
            if (auto* ctrl = mw->appController()) {
                ctrl->sendCommand(QStringLiteral("start_reference"), false);
            }
            return;
        }
        w = w->parentWidget();
    }
    qWarning() << "[PulseEddyPlotWindow] 未找到 MainWindow，无法发送参考线指令";
}

void PulseEddyPlotWindow::onClearReferenceClicked()
{
    QWidget* w = parentWidget();
    while (w) {
        if (auto* mw = qobject_cast<MainWindow*>(w)) {
            if (auto* ctrl = mw->appController()) {
                ctrl->sendCommand(QStringLiteral("clear_reference"), false);
            }
            return;
        }
        w = w->parentWidget();
    }
    qWarning() << "[PulseEddyPlotWindow] 未找到 MainWindow，无法发送清除参考线指令";
}

void PulseEddyPlotWindow::updateReferenceStatus()
{
    if (m_hasReference) {
        m_refStatusLabel->setText(QStringLiteral("参考线: ●已采集"));
        m_refStatusLabel->setStyleSheet("color: #cc3333; font-weight: bold;");
    } else {
        m_refStatusLabel->setText(QStringLiteral("参考线: 未采集"));
        m_refStatusLabel->setStyleSheet("color: #888;");
    }
}

void PulseEddyPlotWindow::applyYAxisMode()
{
    if (!m_plot) return;
    if (m_yManualRadio && m_yManualRadio->isChecked() && m_yMinSpin && m_yMaxSpin) {
        const double lo = m_yMinSpin->value();
        const double hi = m_yMaxSpin->value();
        if (hi > lo) {
            m_plot->yAxis->setRange(lo, hi);
            return;
        }
    }
    // 自适应：手动计算可见数据的非对称范围 (rescale 会产生对称范围)
    double yMin = std::numeric_limits<double>::max();
    double yMax = std::numeric_limits<double>::lowest();
    bool any = false;
    for (int i = 0; i < m_plot->graphCount(); ++i) {
        QCPGraph* g = m_plot->graph(i);
        if (!g || !g->visible()) continue;
        auto dataPtr = g->data();
        if (!dataPtr || dataPtr->isEmpty()) continue;
        for (auto it = dataPtr->constBegin(); it != dataPtr->constEnd(); ++it) {
            const double v = it->value;
            if (std::isfinite(v)) { yMin = qMin(yMin, v); yMax = qMax(yMax, v); any = true; }
        }
    }
    if (any && yMax > yMin) {
        const double margin = qMax((yMax - yMin) * 0.05, 0.001);
        m_plot->yAxis->setRange(yMin - margin, yMax + margin);
    } else {
        m_plot->yAxis->rescale(true);
    }
}

// ============================================================================
// Review / History 模式
// ============================================================================

void PulseEddyPlotWindow::onSelectionChanged(qint64 startMs, qint64 endMs, int mode)
{
    m_reviewStartMs = startMs;
    m_reviewEndMs = endMs;
    const bool nowReview = (mode == static_cast<int>(SelectionState::Review));

    if (nowReview) {
        m_reviewLoadCanceled.storeRelaxed(1); // 取消上一次仍在跑的异步加载
        m_reviewMode = true;
        loadReviewFromDb();
    } else {
        m_reviewLoadCanceled.storeRelaxed(1); // 取消正在运行的异步加载
        m_reviewMode = false;
        m_lastFrameId = 0;
    }
}

void PulseEddyPlotWindow::loadReviewFromDb()
{
    auto* hdp = HistoryDataProvider::instance();
    if (!hdp || !hdp->isDatabaseOpen()) return;

    const qint64 startMs = m_reviewStartMs;
    const qint64 endMs = m_reviewEndMs;
    const quint64 epoch = ++m_reviewEpoch;
    m_reviewLoadCanceled.storeRelaxed(0); // 新加载开始，清除取消标志

    QPointer<PulseEddyPlotWindow> self(this);
    QtConcurrent::run([self, startMs, endMs, epoch]() {
        // UUID 全局唯一连接名，多实例下不碰撞（修复 epoch 计数器非全局唯一的 bug）
        const QString connName = QStringLiteral("pe_review_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

        // RAII：任何 return 路径自动调用 removeDatabase，防止连接名泄漏
        struct DbGuard {
            QString name;
            ~DbGuard() { if (!name.isEmpty()) QSqlDatabase::removeDatabase(name); }
        } guard{connName};

        // 检查取消标志
        if (self && self->m_reviewLoadCanceled.loadRelaxed()) return;

        struct { QByteArray raw; QByteArray ref; bool hasRef; quint64 frameId; } result;
        bool found = false;

        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
            auto* h = HistoryDataProvider::instance();
            if (!h || !h->isDatabaseOpen()) return;
            db.setDatabaseName(h->currentDatabasePath());
            if (!db.open()) return;

            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "SELECT frame_index, raw_values, has_reference, reference_values "
                "FROM pulse_eddy_frames "
                "WHERE timestamp_unix_ms BETWEEN :s AND :e "
                "ORDER BY timestamp_unix_ms DESC LIMIT 1"));
            q.bindValue(":s", startMs);
            q.bindValue(":e", endMs);
            if (q.exec() && q.next()) {
                result.frameId = static_cast<quint64>(q.value(0).toLongLong());
                result.raw = q.value(1).toByteArray();
                result.hasRef = (q.value(2).toInt() != 0);
                result.ref = q.value(3).toByteArray();
                found = true;
            }
            db.close();
        } // db、q 析构，guard 析构自动 removeDatabase

        if (!found) return;
        if (!self || self->m_reviewEpoch != epoch || !self->m_reviewMode) return;

        // Build data vectors from BLOBs
        QVector<double> rawValues, refValues;
        if (!result.raw.isEmpty()) {
            const double* src = reinterpret_cast<const double*>(result.raw.constData());
            const int n = result.raw.size() / static_cast<int>(sizeof(double));
            rawValues.resize(n);
            for (int i = 0; i < n; ++i) rawValues[i] = src[i];
        }
        if (result.hasRef && !result.ref.isEmpty()) {
            const double* src = reinterpret_cast<const double*>(result.ref.constData());
            const int n = result.ref.size() / static_cast<int>(sizeof(double));
            refValues.resize(n);
            for (int i = 0; i < n; ++i) refValues[i] = src[i];
        }

        QMetaObject::invokeMethod(self, [self, rawValues, refValues, frameId = result.frameId, hasRef = result.hasRef]() {
            if (!self || !self->m_reviewMode) return;
            // Render on main thread
            if (!rawValues.isEmpty()) {
                const int nRaw = rawValues.size();
                QVector<double> x(nRaw);
                for (int i = 0; i < nRaw; ++i) x[i] = static_cast<double>(i);
                self->m_plot->graph(0)->setData(x, rawValues, true);
            }
            if (hasRef && !refValues.isEmpty()) {
                const int nRef = refValues.size();
                QVector<double> x(nRef);
                for (int i = 0; i < nRef; ++i) x[i] = static_cast<double>(i);
                self->m_plot->graph(1)->setData(x, refValues, true);
                self->m_plot->graph(1)->setVisible(true);
                self->m_hasReference = true;
            } else {
                self->m_plot->graph(1)->setVisible(false);
                self->m_hasReference = false;
            }
            self->m_plot->xAxis->setRange(0, rawValues.isEmpty() ? 65000 : rawValues.size());
            self->applyYAxisMode();
            self->m_plot->replot(QCustomPlot::rpQueuedReplot);
            self->updateReferenceStatus();
            self->m_frameLabel->setText(QStringLiteral("Review | 帧#: %1").arg(frameId));
        }, Qt::QueuedConnection);
    });
}
