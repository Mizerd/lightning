#include "app/GlobalShortcutsPortal.h"

#include "app/ShortcutRegistry.h"

#include <QFile>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QRandomGenerator>

#ifdef HAVE_QT_DBUS
#include "calls/PortalRequest.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#endif

namespace {
Q_LOGGING_CATEGORY(lcGlobalShortcuts, "lightning.app.globalshortcuts")

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kInterface = "org.freedesktop.portal.GlobalShortcuts";
constexpr auto kRequest = "org.freedesktop.portal.Request";
constexpr auto kSession = "org.freedesktop.portal.Session";
constexpr auto kHostRegistry = "org.freedesktop.host.portal.Registry";

/// Bounded: the probe and the two calls only name a Request, so a portal that
/// does not answer them is not one that is showing a dialog.
constexpr int kCallTimeoutMs = 10000;

QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}

/// One entry of BindShortcuts' `a(sa{sv})`.
struct PortalShortcut
{
    QString id;
    QVariantMap properties;
};

QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s)
{
    arg.beginStructure();
    arg << s.id << s.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s)
{
    arg.beginStructure();
    arg >> s.id >> s.properties;
    arg.endStructure();
    return arg;
}
#endif
} // namespace

#ifdef HAVE_QT_DBUS
Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(QList<PortalShortcut>)

/// Delivers one Request's `Response` once. Same shape as the camera portal's
/// step, which `portal::subscribeBeforeCall` is written against.
class GlobalShortcutsStep : public QObject
{
    Q_OBJECT
public:
    GlobalShortcutsStep(QObject *owner, const QString &requestPath)
        : QObject(owner), m_path(requestPath)
    {
        QDBusConnection::sessionBus().connect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }
    ~GlobalShortcutsStep() override
    {
        QDBusConnection::sessionBus().disconnect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }

Q_SIGNALS:
    /// response: 0 = granted, 1 = user declined, 2 = ended some other way.
    void answered(uint response, const QVariantMap &results);

private Q_SLOTS:
    void onResponse(uint response, const QVariantMap &results)
    {
        Q_EMIT answered(response, results);
        deleteLater();
    }

private:
    QString m_path;
};
#endif

QStringList GlobalShortcutsPortal::actionIds()
{
    return { QStringLiteral("call.toggleMute"),
             QStringLiteral("call.toggleDeafen"),
             QStringLiteral("call.leave") };
}

namespace {
/// The shortcuts spec names a key by its XKB keysym name. Empty for a key
/// it cannot express; the desktop then proposes nothing and the user picks.
QString xkbKeyName(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QString(QChar(key));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QString(QChar(key));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    switch (key) {
    case Qt::Key_Space: return QStringLiteral("space");
    case Qt::Key_Return: return QStringLiteral("Return");
    case Qt::Key_Enter: return QStringLiteral("KP_Enter");
    case Qt::Key_Escape: return QStringLiteral("Escape");
    case Qt::Key_Tab: return QStringLiteral("Tab");
    case Qt::Key_Backspace: return QStringLiteral("BackSpace");
    case Qt::Key_Delete: return QStringLiteral("Delete");
    case Qt::Key_Insert: return QStringLiteral("Insert");
    case Qt::Key_Home: return QStringLiteral("Home");
    case Qt::Key_End: return QStringLiteral("End");
    case Qt::Key_PageUp: return QStringLiteral("Page_Up");
    case Qt::Key_PageDown: return QStringLiteral("Page_Down");
    case Qt::Key_Left: return QStringLiteral("Left");
    case Qt::Key_Right: return QStringLiteral("Right");
    case Qt::Key_Up: return QStringLiteral("Up");
    case Qt::Key_Down: return QStringLiteral("Down");
    case Qt::Key_Print: return QStringLiteral("Print");
    case Qt::Key_Pause: return QStringLiteral("Pause");
    case Qt::Key_Comma: return QStringLiteral("comma");
    case Qt::Key_Period: return QStringLiteral("period");
    case Qt::Key_Minus: return QStringLiteral("minus");
    case Qt::Key_Plus: return QStringLiteral("plus");
    case Qt::Key_Equal: return QStringLiteral("equal");
    case Qt::Key_Slash: return QStringLiteral("slash");
    case Qt::Key_Backslash: return QStringLiteral("backslash");
    case Qt::Key_Semicolon: return QStringLiteral("semicolon");
    case Qt::Key_Apostrophe: return QStringLiteral("apostrophe");
    case Qt::Key_BracketLeft: return QStringLiteral("bracketleft");
    case Qt::Key_BracketRight: return QStringLiteral("bracketright");
    case Qt::Key_QuoteLeft: return QStringLiteral("grave");
    default: return {};
    }
}
} // namespace

