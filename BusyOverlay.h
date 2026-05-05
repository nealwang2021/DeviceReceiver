#ifndef BUSYOVERLAY_H
#define BUSYOVERLAY_H

#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

/**
 * 半透明"加载中"蒙版控件。
 *
 * - 作为 host 控件的子 widget；通过 eventFilter 监听 host 的 Resize/Move/Show/Hide
 *   自动同步覆盖区域。
 * - 鼠标事件透传（WA_TransparentForMouseEvents），不阻断 host 的交互——
 *   review 加载期间用户依然可以重新拖动选择框，因为：
 *     1. 多次拖动会触发 epoch 自增，旧 worker 结果会被丢弃；
 *     2. SQLite WAL 支持多读者并发，重复发起只读查询不会影响实时写入。
 * - paint：80% 黑底 + 居中旋转环 + 单行文本。
 *
 * 用法：
 *   auto* overlay = new BusyOverlay(plotWidget);
 *   overlay->setMessage("加载历史中...");
 *   overlay->showOverlay();   // 通常由 ReviewLoadCoordinator::busyChanged 驱动
 *   overlay->hideOverlay();
 */
class BusyOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit BusyOverlay(QWidget* host);
    ~BusyOverlay() override;

    void setMessage(const QString& message);
    void showOverlay();
    void hideOverlay();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void syncToHost();

    QPointer<QWidget> m_host;
    QString m_message;
    QTimer m_animTimer;
    int m_angleDeg{0};
};

#endif // BUSYOVERLAY_H
