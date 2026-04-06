#ifndef PLOTDATAHUB_H
#define PLOTDATAHUB_H

#include <QReadWriteLock>
#include <QSharedPointer>
#include <QVector>
#include "FrameData.h"

struct PlotSnapshot
{
    quint64 version = 0;
    FrameData::DetectionMode mode = FrameData::Legacy;
    int channelCount = 0;
    QVector<double> timeMs;

    // Real mode data (amplitude only).
    QVector<QVector<double>> realAmp;

    // Complex mode data.
    QVector<QVector<double>> complexReal;
    QVector<QVector<double>> complexImag;
    QVector<QVector<double>> complexMag;
    QVector<QVector<double>> complexPhase;

    // Optional row identity metadata for UI labels.
    QVector<int> rowDisplayIndex;
    QVector<int> rowSourceChannel;
};

class PlotDataHub
{
public:
    static PlotDataHub* instance();

    void reset();
    void setMaxPoints(int maxPoints);
    QSharedPointer<const PlotSnapshot> appendFrames(const QVector<FrameData>& frames);
    QSharedPointer<const PlotSnapshot> snapshot() const;

private:
    PlotDataHub() = default;

    static PlotDataHub* s_instance;
    mutable QReadWriteLock m_lock;
    QSharedPointer<PlotSnapshot> m_snapshot;
    int m_maxPoints = 1000;
};

#endif // PLOTDATAHUB_H
