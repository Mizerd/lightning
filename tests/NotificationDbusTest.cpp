// NotificationManager against a fake notification daemon on a private session
// bus. A daemon that owns org.freedesktop.Notifications but never answers (a
// hung or half-started activatable service) held the GUI thread for Qt's 25 s
// default per call: 75 s for one message, measured 2026-09-29 (introspection,
// GetCapabilities, Notify). Every call is asynchronous now.
// A private `dbus-daemon`, because the fake must own the name a real desktop
// already owns. The fake runs on its own thread so that, when it answers, it
// can answer a caller that blocks.

#include "notifications/NotificationManager.h"
#include "matrix/TimelineEvent.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QThread>

#include <optional>

namespace {

const auto kService = QStringLiteral("org.freedesktop.Notifications");
const auto kPath = QStringLiteral("/org/freedesktop/Notifications");
const auto kInterface = QStringLiteral("org.freedesktop.Notifications");
const auto kRoom = QStringLiteral("!room:example.org");

class FakeDaemon : public QDBusVirtualObject
{
public:
    // CapabilitiesOnly: answers GetCapabilities, then nothing else.
    // NoCapabilities: refuses GetCapabilities, answers the rest.
    enum class Mode { Silent, Answer, CapabilitiesOnly, NoCapabilities };

    explicit FakeDaemon(const QDBusConnection &bus) : m_bus(bus) {}

    QString introspect(const QString &) const override
    {
        // The real interface, so a caller that introspects gets its methods
        // and then waits on the call itself.
        return QStringLiteral(
            "<interface name=\"org.freedesktop.Notifications\">"
            "<method name=\"GetCapabilities\"><arg type=\"as\" direction=\"out\"/></method>"
            "<method name=\"Notify\">"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/>"
            "<arg type=\"a{sv}\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/>"
            "<arg type=\"u\" direction=\"out\"/></method>"
            "<method name=\"CloseNotification\"><arg type=\"u\" direction=\"in\"/></method>"
            "<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
            "<signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
            "</interface>"
            "<interface name=\"org.freedesktop.DBus.Properties\">"
            "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
            "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/>"
            "<arg type=\"as\"/></signal>"
            "</interface>");
    }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &) override
    {
        QMutexLocker lock(&m_mutex);
        m_calls.append(message);
        if (m_mode == Mode::Silent)
            return true; // never answered
        const QString member = message.member();
        if (m_mode == Mode::CapabilitiesOnly
            && member != QLatin1String("GetCapabilities"))
            return true;
        if (member == QLatin1String("GetCapabilities")) {
            if (m_capabilityErrors > 0) {
                --m_capabilityErrors;
                m_bus.send(message.createErrorReply(m_capabilityError,
                                                    QStringLiteral("not yet")));
            } else if (m_mode == Mode::NoCapabilities)
                m_bus.send(message.createErrorReply(
                    QDBusError::NotSupported, QStringLiteral("no")));
            else
                m_bus.send(message.createReply(QVariant(m_capabilities)));
        } else if (member == QLatin1String("Get")) {
            // KDE's Do Not Disturb; most daemons have no such property.
            if (m_inhibited.has_value()
                && message.arguments().value(1).toString()
                    == QLatin1String("Inhibited"))
                m_bus.send(message.createReply(
                    QVariant::fromValue(QDBusVariant(*m_inhibited))));
            else
                m_bus.send(message.createErrorReply(
                    QDBusError::UnknownProperty, QStringLiteral("no")));
        } else if (member == QLatin1String("Notify")) {
            if (m_holdNotify) {
                m_held.append(message);
            } else {
                // As the spec says: a replacement keeps the card's id.
                const quint32 replaces = message.arguments().value(1).toUInt();
                m_bus.send(message.createReply(
                    QVariant(replaces != 0 ? replaces : ++m_nextId)));
            }
        } else {
            m_bus.send(message.createReply());
        }
        return true;
    }

