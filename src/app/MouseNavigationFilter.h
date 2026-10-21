#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>

#include <functional>

class QQuickWindow;

namespace lightning {

// Routes the mouse's Back and Forward buttons to room history, from an event
// filter on the window, so the decision is taken BEFORE Qt Quick delivers the
// press to anything.
//
// That ordering is the point. Delivered to an item, a press can no longer tell
// whether it is dismissing something: a click-away popup (every AppMenu, the
// pickers) closes itself DURING the delivery of the press that lands outside
// it, so a handler reading "is a popup open?" from the press already sees the
// popup gone and navigates as well. A browser's rule is that a Back press on
// an open menu only closes the menu; here the window sees the press while the
// popup is still open, remembers that, and navigates on the release only when
// nothing was open.
//
// The filter never consumes an event: the press and release still reach the
// popup (which dismisses itself) and whatever is under the pointer, exactly as
// without it. In particular a latched middle-click autoscroll still ends on
// that press, and a text field under the pointer keeps its focus and caret.
// Being outside the item tree, it is also not stopped by the items that
// accept every button (a Controls TextArea or TextField, the AllButtons
// MouseAreas): the reason an in-scene handler had to sit above the whole shell.
//
// A navigation is a tap: press and release of the same button, without moving
// past the drag distance, and with no other button held or pressed meanwhile
// (a chord navigates nowhere, as in browsers). Only a mouse or touchpad
// event counts: one from a touchscreen or stylus device never navigates,
// whatever button it carries. The filter does not know rooms; `allowed` and `navigate`
// are the owner's (AppController).
class MouseNavigationFilter : public QObject
{
    Q_OBJECT

public:
    enum class Direction { None, Back, Forward };

    // Qt::BackButton / Qt::ForwardButton are XButton1/2 on Windows and
    // BTN_SIDE/BTN_EXTRA on Linux. Some mice report the kernel's BTN_BACK and
    // BTN_FORWARD instead, which QtWayland maps to ExtraButton4 and
    // ExtraButton3 (in that order: 0x116 BTN_BACK -> ExtraButton4, 0x115
    // BTN_FORWARD -> ExtraButton3); those are Back and Forward too.
    static Direction directionFor(Qt::MouseButton button);

    // Whether any popup other than a tooltip (a dialog, menu, picker or
    // viewer, modal or not) is visible in `window`'s overlay.
    static bool windowHasOpenPopup(const QQuickWindow *window);

    // True when a navigation may start now (checked on the press, and again
    // when the queued navigation runs after the release).
    using Allowed = std::function<bool()>;
    // Performs it; returns whether it moved.
    using Navigate = std::function<bool(Direction)>;

    MouseNavigationFilter(QQuickWindow *window, Allowed allowed,
                          Navigate navigate, QObject *parent = nullptr);
    ~MouseNavigationFilter() override;

    // Out of line: QQuickWindow is only forward-declared here, and reading a
    // QPointer to an incomplete type does not compile on every Qt (§16).
    QQuickWindow *window() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool mayNavigate() const;

    QPointer<QQuickWindow> m_window;
    Allowed m_allowed;
    Navigate m_navigate;
    // The Back/Forward press that may become a tap, and whether navigation
    // was allowed WHEN IT WENT DOWN (before any popup could close itself).
    Direction m_pressed = Direction::None;
    bool m_pressAllowed = false;
    QPointF m_pressPos;
};

} // namespace lightning
