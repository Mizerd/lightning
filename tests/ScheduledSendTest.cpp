// v0.9 (phase 11): "Send later" — ScheduledSendController policy.
//
// Pins the two-mechanism contract and its honesty rules:
//   * a message is scheduled SERVER-side (MSC4140) only when the server
//     has said it supports delayed events, the room is KNOWN unencrypted,
//     and the message carries no thread/reply relation; everything else
//     stays in the LOCAL queue;
//   * a local entry for an UNENCRYPTED room persists across restarts; one
//     for an ENCRYPTED room (or a room whose encryption state is unknown)
//     is memory-only — scheduled plaintext never touches disk (CLAUDE.md
//     §6);
//   * a due local entry is marked "sending" and PERSISTED before the send
//     leaves, goes through the ROOM-level send (any room, not only the open
//     one), and leaves the queue only on the room's real acceptance — a
//     refusal is reported, never reported as sent;
//   * nothing is dispatched while disconnected; the reconnect fires it;
//   * a row found in "sending" on the next start is reported as unsent and
//     never re-fired on a guess;
//   * every server-side mutation is serialized on the entry's one in-flight
//     op: a reschedule/edit cancels FIRST and resubmits only on the server's
//     "cancelled"; a failed cancel is reported and never followed by a
//     replacement (that would deliver the message twice); changes asked
//     while the schedule is in flight apply once the delay id arrives;
//   * a server-held entry past its deadline is retired (the server sent
//     it); one persisted without a delay id is reported, never re-sent;
//   * sign-out drops the memory queue.
//
// HONEST SCOPE: policy and wiring against a fake client. A real homeserver
// accepting or refusing a delayed event, the delayed event actually firing
// server-side, and the composer's Send-later button are NOT exercised here
// and are NOT TESTED.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/ScheduledSendController.h"
#include "storage/SecretStore.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

class FakeSecretStore final : public SecretStore
{
public:
    explicit FakeSecretStore(QObject *parent = nullptr)
        : SecretStore(parent)
    {
    }
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

struct RoomSend {
    quint64 op = 0;
    QString roomId;
    QString body;
    QString threadRootId;
    QString replyToEventId;
    QVariantMap spec;
    // What the persisted queue said about this entry at the moment the
    // send left — the "marked before dispatch" guard.
    QString persistedStatusAtSend;
};

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    using MatrixClient::sendReply;
    using MatrixClient::sendTextMessage;
    using MatrixClient::sendThreadReplyTo;

    QList<RoomInfo> roomSet;
    ConnectionState state = Syncing;
    SettingsManager *settings = nullptr;
    quint64 nextOp = 1;
    bool refuseServerScheduling = false;
    bool roomSendSupported = true;
    int probes = 0;
    QList<RoomSend> roomSends;          // the room-level lane
    QList<RoomSend> legacySends;        // the timeline lane (mock/HTTP)
    struct ScheduleCall { QString roomId; QString body; qint64 delayMs; quint64 op; };
    QList<ScheduleCall> scheduleCalls;
    struct UpdateCall { QString delayId; QString action; quint64 op; };
    QList<UpdateCall> updateCalls;

