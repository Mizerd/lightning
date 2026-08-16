// v0.7.x message forwarding (task #14, forward-spec.md) — ForwardController
// policy tests. Deterministic: a local FakeClient stands in for the SDK
// bridge (no network, no credentials, no live GIPHY/KLIPY/Matrix calls), and
// a real MediaBridge is wired to it exactly as MediaBridgeTest.cpp does, so
// the media-forward path exercises MediaBridge's own dedup/cache machinery
// rather than a second, parallel fake of it.
//
// Pins:
//   * D7 — redacted / local-echo / undecryptable / empty-body content is
//     refused at begin(), never reaching a send;
//   * D4/D5 — a text forward carries the plain body ONLY, through the
//     2-argument sendTextMessage overload, which attaches no relation —
//     never a reply, never m.thread, regardless of what the SOURCE event
//     was;
//   * D1/D2 — a media forward re-fetches through MediaBridge (never trusts
//     bytes carried in the snapshot — the snapshot never carries any) and
//     re-uploads exactly the bytes the fetch returned;
//   * D6 — `forwarded()` (which AppController wires to opening the target
//     room) fires only once the send was actually dispatched, and a
//     dispatch failure leaves the dialog open (`active` stays true) with
//     `error` set, never silently swallowed;
//   * the async media-fetch race: a SECOND begin() (a different row, or the
//     same row forwarded again with a new target) must invalidate whatever
//     the FIRST forward's outstanding fetch was waiting for — a late answer
//     for the abandoned forward must never be sent to the new target;
//   * a cancelled forward's late fetch answer is dropped, not sent anywhere;
//   * an unrelated mediaBytesForStar (another consumer's star/save fetch
//     sharing the same MediaBridge) is ignored rather than mistaken for the
//     forward's own answer;
//   * one activation produces exactly one send — a second forwardTo() while
//     busy is a no-op, not a second dispatch.
//
// HONEST SCOPE: this is ForwardController policy only. The QML picker's own
// gating (MessageDelegate.qml's per-row `eligible` computation; the room
// list's Space/membership filter in ForwardMessageDialog.qml) is declarative
// QML and has no dedicated QML contract test in this round, matching the
// existing convention for DiscoverJoinDialog/ReportMessageDialog (neither
// has one either). Real Matrix sends, encryption, and Element
// interoperability of a forwarded message are NOT TESTED here.

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
    // 0 = simulate a queue-rejection (the send never entered the SDK).
    quint64 nextSendOp = 1;
    bool rejectNextAttachment = false;

    void sendTextMessage(const QString &roomId, const QString &body) override
    {
        textSends.append({ roomId, body });
    }
    // Models the REAL backend: the TIMELINE-scoped send refuses any room
    // but the open one (RustSdkMatrixClient::sendAttachmentBytes gates on
    // timelineActiveFor). Without this the suite accepted a send that fails
    // 100% of the time in the application, for exactly the case forwarding
    // exists to serve.
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

    // Pure virtuals (inert — this class exercises only the media/send
    // surface ForwardController actually touches).
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


// A REAL 1x1 GIF. The forward path re-originates the attachment under this
// account, so it sniffs the bytes rather than trusting the source event's
// claimed type — fixtures must therefore carry genuine magic, exactly as a
// forwarded image would.
// A REAL 1x1 PNG, so a test that must distinguish two payloads can do so by
// their sniffed type rather than by an unsniffable string.
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

} // namespace

class ForwardMessageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // ---- D7 refusals ----

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

    // ---- D4/D5 text path ----

    void textForwardSendsPlainBodyWithNoRelationThenNavigates()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, textSnapshot(QStringLiteral("hi from A")));
        QVERIFY(fwd.active());

        fwd.forwardTo(kRoomB);

        // D5: the ONLY send call is the plain 2-argument sendTextMessage —
        // there is no reply/thread call anywhere in ForwardController, so
        // this can never carry a relation into the target room.
        QCOMPARE(client.textSends.size(), 1);
        QCOMPARE(client.textSends.first().roomId, kRoomB);
        QCOMPARE(client.textSends.first().body, QStringLiteral("hi from A"));
        QCOMPARE(client.attachmentSends.size(), 0);

        // D6: dispatched -> forwarded() fires with the target, and the
        // dialog resets to idle (never left open on success).
        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.first().at(0).toString(), kRoomB);
        QVERIFY(!fwd.active());
        QVERIFY(!fwd.busy());
    }

    // ---- D1/D2 media path ----

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

        // The snapshot passed to begin() never carried bytes — proven by
        // the fact that a real MediaBridge fetch had to be dispatched at
        // all (a snapshot-trusting implementation would have sent
        // immediately, with zero fetches recorded).
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().mediaKey, QStringLiteral("$mediaX"));
        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(fwd.busy());

        const QByteArray realBytes = realGifBytes();
        client.succeed(client.fetches.first().opId, realBytes);

        // The re-upload carries EXACTLY the freshly fetched bytes (never
        // anything from the snapshot, which held none) plus the frozen
        // classification (filename/mime/dimensions) from activation time.
        QCOMPARE(client.attachmentSends.size(), 1);
        const auto &sent = client.attachmentSends.first();
        QCOMPARE(sent.roomId, kRoomB);
        QCOMPARE(sent.bytes, realBytes);
        QCOMPARE(sent.filename, QStringLiteral("cat.png"));
        // The truthful type from the bytes, not the source event's claim.
        QCOMPARE(sent.mime, QStringLiteral("image/gif"));
        // The dimensions the BYTES have, not the ones the source event
        // claimed (100x80). A forward re-originates the attachment under
        // this account, so it must not attest to a shape it did not check.
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

        // D6: never swallowed — reported via `error`, and the picker stays
        // open (the user can retry or pick a different room) rather than
        // silently closing on a failure.
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

    // ---- the async race: a second forward must not be hijacked by a
    // stale answer belonging to the first ----

    void secondBeginInvalidatesFirstForwardsPendingFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        // First forward: row A's photo, sent toward room B — but never
        // resolved before the user picks a completely different message.
        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);
        const quint64 staleOp = client.fetches.first().opId;

        // The user closed that picker and forwarded a DIFFERENT row (B's
        // photo) to a DIFFERENT room instead.
        fwd.begin(kRoomA, kEventB, mediaSnapshot(QStringLiteral("$mediaB")));
        fwd.forwardTo(kRoomA);
        QCOMPARE(client.fetches.size(), 2);

        // The FIRST fetch (row A's photo) resolves late. It must be
        // dropped: it names a media key ($mediaA) that no longer matches
        // what this controller is currently waiting for ($mediaB).
        client.succeed(staleOp, realGifBytes()); // row A — must never be sent
        QCOMPARE(client.attachmentSends.size(), 0);
        QCOMPARE(forwarded.count(), 0);
        QVERIFY(fwd.busy()); // still waiting on row B's fetch

        // Row B's fetch now resolves — THIS is the one that must send, to
        // the target the CURRENT (second) forward actually chose.
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

        // MediaBridge has no cancellation hook for a star-class fetch, so
        // the underlying request still resolves — but nothing must be sent
        // anywhere once the forward itself has been abandoned.
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

        // Some OTHER consumer of the same shared MediaBridge (e.g. the GIF
        // star action) resolves a completely unrelated key while this
        // forward is still waiting.
        bridge.fetchFullForStar(QStringLiteral("$someoneElsesMedia"));
        QCOMPARE(client.fetches.size(), 2);
        client.succeed(client.fetches.at(1).opId, realPngBytes());

        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(fwd.busy()); // still waiting on its own key

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        // THIS forward's own payload, not the unrelated consumer's.
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

        // A rapid second click/Return on the same or a different room row
        // while the first send is still in flight must not dispatch a
        // second fetch.
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

        // The next account's answer for whatever this fetch resolves to
        // must not resurrect the previous account's forward.
        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 0);
    }
    // A forward RE-ORIGINATES the attachment under this account, so the
    // source event's filename — chosen by whoever sent it — must not carry
    // path structure a receiving client could act on when saving, and must
    // not be able to produce a hidden file.
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

    // The source event's claimed MIME is equally sender-chosen. Where the
    // bytes can be identified the truth wins; where they claim to be an
    // image but are not one Lightning recognizes — SVG included, which must
    // never enter a media path — the forward is refused rather than
    // re-broadcast under this account's name.
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

    // The whole point of the feature: forwarding media to a room the user is
    // NOT currently looking at. The timeline-scoped send refuses any room but
    // the open one, so routing through it made every real media forward fail
    // while passing a suite whose fake accepted any room id.
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

    // A backend without a room-scoped send still works for the one case the
    // timeline-scoped path can serve, rather than losing the feature.
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

    // A large image must forward. The first version of this gate reused
    // gif::validateRasterBytes, which carries the saved-GIF store's 4096px /
    // 25 MiB caps — so a 5K screenshot or a big camera JPEG was refused with
    // the same wording as an SVG.
    void aLargeImageIsNotMistakenForUnsafeContent()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        // 5120x2880 — past the GIF store's dimension cap, ordinary content.
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
