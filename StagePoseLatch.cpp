#include "StagePoseLatch.h"
#include <QDebug>

void StagePoseLatch::update(double xMm, double yMm, double zMm, int xPulse, int yPulse, int zPulse, qint64 unixMs)
{
    {
        QMutexLocker locker(&m_mutex);
        m_valid = true;
        m_unixMs = unixMs;
        m_xMm = xMm;
        m_yMm = yMm;
        m_zMm = zMm;
        m_xPulse = xPulse;
        m_yPulse = yPulse;
        m_zPulse = zPulse;
    }
    // 诊断日志：每 50 次打一次
    static int cnt = 0;
    if (++cnt % 50 == 1) {
        qInfo() << "[StagePoseLatch] update #" << cnt
                << "XY=(" << xMm << "," << yMm << ") Z=" << zMm;
    }
}

void StagePoseLatch::applyToFrame(FrameData& frame) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_valid) {
        frame.hasStagePose = false;
        return;
    }
    frame.hasStagePose = true;
    frame.stageTimestampMs = m_unixMs;
    frame.stageXMm = m_xMm;
    frame.stageYMm = m_yMm;
    frame.stageZMm = m_zMm;
    frame.stageXPulse = m_xPulse;
    frame.stageYPulse = m_yPulse;
    frame.stageZPulse = m_zPulse;
}

void StagePoseLatch::clear()
{
    QMutexLocker locker(&m_mutex);
    m_valid = false;
}
