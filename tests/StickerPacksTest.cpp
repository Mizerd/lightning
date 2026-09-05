// MSC2545 stickers and custom emoji — StickerPackManager policy.
//
// What this pins:
//   * snapshot ingestion, and the usage narrowing (a pack's stickers and its
//     emoticons are two different grids over one pack);
//   * the selection rules: an unknown pack id is REFUSED rather than mapped
//     to the first pack, and a snapshot that drops the selected pack lands on
//     one that actually has content;
//   * `canSave` — a plain mxc only (an encrypted sticker has no url and can
//     never go in a pack), never gated on data nobody fetched, and closed
//     while a save is in flight;
//   * the SEND destination is the one passed in, room and thread are
//     separate calls, and a thread send never degrades to a room send (§8);
//   * custom-emoji lookup: prefix matching, one winner per shortcode with the
//     account's own pack first, and a bounded result;
//   * generation isolation — sign-out clears everything, and an answer to a
//     request this manager no longer owns is DROPPED rather than applied;
//   * nothing asks the network on room navigation.
//
// HONEST SCOPE: policy and wiring only, against a fake client. Real
// `im.ponies.*` round trips, a homeserver accepting or refusing an account
// data write, the on-screen picker, and Element interoperability of a sent
// `m.sticker` are NOT exercised here and are NOT TESTED.

#include "matrix/MatrixClient.h"
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackManager.h"
#include "stickers/StickerPackModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    bool packsSupported = true;
    bool refuseFetch = false;
    int fetchCalls = 0;
    int sendCalls = 0;
    int saveCalls = 0;
    QString lastFetchRoom;
    quint64 lastFetchOp = 0;
    quint64 lastSaveOp = 0;
    // The last send, captured exactly as it crossed.
    QString sendRoom, sendRoot, sendUrl, sendBody, sendMime;
    quint64 sendWidth = 0, sendHeight = 0, sendSize = 0;
    QString saveShortcode, saveUrl, saveBody, saveMime;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
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

    bool supportsStickerPacks() const override { return packsSupported; }
    void fetchStickerPacks(const QString &roomId, quint64 opId) override
    {
        if (refuseFetch)
            return;
        ++fetchCalls;
        lastFetchRoom = roomId;
        lastFetchOp = opId;
    }
    void sendSticker(const QString &roomId, const QString &rootId,
                     const QString &url, const QString &body,
                     const QString &mimetype, quint64 width, quint64 height,
                     quint64 size) override
    {
        ++sendCalls;
        sendRoom = roomId;
        sendRoot = rootId;
        sendUrl = url;
        sendBody = body;
        sendMime = mimetype;
        sendWidth = width;
        sendHeight = height;
        sendSize = size;
    }
    int roomsSetCalls = 0;
    QString roomsSetRoom, roomsSetKey;
    bool roomsSetEnabled = false;
    quint64 lastRoomsOp = 0;
    void setStickerRoomPackEnabled(const QString &roomId,
                                   const QString &stateKey, bool enabled,
                                   quint64 opId) override
    {
        ++roomsSetCalls;
        roomsSetRoom = roomId;
        roomsSetKey = stateKey;
        roomsSetEnabled = enabled;
        lastRoomsOp = opId;
    }
    int roomPackAddCalls = 0;
    QString roomPackRoom, roomPackStateKey, roomPackShortcode;
    quint64 lastRoomPackOp = 0;
    void addStickerToRoomPack(const QString &roomId, const QString &stateKey,
                              const QString &shortcode, const QString &,
                              const QString &, const QString &, quint64,
                              quint64, quint64, quint64 opId) override
    {
        ++roomPackAddCalls;
        roomPackRoom = roomId;
        roomPackStateKey = stateKey;
        roomPackShortcode = shortcode;
        lastRoomPackOp = opId;
    }
    int editCalls = 0;
    QString editRoom, editStateKey, editAction, editArgA, editArgB;
    quint64 lastEditOp = 0;
    void editStickerPack(const QString &roomId, const QString &stateKey,
                         const QString &action, const QString &argA,
                         const QString &argB, quint64 opId) override
    {
        ++editCalls;
        editRoom = roomId;
        editStateKey = stateKey;
        editAction = action;
        editArgA = argA;
        editArgB = argB;
        lastEditOp = opId;
    }
    void addStickerToUserPack(const QString &shortcode, const QString &url,
                              const QString &body, const QString &mimetype,
                              quint64, quint64, quint64,
                              quint64 opId) override
    {
        ++saveCalls;
        saveShortcode = shortcode;
        saveUrl = url;
        saveBody = body;
        saveMime = mimetype;
        lastSaveOp = opId;
    }
};

