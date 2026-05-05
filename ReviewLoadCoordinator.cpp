#include "ReviewLoadCoordinator.h"

#include <QCoreApplication>

ReviewLoadCoordinator* ReviewLoadCoordinator::s_instance = nullptr;

ReviewLoadCoordinator* ReviewLoadCoordinator::instance()
{
    if (!s_instance) {
        // parent 设为 qApp，确保程序退出时由 Qt 自动析构。
        s_instance = new ReviewLoadCoordinator(qApp);
    }
    return s_instance;
}

ReviewLoadCoordinator::ReviewLoadCoordinator(QObject* parent)
    : QObject(parent)
{
}

void ReviewLoadCoordinator::beginTask(const QString& taskKey, const QString& descriptor)
{
    if (taskKey.isEmpty()) {
        return;
    }
    const bool wasBusy = (m_total > 0);
    m_taskCounts[taskKey] += 1;
    if (!descriptor.isEmpty()) {
        m_taskDescriptors[taskKey] = descriptor;
    }
    m_total += 1;
    if (!wasBusy) {
        emit busyChanged(true, activeDescriptors());
    } else {
        // 已经 busy，但描述列表可能变化（新增任务）。可选广播。
        emit busyChanged(true, activeDescriptors());
    }
}

void ReviewLoadCoordinator::endTask(const QString& taskKey)
{
    if (taskKey.isEmpty()) {
        return;
    }
    auto it = m_taskCounts.find(taskKey);
    if (it == m_taskCounts.end()) {
        return; // 防御：endTask 多调一次不应抛异常
    }
    if (--it.value() <= 0) {
        m_taskCounts.erase(it);
        m_taskDescriptors.remove(taskKey);
    }
    if (m_total > 0) {
        m_total -= 1;
    }
    if (m_total == 0) {
        emit busyChanged(false, QStringList{});
    } else {
        emit busyChanged(true, activeDescriptors());
    }
}

QStringList ReviewLoadCoordinator::activeDescriptors() const
{
    QStringList out;
    out.reserve(m_taskDescriptors.size());
    for (auto it = m_taskDescriptors.cbegin(); it != m_taskDescriptors.cend(); ++it) {
        out.append(it.value());
    }
    return out;
}
