#include "PlotWindowBase.h"

#include "AppConfig.h"
#include "qcustomplot.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QPen>
#include <QTimer>

// PlotWindowBase 是抽象基类，所有实现在头文件中进行
// 此文件存在以确保 Qt moc 能够处理 Q_OBJECT 宏

void PlotWindowBase::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);
	if (!event) {
		return;
	}

	switch (event->type()) {
	case QEvent::PaletteChange:
	case QEvent::ApplicationPaletteChange:
	case QEvent::StyleChange:
		QTimer::singleShot(0, this, [this]() {
			onThemeChanged();
		});
		break;
	default:
		break;
	}
}

bool PlotWindowBase::isDarkThemeActive() const
{
	if (AppConfig* cfg = AppConfig::instance()) {
		return cfg->currentStyle() == AppConfig::DarkStyle;
	}

	const QColor win = palette().window().color();
	return win.lightness() < 128;
}

void PlotWindowBase::applyThemeToPlot(QCustomPlot* plot, bool dark) const
{
	if (!plot) {
		return;
	}

	const QColor canvasBg = dark ? QColor(28, 28, 28) : QColor(255, 255, 255);
	const QColor rectBg = dark ? QColor(33, 36, 40) : QColor(255, 255, 255);
	const QColor axisColor = dark ? QColor(152, 162, 176) : QColor(138, 148, 160);
	const QColor labelColor = dark ? QColor(222, 228, 236) : QColor(50, 58, 70);
	const QColor tickColor = dark ? QColor(200, 208, 220) : QColor(70, 78, 90);
	const QColor gridMajor = dark ? QColor(74, 82, 94) : QColor(210, 220, 232);
	const QColor gridMinor = dark ? QColor(58, 64, 74) : QColor(198, 208, 220);

	plot->setBackground(QBrush(canvasBg));

	for (int i = 0; i < plot->axisRectCount(); ++i) {
		QCPAxisRect* rect = plot->axisRect(i);
		if (!rect) {
			continue;
		}
		rect->setBackground(QBrush(rectBg));

		const auto axes = rect->axes();
		for (QCPAxis* axis : axes) {
			if (!axis) {
				continue;
			}
			axis->setBasePen(QPen(axisColor, 1));
			axis->setTickPen(QPen(axisColor, 1));
			axis->setSubTickPen(QPen(axisColor, 1));
			axis->setLabelColor(labelColor);
			axis->setTickLabelColor(tickColor);
			if (axis->grid()) {
				axis->grid()->setVisible(true);
				axis->grid()->setPen(QPen(gridMajor, 1, Qt::DotLine));
				axis->grid()->setSubGridPen(QPen(gridMinor, 1, Qt::DotLine));
				axis->grid()->setSubGridVisible(true);
			}
		}
	}

	if (plot->legend) {
		plot->legend->setBrush(QBrush(dark ? QColor(42, 46, 52, 220) : QColor(255, 255, 255, 220)));
		plot->legend->setBorderPen(QPen(axisColor, 1));
		plot->legend->setTextColor(labelColor);
	}
}

void PlotWindowBase::applyThemeToAllPlots() const
{
	const bool dark = isDarkThemeActive();
	const auto plots = findChildren<QCustomPlot*>();
	for (QCustomPlot* plot : plots) {
		applyThemeToPlot(plot, dark);
		plot->replot(QCustomPlot::rpQueuedReplot);
	}
}

void PlotWindowBase::onThemeChanged()
{
	applyThemeToAllPlots();
}
