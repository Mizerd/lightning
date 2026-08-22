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

    // Reflected in the tooltip. The tray icon itself is not repainted: an
    // overlay drawn per unread change is a per-message rasterization for a
    // number the tooltip already carries honestly.
    void setUnreadCount(int count);
    // Shown beside the count so the tooltip identifies the account this
    // window belongs to when several are running.
    void setAccountLabel(const QString &label);

Q_SIGNALS:
    // The user clicked the icon; bring the window back.
    void showRequested();

private:
    void refreshTooltip();

    QSystemTrayIcon *m_icon = nullptr;
    int m_unread = 0;
    QString m_account;
};
