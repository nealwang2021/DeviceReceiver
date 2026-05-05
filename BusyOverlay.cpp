#include "BusyOverlay.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>

namespace {
constexpr int kSpinnerDiameter = 56;       // 圆环外径
constexpr int kSpinnerThickness = 6;       // 环宽
constexpr int kArcSpanDeg = 110;           // 旋转弧的角度跨度
constexpr int kAnimIntervalMs = 60;        // 动画刷新间隔
constexpr int kAnimStepDeg = 18;           // 每帧旋转角度
constexpr int kTextGap = 14;               // spinner 与文本间距
} // namespace

BusyOverlay::BusyOverlay(QWidget* host)
    : QWidget(host)
    , m_host(host)
    , m_message(QStringLiteral("正在加载..."))
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setVisible(false);

    if (m_host) {
        m_host->installEventFilter(this);
    }

    m_animTimer.setInterval(kAnimIntervalMs);
    connect(&m_animTimer, &QTimer::timeout, this, [this]() {
        m_angleDeg = (m_angleDeg + kAnimStepDeg) % 360;
        update();
    });

    syncToHost();
}

BusyOverlay::~BusyOverlay() = default;

void BusyOverlay::setMessage(const QString& message)
{
    if (m_message == message) {
        return;
    }
    m_message = message;
    if (isVisible()) {
        update();
    }
}

void BusyOverlay::showOverlay()
{
    syncToHost();
    if (!isVisible()) {
        show();
        raise();
    }
    if (!m_animTimer.isActive()) {
        m_animTimer.start();
    }
}

void BusyOverlay::hideOverlay()
{
    if (m_animTimer.isActive()) {
        m_animTimer.stop();
    }
    if (isVisible()) {
        hide();
    }
}

bool BusyOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_host.data()) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::ChildAdded:
        case QEvent::LayoutRequest:
            syncToHost();
            if (isVisible()) {
                raise();
            }
            break;
        case QEvent::Hide:
            hide();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BusyOverlay::syncToHost()
{
    if (!m_host) {
        return;
    }
    setGeometry(m_host->rect());
}

void BusyOverlay::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 80% 黑色蒙版
    painter.fillRect(rect(), QColor(0, 0, 0, 200));

    const QPointF center = QRectF(rect()).center();
    const QRectF spinnerRect(center.x() - kSpinnerDiameter / 2.0,
                             center.y() - kSpinnerDiameter / 2.0 - 12.0,
                             kSpinnerDiameter,
                             kSpinnerDiameter);

    // 环底（弱光圈）
    QPen baseRing(QColor(255, 255, 255, 60), kSpinnerThickness);
    baseRing.setCapStyle(Qt::FlatCap);
    painter.setPen(baseRing);
    painter.drawArc(spinnerRect, 0, 360 * 16);

    // 旋转弧（亮色）
    QPen arcPen(QColor(70, 200, 255, 230), kSpinnerThickness);
    arcPen.setCapStyle(Qt::RoundCap);
    painter.setPen(arcPen);
    const int startAngleSixteenths = (90 - m_angleDeg) * 16;
    painter.drawArc(spinnerRect, startAngleSixteenths, -kArcSpanDeg * 16);

    // 文本
    if (!m_message.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(qMax(9.0, f.pointSizeF() + 1.0));
        painter.setFont(f);
        painter.setPen(QPen(QColor(245, 245, 245)));
        QFontMetrics fm(f);
        const int textY = static_cast<int>(spinnerRect.bottom() + kTextGap + fm.ascent());
        const int textX = static_cast<int>(center.x() - fm.horizontalAdvance(m_message) / 2.0);
        painter.drawText(textX, textY, m_message);
    }
}
