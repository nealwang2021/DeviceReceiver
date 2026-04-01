#ifndef PLOTWINDOWBASE_H
#define PLOTWINDOWBASE_H

#include <QWidget>
#include <QVector>
#include "FrameData.h"
#include "PlotDataHub.h"

class PlotWindowBase : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWindowBase(QWidget* parent = nullptr) : QWidget(parent) {}
    ~PlotWindowBase() override = default;

public slots:
    virtual void onDataUpdated(const QVector<FrameData>& frames) = 0;
    virtual void onCriticalFrame(const FrameData& frame) = 0;
    virtual void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) = 0;
};

Q_DECLARE_METATYPE(PlotWindowBase*)

#endif // PLOTWINDOWBASE_H
