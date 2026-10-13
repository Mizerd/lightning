#include "app/NotificationTroubleshooter.h"

#include <QSystemTrayIcon>
#include <QVariantMap>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#endif

namespace {

QVariantMap check(const QString &id, const QString &title,
                  NotificationTroubleshooter::Status status,
                  const QString &detail, const QString &fix = QString())
{
    QVariantMap out;
    out.insert(QStringLiteral("id"), id);
    out.insert(QStringLiteral("title"), title);
    out.insert(QStringLiteral("status"), static_cast<int>(status));
    out.insert(QStringLiteral("detail"), detail);
    out.insert(QStringLiteral("fix"), fix);
    return out;
}

} // namespace

NotificationTroubleshooter::NotificationTroubleshooter(QObject *parent)
    : QObject(parent)
{
}

QString NotificationTroubleshooter::currentPlatform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

NotificationTroubleshooter::DeliveryPath
NotificationTroubleshooter::probeDeliveryPath()
{
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface()
        && bus.interface()->isServiceRegistered(
               QStringLiteral("org.freedesktop.Notifications"))) {
        return DesktopService;
    }
#endif
    if (QSystemTrayIcon::isSystemTrayAvailable()
        && QSystemTrayIcon::supportsMessages()) {
        return TrayBalloon;
    }
    return NoPath;
}

QVariantList NotificationTroubleshooter::evaluate(const Inputs &in)
{
    QVariantList out;

    // 1. Is there anything to deliver through?
    switch (in.delivery) {
    case DesktopService:
        out.append(check(QStringLiteral("delivery"),
                         tr("A notification service is running"), Pass,
                         tr("Your desktop's notification service answered.")));
        break;
    case TrayBalloon:
        out.append(check(QStringLiteral("delivery"),
                         tr("Notifications use the tray icon"), Pass,
                         tr("Lightning shows notifications as balloons from its "
                            "tray icon on this system. They need the icon to be "
                            "visible.")));
        break;
    case NoPath:
        out.append(check(
            QStringLiteral("delivery"), tr("No way to show a notification"),
            Fail,
            in.platform == QLatin1String("linux")
                ? tr("Nothing is listening for notifications on the session bus "
                     "and there is no tray to fall back on. Start a notification "
                     "service (your desktop's own, or one such as dunst or "
                     "mako).")
                : tr("The system tray is not available, so there is nowhere to "
                     "show a notification from.")));
        break;
    }

    // 2. The operating system. Never readable, always said so.
    QString osDetail;
    if (in.platform == QLatin1String("macos")) {
        osDetail = tr("Lightning cannot read whether macOS allows its "
                      "notifications or whether a Focus mode is on. If the test "
                      "below does not appear, check System Settings, "
                      "Notifications and Focus.");
    } else if (in.platform == QLatin1String("windows")) {
        osDetail = tr("Lightning cannot read whether Windows allows its "
                      "notifications or whether Focus assist is on. If the test "
                      "below does not appear, check Settings, System, "
                      "Notifications.");
    } else {
        osDetail = tr("Lightning cannot read whether your desktop is in Do Not "
                      "Disturb or has muted Lightning. If the test below does "
                      "not appear, check your desktop's notification settings.");
    }
    out.append(check(QStringLiteral("system"),
                     tr("System permission and Do Not Disturb"), Unknown,
                     osDetail));

    // 3. Lightning's own switch.
    out.append(in.appEnabled
        ? check(QStringLiteral("app"), tr("Desktop notifications are on"), Pass,
                tr("Lightning is allowed to show notifications."))
        : check(QStringLiteral("app"), tr("Desktop notifications are off"), Fail,
                tr("Lightning is set not to show any desktop notification."),
                QStringLiteral("enableApp")));

    // 4. The account.
    if (!in.rulesKnown) {
        out.append(check(QStringLiteral("account"),
                         tr("Account notification settings"), Unknown,
                         tr("They have not been loaded from your homeserver "
                            "yet.")));
    } else if (in.masterMute) {
        out.append(check(QStringLiteral("account"),
                         tr("All notifications are muted on your account"), Fail,
                         tr("The master mute is on, so your homeserver sends no "
                            "notifications for any room, to any of your "
                            "devices."),
                         QStringLiteral("unmute")));
    } else {
        out.append(check(QStringLiteral("account"),
                         tr("Your account is not muted"), Pass,
                         tr("The master mute is off.")));
    }

    // 5. The open room.
    if (!in.roomId.isEmpty()) {
        if (in.roomMode == 2) {
            out.append(check(QStringLiteral("room"),
                             tr("This room is muted"), Fail,
                             tr("It never notifies. Change it under Room "
                                "information, Notifications.")));
        } else if (in.roomMode == 1) {
            out.append(check(QStringLiteral("room"),
                             tr("This room notifies only for mentions"), Warn,
                             tr("Ordinary messages here stay quiet. Change it "
                                "under Room information, Notifications.")));
        } else if (in.roomMode == 0) {
            out.append(check(QStringLiteral("room"),
                             tr("This room notifies for every message"), Pass,
                             tr("Nothing is overriding it.")));
        } else {
            out.append(check(QStringLiteral("room"),
                             tr("This room follows your defaults"), Pass,
                             tr("It has no setting of its own.")));
        }
    }
    return out;
}

void NotificationTroubleshooter::run(const QString &roomId)
{
    Inputs in;
    if (m_provider)
        in = m_provider(roomId);
    m_checks = evaluate(in);
    m_hasRun = true;
    Q_EMIT checksChanged();
}

void NotificationTroubleshooter::sendTest()
{
    if (m_testSender)
        m_testSender();
    Q_EMIT testSent();
}
