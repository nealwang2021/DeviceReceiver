#ifndef SELECTIONSTATE_H
#define SELECTIONSTATE_H

#include <QObject>

/**
 * 全局「选中时间范围 + 运行模式」状态。
 *
 * - Live：范围自动跟随数据尾部（阵列图/热力图沿用实时行为）。
 * - Review：范围由用户 brush 指定，不再被实时数据自动拉回。
 *
 * 所有窗口（阵列图、阵列热力图、历史总览）都以本单例为唯一时间源。
 */
class SelectionState : public QObject
{
    Q_OBJECT
public:
    enum Mode {
        Live = 0,
        Review = 1,
    };
    Q_ENUM(Mode)

    static SelectionState* instance();

    qint64 startMs() const { return m_startMs; }
    qint64 endMs() const { return m_endMs; }
    Mode mode() const { return m_mode; }

    /// 是否已有有效范围；初始未设置时为 false。
    bool hasRange() const { return m_hasRange; }

    /// 设置范围（不改变模式）；若 start >= end 则直接忽略。
    void setRange(qint64 startMs, qint64 endMs);

    /// 同时设置范围和模式。
    void setRangeAndMode(qint64 startMs, qint64 endMs, Mode mode);

    /// 切换模式。
    void setMode(Mode mode);

signals:
    void selectionChanged(qint64 startMs, qint64 endMs, int mode);

private:
    explicit SelectionState(QObject* parent = nullptr) : QObject(parent) {}

    qint64 m_startMs = 0;
    qint64 m_endMs = 0;
    Mode m_mode = Live;
    bool m_hasRange = false;

    static SelectionState* s_instance;
};

#endif // SELECTIONSTATE_H
