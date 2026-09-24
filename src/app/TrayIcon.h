#pragma once

#include <QObject>
#include <QPixmap>
#include <QString>

class QImage;
class QSystemTrayIcon;

// The system-tray icon. It exists only while the user has enabled it, and it
// owns no close-to-tray policy (Main.qml decides that). Availability is asked
// of the platform: a Linux desktop without a StatusNotifier host has no tray.
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

    // Unread state for the tooltip and badge. The icon is repainted only when
    // the displayed badge changes. `anyUnread` covers unread rooms with no
    // count (server-reported or marked unread by hand), shown as a dot.
    void setUnread(int count, bool anyUnread);
    // Identifies the account in the tooltip when several are running.
    void setAccountLabel(const QString &label);

    // The badge text: empty for none, "\u2022" for the countless dot,
    // otherwise "1".."9" or "9+". Static so it is testable without a tray.
    static QString badgeLabel(int count, bool anyUnread);

    // Badges a macOS menu-bar TEMPLATE image. AppKit reads only the alpha
    // channel and recolours it per appearance, so the badge is a cleared moat,
    // an opaque disc and a knocked-out digit. Qt's cocoa backend maps
    // QIcon::setIsMask(true) to [NSImage setTemplate:]. Compiled everywhere so
    // it is testable without a Mac; only the call site is macOS-only.
    static QPixmap macTemplateBadged(const QPixmap &base, const QString &label);

    // A notification through the icon's balloon, Qt's only delivery without a
    // freedesktop daemon (Windows, macOS). Returns false when the icon is not
    // visible. `image` is the sender's avatar, or null for the app icon.
    bool showMessage(const QString &title, const QString &body,
                     const QImage &image);

Q_SIGNALS:
    void showRequested();
    // The balloon shown by showMessage() was clicked.
    void messageClicked();

private:
    void refreshTooltip();
    void refreshIcon();

    QSystemTrayIcon *m_icon = nullptr;
    int m_unread = 0;
    bool m_anyUnread = false;
    QString m_account;
};