QString GlobalShortcutsPortal::portalTrigger(const QString &portableText)
{
    const QKeySequence sequence(portableText, QKeySequence::PortableText);
    if (sequence.count() != 1)
        return {};
    const int combined = sequence[0].toCombined();
    const Qt::KeyboardModifiers mods(combined & Qt::KeyboardModifierMask);
    const QString key = xkbKeyName(combined & ~Qt::KeyboardModifierMask);
    if (key.isEmpty())
        return {};
    QStringList parts;
    if (mods & Qt::ControlModifier)
        parts << QStringLiteral("CTRL");
    if (mods & Qt::AltModifier)
        parts << QStringLiteral("ALT");
    if (mods & Qt::ShiftModifier)
        parts << QStringLiteral("SHIFT");
    if (mods & Qt::MetaModifier)
        parts << QStringLiteral("LOGO");
    parts << key;
    return parts.join(QLatin1Char('+'));
}

GlobalShortcutsPortal::GlobalShortcutsPortal(ShortcutRegistry *registry,
                                             QObject *parent)
    : QObject(parent), m_registry(registry)
{
#ifdef HAVE_QT_DBUS
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
#endif
}

GlobalShortcutsPortal::~GlobalShortcutsPortal()
{
    // No stateChanged: the object is going away and a receiver may be too.
    teardown();
}

QString GlobalShortcutsPortal::statusText() const
{
    switch (m_state) {
    case Off:
        return tr("Off.");
    case Unavailable:
        return m_detail;
    case Connecting:
        return tr("Waiting for your desktop. It may ask you to allow "
                  "global shortcuts.");
    case Active:
        return tr("On. Your desktop owns these keys, so it can also change "
                  "them.");
    case Failed:
        return m_detail;
    }
    return {};
}

bool GlobalShortcutsPortal::canConfigure() const
{
    return m_state == Active && m_version >= 2;
}

void GlobalShortcutsPortal::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (enabled)
        start();
    else
        stop();
}

void GlobalShortcutsPortal::setState(State state, const QString &detail)
{
    if (m_state == state && m_detail == detail)
        return;
    m_state = state;
    m_detail = detail;
    Q_EMIT stateChanged();
}

void GlobalShortcutsPortal::start()
{
#ifdef HAVE_QT_DBUS
    if (!QDBusConnection::sessionBus().isConnected()) {
        ++m_generation;
        setState(Unavailable,
                 tr("There is no desktop session bus, so there is no way "
                    "to ask your desktop for global shortcuts."));
        return;
    }
    if (!m_watcher) {
        m_watcher = new QDBusServiceWatcher(
            QLatin1String(kService), QDBusConnection::sessionBus(),
            QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                &GlobalShortcutsPortal::onPortalOwnerChanged);
    }
    beginSession();
#else
    qCInfo(lcGlobalShortcuts) << "built without QtDBus";
    setState(Unavailable,
             tr("This build has no D-Bus support, so it cannot use your "
                "desktop's global shortcuts."));
#endif
}

#ifdef HAVE_QT_DBUS
void GlobalShortcutsPortal::beginSession()
{
    ++m_generation;
    setState(Connecting);
    probeVersion(m_generation);
}
#endif

void GlobalShortcutsPortal::teardown()
{
    ++m_generation;
#ifdef HAVE_QT_DBUS
    delete m_watcher;
    m_watcher = nullptr;
    unsubscribeSession();
    if (!m_sessionHandle.isEmpty())
        closeSession(m_sessionHandle);
#endif
    m_sessionHandle.clear();
    m_version = 0;
}

void GlobalShortcutsPortal::stop()
{
    teardown();
    setState(Off);
}

#ifdef HAVE_QT_DBUS
void GlobalShortcutsPortal::probeVersion(int generation)
{
    QDBusMessage probe = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    probe << QString::fromLatin1(kInterface) << QStringLiteral("version");
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(probe, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation)
                    return;
                const QDBusMessage reply = w->reply();
                if (reply.type() != QDBusMessage::ReplyMessage
                    || reply.arguments().isEmpty()) {
                    const QDBusError error = w->error();
                    qCInfo(lcGlobalShortcuts)
                        << "portal probe failed:" << error.name();
                    const bool noPortal =
                        error.type() == QDBusError::ServiceUnknown
                        || error.type() == QDBusError::NoReply
                        || error.type() == QDBusError::Timeout;
                    setState(Unavailable,
                             noPortal
                                 ? tr("xdg-desktop-portal is not running, so "
                                       "there is nobody to ask for global "
                                       "shortcuts.")
                                 : tr("Your desktop does not offer global "
                                       "shortcuts. It needs KDE Plasma 6, "
                                       "GNOME 48 or newer, or Hyprland."));
                    return;
                }
                // Properties.Get wraps the value in a variant.
                const QVariant value = reply.arguments().first();
                m_version = qvariant_cast<QDBusVariant>(value)
                                .variant().toUInt();
                qCInfo(lcGlobalShortcuts)
                    << "portal GlobalShortcuts version" << m_version;
                registerApp(generation);
            });
}

