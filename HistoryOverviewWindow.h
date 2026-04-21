#ifndef HISTORYOVERVIEWWINDOW_H
#define HISTORYOVERVIEWWINDOW_H

#include <QWidget>
#include <QVector>
#include <QPointer>

#include "SqlHistoryQuery.h"

class QCustomPlot;
class QCPItemRect;
class QCPItemLine;
class QCPItemStraightLine;
class QCPGraph;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

/**
 * 历史总览窗口：
 * - 上部：整个会话 DB 的所有通道 min/max 包络
 * - 下部：可拖动/可拉伸的时间范围框（brush）
 * - 变动会广播到 `SelectionState`，阵列图/阵列热力图据此联动
 *
 * 被放置在 MainWindow 底部 dock，与「数据监控」tab 共置。
 */
class HistoryOverviewWindow : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryOverviewWindow(QWidget* parent = nullptr);
    ~HistoryOverviewWindow() override;

public slots:
    /// 刷新总览。fullEnvelope=true 时执行全时间跨度 SQL 包络（成本高）；false 仅更新时间边界。
    void refreshOverview(bool fullEnvelope = true);

    /// 回到实时（把选区对齐到最新尾部）
    void jumpToLive();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onComponentChanged(int index);
    void onDatabaseOpened(const QString& path);
    void onDatabaseClosed();
    void onSelectionChanged(qint64 startMs, qint64 endMs, int mode);
    void onOpenOfflineDatabaseClicked();
    void onReopenSessionDatabaseClicked();

private:
    enum class DragMode {
        None,
        Creating,
        Moving,
        ResizingLeft,
        ResizingRight,
    };

    bool ensureDatabaseAndBounds();
    void liveLightweightTick();
    /// 合并多次「需要刷新」为一次 queued 调用（避免 ctor 与 databaseOpened 双刷新）。
    void scheduleRefreshOverview();
    /// Live 下将选区右对齐到数据尾部；force 时始终写入 SelectionState，否则仅在变化超过阈值时写入。
    void syncLiveTailToSelectionState(bool forceCommit);
    void scheduleEnvelopeRebuild();
    void updateRefreshButtonToolTip();
    void rebuildEnvelope();
    void applyRangeToItems();
    void updateStatusLabels();
    void commitRangeToSelectionState(bool asReview);
    DragMode hitTest(double timeX) const;
    void snapRangeToBounds();
    qint64 timeBoundsSpanMs() const { return m_dataMaxMs - m_dataMinMs; }
    /// 总览包络 / X 轴使用的查询起始时间（会话实时 + Live 时为最近 1h 左端）。
    qint64 plotEnvelopeStartMs() const;
    qint64 plotEnvelopeEndMs() const;
    qint64 plotEnvelopeSpanMs() const;

    QCustomPlot* m_plot = nullptr;
    QCPGraph* m_maxGraph = nullptr;
    QCPGraph* m_minGraph = nullptr;
    QCPItemRect* m_rangeRect = nullptr;
    QCPItemStraightLine* m_leftHandle = nullptr;
    QCPItemStraightLine* m_rightHandle = nullptr;

    QComboBox* m_componentCombo = nullptr;
    QPushButton* m_backToLiveButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_openOfflineDbButton = nullptr;
    QPushButton* m_reopenSessionDbButton = nullptr;
    QLabel* m_dbLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_boundsLabel = nullptr;

    SqlHistoryQuery::Component m_component = SqlHistoryQuery::Component::Amplitude;

    qint64 m_dataMinMs = 0;
    qint64 m_dataMaxMs = 0;
    bool m_hasBounds = false;

    qint64 m_selStartMs = 0;
    qint64 m_selEndMs = 0;

    DragMode m_dragMode = DragMode::None;
    double m_dragAnchorTime = 0.0;
    qint64 m_dragAnchorStart = 0;
    qint64 m_dragAnchorEnd = 0;

    QTimer* m_liveRefreshTimer = nullptr;
    /// 已排队一次 refreshOverview(true)，避免同周期重复 SQL。
    bool m_refreshOverviewQueued = false;
    /// 包络重算防抖：同一事件循环内合并多次触发。
    bool m_envelopeRebuildPending = false;
    /// 会话模式下当前打开的库路径与记录器 sessionDatabasePath 不一致（data 目录兜底）。
    bool m_dbFallbackMismatchSession = false;

    /// 上次执行全时间跨度包络 SQL 的墙钟时间（ms）；0 表示尚未做过全量包络。
    qint64 m_lastFullEnvelopeWallMs = 0;

    /// 用于检测 Live/Review 切换以重算包络时间窗（1h vs 全库）。
    int m_prevSelModeForEnvelope = 0;
};

#endif // HISTORYOVERVIEWWINDOW_H
