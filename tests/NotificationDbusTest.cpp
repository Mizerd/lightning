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
        } else if (member == QLatin1String("Notify")) {
            if (m_holdNotify)
                m_held.append(message);
            else
                m_bus.send(message.createReply(QVariant(++m_nextId)));
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

private:
    QDBusConnection m_bus;
    mutable QMutex m_mutex;
    Mode m_mode = Mode::Silent;
    QStringList m_capabilities;
    bool m_holdNotify = false;
    int m_capabilityErrors = 0;
    QDBusError::ErrorType m_capabilityError = QDBusError::ServiceUnknown;
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
    manager.processEvent(incomingText(), context());
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
    QTest::qWait(200);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);

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
    manager.processEvent(incomingText(QStringLiteral("<b>Bob</b>")), context());
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
        manager.processEvent(incomingText(), context());
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

QTEST_MAIN(NotificationDbusTest)
#include "NotificationDbusTest.moc"
