// GlobalShortcutsPortal against a fake xdg-desktop-portal on a private session
// bus (GitHub #47): the CreateSession / BindShortcuts calls as the portal
// receives them, the Activated / Deactivated signals routed to the app, the
// session's lifecycle, and the desktops with no portal at all.
//
// A private `dbus-daemon`, as NotificationDbusTest: the fake must own
// org.freedesktop.portal.Desktop, which a real desktop already owns. The fake
// runs on its own thread so it can answer a caller that waits.
//
// Not covered (needs a real compositor): that KDE/GNOME/Hyprland actually grab
// the key, and what their permission dialog looks like.

#include "app/GlobalShortcutsPortal.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "calls/PortalRequest.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

namespace {

const auto kService = QStringLiteral("org.freedesktop.portal.Desktop");
const auto kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const auto kInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const auto kSessionInterface = QStringLiteral("org.freedesktop.portal.Session");
const auto kSessionHandle =
    QStringLiteral("/org/freedesktop/portal/desktop/session/test/lightning");

/// The test's own view of one BindShortcuts entry, `(sa{sv})`.
struct BoundShortcut
{
    QString id;
    QVariantMap properties;
};

QDBusArgument &operator<<(QDBusArgument &arg, const BoundShortcut &s)
{
    arg.beginStructure();
    arg << s.id << s.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, BoundShortcut &s)
{
    arg.beginStructure();
    arg >> s.id >> s.properties;
    arg.endStructure();
    return arg;
}

} // namespace

Q_DECLARE_METATYPE(BoundShortcut)
Q_DECLARE_METATYPE(QList<BoundShortcut>)

namespace {

class FakePortal : public QDBusVirtualObject
{
public:
    enum class Mode {
        /// Answers everything, version 2.
        Answer,
        /// Has the service but not the GlobalShortcuts interface.
        NoInterface,
        /// The user declines the bind dialog (Response 1).
        Decline,
        /// Replies to each request and holds back its Response signal.
        HoldResponses,
        /// Grants the bind but reports no shortcut bound.
        BindNothing,
        /// Answers, and has no org.freedesktop.host.portal.Registry.
        NoRegistry,
    };

    explicit FakePortal(const QDBusConnection &bus) : m_bus(bus) {}

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &) override
    {
        QMutexLocker lock(&m_mutex);
        m_calls.append(message);
        const QString member = message.member();
        if (message.interface() == QLatin1String("org.freedesktop.DBus.Properties")) {
            if (m_mode == Mode::NoInterface) {
                m_bus.send(message.createErrorReply(
                    QDBusError::UnknownInterface, QStringLiteral("no")));
            } else {
                m_bus.send(message.createReply(QVariant::fromValue(
                    QDBusVariant(QVariant::fromValue<uint>(m_version)))));
            }
            return true;
        }
        if (message.interface() == QLatin1String("org.freedesktop.host.portal.Registry")) {
            if (m_mode == Mode::NoRegistry) {
                m_bus.send(message.createErrorReply(
                    QDBusError::UnknownInterface, QStringLiteral("no")));
            } else {
                m_bus.send(message.createReply());
            }
            return true;
        }
        if (message.interface() == kSessionInterface) {
            m_bus.send(message.createReply());
            return true;
        }
        if (member == QLatin1String("CreateSession")
            || member == QLatin1String("BindShortcuts")) {
            const bool create = member == QLatin1String("CreateSession");
            const QVariantMap options = qdbus_cast<QVariantMap>(
                message.arguments().last().value<QDBusArgument>());
            const QString token = options.value("handle_token").toString();
            const QString request =
                portal::predictedRequestPath(message.service(), token);
            m_bus.send(message.createReply(
                QVariant::fromValue(QDBusObjectPath(request))));
            QVariantMap results;
            if (create) {
                results.insert("session_handle", kSessionHandle);
            } else {
                // What the portal really bound: echo the request, or nothing.
                QList<BoundShortcut> bound;
                if (m_mode != Mode::BindNothing)
                    bound = qdbus_cast<QList<BoundShortcut>>(
                        message.arguments().value(1).value<QDBusArgument>());
                results.insert("shortcuts", QVariant::fromValue(bound));
            }
            const uint response = (!create && m_mode == Mode::Decline) ? 1 : 0;
            if (m_mode == Mode::HoldResponses)
                m_held.append({ request, response, results });
            else
                sendResponse(request, response, results);
            return true;
        }
        m_bus.send(message.createReply());
        return true;
    }

