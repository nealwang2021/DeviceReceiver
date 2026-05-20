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
    reserveMatrixTail(next->mfImpedanceReal, frames.size());
    reserveMatrixTail(next->mfImpedanceImag, frames.size());
    reserveMatrixTail(next->mfImpedanceMag, frames.size());
    reserveMatrixTail(next->mfImpedancePhase, frames.size());
    reserveMatrixTail(next->mfNormImpedanceReal, frames.size());
    reserveMatrixTail(next->mfNormImpedanceImag, frames.size());

    for (const FrameData& frame : frames) {
        if (frame.detectMode == FrameData::Legacy) {
            continue;
        }

        if (frame.detectMode == FrameData::MultiFreqEddy) {
            const int nPoints = frame.mfFreqPoints.size();
            if (nPoints <= 0) {
                continue;
            }
            const bool modeChanged = (next->mode != FrameData::MultiFreqEddy);
            const bool freqCountChanged = (next->mfFreqPointCount != nPoints);
            if (modeChanged || freqCountChanged || next->timeMs.isEmpty()) {
                next->mode = FrameData::MultiFreqEddy;
                next->channelCount = 0;
                next->mfFreqPointCount = nPoints;
                next->timeMs.clear();
                next->mfFreqFactors.resize(nPoints);
                next->mfFreqHz.resize(nPoints);
                next->mfImpedanceReal.resize(nPoints);
                next->mfImpedanceImag.resize(nPoints);
                next->mfImpedanceMag.resize(nPoints);
                next->mfImpedancePhase.resize(nPoints);
                next->mfNormImpedanceReal.resize(nPoints);
                next->mfNormImpedanceImag.resize(nPoints);
                reserveMatrixTail(next->mfImpedanceReal, frames.size());
                reserveMatrixTail(next->mfImpedanceImag, frames.size());
                reserveMatrixTail(next->mfImpedanceMag, frames.size());
                reserveMatrixTail(next->mfImpedancePhase, frames.size());
                reserveMatrixTail(next->mfNormImpedanceReal, frames.size());
                reserveMatrixTail(next->mfNormImpedanceImag, frames.size());
            }

            const double t = static_cast<double>(frame.timestamp);
            next->timeMs.append(t);

            for (int i = 0; i < nPoints; ++i) {
                const MultiFreqPointResult& pt = frame.mfFreqPoints[i];
                next->mfFreqFactors[i] = pt.frequencyFactor;
                next->mfFreqHz[i] = pt.frequencyHz;
                next->mfImpedanceReal[i].append(pt.impedanceReal_raw);
                next->mfImpedanceImag[i].append(pt.impedanceImag_raw);
                next->mfImpedanceMag[i].append(pt.impedanceMagnitude);
                next->mfImpedancePhase[i].append(pt.impedancePhaseDeg);
                next->mfNormImpedanceReal[i].append(pt.normalizedImpedanceReal);
                next->mfNormImpedanceImag[i].append(pt.normalizedImpedanceImag);
            }
            continue; // MultiFreqEddy handled, skip per-channel logic below
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
            next->mfFreqPointCount = 0;
            next->mfFreqFactors.clear();
            next->mfFreqHz.clear();
            next->mfImpedanceReal.clear();
            next->mfImpedanceImag.clear();
            next->mfImpedanceMag.clear();
            next->mfImpedancePhase.clear();
            next->mfNormImpedanceReal.clear();
            next->mfNormImpedanceImag.clear();
            next->mfImpedancePhase.clear();

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
                // Prefer raw protocol semantics in complex mode:
                // amp/phase/x/y -> magnitude/phase/real/imag.
                const double magRaw = (i < frame.channels_amp.size()) ? frame.channels_amp.at(i) : qQNaN();
                const double phaseRaw = (i < frame.channels_phase.size()) ? frame.channels_phase.at(i) : qQNaN();
                const double mag = std::isfinite(magRaw) ? magRaw : std::hypot(re, im);
                const double phase = std::isfinite(phaseRaw) ? phaseRaw : std::atan2(im, re);
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
    trimFrontMatrix(next->mfImpedanceReal, m_maxPoints);
    trimFrontMatrix(next->mfImpedanceImag, m_maxPoints);
    trimFrontMatrix(next->mfImpedanceMag, m_maxPoints);
    trimFrontMatrix(next->mfImpedancePhase, m_maxPoints);
    trimFrontMatrix(next->mfNormImpedanceReal, m_maxPoints);
    trimFrontMatrix(next->mfNormImpedanceImag, m_maxPoints);

    next->version += 1;
    m_snapshot = next;
    return m_snapshot;
}

QSharedPointer<const PlotSnapshot> PlotDataHub::snapshot() const
{
    QReadLocker locker(&m_lock);
    return m_snapshot;
}