void GlobalShortcutsPortal::registerApp(int generation)
{
    // A launch outside a desktop-entry scope (a terminal, an AppImage) gives
    // the portal no app id, and KDE and GNOME may then refuse the session.
    // The host portal lets a client name itself; a sandbox's portal already
    // knows who it is. Best effort: any error, or no such interface, goes on.
    QString appId = QGuiApplication::desktopFileName();
    if (appId.endsWith(QLatin1String(".desktop")))
        appId.chop(8);
    if (appId.isEmpty() || QFile::exists(QStringLiteral("/.flatpak-info"))) {
        createSession(generation);
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath),
        QLatin1String(kHostRegistry), QStringLiteral("Register"));
    call << appId << QVariantMap{};
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (w->isError())
                    qCInfo(lcGlobalShortcuts)
                        << "app id registration skipped:" << w->error().name();
                if (generation == m_generation)
                    createSession(generation);
            });
}

void GlobalShortcutsPortal::createSession(int generation)
{
    const QString token = freshToken();
    auto onAnswer = [this, generation](uint response,
                                       const QVariantMap &results) {
        const QString handle = results.value(QStringLiteral("session_handle"))
                                   .toString();
        if (generation != m_generation) {
            // Turned off (or on again) while the portal was thinking: close
            // what it made rather than leave a session nobody owns.
            if (response == 0 && !handle.isEmpty())
                closeSession(handle);
            return;
        }
        if (response != 0 || handle.isEmpty()) {
            setState(Failed,
                     tr("Your desktop refused to start a global shortcuts "
                        "session."));
            return;
        }
        m_sessionHandle = handle;
        subscribeSession();
        bindShortcuts(generation);
    };

    auto sub = portal::subscribeBeforeCall<GlobalShortcutsStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath),
        QLatin1String(kInterface), QStringLiteral("CreateSession"));
    call << QVariantMap{
        { QStringLiteral("handle_token"), token },
        { QStringLiteral("session_handle_token"), freshToken() },
    };
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, sub, onAnswer](QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (reply.isError()) {
                    portal::dropSubscription(sub);
                    if (generation == m_generation)
                        setState(Failed,
                                 tr("Your desktop refused to start a global "
                                    "shortcuts session."));
                    return;
                }
                portal::reconcileRequestPath(this, sub, reply.value().path(),
                                             onAnswer);
            });
}

void GlobalShortcutsPortal::bindShortcuts(int generation)
{
    QList<PortalShortcut> shortcuts;
    for (const QString &id : actionIds()) {
        PortalShortcut shortcut;
        shortcut.id = id;
        shortcut.properties.insert(
            QStringLiteral("description"),
            m_registry ? m_registry->descriptionFor(id) : id);
        const QString trigger = portalTrigger(
            m_registry ? m_registry->sequenceFor(id) : QString());
        if (!trigger.isEmpty())
            shortcut.properties.insert(QStringLiteral("preferred_trigger"),
                                       trigger);
        shortcuts.append(shortcut);
    }

    const QString token = freshToken();
    auto onAnswer = [this, generation](uint response,
                                       const QVariantMap &results) {
        if (generation != m_generation)
            return;
        if (response == 0) {
            // A granted request can still have bound nothing.
            const QVariant bound = results.value(QStringLiteral("shortcuts"));
            const QList<PortalShortcut> list =
                bound.canConvert<QDBusArgument>()
                    ? qdbus_cast<QList<PortalShortcut>>(
                          bound.value<QDBusArgument>())
                    : QList<PortalShortcut>();
            if (list.isEmpty())
                setState(Unavailable,
                         tr("Your desktop did not bind any of the "
                            "shortcuts."));
            else
                setState(Active);
        } else if (response == 1) {
            setState(Failed,
                     tr("You declined global shortcuts. Turn this off and "
                        "on again to be asked again."));
        } else {
            setState(Failed,
                     tr("Your desktop could not bind the shortcuts."));
        }
    };
    auto sub = portal::subscribeBeforeCall<GlobalShortcutsStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath),
        QLatin1String(kInterface), QStringLiteral("BindShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
         << QVariant::fromValue(shortcuts)
         << QString() // parent_window: none, we have no portal window handle
         << QVariantMap{ { QStringLiteral("handle_token"), token } };
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, sub, onAnswer](QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (reply.isError()) {
                    portal::dropSubscription(sub);
                    if (generation == m_generation)
                        setState(Failed,
                                 tr("Your desktop could not bind the "
                                    "shortcuts."));
                    return;
                }
                portal::reconcileRequestPath(this, sub, reply.value().path(),
                                             onAnswer);
            });
}