    void reset(Mode mode, uint version = 2)
    {
        QMutexLocker lock(&m_mutex);
        m_mode = mode;
        m_version = version;
        m_calls.clear();
        m_held.clear();
    }
    void releaseHeld()
    {
        QMutexLocker lock(&m_mutex);
        for (const Held &h : std::as_const(m_held))
            sendResponse(h.request, h.response, h.results);
        m_held.clear();
    }
    int count(const QString &member) const
    {
        QMutexLocker lock(&m_mutex);
        int n = 0;
        for (const QDBusMessage &call : m_calls)
            n += call.member() == member ? 1 : 0;
        return n;
    }
    /// Position of the first call to `member` in arrival order, or -1.
    int firstIndex(const QString &member) const
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_calls.size(); ++i) {
            if (m_calls.at(i).member() == member)
                return i;
        }
        return -1;
    }
    QDBusMessage last(const QString &member) const
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_calls.crbegin(); it != m_calls.crend(); ++it) {
            if (it->member() == member)
                return *it;
        }
        return {};
    }
    void emitKey(const QString &signalName, const QString &session,
                 const QString &id)
    {
        QDBusMessage signal = QDBusMessage::createSignal(kPath, kInterface,
                                                         signalName);
        signal << QVariant::fromValue(QDBusObjectPath(session)) << id
               << QVariant::fromValue<qulonglong>(1234) << QVariantMap{};
        m_bus.send(signal);
    }
    void emitClosed(const QString &session)
    {
        QDBusMessage signal =
            QDBusMessage::createSignal(session, kSessionInterface,
                                       QStringLiteral("Closed"));
        signal << QVariantMap{};
        m_bus.send(signal);
    }

private:
    struct Held
    {
        QString request;
        uint response;
        QVariantMap results;
    };
    void sendResponse(const QString &request, uint response,
                      const QVariantMap &results)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            request, QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"));
        signal << response << results;
        m_bus.send(signal);
    }

    QDBusConnection m_bus;
    mutable QMutex m_mutex;
    Mode m_mode = Mode::Answer;
    uint m_version = 2;
    QList<QDBusMessage> m_calls;
    QList<Held> m_held;
};

} // namespace

class GlobalShortcutsPortalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();

    void portalTriggerFollowsTheShortcutsSpec();
    void portalTriggerNamesNonLetterKeysAsKeysyms();
    void globalShortcutsAreOffByDefault();
    void theSettingPersistsAndSignalsOnlyOnChange();
    void appControllerDrivesThePortalFromTheSetting();
    void theAppIdIsRegisteredBeforeTheSessionIsCreated();
    void aPortalWithoutTheRegistryStillWorks();
    void aRestartedPortalIsNoticedAndTheSessionIsMadeAgain();
    void aBindThatBoundNothingIsNotActive();
    void destroyingAnActivePortalEmitsNothing();
    void itIsOffAndTouchesNoBusUntilEnabled();
    void enablingBindsTheCallActionsWithTheRegistrysLabels();
    void aPressedActionIsRoutedAndAnotherSessionsIsNot();
    void releaseIsForwardedToo();
    void disablingClosesTheSessionAndStopsRouting();
    void turningItOffWhileTheSessionIsBeingMadeClosesIt();
    void aDesktopWithoutTheInterfaceSaysSo();
    void aMissingPortalSaysSo();
    void aDeclinedBindIsFailedAndRoutesNothing();
    void aSessionTheDesktopEndsIsFailed();
    void configureNeedsPortalVersionTwo();

private:
    /// A portal with the fake's answers, Active.
    void enableAndWait(GlobalShortcutsPortal &portal);

    QProcess m_daemon;
    QTemporaryDir m_configDir;
    QThread m_fakeThread;
    FakePortal *m_fake = nullptr;
};

void GlobalShortcutsPortalTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("global-shortcuts-portal-test"));
    QGuiApplication::setDesktopFileName(
        QStringLiteral("org.example.LightningTest"));
    qDBusRegisterMetaType<BoundShortcut>();
    qDBusRegisterMetaType<QList<BoundShortcut>>();

    const QString daemon = QStandardPaths::findExecutable("dbus-daemon");
    if (daemon.isEmpty())
        QSKIP("dbus-daemon not found; this suite needs a private bus");
    QStringList args{QStringLiteral("--nofork"),
                     QStringLiteral("--print-address=1")};
    // Our own configuration, with NO service directories. The packaged
    // session.conf carries <standard_session_servicedirs/>, so a call to the
    // absent portal makes the daemon try to ACTIVATE the host's real
    // xdg-desktop-portal on this private bus and wait for it: the caller then
    // sees nothing for its whole timeout, and the "no portal" cases stall in
    // Connecting. A missing name must fail at once, as it does on a desktop
    // with no portal installed.
    QVERIFY(m_configDir.isValid());
    QFile config(m_configDir.filePath(QStringLiteral("bus.conf")));
    QVERIFY(config.open(QIODevice::WriteOnly));
    config.write("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus "
                 "Bus Configuration 1.0//EN\" \"http://www.freedesktop.org/"
                 "standards/dbus/1.0/busconfig.dtd\">\n"
                 "<busconfig><type>session</type>"
                 "<listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>"
                 "<policy context=\"default\">"
                 "<allow send_destination=\"*\" eavesdrop=\"true\"/>"
                 "<allow eavesdrop=\"true\"/><allow own=\"*\"/>"
                 "</policy></busconfig>\n");
    config.close();
    args << QStringLiteral("--config-file=%1").arg(config.fileName());
    m_daemon.start(daemon, args);
    QVERIFY(m_daemon.waitForStarted(5000));
    QVERIFY(m_daemon.waitForReadyRead(5000));
    const QByteArray address = m_daemon.readLine().trimmed();
    QVERIFY(!address.isEmpty());

    // Read by QDBusConnection::sessionBus() on first use, which is later.
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    QVERIFY(QDBusConnection::sessionBus().isConnected());

    QDBusConnection fakeBus = QDBusConnection::connectToBus(
        QString::fromLatin1(address), QStringLiteral("fake-portal"));
    QVERIFY(fakeBus.isConnected());
    QVERIFY(fakeBus.registerService(kService));
    m_fakeThread.start();
    m_fake = new FakePortal(fakeBus);
    m_fake->moveToThread(&m_fakeThread);
    // SubPath: Request and Session objects live below the portal's path.
    QVERIFY(fakeBus.registerVirtualObject(kPath, m_fake,
                                          QDBusConnection::SubPath));
}

void GlobalShortcutsPortalTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-portal"));
    if (m_fake)
        m_fake->deleteLater();
    m_fakeThread.quit();
    m_fakeThread.wait(3000);
    m_fake = nullptr;
    if (m_daemon.state() != QProcess::NotRunning) {
        m_daemon.kill();
        m_daemon.waitForFinished(3000);
    }
}

void GlobalShortcutsPortalTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QDBusConnection fakeBus(QStringLiteral("fake-portal"));
    if (!fakeBus.interface()->isServiceRegistered(kService))
        fakeBus.registerService(kService);
    // A previous case's portal closes its session as it is destroyed; let
    // that arrive before the counts are cleared.
    QTest::qWait(150);
    m_fake->reset(FakePortal::Mode::Answer);
}

void GlobalShortcutsPortalTest::enableAndWait(GlobalShortcutsPortal &portal)
{
    portal.setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(), GlobalShortcutsPortal::Active,
                              5000);
}

