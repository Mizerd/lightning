#include "MouseNavigationFilter.h"

#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>

namespace lightning {

MouseNavigationFilter::Direction
MouseNavigationFilter::directionFor(Qt::MouseButton button)
{
    switch (button) {
    case Qt::BackButton:     // == Qt::ExtraButton1 (XButton1, BTN_SIDE)
    case Qt::ExtraButton4:   // BTN_BACK on QtWayland
        return Direction::Back;
    case Qt::ForwardButton:  // == Qt::ExtraButton2 (XButton2, BTN_EXTRA)
    case Qt::ExtraButton3:   // BTN_FORWARD on QtWayland
        return Direction::Forward;
    default:
        return Direction::None;
    }
}

bool MouseNavigationFilter::windowHasOpenPopup(const QQuickWindow *window)
{
    if (!window || !window->contentItem())
        return false;
    const auto top = window->contentItem()->childItems();
    for (QQuickItem *child : top) {
        if (!child->inherits("QQuickOverlay"))
            continue;
        // As AppController::dismissPopupForEscape(): a popup's item is a
        // child of the overlay and its QObject parent is the QQuickPopup.
        const auto items = child->childItems();
        for (QQuickItem *item : items) {
            QObject *popup = item->parent();
            if (item->isVisible() && popup && popup->inherits("QQuickPopup")
                && !popup->inherits("QQuickToolTip")
                && popup->property("visible").toBool())
                return true;
        }
        return false;
    }
    return false;
}

MouseNavigationFilter::MouseNavigationFilter(QQuickWindow *window,
                                             Allowed allowed,
                                             Navigate navigate,
                                             QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_allowed(std::move(allowed))
    , m_navigate(std::move(navigate))
{
    if (m_window)
        m_window->installEventFilter(this);
}

MouseNavigationFilter::~MouseNavigationFilter()
{
    if (m_window)
        m_window->removeEventFilter(this);
}

QQuickWindow *MouseNavigationFilter::window() const
{
    return m_window;
}

bool MouseNavigationFilter::mayNavigate() const
{
    return !windowHasOpenPopup(m_window) && (!m_allowed || m_allowed());
}

bool MouseNavigationFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window)
        return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    const Direction direction = directionFor(mouse->button());
    if (direction == Direction::None) {
        // Another button going down while Back/Forward is held makes it a
        // chord, not a tap: as browsers do, it navigates nowhere.
        if (type == QEvent::MouseButtonPress)
            m_pressed = Direction::None;
        return false;
    }
    // Only a mouse or touchpad has these buttons; anything synthesized from
    // a touchscreen or a stylus is not a Back press.
    const QInputDevice *device = mouse->device();
    if (device && device->type() != QInputDevice::DeviceType::Mouse
        && device->type() != QInputDevice::DeviceType::TouchPad)
        return false;

    if (type == QEvent::MouseButtonPress) {
        // Pressed while another button is held: a chord, ignored as well.
        if (mouse->buttons() & ~Qt::MouseButtons(mouse->button())) {
            m_pressed = Direction::None;
            return false;
        }
        m_pressed = direction;
        m_pressPos = mouse->scenePosition();
        // Read now, before delivery: the press may be about to close a
        // click-away popup, and then it must only close it.
        m_pressAllowed = mayNavigate();
        return false;
    }

    // Release.
    if (direction != m_pressed)
        return false;
    m_pressed = Direction::None;
    if (!m_pressAllowed)
        return false;
    const qreal moved =
        (mouse->scenePosition() - m_pressPos).manhattanLength();
    if (moved > QGuiApplication::styleHints()->startDragDistance())
        return false;
    // Queued: the release is still to be delivered to whatever grabbed the
    // press, and opening a room rebuilds the scene under it. Asked again when
    // it runs: nothing may have opened, and no sign-out begun, meanwhile.
    QMetaObject::invokeMethod(this, [this, direction] {
        if (mayNavigate() && m_navigate)
            m_navigate(direction);
    }, Qt::QueuedConnection);
    return false;
}

} // namespace lightning