void GlobalShortcutsPortal::closeSession(const QString &handle)
{
    QDBusMessage close = QDBusMessage::createMethodCall(
        QLatin1String(kService), handle, QLatin1String(kSession),
        QStringLiteral("Close"));
    // Fire and forget: the answer changes nothing we do next.
    QDBusConnection::sessionBus().asyncCall(close, kCallTimeoutMs);
}

void GlobalShortcutsPortal::subscribeSession()
{
    if (m_subscribed)
        return;
    auto bus = QDBusConnection::sessionBus();
    // Empty service on purpose: the portal's unique name is not the well-known
    // one, and the session handle in each signal is what identifies ours.
    bus.connect(QString(), QLatin1String(kPath), QLatin1String(kInterface),
                QStringLiteral("Activated"), this,
                SLOT(onPortalActivated(QDBusObjectPath, QString, qulonglong,
                                       QVariantMap)));
    bus.connect(QString(), QLatin1String(kPath), QLatin1String(kInterface),
                QStringLiteral("Deactivated"), this,
                SLOT(onPortalDeactivated(QDBusObjectPath, QString, qulonglong,
                                         QVariantMap)));
    bus.connect(QString(), m_sessionHandle, QLatin1String(kSession),
                QStringLiteral("Closed"), this,
                SLOT(onSessionClosed(QVariantMap)));
    m_subscribed = true;
}

void GlobalShortcutsPortal::unsubscribeSession()
{
    if (!m_subscribed)
        return;
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(QString(), QLatin1String(kPath), QLatin1String(kInterface),
                   QStringLiteral("Activated"), this,
                   SLOT(onPortalActivated(QDBusObjectPath, QString, qulonglong,
                                          QVariantMap)));
    bus.disconnect(QString(), QLatin1String(kPath), QLatin1String(kInterface),
                   QStringLiteral("Deactivated"), this,
                   SLOT(onPortalDeactivated(QDBusObjectPath, QString,
                                            qulonglong, QVariantMap)));
    bus.disconnect(QString(), m_sessionHandle, QLatin1String(kSession),
                   QStringLiteral("Closed"), this,
                   SLOT(onSessionClosed(QVariantMap)));
    m_subscribed = false;
}

void GlobalShortcutsPortal::onPortalActivated(const QDBusObjectPath &session,
                                              const QString &shortcutId,
                                              qulonglong, const QVariantMap &)
{
    // Another client's session sees the same broadcast signal.
    if (m_state != Active || session.path() != m_sessionHandle
        || !actionIds().contains(shortcutId))
        return;
    Q_EMIT activated(shortcutId);
}

void GlobalShortcutsPortal::onPortalDeactivated(const QDBusObjectPath &session,
                                                const QString &shortcutId,
                                                qulonglong,
                                                const QVariantMap &)
{
    if (m_state != Active || session.path() != m_sessionHandle
        || !actionIds().contains(shortcutId))
        return;
    Q_EMIT deactivated(shortcutId);
}

void GlobalShortcutsPortal::onPortalOwnerChanged(const QString &,
                                                 const QString &,
                                                 const QString &newOwner)
{
    if (!m_enabled)
        return;
    // A restarted portal keeps no sessions and sends no Session.Closed, so
    // without this the state would stay Active for ever with no key arriving.
    unsubscribeSession();
    m_sessionHandle.clear();
    m_version = 0;
    if (newOwner.isEmpty()) {
        qCInfo(lcGlobalShortcuts) << "portal went away";
        ++m_generation;
        setState(Unavailable,
                 tr("xdg-desktop-portal is not running, so there is nobody "
                    "to ask for global shortcuts."));
        return;
    }
    qCInfo(lcGlobalShortcuts) << "portal has a new owner, starting again";
    beginSession();
}

void GlobalShortcutsPortal::onSessionClosed(const QVariantMap &)
{
    // The desktop ended it (portal restarted, or the user revoked it in
    // system settings). The handle is dead, so there is nothing to Close.
    qCInfo(lcGlobalShortcuts) << "portal closed the session";
    unsubscribeSession();
    m_sessionHandle.clear();
    ++m_generation;
    setState(Failed,
             tr("Your desktop ended the global shortcuts session. Turn this "
                "off and on again to start it again."));
}
#endif

void GlobalShortcutsPortal::configure()
{
#ifdef HAVE_QT_DBUS
    if (!canConfigure())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath),
        QLatin1String(kInterface), QStringLiteral("ConfigureShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
         << QString() << QVariantMap{};
    QDBusConnection::sessionBus().asyncCall(call, kCallTimeoutMs);
#endif
}

#ifdef HAVE_QT_DBUS
#include "GlobalShortcutsPortal.moc"
#endif