    QString persistedStatusFor(const QString &body) const
    {
        if (!settings)
            return {};
        for (const QVariant &v : settings->scheduledSends()) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("body")).toString() == body)
                return m.value(QStringLiteral("status")).toString();
        }
        return QStringLiteral("<absent>");
    }

    void probeDelayedEvents() override { ++probes; }
    quint64 scheduleMessage(const QString &roomId, const QString &body,
                            const QVariantMap &, const QStringList &,
                            qint64 delayMs) override
    {
        if (refuseServerScheduling)
            return 0;
        const quint64 op = nextOp++;
        scheduleCalls.append({ roomId, body, delayMs, op });
        return op;
    }
    quint64 updateScheduledMessage(const QString &delayId,
                                   const QString &action) override
    {
        const quint64 op = nextOp++;
        updateCalls.append({ delayId, action, op });
        return op;
    }
    quint64 sendRoomMessage(const QString &roomId, const QString &body,
                            const QVariantMap &spec, const QStringList &,
                            const QString &replyToEventId,
                            const QString &threadRootEventId) override
    {
        if (!roomSendSupported)
            return 0;
        const quint64 op = nextOp++;
        roomSends.append({ op, roomId, body, threadRootEventId, replyToEventId, spec,
                           persistedStatusFor(body) });
        return op;
    }

    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &, const QVariantMap &spec) override
    {
        legacySends.append({ 0, roomId, body, {}, {}, spec, persistedStatusFor(body) });
    }
    void sendReply(const QString &roomId, const QString &replyToEventId,
                   const QString &body, const QStringList &,
                   const QVariantMap &spec) override
    {
        legacySends.append({ 0, roomId, body, {}, replyToEventId, spec,
                             persistedStatusFor(body) });
    }
    void sendThreadReplyTo(const QString &roomId, const QString &threadRootId,
                           const QString &inReplyTo, const QString &body,
                           const QStringList &, const QVariantMap &spec) override
    {
        legacySends.append({ 0, roomId, body, threadRootId, inReplyTo, spec,
                             persistedStatusFor(body) });
    }

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@alice:mock.local"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return state; }
    QList<RoomInfo> rooms() const override { return roomSet; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

RoomInfo room(const QString &id, bool encrypted, bool encryptionKnown = true)
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.membership = RoomInfo::Joined;
    info.encrypted = encrypted;
    info.encryptionKnown = encryptionKnown;
    return info;
}

const QString kPlain = QStringLiteral("!plain:mock.local");
const QString kEncrypted = QStringLiteral("!enc:mock.local");
const QString kUnknown = QStringLiteral("!unknown:mock.local");

QVariantMap message(const QString &roomId, const QString &body,
                    const QString &threadRootId = QString(),
                    const QString &replyToEventId = QString())
{
    return QVariantMap{
        { QStringLiteral("roomId"), roomId },
        { QStringLiteral("body"), body },
        { QStringLiteral("threadRootId"), threadRootId },
        { QStringLiteral("replyToEventId"), replyToEventId },
    };
}

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

struct Harness {
    FakeSecretStore secrets;
    SettingsManager settings;
    FakeClient client;
    ScheduledSendController scheduler;

    Harness()
    {
        settings.setSecretStore(&secrets);
        settings.saveSession(QStringLiteral("https://mock.local"),
                             QStringLiteral("@alice:mock.local"),
                             QStringLiteral("DEVICE"),
                             QStringLiteral("token-fixture"));
        client.settings = &settings;
        client.roomSet = { room(kPlain, false), room(kEncrypted, true),
                           room(kUnknown, false, /*encryptionKnown=*/false) };
        scheduler.setSettings(&settings);
        scheduler.setClient(&client);
    }

    QVariantMap entry(const QString &id) const
    {
        for (const QVariant &v : scheduler.pending())
            if (v.toMap().value(QStringLiteral("id")).toString() == id)
                return v.toMap();
        return {};
    }
    void enableServer()
    {
        Q_EMIT client.delayedEventsSupportReceived(true, true);
    }
    // Schedules a server entry and lets the server answer with a delay id.
    QString serverEntry(const QString &body, const QString &delayId)
    {
        const QString id = scheduler.schedule(message(kPlain, body), nowMs() + 90000);
        Q_EMIT client.scheduledSendFinished(client.scheduleCalls.last().op, kPlain, true,
                                            delayId, QString());
        return id;
    }
    void answerRoomSend(bool ok, const QString &category = QString())
    {
        Q_EMIT client.roomSendFinished(client.roomSends.last().op, kPlain, ok, category);
    }
};

} // namespace

class ScheduledSendTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("scheduled-send-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void scheduleRefusesAnEmptyBodyAPastDeadlineAndNoRoom()
    {
        Harness h;
        QVERIFY(h.scheduler.schedule(message(kPlain, QStringLiteral("  ")),
                                     nowMs() + 60000).isEmpty());
        QVERIFY(h.scheduler.schedule(message(QString(), QStringLiteral("hi")),
                                     nowMs() + 60000).isEmpty());
        QVERIFY(h.scheduler.schedule(message(kPlain, QStringLiteral("hi")),
                                     nowMs() - 1).isEmpty());
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void anUnencryptedRoomsEntryPersistsAndAnEncryptedOneStaysInMemory()
    {
        Harness h;
        const QString plainId = h.scheduler.schedule(
            message(kPlain, QStringLiteral("later")), nowMs() + 60000);
        QVERIFY(!plainId.isEmpty());
        QCOMPARE(h.entry(plainId).value(QStringLiteral("mode")).toString(),
                 QStringLiteral("local"));
        QCOMPARE(h.entry(plainId).value(QStringLiteral("volatile")).toBool(), false);
        QCOMPARE(h.settings.scheduledSends().size(), 1);

        const QString encId = h.scheduler.schedule(
            message(kEncrypted, QStringLiteral("secret plan")), nowMs() + 60000);
        QVERIFY(!encId.isEmpty());
        QCOMPARE(h.entry(encId).value(QStringLiteral("volatile")).toBool(), true);
        // Unknown encryption state fails closed, like the draft store.
        const QString unknownId = h.scheduler.schedule(
            message(kUnknown, QStringLiteral("maybe secret")), nowMs() + 60000);
        QCOMPARE(h.entry(unknownId).value(QStringLiteral("volatile")).toBool(), true);

        QCOMPARE(h.scheduler.pendingCount(), 3);
        // Only the unencrypted room's row reached disk, and no encrypted
        // body appears anywhere in the persisted rows.
        const QVariantList rows = h.settings.scheduledSends();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("roomId")).toString(),
                 kPlain);
        for (const QVariant &v : rows) {
            const QString body = v.toMap().value(QStringLiteral("body")).toString();
            QVERIFY(!body.contains(QStringLiteral("secret")));
        }
        QCOMPARE(h.scheduler.pendingForRoom(kEncrypted).size(), 1);
    }

    void serverModeNeedsSupportAnUnencryptedRoomAndNoRelation()
    {
        Harness h;
        // Support unknown: everything is local.
        QVERIFY(!h.scheduler.wouldUseServer(kPlain, {}, {}));
        h.enableServer();
        QCOMPARE(h.scheduler.serverScheduling(), 1);
        QVERIFY(h.scheduler.wouldUseServer(kPlain, {}, {}));
        QVERIFY(!h.scheduler.wouldUseServer(kEncrypted, {}, {}));
        QVERIFY(!h.scheduler.wouldUseServer(kUnknown, {}, {}));
        QVERIFY(!h.scheduler.wouldUseServer(kPlain, QStringLiteral("$root"), {}));
        QVERIFY(!h.scheduler.wouldUseServer(kPlain, {}, QStringLiteral("$reply")));

        const qint64 at = nowMs() + 90000;
        const QString id = h.scheduler.schedule(message(kPlain, QStringLiteral("held")), at);
        QCOMPARE(h.entry(id).value(QStringLiteral("mode")).toString(),
                 QStringLiteral("server"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        QCOMPARE(h.client.scheduleCalls.first().roomId, kPlain);
        // The delay handed to the server is the remaining time, not an
        // absolute instant.
        QVERIFY(h.client.scheduleCalls.first().delayMs > 80000);
        QVERIFY(h.client.scheduleCalls.first().delayMs <= 90000);

        // The server's answer records the delay id.
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.first().op,
                                              kPlain, true,
                                              QStringLiteral("delay-1"), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("delayId")).toString(),
                 QStringLiteral("delay-1"));
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), false);

        // Cancelling a server-held message asks the server and only drops
        // the entry once the server has answered.
        h.scheduler.cancel(id);
        QCOMPARE(h.client.updateCalls.size(), 1);
        QCOMPARE(h.client.updateCalls.first().delayId, QStringLiteral("delay-1"));
        QCOMPARE(h.client.updateCalls.first().action, QStringLiteral("cancel"));
        QVERIFY(!h.entry(id).isEmpty());
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.first().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QVERIFY(h.entry(id).isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
        // A server-held message never went through a local send lane.
        QVERIFY(h.client.roomSends.isEmpty());
        QVERIFY(h.client.legacySends.isEmpty());
    }

    void aServerRefusalForAnEncryptedRoomFallsBackToTheLocalQueueAndSaysSo()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(message(kPlain, QStringLiteral("x")),
                                                nowMs() + 90000);
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.first().op,
                                              kPlain, false, QString(),
                                              QStringLiteral("encrypted_unsupported"));
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("mode")).toString(), QStringLiteral("local"));
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
        QVERIFY(!e.value(QStringLiteral("error")).toString().isEmpty());

        // Any other refusal is a failure the user sees, not a silent retry.
        const QString id2 = h.scheduler.schedule(message(kPlain, QStringLiteral("y")),
                                                 nowMs() + 90000);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op,
                                              kPlain, false, QString(),
                                              QStringLiteral("server"));
        QCOMPARE(h.entry(id2).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(h.scheduler.pendingCount(), 1);
    }

    void aRescheduleOfAServerHeldMessageCancelsFirstAndResubmitsOnlyOnSuccess()
    {
        Harness h;
        h.enableServer();
        const QString id = h.serverEntry(QStringLiteral("held"), QStringLiteral("delay-1"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        const qint64 later = nowMs() + 300000;
        h.scheduler.reschedule(id, later);
        // Exactly one cancel, and NO new delayed event yet.
        QCOMPARE(h.client.updateCalls.size(), 1);
        QCOMPARE(h.client.updateCalls.last().action, QStringLiteral("cancel"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        // While busy, further edits are refused rather than piled up.
        h.scheduler.updateText(id, QStringLiteral("ignored"), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("held"));
        // The server confirms the cancel: only now the replacement goes out,
        // with the new deadline.
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QVERIFY(h.client.scheduleCalls.last().delayMs > 290000);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);
        QCOMPARE(h.entry(id).value(QStringLiteral("delayId")).toString(), QString());
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op, kPlain,
                                              true, QStringLiteral("delay-2"), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("delayId")).toString(),
                 QStringLiteral("delay-2"));
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));

        // An edit takes the same path and carries the new text.
        h.scheduler.updateText(id, QStringLiteral("  edited  "), QString());
        QCOMPARE(h.client.updateCalls.size(), 2);
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("held"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-2"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QCOMPARE(h.client.scheduleCalls.size(), 3);
        QCOMPARE(h.client.scheduleCalls.last().body, QStringLiteral("edited"));
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("edited"));
    }

    void aFailedCancelIsReportedAndNeverFollowedByAReplacement()
    {
        Harness h;
        h.enableServer();
        const QString id = h.serverEntry(QStringLiteral("held"), QStringLiteral("delay-1"));
        h.scheduler.reschedule(id, nowMs() + 300000);
        QCOMPARE(h.client.updateCalls.size(), 1);
        // The server no longer holds it (it may already have been sent).
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), false,
                                                QStringLiteral("not_found"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(e.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("already have been sent")));
        QCOMPARE(e.value(QStringLiteral("busy")).toBool(), false);
        // A plain cancel of that failed row is a local removal: there is
        // nothing left on the server to ask about.
        h.scheduler.cancel(id);
        QCOMPARE(h.client.updateCalls.size(), 1);
        QVERIFY(h.entry(id).isEmpty());
    }

    void changesAskedWhileTheScheduleIsInFlightApplyOnceTheDelayIdArrives()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(message(kPlain, QStringLiteral("held")),
                                                nowMs() + 90000);
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        // The user reschedules before the server has answered.
        const qint64 later = nowMs() + 300000;
        h.scheduler.reschedule(id, later);
        QCOMPARE(h.client.updateCalls.size(), 0);
        // Nothing changes yet: the server still holds the original deadline
        // and the entry says so rather than showing a time the server does
        // not know about.
        QVERIFY(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong() < later);
        // The delay id arrives: the deferred change becomes cancel + resubmit.
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.first().op, kPlain,
                                              true, QStringLiteral("delay-1"), QString());
        QCOMPARE(h.client.updateCalls.size(), 1);
        QCOMPARE(h.client.updateCalls.last().delayId, QStringLiteral("delay-1"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);

        // A cancel asked while in flight is issued once the id is known.
        const QString id2 = h.scheduler.schedule(message(kPlain, QStringLiteral("two")),
                                                 nowMs() + 90000);
        h.scheduler.cancel(id2);
        QVERIFY(!h.entry(id2).isEmpty());
        QCOMPARE(h.client.updateCalls.size(), 1);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op, kPlain,
                                              true, QStringLiteral("delay-9"), QString());
        QCOMPARE(h.client.updateCalls.size(), 2);
        QCOMPARE(h.client.updateCalls.last().delayId, QStringLiteral("delay-9"));
        QCOMPARE(h.client.updateCalls.last().action, QStringLiteral("cancel"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-9"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QVERIFY(h.entry(id2).isEmpty());
    }

    void aDueLocalEntryIsMarkedSendingBeforeItLeavesAndLeavesOnAcceptance()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("due soon")), nowMs() + 60);
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().roomId, kPlain);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("due soon"));
        // The persisted row read "sending" at the moment the send left.
        QCOMPARE(h.client.roomSends.first().persistedStatusAtSend,
                 QStringLiteral("sending"));
        // Still listed, busy, until the room answers.
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("sending"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        h.answerRoomSend(true);
        QVERIFY(h.entry(id).isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
        QTest::qWait(150);
        QCOMPARE(h.client.roomSends.size(), 1);
        QVERIFY(h.client.legacySends.isEmpty());
    }

    void aRefusedRoomSendIsReportedNotPretendedAndCanBeRetried()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("refused")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("forbidden"));
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(e.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("not sent")));
        QCOMPARE(e.value(QStringLiteral("busy")).toBool(), false);
        // It is not re-fired by the timer, only by the user.
        QTest::qWait(150);
        QCOMPARE(h.client.roomSends.size(), 1);
        h.scheduler.sendNow(id);
        QCOMPARE(h.client.roomSends.size(), 2);
        h.answerRoomSend(true);
        QVERIFY(h.entry(id).isEmpty());
    }

    void threadAndReplyEntriesCarryTheirRelationThroughTheRoomLane()
    {
        Harness h;
        h.scheduler.schedule(message(kPlain, QStringLiteral("in thread"),
                                     QStringLiteral("$root"), QString()),
                             nowMs() + 60);
        h.scheduler.schedule(message(kPlain, QStringLiteral("as reply"), QString(),
                                     QStringLiteral("$target")),
                             nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 2, 3000);
        QString threadRoot, replyTarget;
        for (const RoomSend &m : h.client.roomSends) {
            if (m.body == QLatin1String("in thread"))
                threadRoot = m.threadRootId;
            if (m.body == QLatin1String("as reply"))
                replyTarget = m.replyToEventId;
        }
        QCOMPARE(threadRoot, QStringLiteral("$root"));
        QCOMPARE(replyTarget, QStringLiteral("$target"));
        QVERIFY(h.client.legacySends.isEmpty());
    }

    void aBackendWithoutTheRoomLaneFallsBackToTheTimelineSends()
    {
        Harness h;
        h.client.roomSendSupported = false;
        h.scheduler.schedule(message(kPlain, QStringLiteral("legacy"), QString(),
                                     QStringLiteral("$target")),
                             nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.legacySends.size(), 1, 3000);
        QCOMPARE(h.client.legacySends.first().replyToEventId, QStringLiteral("$target"));
        QCOMPARE(h.client.legacySends.first().persistedStatusAtSend,
                 QStringLiteral("sending"));
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void nothingLeavesWhileDisconnectedAndTheReconnectFiresIt()
    {
        Harness h;
        h.client.state = MatrixClient::Connecting;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("after reconnect")), nowMs() + 60);
        QVERIFY(!id.isEmpty());
        QTest::qWait(250);
        QVERIFY(h.client.roomSends.isEmpty());
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));

        h.client.state = MatrixClient::Syncing;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("after reconnect"));
        // The reconnect also (re)probes server support.
        QVERIFY(h.client.probes >= 1);
    }

    void rowsCaughtMidWayOnTheLastRunAreReportedAndNeverRefired()
    {
        Harness h;
        h.settings.setScheduledSends({
            // A local row caught mid-dispatch by a crash.
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("crashed") },
                { QStringLiteral("roomId"), kPlain },
                { QStringLiteral("body"), QStringLiteral("did this go?") },
                { QStringLiteral("sendAtMs"), nowMs() - 5000 },
                { QStringLiteral("mode"), QStringLiteral("local") },
                { QStringLiteral("status"), QStringLiteral("sending") },
            },
            // A server row persisted before the server confirmed it.
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("unconfirmed") },
                { QStringLiteral("roomId"), kPlain },
                { QStringLiteral("body"), QStringLiteral("held?") },
                { QStringLiteral("sendAtMs"), nowMs() + 60000 },
                { QStringLiteral("mode"), QStringLiteral("server") },
                { QStringLiteral("status"), QStringLiteral("pending") },
            },
            // A server row whose deadline passed: the server sent it.
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("done") },
                { QStringLiteral("roomId"), kPlain },
                { QStringLiteral("body"), QStringLiteral("sent by the server") },
                { QStringLiteral("sendAtMs"), nowMs() - 120000 },
                { QStringLiteral("mode"), QStringLiteral("server") },
                { QStringLiteral("status"), QStringLiteral("pending") },
                { QStringLiteral("delayId"), QStringLiteral("delay-old") },
            },
        });
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTest::qWait(150);
        QVERIFY(h.client.roomSends.isEmpty());
        QVERIFY(h.client.scheduleCalls.isEmpty());
        QCOMPARE(h.entry(QStringLiteral("crashed")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(h.entry(QStringLiteral("unconfirmed")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QVERIFY(h.entry(QStringLiteral("unconfirmed")).value(QStringLiteral("error"))
                    .toString().contains(QStringLiteral("before the server confirmed")));
        QVERIFY(h.entry(QStringLiteral("done")).isEmpty());
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QCOMPARE(h.settings.scheduledSends().size(), 2);
        h.scheduler.cancel(QStringLiteral("crashed"));
        QVERIFY(h.entry(QStringLiteral("crashed")).isEmpty());
    }

    void aMissedDeadlineFiresOnceOnTheNextStart()
    {
        Harness h;
        h.settings.setScheduledSends({ QVariantMap{
            { QStringLiteral("id"), QStringLiteral("missed") },
            { QStringLiteral("roomId"), kPlain },
            { QStringLiteral("body"), QStringLiteral("overdue") },
            { QStringLiteral("sendAtMs"), nowMs() - 5000 },
            { QStringLiteral("mode"), QStringLiteral("local") },
            { QStringLiteral("status"), QStringLiteral("pending") },
        } });
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("overdue"));
        h.answerRoomSend(true);
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void localRescheduleAndEditKeepTheEntryPendingAndPersisted()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("draft")), nowMs() + 60000);
        h.scheduler.updateText(id, QStringLiteral("  final  "), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("final"));
        // An empty edit is refused, never a blank message.
        h.scheduler.updateText(id, QStringLiteral("   "), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("final"));
        const qint64 later = nowMs() + 120000;
        h.scheduler.reschedule(id, later);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);
        // A past deadline is refused.
        h.scheduler.reschedule(id, nowMs() - 1);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);
        QCOMPARE(h.settings.scheduledSends().size(), 1);
        QCOMPARE(h.settings.scheduledSends().first().toMap()
                     .value(QStringLiteral("body")).toString(),
                 QStringLiteral("final"));
    }

    void signOutDropsTheMemoryQueue()
    {
        Harness h;
        h.scheduler.schedule(message(kPlain, QStringLiteral("a")), nowMs() + 60000);
        h.scheduler.schedule(message(kEncrypted, QStringLiteral("b")), nowMs() + 60000);
        QCOMPARE(h.scheduler.pendingCount(), 2);
        Q_EMIT h.client.loggedOut();
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QVERIFY(h.scheduler.pending().isEmpty());
        QCOMPARE(h.scheduler.serverScheduling(), -1);
    }
};

QTEST_GUILESS_MAIN(ScheduledSendTest)
#include "ScheduledSendTest.moc"
