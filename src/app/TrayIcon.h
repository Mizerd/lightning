#pragma once

#include <QObject>
#include <QPixmap>
#include <QString>

#include <memory>

class QAction;
class QEvent;
class QImage;
class QMenu;
class QSystemTrayIcon;

class TrayIcon : public QObject
{
    Q_OBJECT

public:
    explicit TrayIcon(QObject *parent = nullptr);
    ~TrayIcon() override;

    static bool platformSupportsTray();

    void setEnabled(bool enabled);
    bool enabled() const { return m_icon != nullptr; }

    void setUnread(int count, bool anyUnread);
    void setAccountLabel(const QString &label);

    static QString badgeLabel(int count, bool anyUnread);

    static QPixmap badged(const QPixmap &base, const QString &label);

    static QPixmap macTemplateBadged(const QPixmap &base, const QString &label);

    bool showMessage(const QString &title, const QString &body,
                     const QImage &image, bool platformSound);

    static int balloonTimeoutMs(bool isMac);

    // Takes the balloon showMessage() raised off the screen where the
    // platform allows it: on Windows NIF_INFO with an empty szInfo removes it
    // (and anything the shell queued behind it). Qt has no such call, so
    // elsewhere this does nothing.
    void hideMessage();
    // Whether hideMessage() can take a balloon down here: Windows only. Where
    // it cannot, an old balloon stays clickable (macOS Notification Center),
    // so NotificationManager cannot assume a click is the latest balloon.
    static bool canWithdrawBalloon();
    static int balloonHoldMs();
    static int balloonHoldMsFor(int durationSeconds);

    static constexpr unsigned kBalloonInfo = 0x01;
    static constexpr unsigned kBalloonUser = 0x04;
    static constexpr unsigned kBalloonNoSound = 0x10;
    static constexpr unsigned kBalloonLargeIcon = 0x20;
    static unsigned balloonInfoFlags(bool hasImage, bool platformSound);
    static QImage balloonImage(const QImage &avatar);

    QMenu *contextMenu();
    static bool activationShowsWindow(int reason);
    static bool activationOpensOwnMenu(int reason, bool nativeMenu);
    // The reason the activated handler acts on. On macOS a Control-click is
    // the standard secondary click, but Qt's cocoa backend reports it as
    // Trigger (only a real RightMouseDown maps to Context), so Trigger with
    // Control held (Qt::MetaModifier on macOS) becomes Context there.
    static int effectiveActivationReason(int reason, bool isMac,
                                         bool controlHeld);

    void retranslate();
    static QString tooltipText(const QString &account, int unread,
                               bool anyUnread);

Q_SIGNALS:
    void showRequested();
    void quitRequested();
    void messageClicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshTooltip();
    void refreshIcon();
    void popUpMenu();
#ifdef Q_OS_WIN
    bool showNativeBalloon(const QString &title, const QString &body,
                           const QImage &image, bool platformSound);
    void releaseBalloonIcon();
    void hideNativeBalloon();
    void *m_balloonIcon = nullptr;
#endif

    QSystemTrayIcon *m_icon = nullptr;
    std::unique_ptr<QMenu> m_menu;
    QAction *m_showAction = nullptr;
    QAction *m_quitAction = nullptr;
    int m_unread = 0;
    bool m_anyUnread = false;
    QString m_account;
};
