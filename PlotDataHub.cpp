#include "PlotDataHub.h"
#include <QtGlobal>
#include <cmath>

PlotDataHub* PlotDataHub::s_instance = nullptr;

namespace {
static void trimFront(QVector<double>& vec, int maxPoints)
{
    if (maxPoints <= 0 || vec.size() <= maxPoints) {
        return;
    }
    vec.remove(0, vec.size() - maxPoints);
}

static void trimFrontMatrix(QVector<QVector<double>>& matrix, int maxPoints)
{
    for (QVector<double>& row : matrix) {
        trimFront(row, maxPoints);
    }
}

static void reserveMatrixTail(QVector<QVector<double>>& matrix, int extraPoints)
{
    if (extraPoints <= 0) {
        return;
    }
    for (QVector<double>& row : matrix) {
        row.reserve(row.size() + extraPoints);
    }
}
} // namespace

PlotDataHub* PlotDataHub::instance()
{
    if (!s_instance) {
        s_instance = new PlotDataHub();
    }
    return s_instance;
}

void PlotDataHub::reset()
{
    QWriteLocker locker(&m_lock);
    m_snapshot.reset();
}

void PlotDataHub::setMaxPoints(int maxPoints)
{
    QWriteLocker locker(&m_lock);
    m_maxPoints = qMax(200, maxPoints);
}

QSharedPointer<const PlotSnapshot> PlotDataHub::appendFrames(const QVector<FrameData>& frames)
{
    if (frames.isEmpty()) {
        return snapshot();
    }

    QWriteLocker locker(&m_lock);
    QSharedPointer<PlotSnapshot> next =
        m_snapshot ? QSharedPointer<PlotSnapshot>::create(*m_snapshot)
                   : QSharedPointer<PlotSnapshot>::create();
    next->timeMs.reserve(next->timeMs.size() + frames.size());
    reserveMatrixTail(next->realAmp, frames.size());
    reserveMatrixTail(next->complexReal, frames.size());
    reserveMatrixTail(next->complexImag, frames.size());
    reserveMatrixTail(next->complexMag, frames.size());
    reserveMatrixTail(next->complexPhase, frames.size());

    for (const FrameData& frame : frames) {
        if (frame.detectMode == FrameData::Legacy) {
            continue;
        }

        const int ch = qBound(0, static_cast<int>(frame.channelCount), 200);
        if (ch <= 0) {
            continue;
        }

        const bool modeChanged = (next->mode != frame.detectMode);
        const bool chChanged = (next->channelCount != ch);
        if (modeChanged || chChanged || next->timeMs.isEmpty()) {
            next->mode = frame.detectMode;
            next->channelCount = ch;
            next->timeMs.clear();
            next->realAmp.clear();
            next->complexReal.clear();
            next->complexImag.clear();
            next->complexMag.clear();
            next->complexPhase.clear();
            next->rowDisplayIndex.clear();
            next->rowSourceChannel.clear();

            if (frame.detectMode == FrameData::MultiChannelReal) {
                next->realAmp.resize(ch);
                reserveMatrixTail(next->realAmp, frames.size());
            } else if (frame.detectMode == FrameData::MultiChannelComplex) {
                next->complexReal.resize(ch);
                next->complexImag.resize(ch);
                next->complexMag.resize(ch);
                next->complexPhase.resize(ch);
                reserveMatrixTail(next->complexReal, frames.size());
                reserveMatrixTail(next->complexImag, frames.size());
                reserveMatrixTail(next->complexMag, frames.size());
                reserveMatrixTail(next->complexPhase, frames.size());
            }

            next->rowDisplayIndex.resize(ch);
            next->rowSourceChannel.resize(ch);
            for (int i = 0; i < ch; ++i) {
                next->rowDisplayIndex[i] = i;
                next->rowSourceChannel[i] = i;
            }
        }

        const double t = static_cast<double>(frame.timestamp);
        next->timeMs.append(t);

        for (int i = 0; i < ch; ++i) {
            if (i < frame.channels_display_index.size()) {
                next->rowDisplayIndex[i] = frame.channels_display_index.at(i);
            }
            if (i < frame.channels_source_channel.size()) {
                next->rowSourceChannel[i] = frame.channels_source_channel.at(i);
            }
        }

        if (frame.detectMode == FrameData::MultiChannelReal) {
            for (int i = 0; i < ch; ++i) {
                const double amp = (i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN();
                next->realAmp[i].append(amp);
            }
        } else if (frame.detectMode == FrameData::MultiChannelComplex) {
            for (int i = 0; i < ch; ++i) {
                const double re = (i < frame.channels_comp0.size()) ? frame.channels_comp0.at(i) : qQNaN();
                const double im = (i < frame.channels_comp1.size()) ? frame.channels_comp1.at(i) : qQNaN();
                const double mag = std::hypot(re, im);
                const double phase = std::atan2(im, re);
                next->complexReal[i].append(re);
                next->complexImag[i].append(im);
                next->complexMag[i].append(mag);
                next->complexPhase[i].append(phase);
            }
        }
    }

    trimFront(next->timeMs, m_maxPoints);
    trimFrontMatrix(next->realAmp, m_maxPoints);
    trimFrontMatrix(next->complexReal, m_maxPoints);
    trimFrontMatrix(next->complexImag, m_maxPoints);
    trimFrontMatrix(next->complexMag, m_maxPoints);
    trimFrontMatrix(next->complexPhase, m_maxPoints);

    next->version += 1;
    m_snapshot = next;
    return m_snapshot;
}

QSharedPointer<const PlotSnapshot> PlotDataHub::snapshot() const
{
    QReadLocker locker(&m_lock);
    return m_snapshot;
}
