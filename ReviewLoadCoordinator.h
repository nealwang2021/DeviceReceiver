#ifndef REVIEWLOADCOORDINATOR_H
#define REVIEWLOADCOORDINATOR_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

/**
 * Review 加载任务全局协调器（单例）。
 *
 * 设计目标：
 *  - 让所有"耗时的历史范围预览查询"（阵列图 envelope、阵列热力图原始行）共享同一套
 *    "正在加载"指示，而不是每个窗口各搞一套且互相不知道。
 *  - 只做引用计数与状态广播，不参与任务调度本身（任务调度仍在各窗口内 QtConcurrent::run）。
 *
 * 线程安全：API 仅允许在创建实例的线程（默认主线程）调用。worker 线程完成后
 * 必须通过 QMetaObject::invokeMethod 切回主线程再调用 endTask。
 */
class ReviewLoadCoordinator : public QObject
{
    Q_OBJECT
public:
    static ReviewLoadCoordinator* instance();

    /// 登记一个新任务。taskKey 必须唯一（建议：模块名 + epoch），descriptor 是给用户看的文本。
    /// 同一个 taskKey 重复 begin 会自增计数，需要相同次数的 endTask 才会完全清零（防御写法）。
    void beginTask(const QString& taskKey, const QString& descriptor);

    /// 任务完成（无论成功/失败/被丢弃，都必须调用，避免 overlay 卡死）。
    void endTask(const QString& taskKey);

    bool busy() const { return m_total > 0; }
    QStringList activeDescriptors() const;

signals:
    /// 任务总数 0↔>0 切换时触发；descriptors 反映当前所有活跃任务的描述文本。
    void busyChanged(bool busy, QStringList descriptors);

private:
    explicit ReviewLoadCoordinator(QObject* parent = nullptr);

    QHash<QString, int> m_taskCounts;
    QHash<QString, QString> m_taskDescriptors;
    int m_total{0};

    static ReviewLoadCoordinator* s_instance;
};

#endif // REVIEWLOADCOORDINATOR_H