void GlobalShortcutsPortalTest::portalTriggerFollowsTheShortcutsSpec()
{
    QCOMPARE(GlobalShortcutsPortal::portalTrigger(QStringLiteral("Ctrl+Shift+U")),
             QStringLiteral("CTRL+SHIFT+U"));
    QCOMPARE(GlobalShortcutsPortal::portalTrigger(QStringLiteral("Ctrl+Alt+A")),
             QStringLiteral("CTRL+ALT+A"));
    QCOMPARE(GlobalShortcutsPortal::portalTrigger(QStringLiteral("Meta+F5")),
             QStringLiteral("LOGO+F5"));
    // A chord of two keys has no representation; the desktop proposes
    // nothing rather than something wrong.
    QCOMPARE(GlobalShortcutsPortal::portalTrigger(QStringLiteral("Ctrl+K, Ctrl+C")),
             QString());
    QCOMPARE(GlobalShortcutsPortal::portalTrigger(QString()), QString());
}

void GlobalShortcutsPortalTest::portalTriggerNamesNonLetterKeysAsKeysyms()
{
    const auto t = [](const char *text) {
        return GlobalShortcutsPortal::portalTrigger(QString::fromLatin1(text));
    };
    QCOMPARE(t("Ctrl+Space"), QStringLiteral("CTRL+space"));
    QCOMPARE(t("Ctrl+PgUp"), QStringLiteral("CTRL+Page_Up"));
    QCOMPARE(t("Alt+Esc"), QStringLiteral("ALT+Escape"));
    QCOMPARE(t("Ctrl+Shift+Left"), QStringLiteral("CTRL+SHIFT+Left"));
    QCOMPARE(t("Ctrl+,"), QStringLiteral("CTRL+comma"));
    QCOMPARE(t("Ctrl+F12"), QStringLiteral("CTRL+F12"));
    QCOMPARE(t("Ctrl+1"), QStringLiteral("CTRL+1"));
    QCOMPARE(t("Ctrl+Backspace"), QStringLiteral("CTRL+BackSpace"));
    // Not expressible: no trigger is better than a name the portal rejects.
    QCOMPARE(t("Media Play"), QString());
    QCOMPARE(t("Ctrl+Volume Up"), QString());
}

void GlobalShortcutsPortalTest::globalShortcutsAreOffByDefault()
{
    SettingsManager settings;
    QVERIFY(!settings.globalShortcutsEnabled());
}