    void reset(Mode mode, const QStringList &capabilities = {})
    {
        QMutexLocker lock(&m_mutex);
        m_mode = mode;
        m_capabilities = capabilities;
        m_holdNotify = false;
        m_inhibited.reset();
        m_capabilityErrors = 0;
        m_held.clear();
        m_calls.clear();
    }
    // Answer the next `count` GetCapabilities with `type`, as a daemon that is
    // not up yet (ServiceUnknown) or not answering (NoReply).
    void failCapabilities(int count, QDBusError::ErrorType type)
    {
        QMutexLocker lock(&m_mutex);
        m_capabilityErrors = count;
        m_capabilityError = type;
    }
    // Answer Get(Inhibited) with this value; unset answers UnknownProperty.
    void setInhibited(bool inhibited)
    {
        QMutexLocker lock(&m_mutex);
        m_inhibited = inhibited;
    }
    void setHoldNotify(bool hold)
    {
        QMutexLocker lock(&m_mutex);
        m_holdNotify = hold;
    }
    // Answer the oldest held Notify with `id`.
    void releaseHeld(quint32 id)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_held.isEmpty())
            m_bus.send(m_held.takeFirst().createReply(QVariant(id)));
    }
    int count(const QString &member) const
    {
        QMutexLocker lock(&m_mutex);
        int n = 0;
        for (const QDBusMessage &call : m_calls)
            n += call.member() == member ? 1 : 0;
        return n;
    }
    QVariantList lastArguments(const QString &member) const
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_calls.crbegin(); it != m_calls.crend(); ++it) {
            if (it->member() == member)
                return it->arguments();
        }
        return {};
    }
    QList<QVariantList> allArguments(const QString &member) const
    {
        QMutexLocker lock(&m_mutex);
        QList<QVariantList> all;
        for (const QDBusMessage &call : m_calls) {
            if (call.member() == member)
                all.append(call.arguments());
        }
        return all;
    }
    QList<quint32> closedIds() const
    {
        QMutexLocker lock(&m_mutex);
        QList<quint32> ids;
        for (const QDBusMessage &call : m_calls) {
            if (call.member() == QLatin1String("CloseNotification"))
                ids.append(call.arguments().value(0).toUInt());
        }
        return ids;
    }
    void emitActionInvoked(quint32 id, const QString &action)
    {
        QDBusMessage signal =
            QDBusMessage::createSignal(kPath, kInterface,
                                       QStringLiteral("ActionInvoked"));
        signal << id << action;
        m_bus.send(signal);
    }
    void emitNotificationClosed(quint32 id, quint32 reason)
    {
        QDBusMessage signal =
            QDBusMessage::createSignal(kPath, kInterface,
                                       QStringLiteral("NotificationClosed"));
        signal << id << reason;
        m_bus.send(signal);
    }
    void emitInhibitedChanged(bool inhibited)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << kInterface
               << QVariantMap{ { QStringLiteral("Inhibited"), inhibited } }
               << QStringList{};
        m_bus.send(signal);
    }

private:
    QDBusConnection m_bus;
    mutable QMutex m_mutex;
    Mode m_mode = Mode::Silent;
    QStringList m_capabilities;
    bool m_holdNotify = false;
    int m_capabilityErrors = 0;
    QDBusError::ErrorType m_capabilityError = QDBusError::ServiceUnknown;
    std::optional<bool> m_inhibited;
    quint32 m_nextId = 40;
    QList<QDBusMessage> m_held;
    QList<QDBusMessage> m_calls;
};

TimelineEvent incomingText(const QString &senderName = QStringLiteral("Bob"))
{
    TimelineEvent event;
    event.eventId = QStringLiteral("$ev:example.org");
    event.roomId = kRoom;
    event.sender = QStringLiteral("@bob:example.org");
    event.senderDisplayName = senderName;
    event.body = QStringLiteral("hello");
    event.type = TimelineEvent::TextMessage;
    event.status = TimelineEvent::Sent;
    return event;
}

NotificationManager::Context context()
{
    NotificationManager::Context context;
    context.selfUserId = QStringLiteral("@alice:example.org");
    context.roomName = QStringLiteral("Lightning Dev");
    context.previewMode = NotificationManager::SenderOnly;
    return context;
}

} // namespace

class NotificationDbusTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void aDaemonThatNeverAnswersNeverBlocksTheCaller();
    void aDaemonThatStopsAnsweringNeverBlocksTheCaller();
    void anAnsweringDaemonShowsTheCardAndRoutesItsClick();
    void aCardAnsweredAfterSignOutIsNotRouted();
    void aDaemonWithoutCapabilitiesStillGetsEscapedText();
    void aDaemonNotUpYetIsAskedAgain();
    void aDaemonThatNeverComesUpIsAskedABoundedNumberOfTimes();
    void aCallCardIsRaisedOnceAndRetiredIfTheCallEnds();
    void theCardCarriesSuppressSoundExactlyWhenTheDesktopsSoundIsNotWanted();
    // 2026-10-15 notification sweep (package P6).
    void aMessageCardIsNormalUrgencyAndCarriesItsActions();
    void signingOutClosesEveryCardItRaised();
    void aRoomsNextMessageReplacesItsCard();
    void aDecryptedMessageReplacesItsEncryptedCard();
    // 2026-10-18 live C4b (Windows, KDE, dunst).
    void anEditFromTheLiveSyncReplacesTheShownCard();
    void aKeyedNoticeIsReplacedAndWithdrawn();
    void theCallCardIsQuietWhenSilentAndStaysDismissed();
    void theDesktopsDoNotDisturbSilencesTheChime();
    // Last: it hands the service name over and back.
    void aRestartedDaemonForgetsItsIds();

