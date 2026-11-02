
#include "notifications/NotificationManager.h"
#include "matrix/TimelineEvent.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QHash>
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
    enum class Mode { Silent, Answer, CapabilitiesOnly, NoCapabilities };

    explicit FakeDaemon(const QDBusConnection &bus) : m_bus(bus) {}

    QString introspect(const QString &) const override
    {
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
            "<method name=\"GetServerInformation\"><arg type=\"s\" direction=\"out\"/>"
            "<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
            "<arg type=\"s\" direction=\"out\"/></method>"
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
            return true;
        const QString member = message.member();
        if (m_mode == Mode::CapabilitiesOnly
            && member != QLatin1String("GetCapabilities")
            && member != QLatin1String("GetServerInformation"))
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
        } else if (member == QLatin1String("GetServerInformation")) {
            if (m_serverInfo.size() == 4) {
                QDBusMessage reply = message.createReply();
                for (const QString &field : std::as_const(m_serverInfo))
                    reply << field;
                m_bus.send(reply);
            } else {
                m_bus.send(message.createErrorReply(
                    QDBusError::UnknownMethod, QStringLiteral("no")));
            }
        } else if (member == QLatin1String("Get")) {
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
                const quint32 replaces = message.arguments().value(1).toUInt();
                const quint32 id = replaces != 0 ? replaces : ++m_nextId;
                m_hintsById.insert(id, qdbus_cast<QVariantMap>(
                    message.arguments().value(6).value<QDBusArgument>()));
                m_lastId = id;
                m_bus.send(message.createReply(QVariant(id)));
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
        m_hintsById.clear();
        m_lastId = 0;
        m_serverInfo.clear();
    }
    void setServerInformation(const QStringList &info)
    {
        QMutexLocker lock(&m_mutex);
        m_serverInfo = info;
    }
    quint32 lastId() const
    {
        QMutexLocker lock(&m_mutex);
        return m_lastId;
    }
    QVariantMap hintsFor(quint32 id) const
    {
        QMutexLocker lock(&m_mutex);
        return m_hintsById.value(id);
    }
    void plasma63PopupTimesOutThenHistoryClick(quint32 id)
    {
        const bool resident =
            hintsFor(id).value(QStringLiteral("resident")).toBool();
        if (!resident) {
            emitNotificationClosed(id, 1);
            return;
        }
        emitActionInvoked(id, QStringLiteral("default"));
    }
    void failCapabilities(int count, QDBusError::ErrorType type)
    {
        QMutexLocker lock(&m_mutex);
        m_capabilityErrors = count;
        m_capabilityError = type;
    }
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
    void emitNotificationReplied(quint32 id, const QString &text)
    {
        QDBusMessage signal =
            QDBusMessage::createSignal(kPath, kInterface,
                                       QStringLiteral("NotificationReplied"));
        signal << id << text;
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
    quint32 m_lastId = 0;
    QStringList m_serverInfo;
    QHash<quint32, QVariantMap> m_hintsById;
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

}

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
    void aMessageCardIsNormalUrgencyAndCarriesItsActions();
    void signingOutClosesEveryCardItRaised();
    void aRoomsNextMessageReplacesItsCard();
    void aDecryptedMessageReplacesItsEncryptedCard();
    void anEditFromTheLiveSyncReplacesTheShownCard();
    void aKeyedNoticeIsReplacedAndWithdrawn();
    void theCallCardIsQuietWhenSilentAndStaysDismissed();
    void theDesktopsDoNotDisturbSilencesTheChime();
    void aCardStillOpensItsRoomFromTheHistoryAfterItsPopupTimedOut();
    void aCardAnsweredFromTheHistoryIsWithdrawnAndNoOtherIs();
    void onlyPlasmaBefore64GetsResidentCards();
    void theCallCardIsNeitherResidentNorClosedByItsClick();
    void aRepliedOrStaleCardIsWithdrawn();
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

    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    QVERIFY(QDBusConnection::sessionBus().isConnected());

    QDBusConnection fakeBus = QDBusConnection::connectToBus(
        QString::fromLatin1(address), QStringLiteral("fake-notifications"));
    QVERIFY(fakeBus.isConnected());
    QVERIFY(fakeBus.registerService(kService));
    m_fakeThread.start();
    m_fake = new FakeDaemon(fakeBus);
    m_fake->moveToThread(&m_fakeThread);
    QVERIFY(fakeBus.registerVirtualObject(kPath, m_fake));
}

void NotificationDbusTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-notifications"));
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
}

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
        QTest::qWaitFor([&] {
            return m_fake->count(QStringLiteral("Notify")) == 1;
        }, 5000);
        return lastNotifyHints(m_fake);
    };

    int own = 0;
    QVariantMap hints = deliver(NotificationManager::SourceLightning,
                                NotificationManager::SoundAll, &own);
    QCOMPARE(own, 1);
    QCOMPARE(hints.value(QStringLiteral("suppress-sound")).toBool(), true);
    QVERIFY(!hints.contains(QStringLiteral("sound-name")));

    own = 0;
    hints = deliver(NotificationManager::SourceSystem,
                    NotificationManager::SoundAll, &own);
    QCOMPARE(own, 0);
    QVERIFY(!hints.contains(QStringLiteral("suppress-sound")));
    QCOMPARE(hints.value(QStringLiteral("sound-name")).toString(),
             QStringLiteral("message-new-instant"));

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
    manager.processEvent(incomingText(), context());
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"), kRoom);
    manager.showIncomingCall(kRoom, QStringLiteral("$call"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"), true, 30);
    manager.closeRoomNotifications(kRoom);
    manager.stopIncomingCall(QStringLiteral("$call"));
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 1000,
             qPrintable(QStringLiteral("the caller waited %1 ms").arg(elapsed)));
    QTRY_COMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 1);
}

void NotificationDbusTest::aDaemonThatStopsAnsweringNeverBlocksTheCaller()
{
    m_fake->reset(FakeDaemon::Mode::CapabilitiesOnly);
    NotificationManager manager;
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QElapsedTimer timer;
    timer.start();
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
    manager.processEvent(incomingText(QStringLiteral("<b>Bob</b>")), context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    QCOMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    const QVariantList notify = m_fake->lastArguments(QStringLiteral("Notify"));
    QCOMPARE(notify.size(), 8);
    QVERIFY2(notify.at(4).toString().contains(QStringLiteral("&lt;b&gt;Bob")),
             qPrintable(notify.at(4).toString()));
    const quint32 id = 41;
    m_fake->emitActionInvoked(id, QStringLiteral("default"));
    QTRY_COMPARE(open.size(), 1);
    QCOMPARE(open.first().at(0).toString(), kRoom);
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
    manager.clearPending();
    m_fake->releaseHeld(80);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 1);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    QTRY_VERIFY(m_fake->closedIds().contains(80));

    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    m_fake->releaseHeld(81);
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
}

void NotificationDbusTest::aDaemonWithoutCapabilitiesStillGetsEscapedText()
{
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
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions"),
                    QStringLiteral("inline-reply") });
    m_fake->failCapabilities(1, QDBusError::ServiceUnknown);
    NotificationManager manager;
    manager.setCapabilityRetryMsForTest(100);
    manager.processEvent(incomingText(QStringLiteral("<b>Bob</b>")), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    QVERIFY(m_fake->lastArguments(QStringLiteral("Notify")).value(4).toString()
                .contains(QStringLiteral("&lt;b&gt;Bob")));
    QVERIFY(!manager.inlineReplySupported());

    QTest::qWait(200);
    TimelineEvent second = incomingText(QStringLiteral("<b>Bob</b>"));
    second.roomId = QStringLiteral("!second:example.org");
    second.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(second, context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("GetCapabilities")), 2);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
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
        TimelineEvent event = incomingText();
        event.roomId = QStringLiteral("!room%1:example.org").arg(i);
        manager.processEvent(event, context());
        QTest::qWait(50);
    }
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
true);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    manager.setCallAcceptOffered(QStringLiteral("$call"), false, false);
    QTest::qWait(200);
    QCOMPARE(m_fake->count(QStringLiteral("Notify")), 1);
    m_fake->releaseHeld(60);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    QCOMPARE(m_fake->lastArguments(QStringLiteral("Notify")).value(1).toUInt(),
             60u);
    m_fake->releaseHeld(60);
    manager.stopIncomingCall(QStringLiteral("$call"));
    QTRY_VERIFY(m_fake->closedIds().contains(60));

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