void GlobalShortcutsPortalTest::theSettingPersistsAndSignalsOnlyOnChange()
{
    {
        SettingsManager settings;
        QSignalSpy changed(&settings,
                           &SettingsManager::globalShortcutsEnabledChanged);
        settings.setGlobalShortcutsEnabled(false);
        QCOMPARE(changed.count(), 0);
        settings.setGlobalShortcutsEnabled(true);
        QCOMPARE(changed.count(), 1);
        settings.setGlobalShortcutsEnabled(true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(settings.globalShortcutsEnabled());
    }
    SettingsManager reopened;
    QVERIFY(reopened.globalShortcutsEnabled());
}

void GlobalShortcutsPortalTest::appControllerDrivesThePortalFromTheSetting()
{
    // AppController is too heavy to build here, so read its source: the
    // portal is created, enabled from the stored value at construction, and
    // enabled again on every change. Dropping either call, or the connect,
    // changes the normalised text below.
    QFile file(QStringLiteral(LIGHTNING_SOURCE_DIR "/src/app/AppController.cpp"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString text = QString::fromUtf8(file.readAll());
    text.remove(QRegularExpression(QStringLiteral("\\s")));
    QVERIFY(text.contains(QStringLiteral(
        "std::make_unique<GlobalShortcutsPortal>(m_shortcuts.get(),this)")));
    QVERIFY(text.contains(QStringLiteral(
        "connect(m_settings.get(),&SettingsManager::globalShortcutsEnabledChanged,")));
    QCOMPARE(text.count(QStringLiteral(
                 "m_globalShortcuts->setEnabled(m_settings->globalShortcutsEnabled())")),
             2);
}

void GlobalShortcutsPortalTest::theAppIdIsRegisteredBeforeTheSessionIsCreated()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    QCOMPARE(m_fake->count(QStringLiteral("Register")), 1);
    const QDBusMessage reg = m_fake->last(QStringLiteral("Register"));
    QCOMPARE(reg.interface(),
             QStringLiteral("org.freedesktop.host.portal.Registry"));
    QCOMPARE(reg.arguments().value(0).toString(),
             QStringLiteral("org.example.LightningTest"));
    QVERIFY(m_fake->firstIndex(QStringLiteral("Register")) >= 0);
    QVERIFY(m_fake->firstIndex(QStringLiteral("Register"))
            < m_fake->firstIndex(QStringLiteral("CreateSession")));
}

void GlobalShortcutsPortalTest::aPortalWithoutTheRegistryStillWorks()
{
    m_fake->reset(FakePortal::Mode::NoRegistry);
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    // Attempted, refused, and the session was made regardless.
    QCOMPARE(m_fake->count(QStringLiteral("Register")), 1);
    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 1);
    // With no app id to give there is nothing to register at all.
    QGuiApplication::setDesktopFileName(QString());
    m_fake->reset(FakePortal::Mode::Answer);
    GlobalShortcutsPortal bare(&registry);
    enableAndWait(bare);
    QGuiApplication::setDesktopFileName(
        QStringLiteral("org.example.LightningTest"));
    QCOMPARE(m_fake->count(QStringLiteral("Register")), 0);
}

void GlobalShortcutsPortalTest::aRestartedPortalIsNoticedAndTheSessionIsMadeAgain()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    QDBusConnection fakeBus(QStringLiteral("fake-portal"));
    // A portal restart ends every session without a Session.Closed.
    QVERIFY(fakeBus.unregisterService(kService));
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(),
                              GlobalShortcutsPortal::Unavailable, 5000);
    QVERIFY(!portal.canConfigure());
    QSignalSpy activated(&portal, &GlobalShortcutsPortal::activated);
    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("call.toggleMute"));
    QTest::qWait(200);
    QCOMPARE(activated.count(), 0);

    QVERIFY(fakeBus.registerService(kService));
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(), GlobalShortcutsPortal::Active,
                              5000);
    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 2);
    QCOMPARE(m_fake->count(QStringLiteral("BindShortcuts")), 2);
}

void GlobalShortcutsPortalTest::aBindThatBoundNothingIsNotActive()
{
    m_fake->reset(FakePortal::Mode::BindNothing);
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    QSignalSpy activated(&portal, &GlobalShortcutsPortal::activated);
    portal.setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->count(QStringLiteral("BindShortcuts")), 1,
                              5000);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(),
                              GlobalShortcutsPortal::Unavailable, 5000);
    QVERIFY(portal.statusText().contains(QStringLiteral("did not bind")));
    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("call.toggleMute"));
    QTest::qWait(200);
    QCOMPARE(activated.count(), 0);
}

void GlobalShortcutsPortalTest::destroyingAnActivePortalEmitsNothing()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    auto *portal = new GlobalShortcutsPortal(&registry);
    enableAndWait(*portal);
    QSignalSpy states(portal, &GlobalShortcutsPortal::stateChanged);
    delete portal;
    QCOMPARE(states.count(), 0);
    // It still closes the session it owned.
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->count(QStringLiteral("Close")), 1, 5000);
}

void GlobalShortcutsPortalTest::itIsOffAndTouchesNoBusUntilEnabled()
{
    // Opt-in: a fresh install must not talk to the portal at all.
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    QVERIFY(!settings.globalShortcutsEnabled());
    QCOMPARE(portal.state(), GlobalShortcutsPortal::Off);
    QTest::qWait(300);
    QCOMPARE(m_fake->count(QStringLiteral("Get")), 0);
    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 0);
    QVERIFY(!portal.canConfigure());
}