private:
    QProcess m_daemon;
    QThread m_fakeThread;
    FakeDaemon *m_fake = nullptr;
};

void NotificationDbusTest::initTestCase()
{
    const QString daemon = QStandardPaths::findExecutable("dbus-daemon");
    if (daemon.isEmpty())
        QSKIP("dbus-daemon not found; this suite needs a private bus");
    // As CameraPortalTest: the daemon's own session.conf, not /etc's.
    QStringList args{QStringLiteral("--nofork"),
                     QStringLiteral("--print-address=1")};
    const QString packaged = QFileInfo(daemon).absolutePath()
        + QStringLiteral("/../share/dbus-1/session.conf");
    if (QFileInfo::exists(packaged))
        args << QStringLiteral("--config-file=%1")
                    .arg(QFileInfo(packaged).canonicalFilePath());
    else
        args << QStringLiteral("--session");
    m_daemon.start(daemon, args);
    QVERIFY(m_daemon.waitForStarted(5000));
    QVERIFY(m_daemon.waitForReadyRead(5000));
    const QByteArray address = m_daemon.readLine().trimmed();
    QVERIFY(!address.isEmpty());

    // Read by QDBusConnection::sessionBus() on first use, which is later.
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    QVERIFY(QDBusConnection::sessionBus().isConnected());

    QDBusConnection fakeBus = QDBusConnection::connectToBus(
        QString::fromLatin1(address), QStringLiteral("fake-notifications"));
    QVERIFY(fakeBus.isConnected());
    QVERIFY(fakeBus.registerService(kService));
    m_fakeThread.start();
    m_fake = new FakeDaemon(fakeBus);
    // Its calls are handled on its own thread.
    m_fake->moveToThread(&m_fakeThread);
    QVERIFY(fakeBus.registerVirtualObject(kPath, m_fake));
}

void NotificationDbusTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-notifications"));
    // Deleted on its own thread, at the latest when the thread finishes.
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

namespace {
QVariantMap lastNotifyHints(const FakeDaemon *fake)
{
    const QVariantList args = fake->lastArguments(QStringLiteral("Notify"));
    return qdbus_cast<QVariantMap>(args.value(6).value<QDBusArgument>());
}
} // namespace

void NotificationDbusTest::theCardCarriesSuppressSoundExactlyWhenTheDesktopsSoundIsNotWanted()
{
    const QStringList caps{ QStringLiteral("body"), QStringLiteral("actions") };
    auto deliver = [&](int source, NotificationManager::SoundMode mode,
                       int *ownPlayed) {
        m_fake->reset(FakeDaemon::Mode::Answer, caps);
        NotificationManager manager;
        manager.setSoundSource(source);
        manager.setOwnSoundPlayer([ownPlayed](bool) {
            ++*ownPlayed;
            return true;
        });
        NotificationManager::Context ctx = context();
        ctx.soundMode = mode;
        manager.processEvent(incomingText(), ctx);
        // No QTRY macro: they return void and this returns the hints.
        QTest::qWaitFor([&] {
            return m_fake->count(QStringLiteral("Notify")) == 1;
        }, 5000);
        return lastNotifyHints(m_fake);
    };

    // Lightning's chime: ours plays once, the desktop's is told to stay quiet.
    int own = 0;
    QVariantMap hints = deliver(NotificationManager::SourceLightning,
                                NotificationManager::SoundAll, &own);
    QCOMPARE(own, 1);
    QCOMPARE(hints.value(QStringLiteral("suppress-sound")).toBool(), true);
    QVERIFY(!hints.contains(QStringLiteral("sound-name")));

    // System default: the desktop's themed sound, nothing of ours.
    own = 0;
    hints = deliver(NotificationManager::SourceSystem,
                    NotificationManager::SoundAll, &own);
    QCOMPARE(own, 0);
    QVERIFY(!hints.contains(QStringLiteral("suppress-sound")));
    QCOMPARE(hints.value(QStringLiteral("sound-name")).toString(),
             QStringLiteral("message-new-instant"));

    // Sound off: silent, and the desktop is told not to add one of its own.
    own = 0;
    hints = deliver(NotificationManager::SourceLightning,
                    NotificationManager::SoundOff, &own);
    QCOMPARE(own, 0);
    QCOMPARE(hints.value(QStringLiteral("suppress-sound")).toBool(), true);
    QVERIFY(!hints.contains(QStringLiteral("sound-name")));
}

