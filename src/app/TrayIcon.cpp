#include "app/TrayIcon.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace {

// The badge is red on every theme on purpose: it is the operating system's
// tray, not Lightning's window, and a badge tinted to match a user theme
// reads as decoration rather than as a count. This is the one colour in the
// application that is deliberately not a theme token.
const QColor kBadgeFill = QColor(0xE5, 0x48, 0x4D);
const QColor kBadgeInk = QColor(0xFF, 0xFF, 0xFF);

// The sizes a tray asks for. Several are rendered so the host picks a sharp
// one instead of scaling: a StatusNotifier host and the Windows notification
// area disagree about the size, and both change it with the display scale.
constexpr int kBadgeSizes[] = { 16, 22, 24, 32, 48, 64 };

// One rendered badge on top of one base pixmap.
QPixmap withBadge(const QPixmap &base, const QString &label)
{
    if (base.isNull() || label.isEmpty())
        return base;
    QPixmap out = base;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal side = qMin(out.width(), out.height());
    // A dot needs less room than a digit, and taking less of the icon keeps
    // the mark itself recognisable.
    const bool dotOnly = (label == QStringLiteral("\u2022"));
    const qreal diameter = dotOnly ? side * 0.42 : side * 0.62;
    const QRectF circle(out.width() - diameter, out.height() - diameter,
                        diameter, diameter);

    painter.setPen(Qt::NoPen);
    painter.setBrush(kBadgeFill);
    painter.drawEllipse(circle);
    if (dotOnly)
        return out;

    QFont font = QGuiApplication::font();
    font.setBold(true);
    // Sized from the circle rather than from a point size, because the same
    // code renders a 16px and a 64px icon.
    font.setPixelSize(qMax(6, qRound(diameter * 0.68)));
    painter.setFont(font);
    painter.setPen(kBadgeInk);
    painter.drawText(circle, Qt::AlignCenter, label);
    return out;
}

} // namespace

TrayIcon::TrayIcon(QObject *parent)
    : QObject(parent)
{
}

TrayIcon::~TrayIcon() = default;

bool TrayIcon::platformSupportsTray()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayIcon::setEnabled(bool enabled)
{
    if (enabled == (m_icon != nullptr))
        return;
    if (!enabled) {
        delete m_icon;
        m_icon = nullptr;
        return;
    }
    if (!platformSupportsTray())
        return;

    // Deliberately NO context menu. A QMenu is a widget, and a tray menu
    // would need its own design (what it offers, how it tracks unread and
    // account state) rather than two entries bolted on. Instead EVERY
    // activation — left click, double click and right click — brings the
    // window back, so the icon is never a dead end, and quitting stays where
    // it already was: in the window (Ctrl+Q).
    //
    // The process IS a QApplication since 0.9.1 (see main.cpp): on X11 with
    // no StatusNotifier watcher Qt falls back to an XEmbed icon that is a
    // QWidget, and under a QGuiApplication that fallback aborted the process
    // the moment this setting was switched on.
    m_icon = new QSystemTrayIcon(this);
    // The window icon, so the tray matches the task switcher. A custom app
    // icon set by the user is already installed as the application icon, so
    // this follows it without a second code path. refreshIcon() rather than
    // setIcon(): an icon created while messages are already unread must open
    // WITH its badge, not gain one at the next change.
    refreshIcon();
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                Q_UNUSED(reason);
                Q_EMIT showRequested();
            });
    refreshTooltip();
    m_icon->show();
}

QString TrayIcon::badgeLabel(int count, bool anyUnread)
{
    if (count > 9)
        return QStringLiteral("9+");
    if (count > 0)
        return QString::number(count);
    // A count of zero with unread messages is not a contradiction: a
    // homeserver can report that a room has something unread without saying
    // how much, and a room the user marked unread by hand never had a count.
    // The dot claims exactly what is known.
    if (anyUnread)
        return QStringLiteral("\u2022");
    return QString{};
}

void TrayIcon::setUnread(int count, bool anyUnread)
{
    const int clamped = qMax(0, count);
    if (clamped == m_unread && anyUnread == m_anyUnread)
        return;
    const QString before = badgeLabel(m_unread, m_anyUnread);
    m_unread = clamped;
    m_anyUnread = anyUnread;
    refreshTooltip();
    // THE WHOLE POINT: the icon is rasterised only when what it would SHOW
    // has changed. 40 unread becoming 41 is a tooltip change and nothing
    // else.
    if (badgeLabel(m_unread, m_anyUnread) != before)
        refreshIcon();
}

void TrayIcon::refreshIcon()
{
    if (!m_icon)
        return;
    const QIcon base = QGuiApplication::windowIcon();
    const QString label = badgeLabel(m_unread, m_anyUnread);
    if (base.isNull() || label.isEmpty()) {
        m_icon->setIcon(base);
        return;
    }
    QIcon badged;
    for (const int size : kBadgeSizes) {
        const QPixmap pixmap = base.pixmap(QSize(size, size));
        if (pixmap.isNull())
            continue;
        badged.addPixmap(withBadge(pixmap, label));
    }
    // A base icon that yielded nothing at any size is left alone rather than
    // replaced with an empty QIcon, which would clear the tray entry.
    m_icon->setIcon(badged.isNull() ? base : badged);
}

void TrayIcon::setAccountLabel(const QString &label)
{
    if (label == m_account)
        return;
    m_account = label;
    refreshTooltip();
}

void TrayIcon::refreshTooltip()
{
    if (!m_icon)
        return;
    QString text = QStringLiteral("Lightning");
    if (!m_account.isEmpty())
        text += QStringLiteral(" — ") + m_account;
    if (m_unread > 0) {
        text += QLatin1Char('\n')
            + tr("%n unread message(s)", "system tray tooltip", m_unread);
    }
    m_icon->setToolTip(text);
}