void GlobalShortcutsPortalTest::enablingBindsTheCallActionsWithTheRegistrysLabels()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    QSignalSpy states(&portal, &GlobalShortcutsPortal::stateChanged);
    portal.setEnabled(true);
    QCOMPARE(portal.state(), GlobalShortcutsPortal::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(), GlobalShortcutsPortal::Active,
                              5000);

    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 1);
    const QVariantList create =
        m_fake->last(QStringLiteral("CreateSession")).arguments();
    const QVariantMap createOptions =
        qdbus_cast<QVariantMap>(create.value(0).value<QDBusArgument>());
    QVERIFY(!createOptions.value("handle_token").toString().isEmpty());
    QVERIFY(!createOptions.value("session_handle_token").toString().isEmpty());

    QCOMPARE(m_fake->count(QStringLiteral("BindShortcuts")), 1);
    const QVariantList bind =
        m_fake->last(QStringLiteral("BindShortcuts")).arguments();
    QCOMPARE(bind.value(0).value<QDBusObjectPath>().path(), kSessionHandle);
    const QList<BoundShortcut> shortcuts =
        qdbus_cast<QList<BoundShortcut>>(bind.value(1).value<QDBusArgument>());
    QStringList ids;
    for (const BoundShortcut &s : shortcuts) {
        ids << s.id;
        // The desktop's list shows the registry's own label, and proposes
        // the registry's own key.
        QCOMPARE(s.properties.value("description").toString(),
                 registry.descriptionFor(s.id));
        QVERIFY(!s.properties.value("description").toString().isEmpty());
        QCOMPARE(s.properties.value("preferred_trigger").toString(),
                 GlobalShortcutsPortal::portalTrigger(
                     registry.sequenceFor(s.id)));
    }
    QCOMPARE(ids, GlobalShortcutsPortal::actionIds());
    QVERIFY(ids.contains(QStringLiteral("call.toggleMute")));
    QVERIFY(ids.contains(QStringLiteral("call.toggleDeafen")));
    QVERIFY(ids.contains(QStringLiteral("call.leave")));
    QCOMPARE(bind.value(2).toString(), QString());
    QVERIFY(!qdbus_cast<QVariantMap>(bind.value(3).value<QDBusArgument>())
                 .value("handle_token").toString().isEmpty());
}

void GlobalShortcutsPortalTest::aPressedActionIsRoutedAndAnotherSessionsIsNot()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    QSignalSpy activated(&portal, &GlobalShortcutsPortal::activated);

    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("call.toggleMute"));
    QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, 5000);
    QCOMPARE(activated.at(0).at(0).toString(),
             QStringLiteral("call.toggleMute"));

    // The broadcast reaches every client; a key of another app's session, or
    // an id we never registered, is none of our business. Signals arrive in
    // order, so once the last genuine press is in, anything the filter let
    // through is in too: assert the exact list, not a count to wait for.
    m_fake->emitKey(QStringLiteral("Activated"),
                    QStringLiteral("/org/freedesktop/portal/desktop/session/other/x"),
                    QStringLiteral("call.leave"));
    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("room.markRead"));
    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("call.toggleDeafen"));
    QTRY_VERIFY_WITH_TIMEOUT(activated.count() >= 2, 5000);
    QTest::qWait(300);
    QCOMPARE(activated.count(), 2);
    QCOMPARE(activated.at(0).at(0).toString(),
             QStringLiteral("call.toggleMute"));
    QCOMPARE(activated.at(1).at(0).toString(),
             QStringLiteral("call.toggleDeafen"));
}

void GlobalShortcutsPortalTest::releaseIsForwardedToo()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    QSignalSpy deactivated(&portal, &GlobalShortcutsPortal::deactivated);
    m_fake->emitKey(QStringLiteral("Deactivated"), kSessionHandle,
                    QStringLiteral("call.toggleDeafen"));
    QTRY_COMPARE_WITH_TIMEOUT(deactivated.count(), 1, 5000);
    QCOMPARE(deactivated.at(0).at(0).toString(),
             QStringLiteral("call.toggleDeafen"));
}

void GlobalShortcutsPortalTest::disablingClosesTheSessionAndStopsRouting()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    QSignalSpy activated(&portal, &GlobalShortcutsPortal::activated);

    portal.setEnabled(false);
    QCOMPARE(portal.state(), GlobalShortcutsPortal::Off);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->count(QStringLiteral("Close")), 1, 5000);
    QCOMPARE(m_fake->last(QStringLiteral("Close")).path(), kSessionHandle);

    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("call.toggleMute"));
    QTest::qWait(300);
    QCOMPARE(activated.count(), 0);

    // And it can be turned on again, with a fresh session.
    enableAndWait(portal);
    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 2);
}

