#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;

// The system-tray presence, and nothing else.
//
// Requested by a tester on Windows: "close to system-tray … and maybe start to
// tray, unread badge". It exists only while the user has asked for it —
// creating a tray icon for a feature nobody turned on puts an icon in
// somebody's tray for no reason.
//
// It owns no policy about what closing the window MEANS. Main.qml decides that
// (it is the only thing that can hide a window), and this class only reports
// what the user asked the tray to do. Availability is asked of the platform
// rather than assumed: a Linux desktop without a StatusNotifier host has no
// tray at all, and "close to tray" there would close the window into nothing.
class TrayIcon : public QObject
{
    Q_OBJECT

public:
    explicit TrayIcon(QObject *parent = nullptr);
    ~TrayIcon() override;

    // Whether this platform/session actually offers a tray right now.
    static bool platformSupportsTray();

    // Creates or destroys the icon. Idempotent.
    void setEnabled(bool enabled);
    bool enabled() const { return m_icon != nullptr; }

    // The account's unread state, in the tooltip AND as a badge on the icon.
    //
    // The badge was declined once, on the grounds that "an overlay drawn per
    // unread change is a per-message rasterization for a number the tooltip
    // already carries". That objection is answered rather than overruled:
    // the icon is repainted only when the DISPLAYED badge changes, and what
    // is displayed is one character (1-9, then "9+", then a plain dot), so a
    // busy room that goes from 40 to 41 unread repaints nothing at all.
    //
    // `anyUnread` is separate from `count` because a homeserver may report
    // that a room has unread messages without saying how many, and a room
    // the user marked unread by hand has no count by construction. A dot in
    // that case is the same conservative claim the room list makes; a
    // fabricated "1" would not be.
    void setUnread(int count, bool anyUnread);
    // Shown beside the count so the tooltip identifies the account this
    // window belongs to when several are running.
    void setAccountLabel(const QString &label);

    // What the badge would SAY for a given state: empty for no badge at all,
    // "\u2022" for the countless dot, otherwise "1".."9" or "9+". Public and
    // static so the repaint rule — repaint only when this string changes —
    // is testable on a machine with no system tray at all.
    static QString badgeLabel(int count, bool anyUnread);

Q_SIGNALS:
    // The user clicked the icon; bring the window back.
    void showRequested();

private:
    void refreshTooltip();
    void refreshIcon();

    QSystemTrayIcon *m_icon = nullptr;
    int m_unread = 0;
    bool m_anyUnread = false;
    QString m_account;
};