void NotificationDbusTest::aDaemonThatNeverAnswersNeverBlocksTheCaller()
{
    m_fake->reset(FakeDaemon::Mode::Silent);
    NotificationManager manager;
    QElapsedTimer timer;
    timer.start();
    // Every entry point that talks to the daemon.
    manager.processEvent(incomingText(), context());
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"), kRoom);
    manager.showIncomingCall(kRoom, QStringLiteral("$call"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"), true, 30);
    manager.closeRoomNotifications(kRoom);
    manager.stopIncomingCall(QStringLiteral("$call"));
    const qint64 elapsed = timer.elapsed();
    // Synchronous calls sat here for 25 s each.
    QVERIFY2(elapsed < 1000,
             qPrintable(QStringLiteral("the caller waited %1 ms").arg(elapsed)));
    // The daemon was still asked, asynchronously, and only once.
    QTRY_COMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 1);
}

void NotificationDbusTest::aDaemonThatStopsAnsweringNeverBlocksTheCaller()
{
    // The capabilities arrive; Notify and CloseNotification never do.
    m_fake->reset(FakeDaemon::Mode::CapabilitiesOnly);
    NotificationManager manager;
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QElapsedTimer timer;
    timer.start();
    // Another room: a second message to the same room would wait for the
    // first card's id (one card per room) and send nothing more here.
    TimelineEvent other = incomingText();
    other.roomId = QStringLiteral("!other:example.org");
    manager.processEvent(other, context());
    manager.showIncomingCall(kRoom, QStringLiteral("$call"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"), true, 30);
    manager.recordPayloadForTest(9, QVariantMap{
        { QStringLiteral("roomId"), kRoom },
        { QStringLiteral("eventId"), QStringLiteral("$ev:example.org") },
        { QStringLiteral("threadRootId"), QString() } });
    manager.closeRoomNotifications(kRoom);
    manager.stopIncomingCall(QStringLiteral("$call"));
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 1000,
             qPrintable(QStringLiteral("the caller waited %1 ms").arg(elapsed)));
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 3);
    QTRY_VERIFY(m_fake->closedIds().contains(9));
}

void NotificationDbusTest::anAnsweringDaemonShowsTheCardAndRoutesItsClick()
{
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions"),
                    QStringLiteral("body-markup") });
    NotificationManager manager;
    QSignalSpy open(&manager, &NotificationManager::openRequested);
    // A member-chosen name with markup in it.
    manager.processEvent(incomingText(QStringLiteral("<b>Bob</b>")), context());
    // The card is raised once the capabilities are known, and its reply id
    // is what a click routes by.
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    QCOMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    const QVariantList notify = m_fake->lastArguments(QStringLiteral("Notify"));
    QCOMPARE(notify.size(), 8);
    // Escaped for a daemon that renders markup; never sent raw.
    QVERIFY2(notify.at(4).toString().contains(QStringLiteral("&lt;b&gt;Bob")),
             qPrintable(notify.at(4).toString()));
    const quint32 id = 41; // the fake's first id
    m_fake->emitActionInvoked(id, QStringLiteral("default"));
    QTRY_COMPARE(open.size(), 1);
    QCOMPARE(open.first().at(0).toString(), kRoom);
    // Reading the room closes the card on the daemon without waiting.
    manager.recordPayloadForTest(id, QVariantMap{
        { QStringLiteral("roomId"), kRoom },
        { QStringLiteral("eventId"), QStringLiteral("$ev:example.org") },
        { QStringLiteral("threadRootId"), QString() } });
    manager.closeRoomNotifications(kRoom);
    QTRY_VERIFY(m_fake->closedIds().contains(id));
}

void NotificationDbusTest::aCardAnsweredAfterSignOutIsNotRouted()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    m_fake->setHoldNotify(true);
    NotificationManager manager;
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    // Sign-out while the daemon is still raising the card: its id must not
    // route a click into the next account.
    manager.clearPending();
    m_fake->releaseHeld(80);
    // Evidence the stale reply was processed, rather than a fixed wait it may
    // outlast under load (finding 150).
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 1);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    // And the card it raised is withdrawn: its preview must not outlive the
    // account (finding 117).
    QTRY_VERIFY(m_fake->closedIds().contains(80));

    // The control: the same card answered before a sign-out is routed.
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    m_fake->releaseHeld(81);
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
}

