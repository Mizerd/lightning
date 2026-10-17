#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#ifdef HAVE_QT_DBUS
#include <QDBusObjectPath>
#endif

class ShortcutRegistry;
#ifdef HAVE_QT_DBUS
class QDBusServiceWatcher;
#endif

/// Call keys that work while another window has the focus, through the XDG
/// desktop portal `org.freedesktop.portal.GlobalShortcuts` (KDE Plasma 6,
/// GNOME 48+, Hyprland). GitHub #47.
///
/// Qt's `Shortcut` only sees keys while a Lightning window is focused, and a
/// Wayland client may not grab keys itself, so the compositor has to do it:
/// the app registers named actions with the portal, the desktop owns the
/// actual key combination (and lets the user change it), and the portal tells
/// us when one is pressed.
///
/// The in-window `Shortcut`s are untouched. When the desktop grabs a
/// combination, the window never sees it; when it grants none, they still work.
///
/// Opt-in (see SettingsManager::globalShortcutsEnabled): a global grab is a
/// system-wide thing and the first bind may show the user a permission dialog.
/// Without QtDBus, or with no portal, the state says why instead of failing
/// silently.
class GlobalShortcutsPortal : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    /// One translated sentence for Settings; never empty.
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    /// The portal can open the desktop's own shortcut editor (portal v2+).
    Q_PROPERTY(bool canConfigure READ canConfigure NOTIFY stateChanged)

public:
    enum State {
        /// Not asked for.
        Off = 0,
        /// Asked for, and this build or desktop cannot do it.
        Unavailable = 1,
        /// Session or bind request in flight (the desktop may be showing the
        /// user a dialog).
        Connecting = 2,
        /// Bound; key presses arrive as `activated`.
        Active = 3,
        /// The portal refused, the user declined, or the session ended.
        Failed = 4,
    };
    Q_ENUM(State)

    /// Action ids sent to the portal. They are ShortcutRegistry ids, so the
    /// desktop's list shows the same labels as Settings and a new action
    /// is added in one place. There is no push-to-talk in the app, so no
    /// press/release pair is registered; `deactivated` is still forwarded.
    static QStringList actionIds();

    /// "Ctrl+Shift+U" as the portal's shortcuts-spec trigger "CTRL+SHIFT+U".
    /// Empty for a sequence the spec cannot express (a chord of several
    /// keys); the desktop then proposes nothing and the user picks.
    static QString portalTrigger(const QString &portableText);

    explicit GlobalShortcutsPortal(ShortcutRegistry *registry,
                                   QObject *parent = nullptr);
    ~GlobalShortcutsPortal() override;

    State state() const { return m_state; }
    QString statusText() const;
    bool canConfigure() const;
    bool enabled() const { return m_enabled; }

    /// Starts or ends the portal session. Idempotent. Nothing touches the
    /// bus while it is false.
    void setEnabled(bool enabled);

    /// Opens the desktop's shortcut editor for this app, when it has one.
    Q_INVOKABLE void configure();

Q_SIGNALS:
    void stateChanged();
    /// A bound action was pressed / released; `actionId` is one of
    /// actionIds().
    void activated(const QString &actionId);
    void deactivated(const QString &actionId);

#ifdef HAVE_QT_DBUS
private Q_SLOTS:
    void onPortalActivated(const QDBusObjectPath &session,
                           const QString &shortcutId, qulonglong timestamp,
                           const QVariantMap &options);
    void onPortalDeactivated(const QDBusObjectPath &session,
                             const QString &shortcutId, qulonglong timestamp,
                             const QVariantMap &options);
    void onSessionClosed(const QVariantMap &details);
    void onPortalOwnerChanged(const QString &service, const QString &oldOwner,
                              const QString &newOwner);
#endif

private:
    void start();
    void stop();
    /// Drops the session and any reply still in flight without telling
    /// anyone; stop() adds the state change, the destructor must not.
    void teardown();
    void beginSession();
    void setState(State state, const QString &detail = QString());
    void probeVersion(int generation);
    void registerApp(int generation);
    void createSession(int generation);
    void bindShortcuts(int generation);
    void closeSession(const QString &handle);
    void subscribeSession();
    void unsubscribeSession();

    ShortcutRegistry *m_registry = nullptr; // not owned
    bool m_enabled = false;
    State m_state = Off;
    QString m_detail;
    int m_version = 0;
    QString m_sessionHandle;
    /// Bumped by every start and stop so a reply that arrives after the user
    /// turned it off (or on again) is dropped, and a session it created is
    /// closed.
    int m_generation = 0;
    bool m_subscribed = false;
#ifdef HAVE_QT_DBUS
    /// Owner changes of the portal's bus name: a restarted portal sends no
    /// Session.Closed, so this is the only way to learn the session is gone.
    QDBusServiceWatcher *m_watcher = nullptr;
#endif
};
