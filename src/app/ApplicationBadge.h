#pragma once

#include <QGuiApplication>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QVariantMap>
#endif

namespace lightning {

// Encode UTF-8 bytes reversibly into one D-Bus object-path component.
// Escape '_' too, so a literal '_2d' cannot collide with an encoded '-'.
inline QString badgeObjectPath(const QString &applicationName)
{
    QByteArray component;
    for (const unsigned char c : applicationName.toUtf8()) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')) {
            component += char(c);
        } else {
            component += '_';
            component += QByteArray(1, char(c)).toHex();
        }
    }
    if (component.isEmpty())
        component = "_";
    return QStringLiteral("/com/canonical/unity/launcherentry/")
        + QString::fromLatin1(component);
}

#ifdef HAVE_QT_DBUS
inline QDBusMessage badgeSignal(const QString &applicationName,
                                const QString &desktopFileName, qint64 number)
{
    auto signal = QDBusMessage::createSignal(
        badgeObjectPath(applicationName),
        QStringLiteral("com.canonical.Unity.LauncherEntry"),
        QStringLiteral("Update"));
    const qint64 count = qBound(qint64(0), number, qint64(9999));
    QVariantMap properties{{QStringLiteral("count-visible"), count > 0}};
    if (count > 0)
        properties.insert(QStringLiteral("count"), count);
    signal.setArguments({QStringLiteral("application://") + desktopFileName
                             + QStringLiteral(".desktop"), properties});
    return signal;
}
#endif

inline void setApplicationBadge(QGuiApplication &app, qint64 number)
{
#ifdef HAVE_QT_DBUS
    // Qt 6.11's Unix implementation appends applicationName() verbatim to
    // its object path. Our persistent identity contains '-', so that signal
    // is rejected. Preserve the identity and send the escaped signal here.
    if (!app.desktopFileName().isEmpty())
        QDBusConnection::sessionBus().send(
            badgeSignal(app.applicationName(), app.desktopFileName(), number));
#else
    app.setBadgeNumber(number);
#endif
}

} // namespace lightning
