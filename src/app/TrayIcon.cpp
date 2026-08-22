#include "app/TrayIcon.h"

#include <QGuiApplication>
#include <QIcon>
#include <QSystemTrayIcon>

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

    // Deliberately NO context menu. QSystemTrayIcon takes a QMenu, which is a
    // QtWidgets type and needs a QApplication; this process constructs a
    // QGuiApplication, and switching the whole application over to widgets to
    // gain two menu entries would be a large change for a small one. Instead
    // EVERY activation — left click, double click and right click — brings
    // the window back, so the icon is never a dead end, and quitting stays
    // where it already was: in the window (Ctrl+Q).
    m_icon = new QSystemTrayIcon(this);
    // The window icon, so the tray matches the task switcher. A custom app
    // icon set by the user is already installed as the application icon, so
    // this follows it without a second code path.
    m_icon->setIcon(QGuiApplication::windowIcon());
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                Q_UNUSED(reason);
                Q_EMIT showRequested();
            });
    refreshTooltip();
    m_icon->show();
}

void TrayIcon::setUnreadCount(int count)
{
    const int clamped = qMax(0, count);
    if (clamped == m_unread)
        return;
    m_unread = clamped;
    refreshTooltip();
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
