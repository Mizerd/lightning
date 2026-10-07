#pragma once

#include <QObject>
#include <QPixmap>
#include <QString>

#include <memory>

class QImage;
class QMenu;
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

    // The colour badge painted on `base` (a red disc in the bottom-right
    // corner, with the label). Geometry is in the pixmap's device-independent
    // units, so it stays inside the pixmap at any devicePixelRatio. Static so
    // it is testable without a tray.
    static QPixmap badged(const QPixmap &base, const QString &label);

    // Badges a macOS menu-bar TEMPLATE image. AppKit reads only the alpha
    // channel and recolours it per appearance, so the badge is a cleared moat,
    // an opaque disc and a knocked-out digit. Qt's cocoa backend maps
    // QIcon::setIsMask(true) to [NSImage setTemplate:]. Compiled everywhere so
    // it is testable without a Mac; only the call site is macOS-only.
    static QPixmap macTemplateBadged(const QPixmap &base, const QString &label);

    // A notification through the icon's balloon, Qt's only delivery without a
    // freedesktop daemon (Windows, macOS). Returns false when the icon is not
    // visible. `image` is the sender's avatar, or null for the app icon.
    // `platformSound`: the platform may add its own notification sound (the
    // sound plan's platformSound: the user chose System default, or our own
    // sound cannot play). False means it must stay silent, because Lightning
    // plays its own chime or none.
    //
    // QSystemTrayIcon::showMessage cannot honour false on Windows: Qt's
    // balloon never sets NIIF_NOSOUND, so Windows played its "Notify System
    // Generic" on every balloon, on top of our chime and with notification
    // sound Off. There the balloon is raised on Qt's own icon (same window
    // and id, so a click still reaches messageClicked()) with the flags
    // balloonInfoFlags() composes, and Qt's call is only the fallback.
    bool showMessage(const QString &title, const QString &body,
                     const QImage &image, bool platformSound);

    // The Win32 NIIF_* values (shellapi.h) a balloon is composed from,
    // mirrored here so the composition is testable off Windows. TrayIcon.cpp
    // static_asserts them against the SDK on Windows.
    static constexpr unsigned kBalloonInfo = 0x01;      // NIIF_INFO
    static constexpr unsigned kBalloonUser = 0x04;      // NIIF_USER
    static constexpr unsigned kBalloonNoSound = 0x10;   // NIIF_NOSOUND
    static constexpr unsigned kBalloonLargeIcon = 0x20; // NIIF_LARGE_ICON
    // dwInfoFlags for a Windows balloon. The icon bits are exactly Qt
    // 6.11.2's (NIIF_INFO without an image, NIIF_USER | NIIF_LARGE_ICON with
    // one); NIIF_NOSOUND is added unless the platform sound was asked for.
    static unsigned balloonInfoFlags(bool hasImage, bool platformSound);
    // The balloon's image: the avatar scaled down (never up) to fit 256x256,
    // keeping its aspect, as Qt's icon.actualSize(QSize(256, 256)) did.
    static QImage balloonImage(const QImage &avatar);

    // The icon's right-click menu: "Show Lightning" and "Quit Lightning".
    // Built on first use, so it is testable without a tray.
    QMenu *contextMenu();
    // Whether an activation raises the window. Not the Context reason: that is
    // the right click which opens the menu (Windows reports both).
    static bool activationShowsWindow(int reason);

Q_SIGNALS:
    void showRequested();
    // "Quit Lightning" in the menu: quit for real, past close-to-tray.
    void quitRequested();
    // The balloon shown by showMessage() was clicked.
    void messageClicked();

private:
    void refreshTooltip();
    void refreshIcon();
#ifdef Q_OS_WIN
    // The balloon raised natively; false when Qt's call must carry it.
    bool showNativeBalloon(const QString &title, const QString &body,
                           const QImage &image, bool platformSound);
    void releaseBalloonIcon();
    // HICON of the last native balloon, kept until the next one replaces it
    // (Qt keeps its own the same way). void* so this header needs no
    // windows.h.
    void *m_balloonIcon = nullptr;
#endif

    QSystemTrayIcon *m_icon = nullptr;
    std::unique_ptr<QMenu> m_menu;
    int m_unread = 0;
    bool m_anyUnread = false;
    QString m_account;
};