void NotificationDbusTest::aDaemonWithoutCapabilitiesStillGetsEscapedText()
{
    // Unknown capabilities must not mean raw member text: the daemon may
    // render markup.
    m_fake->reset(FakeDaemon::Mode::NoCapabilities);
    NotificationManager manager;
    manager.processEvent(incomingText(QStringLiteral("<b>Bob</b>")), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    const QVariantList notify = m_fake->lastArguments(QStringLiteral("Notify"));
    QVERIFY2(notify.value(4).toString().contains(QStringLiteral("&lt;b&gt;Bob")),
             qPrintable(notify.value(4).toString()));
    QVERIFY(!manager.inlineReplySupported());
}

void NotificationDbusTest::aDaemonNotUpYetIsAskedAgain()
{
    // Review B2: a daemon still starting (autostart order) must not fix
    // "escape, no inline reply" for the whole session.
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions"),
                    QStringLiteral("inline-reply") });
    m_fake->failCapabilities(1, QDBusError::ServiceUnknown);
    NotificationManager manager;
    manager.setCapabilityRetryMsForTest(100);
    manager.processEvent(incomingText(QStringLiteral("<b>Bob</b>")), context());
    // Delivered anyway, escaped, while the capabilities are unknown.
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QVERIFY(m_fake->lastArguments(QStringLiteral("Notify")).value(4).toString()
                .contains(QStringLiteral("&lt;b&gt;Bob")));
    QVERIFY(!manager.inlineReplySupported());

    QTest::qWait(200);
    // Another room: a second message in the same room is folded into that
    // room's card as "2 new messages" (one card per room), which would hide
    // the body this case is about.
    TimelineEvent second = incomingText(QStringLiteral("<b>Bob</b>"));
    second.roomId = QStringLiteral("!second:example.org");
    second.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(second, context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 2);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    // The answer is used: no body-markup, so the text goes as it is, and the
    // inline reply is offered.
    const QString body =
        m_fake->lastArguments(QStringLiteral("Notify")).value(4).toString();
    QVERIFY2(body.contains(QStringLiteral("<b>Bob</b>")), qPrintable(body));
    QVERIFY(manager.inlineReplySupported());
}

void NotificationDbusTest::aDaemonThatNeverComesUpIsAskedABoundedNumberOfTimes()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    m_fake->failCapabilities(1000, QDBusError::NoReply);
    NotificationManager manager;
    manager.setCapabilityRetryMsForTest(0);
    for (int i = 0; i < 6; ++i) {
        // A room each: one card per room would otherwise fold a message that
        // arrives while its room's card is being raised into the next one.
        TimelineEvent event = incomingText();
        event.roomId = QStringLiteral("!room%1:example.org").arg(i);
        manager.processEvent(event, context());
        QTest::qWait(50);
    }
    // Every message is still delivered, and the daemon is asked at most four
    // times.
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 6);
    QTest::qWait(200);
    QCOMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 4);
    QCOMPARE(manager.capabilityRequestsForTest(), 4);
}