void GlobalShortcutsPortalTest::turningItOffWhileTheSessionIsBeingMadeClosesIt()
{
    m_fake->reset(FakePortal::Mode::HoldResponses);
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    portal.setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->count(QStringLiteral("CreateSession")), 1,
                              5000);
    portal.setEnabled(false);
    // The portal answers late: the session it made must not be left open and
    // must not be bound.
    m_fake->releaseHeld();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->count(QStringLiteral("Close")), 1, 5000);
    QCOMPARE(m_fake->last(QStringLiteral("Close")).path(), kSessionHandle);
    QCOMPARE(m_fake->count(QStringLiteral("BindShortcuts")), 0);
    QCOMPARE(portal.state(), GlobalShortcutsPortal::Off);
}

void GlobalShortcutsPortalTest::aDesktopWithoutTheInterfaceSaysSo()
{
    m_fake->reset(FakePortal::Mode::NoInterface);
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    portal.setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(),
                              GlobalShortcutsPortal::Unavailable, 5000);
    QVERIFY(portal.statusText().contains(QStringLiteral("Plasma 6")));
    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 0);
    QVERIFY(!portal.canConfigure());
}

void GlobalShortcutsPortalTest::aMissingPortalSaysSo()
{
    QDBusConnection fakeBus(QStringLiteral("fake-portal"));
    QVERIFY(fakeBus.unregisterService(kService));
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    portal.setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(),
                              GlobalShortcutsPortal::Unavailable, 5000);
    QVERIFY(portal.statusText().contains(QStringLiteral("xdg-desktop-portal")));
    QCOMPARE(m_fake->count(QStringLiteral("CreateSession")), 0);
}

void GlobalShortcutsPortalTest::aDeclinedBindIsFailedAndRoutesNothing()
{
    m_fake->reset(FakePortal::Mode::Decline);
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    QSignalSpy activated(&portal, &GlobalShortcutsPortal::activated);
    portal.setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(), GlobalShortcutsPortal::Failed,
                              5000);
    QVERIFY(portal.statusText().contains(QStringLiteral("declined")));
    m_fake->emitKey(QStringLiteral("Activated"), kSessionHandle,
                    QStringLiteral("call.toggleMute"));
    QTest::qWait(300);
    QCOMPARE(activated.count(), 0);
}

void GlobalShortcutsPortalTest::aSessionTheDesktopEndsIsFailed()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    GlobalShortcutsPortal portal(&registry);
    enableAndWait(portal);
    m_fake->emitClosed(kSessionHandle);
    QTRY_COMPARE_WITH_TIMEOUT(portal.state(), GlobalShortcutsPortal::Failed,
                              5000);
    // The handle is dead, so there is nothing for us to close.
    QCOMPARE(m_fake->count(QStringLiteral("Close")), 0);
}

void GlobalShortcutsPortalTest::configureNeedsPortalVersionTwo()
{
    SettingsManager settings;
    ShortcutRegistry registry(&settings);
    {
        m_fake->reset(FakePortal::Mode::Answer, 1);
        GlobalShortcutsPortal portal(&registry);
        enableAndWait(portal);
        QVERIFY(!portal.canConfigure());
        portal.configure();
        QTest::qWait(300);
        QCOMPARE(m_fake->count(QStringLiteral("ConfigureShortcuts")), 0);
    }
    {
        m_fake->reset(FakePortal::Mode::Answer, 2);
        GlobalShortcutsPortal portal(&registry);
        enableAndWait(portal);
        QVERIFY(portal.canConfigure());
        portal.configure();
        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->count(QStringLiteral("ConfigureShortcuts")), 1, 5000);
        QCOMPARE(m_fake->last(QStringLiteral("ConfigureShortcuts"))
                     .arguments().value(0).value<QDBusObjectPath>().path(),
                 kSessionHandle);
    }
}

QTEST_MAIN(GlobalShortcutsPortalTest)
#include "GlobalShortcutsPortalTest.moc"