QVariantMap image(const QString &shortcode, const QString &url,
                  bool isSticker, bool isEmoticon,
                  const QString &body = QString())
{
    QVariantMap m;
    m.insert(QStringLiteral("shortcode"), shortcode);
    m.insert(QStringLiteral("url"), url);
    m.insert(QStringLiteral("body"), body.isEmpty() ? shortcode : body);
    m.insert(QStringLiteral("mimetype"), QStringLiteral("image/png"));
    m.insert(QStringLiteral("width"), 128);
    m.insert(QStringLiteral("height"), 96);
    m.insert(QStringLiteral("size"), qlonglong(4096));
    m.insert(QStringLiteral("isEmoticon"), isEmoticon);
    m.insert(QStringLiteral("isSticker"), isSticker);
    return m;
}

QVariantMap pack(const QString &id, const QString &source,
                 const QString &displayName, const QVariantList &images,
                 const QString &attribution = QString())
{
    QVariantMap p;
    p.insert(QStringLiteral("id"), id);
    p.insert(QStringLiteral("displayName"), displayName);
    p.insert(QStringLiteral("avatarUrl"), QString());
    p.insert(QStringLiteral("attribution"), attribution);
    p.insert(QStringLiteral("source"), source);
    p.insert(QStringLiteral("roomId"),
             source == QLatin1String("room")
                 ? QStringLiteral("!r:example.org") : QString());
    p.insert(QStringLiteral("stateKey"), QString());
    p.insert(QStringLiteral("images"), images);
    return p;
}

const QString kRoom = QStringLiteral("!room:example.org");
const QString kOther = QStringLiteral("!other:example.org");
const QString kMxcA = QStringLiteral("mxc://example.org/a");
const QString kMxcB = QStringLiteral("mxc://example.org/b");
const QString kMxcC = QStringLiteral("mxc://example.org/c");

// A snapshot with a user pack (one sticker, one emoticon) and a room pack
// (one sticker, one emoticon whose shortcode COLLIDES with the user pack's).
QVariantList twoPacks()
{
    return {
        pack(QStringLiteral("user"), QStringLiteral("user"),
             QStringLiteral("Your pack"),
             { image(QStringLiteral("cat"), kMxcA, true, false),
               image(QStringLiteral("wave"), kMxcB, false, true) }),
        pack(QStringLiteral("room:!r:example.org:"), QStringLiteral("room"),
             QStringLiteral("Cat Lovers"),
             { image(QStringLiteral("dog"), kMxcC, true, false),
               image(QStringLiteral("wave"),
                     QStringLiteral("mxc://example.org/other-wave"),
                     false, true) },
             QStringLiteral("by the room")),
    };
}

} // namespace

class StickerPacksTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // ---- capability -----------------------------------------------------

    void aBackendWithoutPacksOffersNothingAndNeverAsks()
    {
        FakeClient client;
        client.packsSupported = false;
        StickerPackManager manager;
        manager.setClient(&client);

        QVERIFY(!manager.available());
        manager.refresh();
        manager.refreshIfStale();
        QCOMPARE(client.fetchCalls, 0);
        // And nothing may be saved through a backend that has no packs.
        QVERIFY(!manager.canSave(kMxcA));
    }

    // ---- snapshot + usage narrowing -------------------------------------

    void aSnapshotNarrowsToTheSelectedPackAndTheAskedForUsage()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        QCOMPARE(manager.packs()->count(), 2);
        QVERIFY(manager.loaded());
        // The first pack with a STICKER is selected, and the grid holds that
        // pack's stickers only — not its emoticon, and not the other pack's.
        QCOMPARE(manager.selectedPackId(), QStringLiteral("user"));
        QCOMPARE(manager.images()->count(), 1);
        QCOMPARE(manager.images()->get(0).value(QStringLiteral("shortcode"))
                     .toString(),
                 QStringLiteral("cat"));

        manager.setUsage(QStringLiteral("emoticon"));
        QCOMPARE(manager.images()->count(), 1);
        QCOMPARE(manager.images()->get(0).value(QStringLiteral("shortcode"))
                     .toString(),
                 QStringLiteral("wave"));

        // Both packs hold one of each, so both are usable under either usage.
        QCOMPARE(manager.usablePackCount(), 2);
    }

    void anUnknownPackIdIsRefusedRatherThanMappedToTheFirstPack()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        manager.setSelectedPackId(QStringLiteral("room:!r:example.org:"));
        QCOMPARE(manager.selectedPackId(),
                 QStringLiteral("room:!r:example.org:"));
        QCOMPARE(manager.images()->get(0).value(QStringLiteral("shortcode"))
                     .toString(),
                 QStringLiteral("dog"));

        // A tab that quietly becomes a different tab is worse than one that
        // does not move.
        manager.setSelectedPackId(QStringLiteral("room:!gone:example.org:"));
        QCOMPARE(manager.selectedPackId(),
                 QStringLiteral("room:!r:example.org:"));
    }

    void aSnapshotThatDropsTheSelectedPackLandsOnOneWithContent()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        manager.setSelectedPackId(QStringLiteral("room:!r:example.org:"));

        // The room pack is gone, and the surviving pack's only sticker is
        // what the grid must show.
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });
        QCOMPARE(manager.selectedPackId(), QStringLiteral("user"));
        QCOMPARE(manager.images()->count(), 1);
    }

    void anEmoticonOnlyPackIsNotSelectedForAStickerGridWhenAnotherPackHasOne()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        // Emoticon-only pack FIRST: landing on it would open the picker on an
        // empty grid while a populated pack sits one tab away.
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("wave"), kMxcB, false, true) }),
              pack(QStringLiteral("room:!r:example.org:"),
                   QStringLiteral("room"), QStringLiteral("Cat Lovers"),
                   { image(QStringLiteral("dog"), kMxcC, true, false) }) });
        QCOMPARE(manager.selectedPackId(),
                 QStringLiteral("room:!r:example.org:"));
        QCOMPARE(manager.usablePackCount(), 1);
    }

    void aRowWithoutAUrlOrAShortcodeNeverBecomesATile()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap noUrl = image(QStringLiteral("broken"), QString(), true,
                                  false);
        QVariantMap noCode = image(QString(), kMxcC, true, false);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { noUrl, noCode,
                     image(QStringLiteral("cat"), kMxcA, true, false) }) });
        QCOMPARE(manager.images()->count(), 1);
    }

    // ---- saving ("add to my stickers") ----------------------------------

    void savingRefusesAnythingThatIsNotAPlainMxc()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);

        // An ENCRYPTED sticker carries an EncryptedFile and no url at all, so
        // there is nothing a pack could hold. Same for anything a caller
        // could otherwise assemble.
        QVERIFY(!manager.canSave(QString()));
        QVERIFY(!manager.canSave(QStringLiteral("https://example.org/a.png")));
        QVERIFY(!manager.canSave(QStringLiteral("file:///etc/passwd")));
        QVERIFY(manager.canSave(kMxcA));

        manager.saveSticker(QStringLiteral("https://example.org/a.png"),
                            QStringLiteral("cat"),
                            QStringLiteral("image/png"), 1, 1, 1);
        QCOMPARE(client.saveCalls, 0);
    }

    void savingIsOfferedWithoutALoadedSnapshotAndIsClosedWhileOneIsInFlight()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);

        // Nothing has been fetched: gating here would grey the action out
        // because nothing LOOKED.
        QVERIFY(!manager.loaded());
        QVERIFY(manager.canSave(kMxcA));

        manager.saveSticker(kMxcA, QStringLiteral("A cat"),
                            QStringLiteral("image/png"), 128, 96, 4096);
        QCOMPARE(client.saveCalls, 1);
        QCOMPARE(client.saveUrl, kMxcA);
        // The shortcode is derived from the BODY, not from an event id.
        QCOMPARE(client.saveShortcode, QStringLiteral("A cat"));
        QVERIFY(manager.saving());
        // One at a time.
        QVERIFY(!manager.canSave(kMxcB));
        manager.saveSticker(kMxcB, QStringLiteral("dog"),
                            QStringLiteral("image/png"), 1, 1, 1);
        QCOMPARE(client.saveCalls, 1);
    }

    void aSuccessfulSaveRefetchesAndAFailureReportsItsCategory()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QSignalSpy finished(&manager, &StickerPackManager::saveFinished);

        manager.saveSticker(kMxcA, QStringLiteral("cat"),
                            QStringLiteral("image/png"), 1, 1, 1);
        const int before = client.fetchCalls;
        Q_EMIT client.stickerPackAddFinished(client.lastSaveOp, true,
                                             QString(),
                                             QStringLiteral("cat"));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cat"));
        // Both destinations report here, so the report names which one.
        QCOMPARE(finished.at(0).at(3).toString(), QStringLiteral("account"));
        // Nothing is applied optimistically: the authoritative pack is
        // re-read after the write.
        QCOMPARE(client.fetchCalls, before + 1);
        QVERIFY(!manager.saving());

        manager.saveSticker(kMxcB, QStringLiteral("cat"),
                            QStringLiteral("image/png"), 1, 1, 1);
        Q_EMIT client.stickerPackAddFinished(client.lastSaveOp, false,
                                             QStringLiteral("duplicate"),
                                             QString());
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.at(1).at(0).toBool(), false);
        QCOMPARE(finished.at(1).at(1).toString(),
                 QStringLiteral("duplicate"));
    }

    void isSavedAnswersOnlyFromTheAccountsOwnPack()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        QVERIFY(manager.isSaved(kMxcA));   // the user pack's sticker
        QVERIFY(manager.isSaved(kMxcB));   // the user pack's emoticon
        QVERIFY(!manager.isSaved(kMxcC));  // a ROOM pack's sticker
        QVERIFY(!manager.isSaved(QString()));
    }

    // ---- sending --------------------------------------------------------

    void aSendCarriesTheImageToTheDestinationItWasGiven()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        manager.sendToRoom(kRoom, manager.images()->get(0));
        QCOMPARE(client.sendCalls, 1);
        QCOMPARE(client.sendRoom, kRoom);
        QVERIFY(client.sendRoot.isEmpty());
        QCOMPARE(client.sendUrl, kMxcA);
        QCOMPARE(client.sendBody, QStringLiteral("cat"));
        QCOMPARE(client.sendMime, QStringLiteral("image/png"));
        QCOMPARE(client.sendWidth, 128u);
        QCOMPARE(client.sendHeight, 96u);
        QCOMPARE(client.sendSize, 4096u);
    }

    void aThreadSendCarriesItsRootAndNeverDegradesToARoomSend()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        const QString root = QStringLiteral("$root:example.org");

        manager.sendToThread(kRoom, root, manager.images()->get(0));
        QCOMPARE(client.sendCalls, 1);
        QCOMPARE(client.sendRoot, root);

        // §8: a thread send with no root must FAIL, never fall back to the
        // room timeline.
        manager.sendToThread(kRoom, QString(), manager.images()->get(0));
        QCOMPARE(client.sendCalls, 1);
    }

    void aSendWithNoUrlIsRefusedRatherThanDispatched()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap empty;
        empty.insert(QStringLiteral("shortcode"), QStringLiteral("cat"));
        manager.sendToRoom(kRoom, empty);
        manager.sendToRoom(QString(), image(QStringLiteral("cat"), kMxcA,
                                            true, false));
        QCOMPARE(client.sendCalls, 0);
    }

    // ---- custom emoji ---------------------------------------------------

    void emoticonLookupMatchesByPrefixDedupesAndIsBounded()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        // Only emoticons, never stickers.
        const QVariantList all = manager.findEmoticons(QString(), 50);
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("shortcode")).toString(),
                 QStringLiteral("wave"));
        // One winner per shortcode, and the ACCOUNT's pack comes first in the
        // snapshot, so the user's own "wave" beats the room pack's.
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("url")).toString(),
                 kMxcB);
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("packName")).toString(),
                 QStringLiteral("Your pack"));

        QCOMPARE(manager.findEmoticons(QStringLiteral("WA"), 50).size(), 1);
        QVERIFY(manager.findEmoticons(QStringLiteral("zz"), 50).isEmpty());
        QCOMPARE(manager.findEmoticons(QString(), 0).size(), 1);

        const QVariantMap one = manager.emoticon(QStringLiteral("wave"));
        QCOMPARE(one.value(QStringLiteral("url")).toString(), kMxcB);
        QVERIFY(manager.emoticon(QStringLiteral("cat")).isEmpty()); // a sticker
        QVERIFY(manager.emoticon(QString()).isEmpty());
    }

    void theEmoticonBoundHoldsAgainstAHugeRequest()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantList images;
        for (int n = 0; n < 300; ++n) {
            images.append(image(QStringLiteral("e%1").arg(n, 4, 10,
                                                          QChar('0')),
                                QStringLiteral("mxc://example.org/%1").arg(n),
                                false, true));
        }
        manager.applySnapshotForTest(
            kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"), images) });
        // A completion popup that can grow without limit covers the composer.
        QCOMPARE(manager.findEmoticons(QString(), 10000).size(), 64);
    }

    // ---- refresh policy and generation isolation ------------------------

    void roomNavigationMarksTheSnapshotStaleAndAsksNothing()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.setActiveRoomId(kRoom);
        manager.refreshIfStale();
        QCOMPARE(client.fetchCalls, 1);
        QCOMPARE(client.lastFetchRoom, kRoom);
        Q_EMIT client.stickerPacksReceived(client.lastFetchOp, kRoom, false, twoPacks());

        // A second open over the SAME room re-reads nothing.
        manager.refreshIfStale();
        QCOMPARE(client.fetchCalls, 1);

        // Walking through rooms issues no request at all...
        manager.setActiveRoomId(kOther);
        manager.setActiveRoomId(kRoom);
        manager.setActiveRoomId(kOther);
        QCOMPARE(client.fetchCalls, 1);
        // ...and the next open picks the new room's packs up.
        manager.refreshIfStale();
        QCOMPARE(client.fetchCalls, 2);
        QCOMPARE(client.lastFetchRoom, kOther);
    }

    void aRefreshWhileOneIsInFlightIsOwedNotDropped()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.refresh();
        QCOMPARE(client.fetchCalls, 1);
        manager.refresh();
        manager.refresh();
        QCOMPARE(client.fetchCalls, 1);

        // The in-flight read predates whatever made the refresh due, so its
        // answer is stale by construction and the owed one goes out.
        Q_EMIT client.stickerPacksReceived(client.lastFetchOp, QString(), false, twoPacks());
        QCOMPARE(client.fetchCalls, 2);
    }

    void anAnswerThisManagerNoLongerOwnsIsDropped()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.refresh();
        const quint64 op = client.lastFetchOp;

        // Not our op — a stale request, or another surface's.
        Q_EMIT client.stickerPacksReceived(op + 99, QString(), false, twoPacks());
        QVERIFY(!manager.loaded());
        QCOMPARE(manager.packs()->count(), 0);

        Q_EMIT client.stickerPacksReceived(op, QString(), false, twoPacks());
        QVERIFY(manager.loaded());
    }

    void signOutClearsEverythingAndTheOldAccountsAnswerCannotRepaintIt()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.refresh();
        const quint64 op = client.lastFetchOp;
        Q_EMIT client.stickerPacksReceived(op, QString(), false, twoPacks());
        QCOMPARE(manager.packs()->count(), 2);
        QVERIFY(manager.isSaved(kMxcA));

        Q_EMIT client.loggedOut();
        QCOMPARE(manager.packs()->count(), 0);
        QCOMPARE(manager.images()->count(), 0);
        QVERIFY(manager.selectedPackId().isEmpty());
        QVERIFY(!manager.loaded());
        // Packs are ACCOUNT DATA. The previous account's snapshot must never
        // populate the next one.
        QVERIFY(!manager.isSaved(kMxcA));
        Q_EMIT client.stickerPacksReceived(op, QString(), false, twoPacks());
        QCOMPARE(manager.packs()->count(), 0);
    }

    // ---- room-pack activation (im.ponies.emote_rooms) -------------------

    void turningARoomPackOnCarriesItsRoomAndStateKey()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        manager.setRoomPackEnabled(QStringLiteral("room:!r:example.org:"),
                                   true);
        QCOMPARE(client.roomsSetCalls, 1);
        QCOMPARE(client.roomsSetRoom, QStringLiteral("!r:example.org"));
        // The empty state key is the room's DEFAULT pack and is a real key,
        // not a missing one.
        QVERIFY(client.roomsSetKey.isEmpty());
        QCOMPARE(client.roomsSetEnabled, true);
        QVERIFY(manager.togglingRoomPack());

        // One at a time: two concurrent read-modify-writes of the same
        // account-data event would race.
        manager.setRoomPackEnabled(QStringLiteral("room:!r:example.org:"),
                                   false);
        QCOMPARE(client.roomsSetCalls, 1);
    }

    void theAccountsOwnPackHasNothingToEnableAndIsRefused()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        // A user pack is global by definition; writing its id into
        // im.ponies.emote_rooms would invent a shape the MSC does not have.
        manager.setRoomPackEnabled(QStringLiteral("user"), true);
        // And an id the snapshot does not hold at all.
        manager.setRoomPackEnabled(QStringLiteral("room:!gone:example.org:"),
                                   true);
        QCOMPARE(client.roomsSetCalls, 0);
    }

    void aRoomPackToggleIsNotAppliedOptimistically()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy done(&manager,
                        &StickerPackManager::roomPackToggleFinished);

        manager.setRoomPackEnabled(QStringLiteral("room:!r:example.org:"),
                                   true);
        const int before = client.fetchCalls;
        Q_EMIT client.stickerPackRoomsSet(client.lastRoomsOp, true, QString(),
                                          QStringLiteral("!r:example.org"),
                                          QString(), true);
        // The authoritative snapshot is what moves the switch.
        QCOMPARE(client.fetchCalls, before + 1);
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.at(0).at(0).toBool(), true);
        QVERIFY(!manager.togglingRoomPack());

        // A refusal re-reads NOTHING and reports its category.
        manager.setRoomPackEnabled(QStringLiteral("room:!r:example.org:"),
                                   false);
        const int after = client.fetchCalls;
        Q_EMIT client.stickerPackRoomsSet(client.lastRoomsOp, false,
                                          QStringLiteral("forbidden"),
                                          QStringLiteral("!r:example.org"),
                                          QString(), false);
        QCOMPARE(client.fetchCalls, after);
        QCOMPARE(done.size(), 2);
        QCOMPARE(done.at(1).at(1).toString(), QStringLiteral("forbidden"));
    }

    void theSnapshotCarriesEnabledAndCanManageThroughToTheModel()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap roomPack =
            pack(QStringLiteral("room:!r:example.org:"),
                 QStringLiteral("room"), QStringLiteral("Cat Lovers"),
                 { image(QStringLiteral("dog"), kMxcC, true, false) });
        roomPack.insert(QStringLiteral("enabledGlobally"), true);
        roomPack.insert(QStringLiteral("canManage"), true);
        manager.applySnapshotForTest(kRoom, false, { roomPack });

        const QVariantMap row = manager.packs()->get(0);
        QCOMPARE(row.value(QStringLiteral("enabledGlobally")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("canManage")).toBool(), true);

        // Both default to FALSE when the payload says nothing: an unknown
        // permission must never be presented as permission.
        manager.applySnapshotForTest(kOther, false, twoPacks());
        const QVariantMap plain = manager.packs()->get(1);
        QCOMPARE(plain.value(QStringLiteral("enabledGlobally")).toBool(), false);
        QCOMPARE(plain.value(QStringLiteral("canManage")).toBool(), false);
    }

    // ---- the ROOM-STATE write (im.ponies.room_emotes) -------------------

    void addingToARoomPackIsRefusedWithoutAPermissionTheSnapshotReported()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);

        // Nothing loaded: absence of the claim is NOT permission.
        QVERIFY(!manager.canSaveToRoom(kRoom, kMxcA));

        // A snapshot that says nothing about the permission.
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QVERIFY(!manager.canSaveToRoom(kRoom, kMxcA));
        manager.saveStickerToRoom(kRoom, kMxcA, QStringLiteral("cat"),
                                  QStringLiteral("image/png"), 1, 1, 1);
        QCOMPARE(client.roomPackAddCalls, 0);

        // A snapshot that DOES.
        manager.applySnapshotForTest(kRoom, true, twoPacks());
        QVERIFY(manager.canSaveToRoom(kRoom, kMxcA));
        // ...but a permission learned about THIS room says nothing about
        // another one.
        QVERIFY(!manager.canSaveToRoom(kOther, kMxcA));
        // ...and the mxc rule still applies.
        QVERIFY(!manager.canSaveToRoom(
            kRoom, QStringLiteral("https://example.org/a.png")));
    }

    void aRoomPackAddCarriesTheDefaultStateKeyAndReportsLikeAUserPackAdd()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, true, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::saveFinished);

        manager.saveStickerToRoom(kRoom, kMxcA, QStringLiteral("A cat"),
                                  QStringLiteral("image/png"), 128, 96, 4096);
        QCOMPARE(client.roomPackAddCalls, 1);
        QCOMPARE(client.roomPackRoom, kRoom);
        // The EMPTY state key is the room's DEFAULT pack, which is what
        // MSC2545 means by it — not a missing key.
        QVERIFY(client.roomPackStateKey.isEmpty());
        QCOMPARE(client.roomPackShortcode, QStringLiteral("A cat"));
        QVERIFY(manager.saving());

        // One save at a time, whichever pack it targets: both paths share the
        // one in-flight slot and the one report.
        QVERIFY(!manager.canSave(kMxcB));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomPackOp, false,
                                             QStringLiteral("forbidden"),
                                             QString());
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(1).toString(),
                 QStringLiteral("forbidden"));
        QCOMPARE(finished.at(0).at(3).toString(), QStringLiteral("room"));
        QVERIFY(!manager.saving());
    }

    void theRevisionMovesOnEveryStateChangeSoAnInvokableBindingReEvaluates()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        const int start = manager.revision();
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QVERIFY(manager.revision() > start);
        const int afterSnapshot = manager.revision();
        manager.setUsage(QStringLiteral("emoticon"));
        QVERIFY(manager.revision() > afterSnapshot);
    }

    // ── Pack management (MSC2545 CRUD) ──────────────────────────────────
    //
    // Four verbs share one op slot and one permission rule. What is pinned
    // here is that they route to the right STORE, that the permission rule
    // is asked before anything is sent, and that a second click cannot race
    // the first.

    void anAccountPackIsAlwaysManageableAndARoomPackAsksThePowerLevel()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);

        QVariantMap roomPack = pack(QStringLiteral("room:!r:example.org:"),
                                    QStringLiteral("room"),
                                    QStringLiteral("Cat Lovers"),
                                    { image(QStringLiteral("dog"), kMxcC,
                                            true, false) });
        // Not manageable: the snapshot did not say this account may write it.
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }),
              roomPack });
        QVERIFY2(manager.canManagePack(QStringLiteral("user")),
                 "an account's own pack is account data; nobody holds a "
                 "power level over it");
        QVERIFY2(!manager.canManagePack(QStringLiteral("room:!r:example.org:")),
                 "the ABSENCE of the claim is not permission");
        QVERIFY(!manager.canManagePack(QStringLiteral("nosuchpack")));

        // ...and with the claim present, it is.
        roomPack.insert(QStringLiteral("canManage"), true);
        manager.applySnapshotForTest(kRoom, true, { roomPack });
        QVERIFY(manager.canManagePack(QStringLiteral("room:!r:example.org:")));
    }

    void anUnmanageablePackSendsNothingAtAll()
    {
        // The UI hides the actions, but the gate is HERE: a caller that did
        // not ask must not reach the server and collect a refusal.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("room:!r:example.org:"),
                   QStringLiteral("room"), QStringLiteral("Cat Lovers"),
                   { image(QStringLiteral("dog"), kMxcC, true, false) }) });

        manager.removeImageFromPack(QStringLiteral("room:!r:example.org:"),
                                    QStringLiteral("dog"));
        manager.renamePack(QStringLiteral("room:!r:example.org:"),
                           QStringLiteral("Mine now"));
        manager.deletePack(QStringLiteral("room:!r:example.org:"));
        QCOMPARE(client.editCalls, 0);
        QVERIFY(!manager.editing());
    }

    void eachVerbSendsItsOwnActionToTheRightStore()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });

        manager.removeImageFromPack(QStringLiteral("user"),
                                    QStringLiteral("cat"));
        QCOMPARE(client.editCalls, 1);
        QCOMPARE(client.editAction, QStringLiteral("remove_image"));
        QCOMPARE(client.editArgA, QStringLiteral("cat"));
        // An ACCOUNT pack carries no room: an empty room id is what selects
        // the account-data store on the far side, so a stray room id here
        // would write somebody's room instead.
        QVERIFY2(client.editRoom.isEmpty(),
                 "an account pack must not be routed to a room");

        // One op slot: a second verb while the first is in flight sends
        // nothing rather than racing it.
        manager.renamePack(QStringLiteral("user"), QStringLiteral("Mine"));
        QCOMPARE(client.editCalls, 1);
        QVERIFY(manager.editing());

        // The answer frees it, and the next verb goes through.
        Q_EMIT client.stickerPackEditFinished(client.lastEditOp, true,
                                              QString(), QString());
        QVERIFY(!manager.editing());
        manager.renamePack(QStringLiteral("user"), QStringLiteral("Mine"));
        QCOMPARE(client.editCalls, 2);
        QCOMPARE(client.editAction, QStringLiteral("set_name"));
        QCOMPARE(client.editArgA, QStringLiteral("Mine"));
    }

    void anEmptyPackNameIsSentRatherThanRefused()
    {
        // Clearing the name is a REAL state — for a room pack it restores
        // MSC2545's fallback to the room's own name. A guard that treated
        // empty as "nothing to do" would make that unreachable.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });

        manager.renamePack(QStringLiteral("user"), QString());
        QCOMPARE(client.editCalls, 1);
        QCOMPARE(client.editAction, QStringLiteral("set_name"));
        QVERIFY(client.editArgA.isEmpty());
    }

    void arenameNeedsBothNamesAndAnEmptyTargetIsRefusedLocally()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });

        manager.renameImageInPack(QStringLiteral("user"), QStringLiteral("cat"),
                                  QStringLiteral("   "));
        manager.renameImageInPack(QStringLiteral("user"), QString(),
                                  QStringLiteral("dog"));
        QCOMPARE(client.editCalls, 0);

        manager.renameImageInPack(QStringLiteral("user"), QStringLiteral("cat"),
                                  QStringLiteral("kitty"));
        QCOMPARE(client.editCalls, 1);
        QCOMPARE(client.editAction, QStringLiteral("rename_image"));
        QCOMPARE(client.editArgA, QStringLiteral("cat"));
        QCOMPARE(client.editArgB, QStringLiteral("kitty"));
    }

    void aFailedEditDoesNotRereadAndReportsTheCategory()
    {
        // Nothing was applied optimistically, so a refusal must leave the
        // last known pack alone rather than emptying the picker.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });
        const int fetchesBefore = client.fetchCalls;

        QSignalSpy spy(&manager, &StickerPackManager::editFinished);
        manager.renameImageInPack(QStringLiteral("user"), QStringLiteral("cat"),
                                  QStringLiteral("dog"));
        Q_EMIT client.stickerPackEditFinished(
            client.lastEditOp, false, QStringLiteral("shortcode_taken"),
            QString());

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toString(),
                 QStringLiteral("shortcode_taken"));
        QCOMPARE(client.fetchCalls, fetchesBefore);
        QCOMPARE(manager.images()->count(), 1);
        QVERIFY(!manager.editing());
    }

    void anAnswerForAnOldOperationIsIgnored()
    {
        // A late answer from a previous edit must not free the slot the
        // current one is holding, or the UI re-enables mid-flight.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });

        manager.deletePack(QStringLiteral("user"));
        const quint64 current = client.lastEditOp;
        QVERIFY(manager.editing());
        Q_EMIT client.stickerPackEditFinished(current + 999, true, QString(),
                                              QString());
        QVERIFY2(manager.editing(),
                 "a stale answer must not free the live operation");
        Q_EMIT client.stickerPackEditFinished(current, true, QString(),
                                              QString());
        QVERIFY(!manager.editing());
    }

    void packInfoAnswersIdentityAndPermissionInOneCall()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });

        const QVariantMap info = manager.packInfo(QStringLiteral("user"));
        QCOMPARE(info.value(QStringLiteral("packId")).toString(),
                 QStringLiteral("user"));
        QCOMPARE(info.value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("Your pack"));
        QCOMPARE(info.value(QStringLiteral("canManage")).toBool(), true);
        // An unknown id answers EMPTY rather than a map of blanks, so a
        // caller cannot mistake it for a real pack it may edit.
        QVERIFY(manager.packInfo(QStringLiteral("nope")).isEmpty());
    }
};

QTEST_MAIN(StickerPacksTest)
#include "StickerPacksTest.moc"