void NotificationDbusTest::aCallCardIsRaisedOnceAndRetiredIfTheCallEnds()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    m_fake->setHoldNotify(true);
    NotificationManager manager;
    manager.showIncomingCall(kRoom, QStringLiteral("$call"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"), true, 30,
                             /*acceptOffered=*/true);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    // A redraw while the card's id is still unknown waits for it rather than
    // raising a second card (replaces_id 0 again).
    manager.setCallAcceptOffered(QStringLiteral("$call"), false, false);
    QTest::qWait(200);
    QCOMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    m_fake->releaseHeld(60);
    // The redraw follows the answer and replaces that card.
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    QCOMPARE(m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt(),
             60u);
    m_fake->releaseHeld(60);
    manager.stopIncomingCall(QStringLiteral("$call"));
    QTRY_VERIFY(m_fake->closedIds().contains(60));

    // A call that ends before its first card's id is known: nothing can be
    // closed yet, and the card the daemon then raises is closed on arrival.
    manager.showIncomingCall(kRoom, QStringLiteral("$call2"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"), true, 30);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 3);
    QCOMPARE(m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt(),
             0u);
    manager.stopIncomingCall(QStringLiteral("$call2"));
    QTest::qWait(100);
    QVERIFY(!m_fake->closedIds().contains(70));
    m_fake->releaseHeld(70);
    QTRY_VERIFY(m_fake->closedIds().contains(70));
    QVERIFY(manager.activeCallIdForTest().isEmpty());
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

// ── 2026-10-15 notification sweep (P6) ─────────────────────────────────────

namespace {
QVariantMap hintsOf(const QVariantList &notifyArgs)
{
    return qdbus_cast<QVariantMap>(notifyArgs.value(6).value<QDBusArgument>());
}
} // namespace

// Findings 22/149: the actions and urgency a card actually carries over the
// bus. A mention used to be critical (2), which GNOME and KDE let through Do
// Not Disturb and never expire; nothing asserted either.
void NotificationDbusTest::aMessageCardIsNormalUrgencyAndCarriesItsActions()
{
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions"),
                    QStringLiteral("inline-reply") });
    NotificationManager manager;
    TimelineEvent mention = incomingText();
    mention.mentionsMe = true;
    manager.processEvent(mention, context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QVariantList args = m_fake->lastArguments(QStringLiteral("Notify"));
    QCOMPARE(args.value(5).toStringList(),
             NotificationManager::messageCardShape(true, true).actions);
    QVERIFY(args.value(5).toStringList().contains(QStringLiteral("mark-read")));
    QVERIFY(args.value(5).toStringList().contains(QStringLiteral("inline-reply")));
    QVariantMap hints = hintsOf(args);
    QVERIFY2(hints.value(QStringLiteral("urgency")).toInt() == 1,
             "a mention card was not normal urgency");
    QVERIFY(hints.contains(QStringLiteral("x-kde-reply-placeholder-text")));

    // A notice: open only, normal urgency, no reply box.
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"), kRoom);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    args = m_fake->lastArguments(QStringLiteral("Notify"));
    QCOMPARE(args.value(5).toStringList(),
             (QStringList{ QStringLiteral("default"), QObject::tr("Open") }));
    hints = hintsOf(args);
    QCOMPARE(hints.value(QStringLiteral("urgency")).toInt(), 1);
    QVERIFY(!hints.contains(QStringLiteral("x-kde-reply-placeholder-text")));
}

// Findings 114/117: sign-out, removal and a switch left every card of the
// account (with "Sender and message" previews) in the history for good.
void NotificationDbusTest::signingOutClosesEveryCardItRaised()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    manager.processEvent(incomingText(), context());
    TimelineEvent other = incomingText();
    other.roomId = QStringLiteral("!other:example.org");
    manager.processEvent(other, context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 2);
    const QList<QVariantList> raised =
        m_fake->allArguments(QStringLiteral("Notify"));
    QCOMPARE(raised.size(), 2);
    QVERIFY(m_fake->closedIds().isEmpty());
    manager.clearPending();
    QTRY_COMPARE(m_fake->closedIds().size(), 2);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

// Findings 24/123/25: every message was a new card (replaces_id 0): 30
// messages, 30 cards, and past 64 the oldest could never be withdrawn.
void NotificationDbusTest::aRoomsNextMessageReplacesItsCard()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    const quint32 first =
        m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt();
    QCOMPARE(first, 0u);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 1);
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    const quint32 replaces =
        m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt();
    QVERIFY2(replaces != 0, "the room's second message raised a second card");
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 2);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    // While a room's first card is still being raised, its next message waits
    // for the id instead of raising a duplicate.
    m_fake->setHoldNotify(true);
    TimelineEvent a = incomingText();
    a.roomId = QStringLiteral("!busy:example.org");
    manager.processEvent(a, context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 3);
    manager.processEvent(a, context());
    manager.processEvent(a, context());
    m_fake->releaseHeld(500);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 4);
    QCOMPARE(m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt(),
             500u);
    m_fake->releaseHeld(500);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 4);
    QCOMPARE(m_fake->count(QStringLiteral("Notify")), 4);
}

