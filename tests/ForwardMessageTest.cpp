// ForwardController policy. A local FakeClient stands in for the SDK bridge
// (no network or credentials), with a real MediaBridge wired to it as in
// MediaBridgeTest.cpp, so media forwards use MediaBridge's own dedup/cache.
// Pins:
//   * redacted, local-echo, undecryptable and empty content is refused at
//     begin();
//   * a text forward sends the plain body through the 2-argument
//     sendTextMessage, which attaches no relation (no reply, no m.thread);
//   * a media forward re-fetches through MediaBridge (snapshots carry no
//     bytes) and re-uploads exactly the fetched bytes;
//   * `forwarded()` (which opens the target room) fires only after dispatch,
//     and a dispatch failure keeps the dialog open with `error` set;
//   * a second begin() invalidates the first forward's pending fetch, so a
//     late answer is never sent to the new target;
//   * a cancelled forward's late fetch answer is dropped;
//   * another consumer's mediaBytesForStar answer on the shared MediaBridge
//     is ignored;
//   * a second forwardTo() while busy is a no-op.
// Controller policy only: the QML picker's gating, real Matrix sends,
// encryption and Element interoperability are not tested here.

#include "app/ForwardController.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"

#include <QSignalSpy>
#include <QBuffer>
#include <QImage>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // ---- media fetch (drives MediaBridge::fetchFullForStar) ----
    quint64 nextOp = 1;
    struct Fetch { quint64 opId; QString mediaKey; };
    QList<Fetch> fetches;
    bool rejectFetches = false;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int /*kind*/,
                       int /*timeoutClass*/) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mediaKey });
        return op;
    }
    quint64 fetchMxcThumbnail(const QString &, int, int) override { return 0; }
    void cancelMediaFetch(quint64) override {}

    void succeed(quint64 opId, const QByteArray &bytes,
                const QString &mime = QStringLiteral("image/png"))
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes, mime, QString());
    }
    void fail(quint64 opId, const QString &category)
    {
        Q_EMIT mediaFailed(opId, QString(), 0, category);
    }
    void emitLoggedOut() { Q_EMIT loggedOut(); }

    // ---- sends (what ForwardController is actually proven against) ----
    struct TextSend { QString roomId; QString body; };
    struct AttachmentSend {
        QString roomId; QByteArray bytes; QString filename; QString mime;
        int width; int height;
    };
    QList<TextSend> textSends;
    QList<AttachmentSend> attachmentSends;
    // 0 simulates a queue rejection (the send never entered the SDK).
    quint64 nextSendOp = 1;
    bool rejectNextAttachment = false;

    void sendTextMessage(const QString &roomId, const QString &body) override
    {
        textSends.append({ roomId, body });
    }
    struct ThreadSend { QString roomId; QString rootId; QString body; };
    QList<ThreadSend> threadSends;
    void sendThreadReplyTo(const QString &roomId, const QString &rootId,
                           const QString &, const QString &body) override
    {
        threadSends.append({ roomId, rootId, body });
    }
    // Like the real backend, the timeline-scoped send refuses any room but the
    // open one (RustSdkMatrixClient::sendAttachmentBytes gates on
    // timelineActiveFor).
    QString openRoomId;
    bool roomScopedSupported = true;

    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &bytes,
                               const QString &filename, const QString &mime,
                               int width, int height) override
    {
        if (!openRoomId.isEmpty() && roomId != openRoomId)
            return 0;
        attachmentSends.append({ roomId, bytes, filename, mime, width, height });
        if (rejectNextAttachment) {
            rejectNextAttachment = false;
            return 0;
        }
        return nextSendOp++;
    }

    bool supportsRoomScopedAttachmentSend() const override
    { return roomScopedSupported; }
    quint64 sendAttachmentBytesToRoom(const QString &roomId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime,
                                      int width, int height) override
    {
        attachmentSends.append({ roomId, bytes, filename, mime, width, height });
        if (rejectNextAttachment) {
            rejectNextAttachment = false;
            return 0;
        }
        return nextSendOp++;
    }

    // Inert pure virtuals: only the media/send surface is exercised.
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
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

