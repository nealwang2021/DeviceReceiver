#ifndef PLOTWINDOWBASE_H
#define PLOTWINDOWBASE_H

#include <QWidget>
#include <QVector>
#include "FrameData.h"
#include "PlotDataHub.h"

class QCustomPlot;
class QEvent;

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

protected:
    void changeEvent(QEvent* event) override;
    bool isDarkThemeActive() const;
    void applyThemeToPlot(QCustomPlot* plot, bool dark) const;
    void applyThemeToAllPlots() const;
    virtual void onThemeChanged();
};

Q_DECLARE_METATYPE(PlotWindowBase*)

#endif // PLOTWINDOWBASE_H