namespace {
QVariantMap hintsOf(const QVariantList &notifyArgs)
{
    return qdbus_cast<QVariantMap>(notifyArgs.value(6).value<QDBusArgument>());
}
}

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
    QCOMPARE(hintsOf(args).value(QStringLiteral("suppress-sound")).toBool(),
             true);
}

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
    QCOMPARE(hintsOf(args).value(QStringLiteral("suppress-sound")).toBool(),
             true);
    QTRY_COMPARE(manager.notifyRepliesProcessedForTest(), 2);
    QCOMPARE(delivered.count(), 0);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

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

void NotificationDbusTest::theCallCardIsQuietWhenSilentAndStaysDismissed()
{
    m_fake->reset(FakeDaemon::Mode::Answer);
    NotificationManager manager;
    QSignalSpy dismissed(&manager, &NotificationManager::callCardDismissed);
    manager.showIncomingCall(kRoom, QStringLiteral("$call"),
                             QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"),
false, 30);
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
    m_fake->emitInhibitedChanged(false);
    QTRY_VERIFY(!manager.desktopInhibited());
    TimelineEvent next = incomingText();
    next.roomId = QStringLiteral("!next:example.org");
    manager.processEvent(next, ctx);
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    QCOMPARE(own, 1);
}

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
    QTRY_VERIFY(m_fake->count(QStringLiteral("GetCapabilities")) >= 2);
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
}

void NotificationDbusTest::aCardStillOpensItsRoomFromTheHistoryAfterItsPopupTimedOut()
{
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions"),
                    QStringLiteral("persistence") });
    m_fake->setServerInformation({ QStringLiteral("Plasma"), QStringLiteral("KDE"),
                                   QStringLiteral("6.3.5"), QStringLiteral("1.2") });
    NotificationManager manager;
    QSignalSpy open(&manager, &NotificationManager::openRequested);
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    const quint32 id = m_fake->lastId();
    QVERIFY(id != 0);

    m_fake->plasma63PopupTimesOutThenHistoryClick(id);
    QTRY_COMPARE(open.size(), 1);
    QCOMPARE(open.first().at(0).toString(), kRoom);
    QCOMPARE(m_fake->hintsFor(id).value(QStringLiteral("resident")).toBool(), true);
    QTRY_VERIFY(m_fake->closedIds().contains(id));
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