// Findings 46/124/32: "Encrypted message" stayed for good even when the key
// arrived a second later. The card is replaced in place, silently.
void NotificationDbusTest::aDecryptedMessageReplacesItsEncryptedCard()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    manager.setUndecryptableHoldMsForTest(0);
    NotificationManager::Context ctx = context();
    ctx.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent utd = incomingText();
    utd.eventId = QStringLiteral("$utd:example.org");
    utd.isEncrypted = true;
    utd.undecryptable = true;
    manager.processEvent(utd, ctx);
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    QVariantList args = m_fake->lastArguments(QStringLiteral("Notify"));
    QVERIFY(!args.value(4).toString().contains(QStringLiteral("hello")));
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 1);

    TimelineEvent plain = incomingText();
    plain.eventId = utd.eventId;
    plain.isEncrypted = true;
    plain.body = QStringLiteral("the real text");
    manager.processDecrypted(plain, ctx);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    args = m_fake->lastArguments(QStringLiteral("Notify"));
    QVERIFY2(args.value(1).toUInt() != 0,
             "the decrypted text raised a second card");
    QVERIFY2(args.value(4).toString().contains(QStringLiteral("the real text")),
             qPrintable(args.value(4).toString()));
    // It alerted once already.
    QCOMPARE(hintsOf(args).value(QStringLiteral("suppress-sound")).toBool(),
             true);
}

// Live C4b, 2026-10-18, on all three desktops that could show it: a card
// raised for a message in a room with no open timeline kept the ORIGINAL text
// after the sender edited it, while the room list already showed the edit.
// Such a room has no SDK aggregate (no Set reaches processUpdated); its edit
// arrives through the live sync as a raw m.replace, which processEvent()
// dropped as a suppressed edit. The card is replaced in place (replaces_id),
// silently, and only by the original sender's own edit.
void NotificationDbusTest::anEditFromTheLiveSyncReplacesTheShownCard()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    NotificationManager::Context ctx = context();
    ctx.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent original = incomingText();
    original.body = QStringLiteral("toast orig text");
    manager.processEvent(original, ctx);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 1);
    QCOMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    // What RustTimelineIngest makes of the sync's m.replace: the "* "
    // fallback as body, m.new_content beside it, and the account's
    // suppress-edits rule as the verdict.
    const auto editOf = [&original](const QString &id, const QString &sender,
                                    const QString &text, qint64 ms) {
        TimelineEvent edit = incomingText();
        edit.eventId = id;
        edit.sender = sender;
        edit.body = QStringLiteral("* ") + text;
        edit.isEdit = true;
        edit.replacesEventId = original.eventId;
        edit.editNewBody = text;
        edit.editNewBodyAvailable = true;
        edit.pushVerdict = TimelineEvent::PushVerdict::Quiet;
        edit.timestamp = QDateTime::fromMSecsSinceEpoch(ms);
        return edit;
    };
    QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
    manager.processEvent(editOf(QStringLiteral("$e1:example.org"),
                                original.sender,
                                QStringLiteral("toast EDITED text"), 2000),
                         ctx);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    QVariantList args = m_fake->lastArguments(QStringLiteral("Notify"));
    const quint32 shown = args.value(1).toUInt();
    QVERIFY2(shown != 0,
             "the edit raised a second card instead of replacing the shown one");
    QVERIFY2(args.value(4).toString().contains(QStringLiteral("toast EDITED text")),
             qPrintable(args.value(4).toString()));
    QVERIFY(!args.value(4).toString().contains(QStringLiteral("* ")));
    // It alerted once already; an edit is not news.
    QCOMPARE(hintsOf(args).value(QStringLiteral("suppress-sound")).toBool(),
             true);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 2);
    QCOMPARE(delivered.count(), 0);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    // Another member's m.replace of that message, and an older edit delivered
    // late, change nothing. The next valid edit is the third Notify, so the
    // two above were not merely slow.
    manager.processEvent(editOf(QStringLiteral("$e2:example.org"),
                                QStringLiteral("@mallory:example.org"),
                                QStringLiteral("words put in Bob's mouth"), 3000),
                         ctx);
    manager.processEvent(editOf(QStringLiteral("$e0:example.org"),
                                original.sender,
                                QStringLiteral("an older edit"), 1500),
                         ctx);
    manager.processEvent(editOf(QStringLiteral("$e3:example.org"),
                                original.sender,
                                QStringLiteral("second edit"), 4000),
                         ctx);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 3);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 3);
    const QList<QVariantList> all = m_fake->allArguments(QStringLiteral("Notify"));
    QCOMPARE(all.size(), 3);
    QCOMPARE(all.at(2).value(1).toUInt(), shown);
    QVERIFY2(all.at(2).value(4).toString().contains(QStringLiteral("second edit")),
             qPrintable(all.at(2).value(4).toString()));
    for (const QVariantList &call : all) {
        QVERIFY(!call.value(4).toString().contains(QStringLiteral("mallory")));
        QVERIFY(!call.value(4).toString().contains(QStringLiteral("mouth")));
        QVERIFY(!call.value(4).toString().contains(QStringLiteral("older edit")));
    }
}