const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");
const QString kEventA = QStringLiteral("$eventA");
const QString kEventB = QStringLiteral("$eventB");


// Real 1x1 PNG and GIF bytes: the forward path sniffs bytes rather than
// trusting the source event's claimed type, so fixtures carry genuine magic
// and two payloads can be told apart by their sniffed type.
static QByteArray realPngBytes()
{
    return QByteArray::fromHex(
        "89504e470d0a1a0a0000000d494844520000000100000001080200000"
        "0907753de0000000c49444154789c63f8cfc0000003010100c9fe92ef"
        "0000000049454e44ae426082");
}

static QByteArray realGifBytes()
{
    return QByteArray::fromHex(
        "47494638396101000100800000000000ffffff21f90401000000"
        "002c00000000010001000002024401003b");
}


QVariantMap textSnapshot(const QString &body = QStringLiteral("hello there"))
{
    QVariantMap m;
    m.insert(QStringLiteral("body"), body);
    m.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Alice"));
    return m;
}

QVariantMap mediaSnapshot(const QString &mediaKey,
                         const QString &filename = QStringLiteral("cat.png"),
                         const QString &mime = QStringLiteral("image/png"))
{
    QVariantMap m;
    m.insert(QStringLiteral("isImage"), true);
    m.insert(QStringLiteral("mediaKey"), mediaKey);
    m.insert(QStringLiteral("mediaFilename"), filename);
    m.insert(QStringLiteral("mediaMimetype"), mime);
    m.insert(QStringLiteral("mediaWidth"), 100);
    m.insert(QStringLiteral("mediaHeight"), 80);
    return m;
}

/// A selection snapshot: like textSnapshot(), plus the event id the bulk
/// lane reports failures against.
QVariantMap selectedText(const QString &eventId, const QString &body)
{
    QVariantMap m = textSnapshot(body);
    m.insert(QStringLiteral("eventId"), eventId);
    return m;
}

QVariantMap selectedMedia(const QString &eventId)
{
    QVariantMap m = mediaSnapshot(QStringLiteral("key:") + eventId);
    m.insert(QStringLiteral("eventId"), eventId);
    return m;
}

} // namespace

class ForwardMessageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // Multi-message, multi-destination: N messages into M rooms can partially
    // fail, and must never be reported otherwise.

    void everyMessageReachesEveryDestination()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"), {
            selectedText(QStringLiteral("$a"), QStringLiteral("first")),
            selectedText(QStringLiteral("$b"), QStringLiteral("second")),
        });
        QVERIFY(fwd.selectionActive());
        fwd.sendSelection({
            QVariantMap{ { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } },
            QVariantMap{ { QStringLiteral("roomId"), QStringLiteral("!y:e.org") } },
        });

        QCOMPARE(client.textSends.size(), 4);
        QCOMPARE(fwd.progressTotal(), 4);
        QCOMPARE(fwd.progressDone(), 4);
        QCOMPARE(fwd.failureCount(), 0);
    }

    // A thread destination is a relation the target room chose; unlike the
    // source's thread, which is never carried over.
    void aThreadDestinationSendsIntoThatThread()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"),
                           { selectedText(QStringLiteral("$a"),
                                         QStringLiteral("hello")) });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") },
            { QStringLiteral("threadRootId"), QStringLiteral("$root") },
        } });
        QCOMPARE(client.threadSends.size(), 1);
        QCOMPARE(client.threadSends.at(0).rootId, QStringLiteral("$root"));
        QVERIFY(client.textSends.isEmpty());
    }

    // One failure among several is not success, and the report names which
    // pair failed.
    void oneFailureAmongManyIsReportedAsThatPair()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        // A media snapshot cannot use the bulk lane; it is reported, not
        // dropped or sent as its caption.
        fwd.beginSelection(QStringLiteral("!src:example.org"), {
            selectedText(QStringLiteral("$a"), QStringLiteral("fine")),
            selectedMedia(QStringLiteral("$m")),
        });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });

        QCOMPARE(fwd.progressTotal(), 2);
        QCOMPARE(fwd.progressDone(), 2);
        QCOMPARE(fwd.failureCount(), 1);
        const QVariantMap failure = fwd.failures().at(0).toMap();
        QCOMPARE(failure.value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$m"));
        QCOMPARE(failure.value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!x:e.org"));
        QVERIFY(!failure.value(QStringLiteral("message")).toString().isEmpty());
        // The successful one really went.
        QCOMPARE(client.textSends.size(), 1);
    }

    // Retry re-dispatches only the failures; resending successes would
    // duplicate them.
    void retryResendsOnlyTheFailures()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"), {
            selectedText(QStringLiteral("$a"), QStringLiteral("fine")),
            selectedMedia(QStringLiteral("$m")),
        });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });
        QCOMPARE(client.textSends.size(), 1);
        QCOMPARE(fwd.failureCount(), 1);

        fwd.retryFailures();
        QCOMPARE(fwd.progressTotal(), 1);
        QVERIFY2(client.textSends.size() == 1,
                 "retry resent a message that had already succeeded");
    }

    // Context mode discloses the source room and original sender, so it must
    // be chosen; an unknown mode falls back to the private one.
    void contextModeAttributesAndIsNeverTheDefault()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        QCOMPARE(fwd.forwardMode(), QStringLiteral("content"));
        fwd.setForwardMode(QStringLiteral("nonsense"));
        QCOMPARE(fwd.forwardMode(), QStringLiteral("content"));

        QVariantMap snap = selectedText(QStringLiteral("$a"),
                                        QStringLiteral("the body"));
        snap.insert(QStringLiteral("senderName"), QStringLiteral("Ann"));
        snap.insert(QStringLiteral("sourceRoomName"), QStringLiteral("Secret"));
        fwd.beginSelection(QStringLiteral("!src:example.org"), { snap });

        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });
        QVERIFY2(!client.textSends.at(0).body.contains(QStringLiteral("Secret")),
                 "content mode leaked the source room's name");

        client.textSends.clear();
        fwd.setForwardMode(QStringLiteral("context"));
        fwd.beginSelection(QStringLiteral("!src:example.org"), { snap });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });
        const QString body = client.textSends.at(0).body;
        QVERIFY2(body.contains(QStringLiteral("Ann")), qPrintable(body));
        QVERIFY2(body.contains(QStringLiteral("Secret")), qPrintable(body));
        QVERIFY2(body.contains(QStringLiteral("the body")), qPrintable(body));
    }

    void beginRefusesRedactedEvent()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot();
        snap.insert(QStringLiteral("redacted"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesLocalEcho()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot();
        snap.insert(QStringLiteral("isLocalEcho"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesUndecryptableEvent()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot();
        snap.insert(QStringLiteral("undecryptable"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesEmptyTextBody()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        fwd.begin(kRoomA, kEventA, textSnapshot(QStringLiteral("   ")));

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesMediaWithNoMediaKey()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap;
        snap.insert(QStringLiteral("isImage"), true);
        // mediaKey deliberately absent.
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    // ---- text path ----

    void textForwardSendsPlainBodyWithNoRelationThenNavigates()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, textSnapshot(QStringLiteral("hi from A")));
        QVERIFY(fwd.active());

        fwd.forwardTo(kRoomB);

        // The only send is the plain 2-argument sendTextMessage;
        // ForwardController has no reply or thread call.
        QCOMPARE(client.textSends.size(), 1);
        QCOMPARE(client.textSends.first().roomId, kRoomB);
        QCOMPARE(client.textSends.first().body, QStringLiteral("hi from A"));
        QCOMPARE(client.attachmentSends.size(), 0);

        // Dispatched: forwarded() fires with the target and the dialog resets.
        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.first().at(0).toString(), kRoomB);
        QVERIFY(!fwd.active());
        QVERIFY(!fwd.busy());
    }

    // ---- media path ----

    void mediaForwardRefetchesFreshBytesAndReuploadsThem()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);

        // A MediaBridge fetch was dispatched, so the snapshot's bytes were not
        // trusted (it carries none).
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().mediaKey, QStringLiteral("$mediaX"));
        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(fwd.busy());

        const QByteArray realBytes = realGifBytes();
        client.succeed(client.fetches.first().opId, realBytes);

        // The re-upload carries exactly the fetched bytes plus the
        // classification frozen at activation.
        QCOMPARE(client.attachmentSends.size(), 1);
        const auto &sent = client.attachmentSends.first();
        QCOMPARE(sent.roomId, kRoomB);
        QCOMPARE(sent.bytes, realBytes);
        QCOMPARE(sent.filename, QStringLiteral("cat.png"));
        // The type from the bytes, not the source event's claim.
        QCOMPARE(sent.mime, QStringLiteral("image/gif"));
        // The dimensions the bytes have, not the claimed 100x80: a forward
        // re-originates the attachment and must not attest to an unchecked
        // shape.
        QCOMPARE(sent.width, 1);
        QCOMPARE(sent.height, 1);

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.first().at(0).toString(), kRoomB);
        QVERIFY(!fwd.active());
    }

    void mediaFetchFailureLeavesDialogOpenWithError()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        client.fail(client.fetches.first().opId, QStringLiteral("network"));

        // Reported via `error`, and the picker stays open for a retry.
        QCOMPARE(forwarded.count(), 0);
        QVERIFY(fwd.active());
        QVERIFY(!fwd.busy());
        QVERIFY(!fwd.error().isEmpty());
        QCOMPARE(client.attachmentSends.size(), 0);
    }

    void attachmentDispatchFailureLeavesDialogOpenWithError()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        client.rejectNextAttachment = true;
        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.first().opId, realGifBytes());

        QCOMPARE(client.attachmentSends.size(), 1); // attempted
        QCOMPARE(forwarded.count(), 0);              // but never dispatched
        QVERIFY(fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    // ---- a second forward must not take a stale answer meant for the first
    // ----

    void secondBeginInvalidatesFirstForwardsPendingFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        // First forward: row A's photo toward room B, unresolved when the user
        // picks another message.
        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);
        const quint64 staleOp = client.fetches.first().opId;

        // A different row (B's photo) to a different room.
        fwd.begin(kRoomA, kEventB, mediaSnapshot(QStringLiteral("$mediaB")));
        fwd.forwardTo(kRoomA);
        QCOMPARE(client.fetches.size(), 2);

        // The first fetch resolves late and is dropped: its key ($mediaA) is
        // not what the controller now waits for ($mediaB).
        client.succeed(staleOp, realGifBytes()); // row A: must never be sent
        QCOMPARE(client.attachmentSends.size(), 0);
        QCOMPARE(forwarded.count(), 0);
        QVERIFY(fwd.busy()); // still waiting on row B's fetch

        // Row B's fetch resolves and is sent to the current forward's target.
        client.succeed(client.fetches.at(1).opId, realPngBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().roomId, kRoomA);
        QCOMPARE(client.attachmentSends.first().bytes, realPngBytes());
        QCOMPARE(forwarded.count(), 1);
    }

    void cancelledForwardIgnoresLateMediaAnswer()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        fwd.cancel();
        QVERIFY(!fwd.active());

        // A star-class fetch cannot be cancelled, so it still resolves, but
        // nothing is sent once the forward was abandoned.
        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 0);
        QCOMPARE(forwarded.count(), 0);
    }

    void unrelatedMediaBridgeTrafficIsIgnored()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        // Another consumer of the shared MediaBridge (e.g. the GIF star)
        // resolves an unrelated key meanwhile.
        bridge.fetchFullForStar(QStringLiteral("$someoneElsesMedia"));
        QCOMPARE(client.fetches.size(), 2);
        client.succeed(client.fetches.at(1).opId, realPngBytes());

        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(fwd.busy()); // still waiting on its own key

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        // This forward's own payload, not the other consumer's.
        QCOMPARE(client.attachmentSends.first().bytes, realGifBytes());
    }

    // ---- one activation, one event ----

    void secondForwardToWhileBusyIsANoOp()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);
        QVERIFY(fwd.busy());

        // A second activation while the send is in flight dispatches no second
        // fetch.
        fwd.forwardTo(kRoomA);
        QCOMPARE(client.fetches.size(), 1);

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().roomId, kRoomB);
    }

    // ---- account isolation ----

    void logoutResetsAnInProgressForward()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QVERIFY(fwd.active());

        client.emitLoggedOut();
        QVERIFY(!fwd.active());
        QVERIFY(!fwd.busy());

        // A late answer must not resurrect the previous account's forward.
        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 0);
    }
    // The source filename is sender-chosen: it must carry no path structure a
    // receiving client could act on and cannot produce a hidden file.
    void forwardedFilenameIsSanitized()
    {
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral("../../../etc/passwd")),
                 QStringLiteral("passwd"));
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral("C:\\Windows\\System32\\evil.dll")),
                 QStringLiteral("evil.dll"));
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral(".bashrc")),
                 QStringLiteral("bashrc"));
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral("holiday.jpg")),
                 QStringLiteral("holiday.jpg"));
        // Bounded, so a pathological name cannot become the event body.
        QVERIFY(ForwardController::sanitizedForwardFilename(
                    QString(400, QLatin1Char('a'))).size() <= 128);
    }

    // The claimed MIME is sender-chosen too. Identifiable bytes win; bytes
    // claimed as an image that Lightning does not recognise (SVG included,
    // which never enters a media path) are refused rather than re-broadcast.
    void unverifiableImageClaimIsRefusedRatherThanReUploaded()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$svg")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        client.succeed(client.fetches.first().opId,
                       QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"));

        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(!fwd.error().isEmpty());
        QVERIFY2(fwd.active(),
                 "the dialog must stay open so the refusal is visible");
    }

    // Media can be forwarded to a room whose timeline is not open; the
    // timeline-scoped send refuses any room but the open one.
    void mediaForwardsToARoomWhoseTimelineIsNotOpen()
    {
        FakeClient client;
        client.openRoomId = kRoomA; // the user is reading room A
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomB); // ...and forwards into room B
        client.succeed(client.fetches.first().opId, realGifBytes());

        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().roomId, kRoomB);
        QCOMPARE(forwarded.count(), 1);
        QVERIFY(fwd.error().isEmpty());
    }

    // A backend without a room-scoped send still forwards into the open room.
    void backendWithoutRoomScopedSendStillForwardsIntoTheOpenRoom()
    {
        FakeClient client;
        client.roomScopedSupported = false;
        client.openRoomId = kRoomA;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomA);
        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QVERIFY(fwd.error().isEmpty());
    }

    // A large image forwards: the saved-GIF store's 4096px / 25 MiB caps
    // (gif::validateRasterBytes) must not apply here.
    void aLargeImageIsNotMistakenForUnsafeContent()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        // 5120x2880: past the GIF store's dimension cap, ordinary content.
        QImage big(5120, 2880, QImage::Format_RGB32);
        big.fill(Qt::blue);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        QVERIFY(big.save(&buf, "PNG"));
        buf.close();

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$big")));
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.first().opId, png);

        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().mime, QStringLiteral("image/png"));
        QCOMPARE(client.attachmentSends.first().width, 5120);
        QVERIFY2(fwd.error().isEmpty(), "a large PNG is ordinary content");
    }

};

QTEST_MAIN(ForwardMessageTest)
#include "ForwardMessageTest.moc"
