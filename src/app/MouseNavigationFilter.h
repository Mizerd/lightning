#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>

#include <functional>

class QQuickWindow;

namespace lightning {

class MouseNavigationFilter : public QObject
{
    Q_OBJECT

public:
    enum class Direction { None, Back, Forward };

    static Direction directionFor(Qt::MouseButton button);

    static bool windowHasOpenPopup(const QQuickWindow *window);

    using Allowed = std::function<bool()>;
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
    Direction m_pressed = Direction::None;
    bool m_pressAllowed = false;
    bool m_popupClosedThisEvent = false;
    QPointF m_pressPos;
};

}
