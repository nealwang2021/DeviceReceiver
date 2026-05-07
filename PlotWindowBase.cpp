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

void PlotWindowBase::applyConfiguredOpenGl(QCustomPlot* plot)
{
	if (!plot) {
		return;
	}
#ifdef QCUSTOMPLOT_USE_OPENGL
	bool enabled = true;
	if (auto* cfg = AppConfig::instance()) {
		enabled = cfg->qcustomPlotOpenGlEnabled();
	}
	if (plot->openGl() != enabled) {
		plot->setOpenGl(enabled);
	}
#endif
	// 性能档对软渲染同样生效；即便编译期未开 OpenGL 也要应用。
	applyConfiguredPerformance(plot);
}

void PlotWindowBase::applyConfiguredOpenGlToTree(QWidget* root)
{
	if (!root) {
		return;
	}
	const auto plots = root->findChildren<QCustomPlot*>();
	for (QCustomPlot* plot : plots) {
		applyConfiguredOpenGl(plot);
		// 切换 OpenGL / 性能档后内部 paint buffer 与 hint 标记需重绘才能生效。
		plot->replot(QCustomPlot::rpQueuedReplot);
	}
}

void PlotWindowBase::applyConfiguredPerformance(QCustomPlot* plot)
{
	if (!plot) {
		return;
	}
	// 默认按「OpenGL 启用 = Quality 档」推断；若 AppConfig 不可用，按 Quality 处理。
	bool quality = true;
	if (auto* cfg = AppConfig::instance()) {
		quality = cfg->qcustomPlotOpenGlEnabled();
	}

	if (quality) {
		// 恢复 QCustomPlot 默认高画质：清掉 perf 档遗留的标志。
		plot->setNoAntialiasingOnDrag(false);
		plot->setPlottingHints(QCP::phCacheLabels);
	} else {
		// 软渲染性能档：拖拽期不抗锯齿 + 直线段批量快路径。
		plot->setNoAntialiasingOnDrag(true);
		plot->setPlottingHints(QCP::phFastPolylines | QCP::phCacheLabels);
	}

	// 对每条曲线切换抗锯齿；ColorMap 等其它 plottable 不改，保留原视觉。
	const int count = plot->plottableCount();
	for (int i = 0; i < count; ++i) {
		if (auto* graph = qobject_cast<QCPGraph*>(plot->plottable(i))) {
			graph->setAntialiased(quality);
		}
	}
}

void PlotWindowBase::applyConfiguredPerformanceToTree(QWidget* root)
{
	if (!root) {
		return;
	}
	const auto plots = root->findChildren<QCustomPlot*>();
	for (QCustomPlot* plot : plots) {
		applyConfiguredPerformance(plot);
	}
}