// Findings 80/125: an invite or verification card had no way to be
// withdrawn once the thing it announced was over.
void NotificationDbusTest::aKeyedNoticeIsReplacedAndWithdrawn()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    manager.showGeneric(QStringLiteral("Verification"),
                        QStringLiteral("A session wants to verify"), QString(),
                        QString(), false, QStringLiteral("verify:1"));
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 1);
    manager.showGeneric(QStringLiteral("Verification"),
                        QStringLiteral("Still waiting"), QString(), QString(),
                        false, QStringLiteral("verify:1"));
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    const quint32 id =
        m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt();
    QVERIFY(id != 0);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 2);
    manager.withdrawGeneric(QStringLiteral("verify:1"));
    QTRY_VERIFY(m_fake->closedIds().contains(id));
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

// Findings 28/109: the call card sent no suppress-sound, so a daemon with a
// default sound played it over Lightning's ringer. Finding 29: a card the
// user dismissed came back when the call's join state changed. Finding 82:
// the dismissal did not reach the ringer.
void NotificationDbusTest::theCallCardIsQuietWhenSilentAndStaysDismissed()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    QSignalSpy dismissed(&manager, &NotificationManager::callCardDismissed);
    manager.showIncomingCall(kRoom, QStringLiteral("$call"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"),
                             /*sound=*/false, 30);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    const QVariantMap hints = lastNotifyHints(m_fake);
    QCOMPARE(hints.value(QStringLiteral("suppress-sound")).toBool(), true);
    QCOMPARE(hints.value(QStringLiteral("urgency")).toInt(), 2);
    QTRY_VERIFY(manager.activeCallNotificationIdForTest() != 0);
    const quint32 id = manager.activeCallNotificationIdForTest();

    m_fake->emitNotificationClosed(id, 2);
    QTRY_COMPARE(dismissed.count(), 1);
    QCOMPARE(dismissed.first().at(0).toString(), QStringLiteral("$call"));
    QCOMPARE(manager.activeCallNotificationIdForTest(), 0u);

    // The join state changes; then a message is the marker. The bus keeps
    // order, so a redraw would arrive before it.
    manager.setCallAcceptOffered(QStringLiteral("$call"), true, true);
    TimelineEvent marker = incomingText();
    marker.roomId = QStringLiteral("!marker:example.org");
    manager.processEvent(marker, context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    const QList<QVariantList> all =
        m_fake->allArguments(QStringLiteral("Notify"));
    QVERIFY2(!all.at(1).value(5).toStringList().contains(
                 QStringLiteral("decline")),
             "a dismissed call card was raised again");
    manager.stopIncomingCall(QStringLiteral("$call"));
}

// Findings 23/104: Lightning's chime ignored the desktop's Do Not Disturb.
void NotificationDbusTest::theDesktopsDoNotDisturbSilencesTheChime()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    m_fake->setInhibited(true);
    NotificationManager manager;
    int own = 0;
    manager.setOwnSoundPlayer([&own](bool) { ++own; return true; });
    NotificationManager::Context ctx = context();
    ctx.soundMode = NotificationManager::SoundAll;
    manager.processEvent(incomingText(), ctx);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QVERIFY(manager.desktopInhibited());
    QVERIFY2(own == 0, "the chime played under Do Not Disturb");
    QCOMPARE(lastNotifyHints(m_fake).value(QStringLiteral("suppress-sound"))
                 .toBool(),
             true);
    // Do Not Disturb ends; the next message chimes.
    m_fake->emitInhibitedChanged(false);
    QTRY_VERIFY(!manager.desktopInhibited());
    TimelineEvent next = incomingText();
    next.roomId = QStringLiteral("!next:example.org");
    manager.processEvent(next, ctx);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    QCOMPARE(own, 1);
}

// Finding 26: after a daemon restart our ids named other applications'
// cards (and our clicks routed theirs), and the capabilities were never
// asked again.
void NotificationDbusTest::aRestartedDaemonForgetsItsIds()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    QCOMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 1);

    QDBusConnection fakeBus(QStringLiteral("fake-notifications"));
    QVERIFY(fakeBus.unregisterService(kService));
    QVERIFY(fakeBus.registerService(kService));
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 0);
    QCOMPARE(manager.capabilityRequestsForTest(), 0);

    TimelineEvent next = incomingText();
    next.roomId = QStringLiteral("!next:example.org");
    manager.processEvent(next, context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 2);
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
}

QTEST_MAIN(NotificationDbusTest)
#include "NotificationDbusTest.moc"
