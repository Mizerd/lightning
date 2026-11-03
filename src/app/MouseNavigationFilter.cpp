#include "MouseNavigationFilter.h"

#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QWindow>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>

namespace lightning {

MouseNavigationFilter::Direction
MouseNavigationFilter::directionFor(Qt::MouseButton button)
{
    switch (button) {
    case Qt::BackButton:
    case Qt::ExtraButton4:
        return Direction::Back;
    case Qt::ForwardButton:
    case Qt::ExtraButton3:
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
        const auto items = child->childItems();
        for (QQuickItem *item : items) {
            QObject *popup = item->parent();
            if (item->isVisible() && popup && popup->inherits("QQuickPopup")
                && !popup->inherits("QQuickToolTip")
                && popup->property("visible").toBool())
                return true;
        }
        break;
    }
    for (const QWindow *w : QGuiApplication::topLevelWindows()) {
        if (!w->isVisible() || !w->inherits("QQuickPopupWindow"))
            continue;
        for (const QWindow *p = w->transientParent(); p;
             p = p->transientParent()) {
            if (p == window)
                return true;
        }
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
    qApp->installEventFilter(this);
}

MouseNavigationFilter::~MouseNavigationFilter()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

QQuickWindow *MouseNavigationFilter::window() const
{
    return m_window;
}

bool MouseNavigationFilter::mayNavigate() const
{
    return !m_popupClosedThisEvent && !windowHasOpenPopup(m_window)
        && (!m_allowed || m_allowed());
}

bool MouseNavigationFilter::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    // Qt closes a Qt::Popup window on an outside press before delivering it,
    // so the press would otherwise read "no popup open" and navigate.
    if (type == QEvent::Hide && watched != m_window && watched->isWindowType()
        && watched->inherits("QQuickPopupWindow")) {
        m_popupClosedThisEvent = true;
        QMetaObject::invokeMethod(this, [this] { m_popupClosedThisEvent = false; },
                                  Qt::QueuedConnection);
        return false;
    }
    if (watched != m_window)
        return false;
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    const Direction direction = directionFor(mouse->button());
    if (direction == Direction::None) {
        if (type == QEvent::MouseButtonPress)
            m_pressed = Direction::None;
        return false;
    }
    const QInputDevice *device = mouse->device();
    if (device && device->type() != QInputDevice::DeviceType::Mouse
        && device->type() != QInputDevice::DeviceType::TouchPad)
        return false;

    if (type == QEvent::MouseButtonPress) {
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

    if (direction != m_pressed)
        return false;
    m_pressed = Direction::None;
    if (!m_pressAllowed)
        return false;
    const qreal moved =
        (mouse->scenePosition() - m_pressPos).manhattanLength();
    if (moved > QGuiApplication::styleHints()->startDragDistance())
        return false;
    QMetaObject::invokeMethod(this, [this, direction] {
        if (mayNavigate() && m_navigate)
            m_navigate(direction);
    }, Qt::QueuedConnection);
    return false;
}

}