void NotificationDbusTest::aCardAnsweredFromTheHistoryIsWithdrawnAndNoOtherIs()
{
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions") });
    NotificationManager manager;
    QSignalSpy read(&manager, &NotificationManager::markReadRequested);
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    const quint32 id = m_fake->lastId();
    m_fake->emitNotificationClosed(id, 1);
    m_fake->emitActionInvoked(id, QStringLiteral("mark-read"));
    QTRY_COMPARE(read.size(), 1);
    QCOMPARE(read.first().at(1).toString(), kRoom);
    QTRY_VERIFY(m_fake->closedIds().contains(id));

    const quint32 foreign = id + 1000;
    m_fake->emitActionInvoked(foreign, QStringLiteral("default"));
    m_fake->emitActionInvoked(foreign, QStringLiteral("mark-read"));
    TimelineEvent other = incomingText();
    other.roomId = QStringLiteral("!other:example.org");
    other.eventId = QStringLiteral("$ev2:example.org");
    QSignalSpy open(&manager, &NotificationManager::openRequested);
    manager.processEvent(other, context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    m_fake->emitActionInvoked(m_fake->lastId(), QStringLiteral("default"));
    QTRY_COMPARE(open.size(), 1);
    QVERIFY(!m_fake->closedIds().contains(foreign));
    QCOMPARE(read.size(), 1);
}

void NotificationDbusTest::onlyPlasmaBefore64GetsResidentCards()
{
    struct Daemon {
        QStringList info;
        bool resident;
    };
    const QList<Daemon> daemons{
        { { QStringLiteral("gnome-shell"), QStringLiteral("GNOME"),
            QStringLiteral("48.2"), QStringLiteral("1.2") }, false },
        { { QStringLiteral("Plasma"), QStringLiteral("KDE"),
            QStringLiteral("6.3.5"), QStringLiteral("1.2") }, true },
        { { QStringLiteral("Plasma"), QStringLiteral("KDE"),
            QStringLiteral("5.27.11"), QStringLiteral("1.2") }, true },
        { { QStringLiteral("Plasma"), QStringLiteral("KDE"),
            QStringLiteral("6.4.0"), QStringLiteral("1.2") }, false },
        { { QStringLiteral("Plasma"), QStringLiteral("KDE"),
            QStringLiteral("6.6.6"), QStringLiteral("1.2") }, false },
        { { QStringLiteral("dunst"), QStringLiteral("knopwob"),
            QStringLiteral("1.12.2"), QStringLiteral("1.2") }, false },
        { {}, false },
    };
    int checked = 0;
    for (const Daemon &daemon : daemons) {
        m_fake->reset(FakeDaemon::Mode::Answer,
                      { QStringLiteral("body"), QStringLiteral("actions") });
        m_fake->setServerInformation(daemon.info);
        NotificationManager manager;
        manager.processEvent(incomingText(), context());
        QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
        const QVariantMap hints = m_fake->hintsFor(m_fake->lastId());
        QVERIFY2(hints.value(QStringLiteral("resident")).toBool() == daemon.resident,
                 qPrintable(daemon.info.join(QLatin1Char(' '))));
        if (!daemon.resident)
            QVERIFY(!hints.contains(QStringLiteral("resident")));
        ++checked;
    }
    QCOMPARE(checked, daemons.size());
}

void NotificationDbusTest::theCallCardIsNeitherResidentNorClosedByItsClick()
{
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions") });
    m_fake->setServerInformation({ QStringLiteral("Plasma"), QStringLiteral("KDE"),
                                   QStringLiteral("6.3.5"), QStringLiteral("1.2") });
    NotificationManager manager;
    QSignalSpy open(&manager, &NotificationManager::openRequested);
    manager.showIncomingCall(kRoom, QStringLiteral("$call"), QStringLiteral("Call"),
                             QStringLiteral("Bob is calling"), true, 30);
    QTRY_VERIFY(manager.activeCallNotificationIdForTest() != 0);
    const quint32 id = manager.activeCallNotificationIdForTest();
    QVERIFY(!m_fake->hintsFor(id).contains(QStringLiteral("resident")));
    m_fake->emitActionInvoked(id, QStringLiteral("default"));
    QTRY_COMPARE(open.size(), 1);
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(m_fake->count(QStringLiteral("Notify")), 2);
    QVERIFY(m_fake->hintsFor(m_fake->lastId()).value(QStringLiteral("resident")).toBool());
    QVERIFY(!m_fake->closedIds().contains(id));
}

void NotificationDbusTest::aRepliedOrStaleCardIsWithdrawn()
{
    m_fake->reset(FakeDaemon::Mode::Answer,
                  { QStringLiteral("body"), QStringLiteral("actions"),
                    QStringLiteral("inline-reply") });
    m_fake->setServerInformation({ QStringLiteral("Plasma"), QStringLiteral("KDE"),
                                   QStringLiteral("6.3.5"), QStringLiteral("1.2") });
    NotificationManager manager;
    QSignalSpy replied(&manager, &NotificationManager::replyRequested);
    manager.processEvent(incomingText(), context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    const quint32 replyId = m_fake->lastId();
    m_fake->emitActionInvoked(replyId, QStringLiteral("inline-reply"));
    m_fake->emitNotificationReplied(replyId, QStringLiteral("on my way"));
    QTRY_COMPARE(replied.size(), 1);
    QCOMPARE(replied.first().at(1).toString(), kRoom);
    QTRY_VERIFY(m_fake->closedIds().contains(replyId));

    TimelineEvent other = incomingText();
    other.roomId = QStringLiteral("!other:example.org");
    other.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(other, context());
    QTRY_COMPARE(manager.pendingPayloadCountForTest(), 1);
    const quint32 staleId = m_fake->lastId();
    manager.clearPending();
    QTRY_COMPARE(m_fake->closedIds().count(staleId), 1);
    QSignalSpy open(&manager, &NotificationManager::openRequested);
    m_fake->emitActionInvoked(staleId, QStringLiteral("default"));
    QTRY_COMPARE(open.size(), 1);
    QCOMPARE(open.first().at(0).toString(), QString());
    QTRY_COMPARE(m_fake->closedIds().count(staleId), 2);
}

QTEST_MAIN(NotificationDbusTest)
#include "NotificationDbusTest.moc"
