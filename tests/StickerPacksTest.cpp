// MSC2545 stickers and custom emoji: StickerPackManager policy.
//
// What this pins:
//   * snapshot ingestion, and the usage narrowing (a pack's stickers and its
//     emoticons are two different grids over one pack);
//   * the selection rules: an unknown pack id is refused rather than mapped
//     to the first pack, and a snapshot that drops the selected pack lands on
//     one that has content;
//   * `canSave`: a plain mxc only (an encrypted sticker has no url), never
//     gated on data nobody fetched, and closed while a save is in flight;
//   * the send destination is the one passed in, room and thread are
//     separate calls, and a thread send never degrades to a room send;
//   * custom-emoji lookup: prefix matching, one winner per shortcode with the
//     account's own pack first, and a bounded result;
//   * generation isolation: sign-out clears everything, and an answer to a
//     request this manager no longer owns is dropped;
//   * nothing asks the network on room navigation.
//
// Policy and wiring only, against a fake client: real `im.ponies.*` round
// trips, the on-screen picker and Element interoperability are not exercised.

#include "matrix/MatrixClient.h"
#include "media/SvgRasterJob.h"
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackArchive.h"
#include "stickers/StickerPackManager.h"
#include "stickers/StickerPackModel.h"

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QSettings>
#include <QDir>
#include <QJSEngine>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <limits>
#include <QElapsedTimer>
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
    QString saveShortcode, saveUrl, saveBody, saveMime, saveUsage;
    // The last file upload, as it crossed.
    int uploadCalls = 0;
    QString uploadPath;
    QString uploadShortcode, uploadUsage;
    QStringList uploadPaths;
    quint64 lastUploadOp = 0;
    QString uploadBody;
    void uploadStickerToUserPack(const QString &shortcode, const QString &body,
                                 const QString &localPath,
                                 const QString &usage,
                                 quint64 opId) override
    {
        ++uploadCalls;
        uploadPath = localPath;
        uploadPaths.append(localPath);
        uploadShortcode = shortcode;
        uploadBody = body;
        uploadUsage = usage;
        lastUploadOp = opId;
    }

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString userId = QStringLiteral("@me:example.org");
    QString currentUserId() const override { return userId; }
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
    bool lastFetchDeep = false;
    void fetchStickerPacks(const QString &roomId, quint64 opId,
                           bool deep) override
    {
        if (refuseFetch)
            return;
        ++fetchCalls;
        lastFetchRoom = roomId;
        lastFetchOp = opId;
        lastFetchDeep = deep;
    }
    int roomUploadCalls = 0;
    QString roomUploadKey, roomUploadNs, roomUploadCode, roomUploadPath, roomUploadReuse;
    quint64 lastRoomUploadOp = 0;
    void uploadStickerToRoomPack(const QString &, const QString &stateKey,
                                 const QString &ns, const QString &shortcode,
                                 const QString &, const QString &localPath,
                                 const QString &reuseUrl, quint64 opId) override
    {
        ++roomUploadCalls;
        roomUploadKey = stateKey;
        roomUploadNs = ns;
        roomUploadCode = shortcode;
        roomUploadPath = localPath;
        roomUploadReuse = reuseUrl;
        lastRoomUploadOp = opId;
    }
    int migrateCalls = 0;
    quint64 lastMigrateOp = 0;
    bool lastMigrateStartNew = false;
    void migrateUserStickerPack(quint64 opId, bool startNew) override
    {
        ++migrateCalls;
        lastMigrateOp = opId;
        lastMigrateStartNew = startNew;
    }
    int createCalls = 0;
    QString createRoom, createKey, createName, createUsage;
    void createStickerRoomPack(const QString &roomId, const QString &stateKey,
                               const QString &name, const QString &usage,
                               quint64 opId) override
    {
        ++createCalls;
        createRoom = roomId;
        createKey = stateKey;
        createName = name;
        createUsage = usage;
        lastEditOp = opId;
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
    QString roomsSetRoom, roomsSetKey, roomsSetNs;
    bool roomsSetEnabled = false;
    quint64 lastRoomsOp = 0;
    void setStickerRoomPackEnabled(const QString &roomId,
                                   const QString &stateKey, const QString &ns,
                                   bool enabled, quint64 opId) override
    {
        ++roomsSetCalls;
        roomsSetRoom = roomId;
        roomsSetKey = stateKey;
        roomsSetNs = ns;
        roomsSetEnabled = enabled;
        lastRoomsOp = opId;
    }
    int roomPackAddCalls = 0;
    QString roomPackRoom, roomPackStateKey, roomPackShortcode, roomPackNs;
    quint64 lastRoomPackOp = 0;
    void addStickerToRoomPack(const QString &roomId, const QString &stateKey,
                              const QString &ns,
                              const QString &shortcode, const QString &,
                              const QString &, const QString &, quint64,
                              quint64, quint64, quint64 opId) override
    {
        ++roomPackAddCalls;
        roomPackRoom = roomId;
        roomPackStateKey = stateKey;
        roomPackNs = ns;
        roomPackShortcode = shortcode;
        lastRoomPackOp = opId;
    }
    // Pack images fetched by their own mxc (export).
    QList<QPair<quint64, QString>> mxcFetches;
    quint64 nextFetchOp = 1000;
    quint64 fetchMxcThumbnail(const QString &mxc, int, int) override
    {
        const quint64 op = nextFetchOp++;
        mxcFetches.append({op, mxc});
        return op;
    }
    int editCalls = 0;
    QString editRoom, editStateKey, editNs, editAction, editArgA, editArgB;
    quint64 lastEditOp = 0;
    void editStickerPack(const QString &roomId, const QString &stateKey,
                         const QString &ns, const QString &action,
                         const QString &argA, const QString &argB,
                         quint64 opId) override
    {
        ++editCalls;
        editRoom = roomId;
        editStateKey = stateKey;
        editNs = ns;
        editAction = action;
        editArgA = argA;
        editArgB = argB;
        lastEditOp = opId;
    }
    void addStickerToUserPack(const QString &shortcode, const QString &url,
                              const QString &body, const QString &mimetype,
                              quint64, quint64, quint64,
                              const QString &usage, quint64 opId) override
    {
        ++saveCalls;
        saveUsage = usage;
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

    // The pack editor lists editorImages. `images` is narrowed to the usage
    // the picker shows, so editing through it hid a pack's emoji (and made an
    // emoji-only pack read "no images"): C41-D01 / C41-F07.
    void theEditorListsEveryImageWhateverTheGridUsageIs()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        QCOMPARE(manager.images()->count(), 1);
        QCOMPARE(manager.editorImages()->count(), 2);
        manager.setUsage(QStringLiteral("emoticon"));
        QCOMPARE(manager.images()->count(), 1);
        QCOMPARE(manager.editorImages()->count(), 2);

        // An emoji-only pack: the grid is empty, the editor is not.
        QVariantList only;
        only.append(pack(QStringLiteral("user"), QStringLiteral("user"),
                         QStringLiteral("Your pack"),
                         { image(QStringLiteral("bolt"), kMxcA, false, true) }));
        manager.setUsage(QStringLiteral("sticker"));
        manager.applySnapshotForTest(kRoom, false, only);
        QCOMPARE(manager.images()->count(), 0);
        QCOMPARE(manager.editorImages()->count(), 1);
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

    void aRowWithoutAUrlNeverBecomesATileAndAnEmptyKeyIsKeptButNotTypable()
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
        // No url: nothing to show or send. An empty key is a malformed key
        // another client wrote (Matrix v1.19: show it, let the user repair
        // it); the sticker still sends by its url, but it is never offered as
        // a typed `:shortcode:`.
        QCOMPARE(manager.images()->count(), 2);
        QCOMPARE(manager.editorImages()->count(), 2);
        const int validRole = manager.editorImages()->roleNames().key("keyValid");
        bool sawInvalid = false;
        for (int i = 0; i < manager.editorImages()->rowCount(); ++i)
            sawInvalid = sawInvalid
                || !manager.editorImages()->data(manager.editorImages()->index(i),
                                                 validRole).toBool();
        QVERIFY(sawInvalid);
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

        // A thread send with no root must fail, never fall back to the room
        // timeline.
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

        // Only emoticons, never stickers. Both packs' `wave` are offered,
        // each naming its pack (PACK-PRO-004 / PACK-UX-004): a collision is
        // shown, never hidden behind the first pack.
        const QVariantList all = manager.findEmoticons(QString(), 50);
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("shortcode")).toString(),
                 QStringLiteral("wave"));
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("url")).toString(),
                 kMxcB);
        QCOMPARE(all.at(0).toMap().value(QStringLiteral("packName")).toString(),
                 QStringLiteral("Your pack"));
        QCOMPARE(all.at(1).toMap().value(QStringLiteral("packName")).toString(),
                 QStringLiteral("Cat Lovers"));

        QCOMPARE(manager.findEmoticons(QStringLiteral("WA"), 50).size(), 2);
        QVERIFY(manager.findEmoticons(QStringLiteral("zz"), 50).isEmpty());
        QCOMPARE(manager.findEmoticons(QString(), 0).size(), 2);

        // A typed `:wave:` is ambiguous (two different images): it resolves
        // to nothing rather than to the first pack's.
        QVERIFY(manager.uniqueEmoticonUrl(QStringLiteral("wave")).isEmpty());
        QVERIFY(manager.uniqueEmoticonUrl(QStringLiteral("cat")).isEmpty());

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

    void reactionsIncludeEveryEligiblePackBeyondCompletionLimits()
    {
        StickerPackManager manager;
        QVariantList ownImages;
        for (int n = 0; n < 80; ++n)
            ownImages.append(image(QStringLiteral("e%1").arg(n),
                                   QStringLiteral("mxc://example.org/e%1").arg(n),
                                   true, true));
        auto space = pack(QStringLiteral("room:!space:example.org:animals"),
                          QStringLiteral("room"), QStringLiteral("Space"),
                          { image(QStringLiteral("e0"), kMxcA, false, true),
                            image(QStringLiteral("sticker_only"), kMxcB, true, false) });
        space.insert(QStringLiteral("roomId"), QStringLiteral("!space:example.org"));
        space.insert(QStringLiteral("enabledGlobally"), true);
        auto local = pack(QStringLiteral("room:!room:example.org:"),
                          QStringLiteral("room"), QStringLiteral("Current room"),
                          { image(QStringLiteral("local"), kMxcC, true, true) });
        local.insert(QStringLiteral("roomId"), kRoom);
        local.insert(QStringLiteral("enabledGlobally"), false);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"), ownImages), space, local });

        // Execute the picker's actual binding against the real C++ manager.
        // On the old QML this returns only the first 32 account images.
        QFile picker(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
            QStringLiteral("../qml/EmojiPicker.qml")));
        QVERIFY(picker.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(picker.readAll());
        const int propertyAt = qml.indexOf(QStringLiteral("readonly property var customEmoji:"));
        QVERIFY(propertyAt >= 0);
        const int bodyAt = qml.indexOf(QLatin1Char('{'), propertyAt);
        const int bodyEnd = qml.indexOf(QStringLiteral("\n    }"), bodyAt);
        QVERIFY(bodyAt > propertyAt && bodyEnd > bodyAt);
        QJSEngine engine;
        QJSEngine::setObjectOwnership(&manager, QJSEngine::CppOwnership);
        QJSValue app = engine.newObject();
        app.setProperty(QStringLiteral("stickers"), engine.newQObject(&manager));
        engine.globalObject().setProperty(QStringLiteral("app"), app);
        QJSValue context = engine.newObject();
        context.setProperty(QStringLiteral("customEmojiOffered"), true);
        engine.globalObject().setProperty(QStringLiteral("picker"), context);
        const QJSValue result = engine.evaluate(
            QStringLiteral("(function() {%1\n})()")
                .arg(qml.mid(bodyAt + 1, bodyEnd - bodyAt - 1)));
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        const QVariantList reactions = result.toVariant().toList();
        QCOMPARE(reactions.size(), 82);
        QCOMPARE(reactions.at(80).toMap().value(QStringLiteral("url")).toString(), kMxcA);
        QCOMPARE(reactions.at(80).toMap().value(QStringLiteral("packName")).toString(),
                 QStringLiteral("Space"));
        QCOMPARE(reactions.at(81).toMap().value(QStringLiteral("url")).toString(), kMxcC);
        QVERIFY(manager.shortcodeForUrl(kMxcB).isEmpty()); // sticker only
        QCOMPARE(manager.shortcodeForUrl(kMxcA), QStringLiteral("e0"));
        QCOMPARE(manager.emoticon(QStringLiteral("local")).value(QStringLiteral("url")).toString(),
                 kMxcC);
        QCOMPARE(manager.findEmoticons(QString(), 1000).size(), 64);
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
                                          QString(), true, QStringLiteral("none"));
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
                                          QString(), false, QStringLiteral("none"));
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

    // ---- the room-state write (im.ponies.room_emotes) -------------------

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

    void aRoomPackAddCreatesAStablePackUnderAFreshKeyAndReportsLikeAUserPackAdd()
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
        // The empty key has no "default pack" meaning in Matrix v1.19
        // (PACK-LTN-010): a room with no pack gets a NEW stable pack under a
        // fresh, non-empty key.
        QCOMPARE(client.roomPackStateKey, QStringLiteral("stickers"));
        QCOMPARE(client.roomPackNs, QStringLiteral("stable"));
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

    void aSaveTargetsTheRoomsExistingWritablePackInItsOwnDialect()
    {
        // PACK-MIG-008 / PACK-UX-002: a legacy pack is written as legacy (no
        // silent namespace move), a stable one is preferred, and the empty key
        // is only targeted when that IS the existing pack's key.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap legacy = pack(QStringLiteral("L"), QStringLiteral("room"),
                                  QStringLiteral("Old"), {});
        legacy.insert(QStringLiteral("roomId"), kRoom);
        legacy.insert(QStringLiteral("stateKey"), QString());
        legacy.insert(QStringLiteral("namespace"), QStringLiteral("legacy"));
        legacy.insert(QStringLiteral("canManage"), true);
        manager.applySnapshotForTest(kRoom, true, { legacy });
        manager.saveStickerToRoom(kRoom, kMxcA, QStringLiteral("A cat"),
                                  QStringLiteral("image/png"), 1, 1, 1);
        QCOMPARE(client.roomPackNs, QStringLiteral("legacy"));
        QCOMPARE(client.roomPackStateKey, QString());
        Q_EMIT client.stickerPackAddFinished(client.lastRoomPackOp, false,
                                             QStringLiteral("x"), QString());

        QVariantMap stable = legacy;
        stable.insert(QStringLiteral("id"), QStringLiteral("S"));
        stable.insert(QStringLiteral("stateKey"), QStringLiteral("animals_v1"));
        stable.insert(QStringLiteral("namespace"), QStringLiteral("stable"));
        manager.applySnapshotForTest(kRoom, true, { legacy, stable });
        manager.saveStickerToRoom(kRoom, kMxcA, QStringLiteral("A cat"),
                                  QStringLiteral("image/png"), 1, 1, 1);
        QCOMPARE(client.roomPackNs, QStringLiteral("stable"));
        QCOMPARE(client.roomPackStateKey, QStringLiteral("animals_v1"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomPackOp, false,
                                             QStringLiteral("x"), QString());

        // Edits and "use everywhere" carry the pack's dialect too.
        manager.renamePack(QStringLiteral("L"), QStringLiteral("New"));
        QCOMPARE(client.editNs, QStringLiteral("legacy"));
        Q_EMIT client.stickerPackEditFinished(client.lastEditOp, false,
                                              QStringLiteral("x"), QString());
        manager.setRoomPackEnabled(QStringLiteral("S"), true);
        QCOMPARE(client.roomsSetNs, QStringLiteral("stable"));

        // A sticker never goes into an emoji-only pack (it would vanish from
        // the sticker picker): the next writable sticker-capable pack, or a
        // new one, takes it.
        QVariantMap emojiOnly = stable;
        emojiOnly.insert(QStringLiteral("id"), QStringLiteral("E"));
        emojiOnly.insert(QStringLiteral("stateKey"), QStringLiteral("animals"));
        emojiOnly.insert(QStringLiteral("packSticker"), false);
        manager.applySnapshotForTest(kRoom, true, { emojiOnly });
        manager.saveStickerToRoom(kRoom, kMxcA, QStringLiteral("A cat"),
                                  QStringLiteral("image/png"), 1, 1, 1);
        QCOMPARE(client.roomPackNs, QStringLiteral("stable"));
        QCOMPARE(client.roomPackStateKey, QStringLiteral("stickers"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomPackOp, false,
                                             QStringLiteral("x"), QString());

        // A fresh key never collides with a key the room already uses.
        QVariantMap taken = stable;
        taken.insert(QStringLiteral("id"), QStringLiteral("T"));
        taken.insert(QStringLiteral("stateKey"), QStringLiteral("stickers"));
        taken.insert(QStringLiteral("canManage"), false);
        manager.applySnapshotForTest(kRoom, true, { taken });
        manager.createRoomPack(kRoom, QStringLiteral("Animals"),
                               QStringLiteral("emoticon"));
        QCOMPARE(client.createCalls, 1);
        QCOMPARE(client.createKey, QStringLiteral("stickers-2"));
        QCOMPARE(client.createUsage, QStringLiteral("emoticon"));
    }

    void openingAPackSurfaceAfterNavigationReadsTheRoomDeeply()
    {
        // Navigation reads shallow (no full room state); the first picker or
        // manager open in that room must still read it deep, or the room's own
        // packs stay invisible until a manual reload.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.setActiveRoomId(kRoom);
        QTRY_COMPARE(client.fetchCalls, 1);
        QVERIFY(!client.lastFetchDeep);
        Q_EMIT client.stickerPacksReceived(client.lastFetchOp, kRoom, false, twoPacks());
        manager.refreshIfStale();
        QCOMPARE(client.fetchCalls, 2);
        QVERIFY(client.lastFetchDeep);
        Q_EMIT client.stickerPacksReceived(client.lastFetchOp, kRoom, false, twoPacks());
        // Covered now: a second open asks for nothing.
        manager.refreshIfStale();
        QCOMPARE(client.fetchCalls, 2);
    }

    void aParentSpaceChangeRereadsTheSpacePacks()
    {
        // PACK-PRO-023: a canonical Space's child list changing (or the room's
        // own parent claim) can add or drop that Space's packs; the snapshot
        // remembers which Spaces it walked and re-reads when one changes.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.setActiveRoomId(kRoom);
        manager.refresh();
        Q_EMIT client.stickerPackSnapshotMeta(client.lastFetchOp,
            { { QStringLiteral("canonicalAncestorIds"),
                QStringList{ QStringLiteral("!space:example.org") } } });
        Q_EMIT client.stickerPacksReceived(client.lastFetchOp, kRoom, false, twoPacks());
        const int before = client.fetchCalls;
        Q_EMIT client.stickerPacksChanged(QStringLiteral("!space:example.org"));
        QTRY_COMPARE(client.fetchCalls, before + 1);
    }

    void aSyncChangeRereadsOnceAndOnlyWhenItConcernsTheSnapshot()
    {
        // PACK-LTN-002 / PACK-PER-008: another device's subscription edit or a
        // member's pack edit updates an open picker, a burst coalesces into one
        // read, and an unrelated room's pack change reads nothing.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.setActiveRoomId(kRoom);
        manager.refresh();
        Q_EMIT client.stickerPacksReceived(client.lastFetchOp, kRoom, false,
                                           twoPacks());
        QVERIFY(client.lastFetchDeep);
        const int before = client.fetchCalls;

        Q_EMIT client.stickerPacksChanged(QStringLiteral("!unrelated:example.org"));
        QTest::qWait(400);
        QCOMPARE(client.fetchCalls, before);

        Q_EMIT client.stickerPacksChanged(QString());
        Q_EMIT client.stickerPacksChanged(kRoom);
        Q_EMIT client.stickerPacksChanged(QStringLiteral("!r:example.org"));
        QTRY_COMPARE(client.fetchCalls, before + 1);
        QTest::qWait(400);
        QCOMPARE(client.fetchCalls, before + 1);
        QCOMPARE(client.lastFetchRoom, kRoom);
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
    // Four verbs share one op slot and one permission rule: each routes to the
    // right store, the permission rule is asked before sending, and a second
    // click cannot race the first.

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

    void arenameNeedsATargetButAnEmptySourceKeyIsARealKey()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false) }) });

        manager.renameImageInPack(QStringLiteral("user"), QStringLiteral("cat"),
                                  QString());
        QCOMPARE(client.editCalls, 0);

        // "" is a key a malformed pack can hold; repairing it must reach the
        // backend (which answers not_found when there is no such key).
        manager.renameImageInPack(QStringLiteral("user"), QString(),
                                  QStringLiteral("dog"));
        QCOMPARE(client.editCalls, 1);
        QCOMPARE(client.editArgA, QString());
        Q_EMIT client.stickerPackEditFinished(client.lastEditOp, false,
                                              QStringLiteral("not_found"),
                                              QString());

        manager.renameImageInPack(QStringLiteral("user"), QStringLiteral("cat"),
                                  QStringLiteral("kitty"));
        QCOMPARE(client.editCalls, 2);
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

    // ---- uploading a picked file (SVG is rasterised on send) -----------------

    void cleanup() { lightning::svgraster::hooks() = {}; }

    void anSvgStickerIsUploadedAsAPngNeverAsSvg()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString svgPath = dir.filePath(QStringLiteral("smile.svg"));
        {
            QFile f(svgPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'>"
                    "<circle cx='8' cy='8' r='7' fill='#fc0'/></svg>");
        }
        manager.uploadSticker(QUrl::fromLocalFile(svgPath), QString());
        // The single-flight slot is held while it converts, nothing sent yet.
        QVERIFY(manager.saving());
        QCOMPARE(client.uploadCalls, 0);
        QTRY_COMPARE_WITH_TIMEOUT(client.uploadCalls, 1, 10000);
        QVERIFY(client.uploadPath != svgPath);
        QVERIFY(client.uploadPath.endsWith(QStringLiteral(".png")));
        // 16x16 is below the sticker floor: drawn at 512, not upscaled.
        QCOMPARE(QImage(client.uploadPath).size(), QSize(512, 512));
        QVERIFY(manager.saving());   // until the client answers
    }

    void aRefusedSvgStickerEndsTheOpWithItsReasonAndUploadsNothing()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString svgPath = dir.filePath(QStringLiteral("trap.svg"));
        {
            QFile f(svgPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
                    "<image href='/etc/hostname'/></svg>");
        }
        QSignalSpy finished(&manager, &StickerPackManager::saveFinished);
        manager.uploadSticker(QUrl::fromLocalFile(svgPath), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(finished.first().at(1).toString(),
                 QStringLiteral("svg_external_image"));
        QCOMPARE(client.uploadCalls, 0);
        QVERIFY(!manager.saving());
        QVERIFY(manager.svgMessage(QStringLiteral("external_image"))
                    .contains(QStringLiteral("links to other files")));
    }

    void aPngStickerStillUploadsItsOwnFile()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pngPath = dir.filePath(QStringLiteral("cat.png"));
        QImage image(4, 4, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(pngPath, "PNG"));
        manager.uploadSticker(QUrl::fromLocalFile(pngPath), QString());
        QCOMPARE(client.uploadCalls, 1);
        QCOMPARE(client.uploadPath, pngPath);
    }

    // ---- usage, shortcode validation, transfers ---------------------------

    void aSavedStickerIsAStickerAndAnUploadedEmojiIsAnEmoticon()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.saveSticker(kMxcA, QStringLiteral("cat"), QStringLiteral("image/png"),
                            1, 1, 10);
        QCOMPARE(client.saveUsage, QStringLiteral("sticker"));
        Q_EMIT client.stickerPackAddFinished(client.lastSaveOp, true, QString(),
                                             QStringLiteral("cat"));

        QTemporaryDir dir;
        const QString pngPath = dir.filePath(QStringLiteral("cat.png"));
        QImage image(4, 4, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(pngPath, "PNG"));
        manager.uploadSticker(QUrl::fromLocalFile(pngPath), QString());
        QCOMPARE(client.uploadUsage, QStringLiteral("sticker"));
        Q_EMIT client.stickerPackAddFinished(client.lastUploadOp, true,
                                             QString(), QStringLiteral("cat"));
        manager.uploadImage(QUrl::fromLocalFile(pngPath), QString(),
                            QStringLiteral("emoticon"));
        QCOMPARE(client.uploadUsage, QStringLiteral("emoticon"));
        Q_EMIT client.stickerPackAddFinished(client.lastUploadOp, true,
                                             QString(), QStringLiteral("cat"));
        // An unknown spelling is a sticker, never an emoji.
        manager.uploadImage(QUrl::fromLocalFile(pngPath), QString(),
                            QStringLiteral("emoji!"));
        QCOMPARE(client.uploadUsage, QStringLiteral("sticker"));
    }

    void editsCarryTheExactKeyNotTheDisplayedName()
    {
        // PACK-LTN-006: the editor used to send the DISPLAYED (repaired)
        // shortcode, so removing `hello__world` asked for `hello_world`.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap odd = image(QStringLiteral("bad key"), kMxcB, true, false);
        odd.insert(QStringLiteral("key"), QStringLiteral("bad key"));
        odd.insert(QStringLiteral("keyValid"), false);
        QVariantMap exact = image(QStringLiteral("hello__world"), kMxcA, true, true);
        exact.insert(QStringLiteral("key"), QStringLiteral("hello__world"));
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"),
                   QStringLiteral("Your pack"),
                   { exact, odd,
                     image(QStringLiteral("hello_world"), kMxcC, true, true) }) });
        manager.setSelectedPackId(QStringLiteral("user"));

        StickerImageModel *rows = manager.editorImages();
        QCOMPARE(rows->rowCount(), 3);
        QHash<QString, bool> validity;
        const int keyRole = rows->roleNames().key("key");
        const int validRole = rows->roleNames().key("keyValid");
        QVERIFY(keyRole > 0 && validRole > 0);
        for (int i = 0; i < rows->rowCount(); ++i)
            validity.insert(rows->data(rows->index(i), keyRole).toString(),
                            rows->data(rows->index(i), validRole).toBool());
        QCOMPARE(validity.value(QStringLiteral("hello__world")), true);
        QCOMPARE(validity.value(QStringLiteral("bad key"), true), false);

        manager.removeImageFromPack(QStringLiteral("user"),
                                    QStringLiteral("hello__world"));
        QCOMPARE(client.editArgA, QStringLiteral("hello__world"));

        // An invalid key is never offered as typed text, and lookup by text is
        // exact-case.
        const QVariantList completions = manager.findEmoticons(QString(), 10);
        for (const QVariant &row : completions)
            QVERIFY(row.toMap().value(QStringLiteral("key")).toString()
                    != QStringLiteral("bad key"));
        QVERIFY(!manager.emoticon(QStringLiteral("hello__world")).isEmpty());
        QVERIFY(manager.emoticon(QStringLiteral("HELLO__WORLD")).isEmpty());
    }

    void anOpenEditorKeepsItsOwnPackWhenThePickerSelectionMoves()
    {
        // PACK-UX-021: the editor's rows were the manager-wide selection, so a
        // picker tab change swapped the rows under an open editor.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        manager.setEditorPackId(QStringLiteral("room:!r:example.org:"));
        manager.setSelectedPackId(QStringLiteral("user"));
        StickerImageModel *rows = manager.editorImages();
        const int keyRole = rows->roleNames().key("key");
        QCOMPARE(rows->rowCount(), 2);
        QCOMPARE(rows->data(rows->index(0), keyRole).toString(), QStringLiteral("dog"));
        // A refresh keeps it on the editor's pack too.
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QCOMPARE(rows->data(rows->index(0), keyRole).toString(), QStringLiteral("dog"));
        // A description edit names the exact key and the pack's own store.
        manager.setImageDescription(QStringLiteral("user"), QStringLiteral("cat"),
                                    QStringLiteral("A sleepy cat"));
        QCOMPARE(client.editAction, QStringLiteral("set_body"));
        QCOMPARE(client.editArgA, QStringLiteral("cat"));
        QCOMPARE(client.editArgB, QStringLiteral("A sleepy cat"));
    }

    void theMigrationPreviewSplitsByUsageAndARunReportsEachStep()
    {
        // PACK-MIG-001/002/003: the preview comes from the snapshot (nothing
        // is written by looking), counts each new pack, and names what must
        // stay behind; a run is single-flight and re-reads afterwards.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap bad = image(QStringLiteral("bad key"), QStringLiteral("mxc://e/9"),
                                true, false);
        bad.insert(QStringLiteral("key"), QStringLiteral("bad key"));
        bad.insert(QStringLiteral("keyValid"), false);
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"), QStringLiteral("Mine"),
                   { image(QStringLiteral("s"), kMxcA, true, false),
                     image(QStringLiteral("e"), kMxcB, false, true),
                     image(QStringLiteral("b"), kMxcC, true, true), bad }) });
        const QVariantMap preview = manager.userPackMigrationPreview();
        QCOMPARE(preview.value(QStringLiteral("stickers")).toInt(), 1);
        QCOMPARE(preview.value(QStringLiteral("emoji")).toInt(), 1);
        QCOMPARE(preview.value(QStringLiteral("both")).toInt(), 1);
        QCOMPARE(preview.value(QStringLiteral("invalid")).toInt(), 1);
        QCOMPARE(client.migrateCalls, 0);

        QSignalSpy done(&manager, &StickerPackManager::migrationFinished);
        manager.migrateUserPack();
        manager.migrateUserPack();
        QCOMPARE(client.migrateCalls, 1);
        QVERIFY(manager.migrating());
        const int fetches = client.fetchCalls;
        Q_EMIT client.stickerPackMigrated(client.lastMigrateOp, false,
                                          QStringLiteral("network"),
                                          { { QStringLiteral("packs"), 2 } });
        QCOMPARE(done.count(), 1);
        QVERIFY(!manager.migrating());
        QVERIFY(!client.lastMigrateStartNew);
        // Part-way still re-reads, so the packs that did land show up.
        QCOMPARE(client.fetchCalls, fetches + 1);
        // The explicit "start a new pack room" choice reaches the backend.
        manager.migrateUserPack(true);
        QVERIFY(client.lastMigrateStartNew);
    }

    void aHiddenPackLeavesThePickersButNotTheRoomAndPersistsPerAccount()
    {
        // PACK-UX-011: hiding is a local view choice, per account, persisted.
        QTemporaryDir dir;
        const QString state = dir.filePath(QStringLiteral("local.ini"));
        FakeClient client;
        StickerPackManager manager;
        manager.setLocalStatePath(state);
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QCOMPARE(manager.findEmoticons(QString(), 50).size(), 2);
        manager.setPackHidden(QStringLiteral("room:!r:example.org:"), true);
        QCOMPARE(client.editCalls, 0);           // nothing written to Matrix
        QCOMPARE(client.roomsSetCalls, 0);       // no unsubscribe
        QCOMPARE(manager.findEmoticons(QString(), 50).size(), 1);
        QCOMPARE(manager.packs()->count(), 2);   // still listed in settings
        QVERIFY(manager.isPackHidden(QStringLiteral("room:!r:example.org:")));

        // Survives a new manager (restart), and a refresh keeps it hidden.
        StickerPackManager again;
        again.setLocalStatePath(state);
        again.setClient(&client);
        again.applySnapshotForTest(kRoom, false, twoPacks());
        QCOMPARE(again.findEmoticons(QString(), 50).size(), 1);
        // Another account on the same device sees its own choice only.
        FakeClient other;
        other.userId = QStringLiteral("@other:example.org");
        StickerPackManager otherManager;
        otherManager.setLocalStatePath(state);
        otherManager.setClient(&other);
        otherManager.applySnapshotForTest(kRoom, false, twoPacks());
        QCOMPARE(otherManager.findEmoticons(QString(), 50).size(), 2);
    }

    void customRecentsAndFavouritesAreLocalPerAccountAndSurviveAMissingPack()
    {
        // PACK-REC-010, PACK-UX-012, PACK-PRO-057: separate from the Unicode
        // recents, newest first, bounded, per account; an image whose pack is
        // gone stays usable and is marked stale.
        QTemporaryDir dir;
        const QString state = dir.filePath(QStringLiteral("local.ini"));
        FakeClient client;
        StickerPackManager manager;
        manager.setLocalStatePath(state);
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        const QVariantMap wave = image(QStringLiteral("wave"), kMxcB, false, true);
        const QVariantMap cat = image(QStringLiteral("cat"), kMxcA, true, false);
        manager.recordCustomUse(wave);
        manager.sendToRoom(kRoom, cat);  // a sticker send is a use
        QVariantList recents = manager.customRecents();
        QCOMPARE(recents.size(), 2);
        QCOMPARE(recents.at(0).toMap().value(QStringLiteral("url")).toString(), kMxcA);
        manager.recordCustomUse(wave);
        recents = manager.customRecents();
        QCOMPARE(recents.size(), 2);
        QCOMPARE(recents.at(0).toMap().value(QStringLiteral("url")).toString(), kMxcB);
        for (int i = 0; i < 40; ++i)
            manager.recordCustomUse(image(QStringLiteral("e%1").arg(i),
                                          QStringLiteral("mxc://e/%1").arg(i), true, true));
        QCOMPARE(manager.customRecents().size(), 24);

        manager.toggleFavourite(wave);
        QVERIFY(manager.isFavourite(kMxcB));
        QCOMPARE(manager.reactionEmoticons().at(0).toMap()
                     .value(QStringLiteral("url")).toString(), kMxcB);
        // The pack goes away: the favourite stays, marked stale, still sendable.
        manager.applySnapshotForTest(kRoom, false, {});
        const QVariantMap fav = manager.favourites().at(0).toMap();
        QCOMPARE(fav.value(QStringLiteral("url")).toString(), kMxcB);
        QVERIFY(fav.value(QStringLiteral("stale")).toBool());
        manager.forgetCustomEntry(kMxcB);
        QVERIFY(!manager.isFavourite(kMxcB));

        FakeClient other;
        other.userId = QStringLiteral("@other:example.org");
        StickerPackManager otherManager;
        otherManager.setLocalStatePath(state);
        otherManager.setClient(&other);
        QVERIFY(otherManager.customRecents().isEmpty());
        // Junk is refused.
        manager.recordCustomUse({ { QStringLiteral("url"), QStringLiteral("https://x/y") } });
        QVERIFY(manager.customRecents().at(0).toMap().value(QStringLiteral("url"))
                    .toString().startsWith(QLatin1String("mxc://")));
    }

    void addingImagesToARoomPackUploadsEachAndRetriesWithoutReuploading()
    {
        // PACK-UX-002/013/014, PACK-PRO-050: several files into the chosen
        // (non-empty-key) pack, names from the files, one at a time, per-item
        // outcome, and a failed pack write retried with the media it already
        // uploaded.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap target = pack(QStringLiteral("S"), QStringLiteral("room"),
                                  QStringLiteral("Animals"),
                                  { image(QStringLiteral("cat"), kMxcA, true, false) });
        target.insert(QStringLiteral("roomId"), kRoom);
        target.insert(QStringLiteral("stateKey"), QStringLiteral("animals"));
        target.insert(QStringLiteral("namespace"), QStringLiteral("stable"));
        target.insert(QStringLiteral("canManage"), true);
        manager.applySnapshotForTest(kRoom, true, { target });
        QSignalSpy done(&manager, &StickerPackManager::transferFinished);
        manager.uploadImagesToPack(QStringLiteral("S"),
            { QUrl::fromLocalFile(QStringLiteral("/tmp/cat.png")),
              QUrl::fromLocalFile(QStringLiteral("/tmp/dog.png")),
              QUrl::fromLocalFile(QStringLiteral("/tmp/dog.webp")) });
        QCOMPARE(client.roomUploadCalls, 1);
        QCOMPARE(client.roomUploadKey, QStringLiteral("animals"));
        QCOMPARE(client.roomUploadNs, QStringLiteral("stable"));
        // `cat` is taken in the pack: proposed under a free name instead.
        QCOMPARE(client.roomUploadCode, QStringLiteral("cat-2"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomUploadOp, true, QString(),
                                             QStringLiteral("cat-2"));
        QCOMPARE(client.roomUploadCalls, 2);
        QCOMPARE(client.roomUploadCode, QStringLiteral("dog"));
        // Media landed, the pack write failed: kept for Retry with its mxc.
        Q_EMIT client.stickerPackUploaded(client.lastRoomUploadOp,
                                          QStringLiteral("mxc://example.org/dogmedia"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomUploadOp, false,
                                             QStringLiteral("network"), QString());
        QCOMPARE(client.roomUploadCalls, 3);
        QCOMPARE(client.roomUploadCode, QStringLiteral("dog-2"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomUploadOp, true, QString(),
                                             QStringLiteral("dog-2"));
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(3).toInt(), 2);   // added
        QCOMPARE(done.at(0).at(5).toInt(), 1);   // failed
        QCOMPARE(manager.failedUploadCount(), 1);

        manager.retryFailedUploads();
        QCOMPARE(client.roomUploadCalls, 4);
        QCOMPARE(client.roomUploadCode, QStringLiteral("dog"));
        QCOMPARE(client.roomUploadReuse, QStringLiteral("mxc://example.org/dogmedia"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomUploadOp, true, QString(),
                                             QStringLiteral("dog"));
        QCOMPARE(manager.failedUploadCount(), 0);

        // Without the permission nothing is sent.
        target.insert(QStringLiteral("canManage"), false);
        manager.applySnapshotForTest(kRoom, true, { target });
        manager.uploadImagesToPack(QStringLiteral("S"),
            { QUrl::fromLocalFile(QStringLiteral("/tmp/x.png")) });
        QCOMPARE(client.roomUploadCalls, 4);
    }

    void anEditorCanTellWhatAnotherDeviceChanged()
    {
        // PACK-PRO-045 / PACK-UX-023: the base taken at open, compared with a
        // later snapshot, names what was added, removed and changed elsewhere.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        const QVariantMap base = manager.packImageMap(QStringLiteral("user"));
        QCOMPARE(base.size(), 2);
        QVERIFY(!manager.comparePack(QStringLiteral("user"), base)
                     .value(QStringLiteral("any")).toBool());
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"), QStringLiteral("Your pack"),
                   { image(QStringLiteral("cat"), kMxcA, true, false, QStringLiteral("new body")),
                     image(QStringLiteral("fox"), kMxcC, true, false) }) });
        const QVariantMap diff = manager.comparePack(QStringLiteral("user"), base);
        QVERIFY(diff.value(QStringLiteral("any")).toBool());
        QCOMPARE(diff.value(QStringLiteral("added")).toStringList(), QStringList{ "fox" });
        QCOMPARE(diff.value(QStringLiteral("removed")).toStringList(), QStringList{ "wave" });
        QCOMPARE(diff.value(QStringLiteral("changed")).toStringList(), QStringList{ "cat" });
    }

    void aRoomPackSharesAStandardRoomLinkAndItsKeyAndNothingPrivate()
    {
        // PACK-LIVE-020 (text half), PACK-PRO-058: an ordinary matrix.to room
        // link any client opens, the pack's name and exact key; nothing for
        // the personal pack.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap roomPack = pack(QStringLiteral("S"), QStringLiteral("room"),
                                    QStringLiteral("Animals"), {});
        roomPack.insert(QStringLiteral("roomId"), QStringLiteral("!abc:example.org"));
        roomPack.insert(QStringLiteral("stateKey"), QStringLiteral("animals_v1"));
        manager.applySnapshotForTest(kRoom, false,
            { roomPack, pack(QStringLiteral("user"), QStringLiteral("user"),
                             QStringLiteral("Mine"), {}) });
        QCOMPARE(manager.shareLink(QStringLiteral("S")),
                 QStringLiteral("https://matrix.to/#/%21abc%3Aexample.org?via=example.org"));
        const QString text = manager.shareText(QStringLiteral("S"));
        QVERIFY(text.contains(QStringLiteral("Animals")));
        QVERIFY(text.contains(QStringLiteral("animals_v1")));
        QVERIFY(text.contains(manager.shareLink(QStringLiteral("S"))));
        QVERIFY(!text.contains(QStringLiteral("mxc://")));
        QVERIFY(manager.shareText(QStringLiteral("user")).isEmpty());

        // Copy link puts exactly that text on the clipboard (the GUI round
        // could not read the private display's clipboard back).
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        QVERIFY(manager.copyShareText(QStringLiteral("S")));
        QCOMPARE(QGuiApplication::clipboard()->text(), text);
        QVERIFY(!manager.copyShareText(QStringLiteral("user")));
        QCOMPARE(QGuiApplication::clipboard()->text(), text);
    }

    void aPrivateImageIsUploadedOnlyAfterConfirmationAndNeverToAnotherAccount()
    {
        // PACK-PRO-041 / PACK-LTN-032 / PACK-UX-015/016: the manager fetches
        // the decrypted bytes only on the confirmed call; a cancel, an
        // account switch or non-raster bytes upload nothing.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QStringList fetched;
        manager.setPrivateMediaFetch([&fetched](const QString &k) { fetched << k; });
        const QByteArray png = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(40, 'p');

        // Cancel before the bytes arrive: nothing is uploaded.
        manager.republishPrivateImage(QStringLiteral("key1"));
        QCOMPARE(fetched, QStringList{ QStringLiteral("key1") });
        manager.cancelRepublish();
        manager.onPrivateMediaBytes(QStringLiteral("key1"), true, png);
        QCOMPARE(client.uploadCalls, 0);

        // Another account by the time the bytes land: nothing is uploaded.
        manager.republishPrivateImage(QStringLiteral("key2"));
        client.userId = QStringLiteral("@someone-else:example.org");
        manager.onPrivateMediaBytes(QStringLiteral("key2"), true, png);
        QCOMPARE(client.uploadCalls, 0);
        client.userId = QStringLiteral("@me:example.org");

        // SVG (or anything not a plain raster) is refused.
        QSignalSpy refused(&manager, &StickerPackManager::saveFinished);
        manager.republishPrivateImage(QStringLiteral("key3"));
        manager.onPrivateMediaBytes(QStringLiteral("key3"), true,
                                    QByteArray("<svg xmlns='http://www.w3.org/2000/svg'/>"));
        QCOMPARE(client.uploadCalls, 0);
        QCOMPARE(refused.count(), 1);

        // Confirmed and valid: one upload to the personal pack, of a file
        // holding exactly those bytes.
        manager.republishPrivateImage(QStringLiteral("key4"));
        manager.onPrivateMediaBytes(QStringLiteral("key4"), true, png);
        QCOMPARE(client.uploadCalls, 1);
        QFile written(client.uploadPath);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), png);
        written.close();
        // Review S1: owner-only, and gone once the upload has answered.
        QCOMPARE(QFileInfo(client.uploadPath).permissions()
                     & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                        | QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        // Review M1: the private message's caption never names the copy.
        QCOMPARE(client.uploadShortcode, QStringLiteral("sticker"));
        QVERIFY(!client.uploadBody.contains(QStringLiteral("cat")));
        Q_EMIT client.stickerPackAddFinished(client.lastUploadOp, true, QString(),
                                             QStringLiteral("sticker"));
        QVERIFY(!QFileInfo::exists(client.uploadPath));
    }

    void searchOverTheLargestSnapshotStaysInteractive()
    {
        // PACK-PER-001/002: the in-memory search the completion popup and the
        // palette run on every keystroke, over the largest snapshot the
        // backend allows (64 packs x 512 images), never waits on media. The
        // target is p95 <= 50 ms; asserted as BEST of 7 under 50 ms so a loaded
        // machine cannot flake it (CLAUDE.md: wall-clock assertions need
        // headroom), and it measures ~1-3 ms on the dev box.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantList packs;
        for (int p = 0; p < 64; ++p) {
            QVariantList images;
            for (int i = 0; i < 512; ++i)
                images.append(image(QStringLiteral("p%1_img%2").arg(p).arg(i),
                                    QStringLiteral("mxc://e/%1_%2").arg(p).arg(i),
                                    true, true));
            QVariantMap pk = pack(QStringLiteral("room:%1").arg(p), QStringLiteral("room"),
                                  QStringLiteral("Pack %1").arg(p), images);
            packs.append(pk);
        }
        manager.applySnapshotForTest(kRoom, false, packs);
        qint64 best = std::numeric_limits<qint64>::max();
        for (int round = 0; round < 7; ++round) {
            QElapsedTimer timer;
            timer.start();
            const QVariantList hits = manager.findEmoticons(QStringLiteral("p63_img5"), 12);
            best = qMin(best, timer.elapsed());
            QVERIFY(!hits.isEmpty());
        }
        QVERIFY2(best < 50, qPrintable(QStringLiteral("best of 7: %1 ms").arg(best)));
        // The reaction list (every image) also returns promptly.
        QElapsedTimer all;
        all.start();
        QCOMPARE(manager.reactionEmoticons().size(), 64 * 512);
        QVERIFY2(all.elapsed() < 2000, qPrintable(QString::number(all.elapsed())));
    }

    void anotherAccountNeverRetriesThisOnesFailedUploads()
    {
        // Review S3: a failed batch (with its uploaded mxc) is this account's.
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QVariantMap target = pack(QStringLiteral("S"), QStringLiteral("room"),
                                  QStringLiteral("P"), {});
        target.insert(QStringLiteral("roomId"), kRoom);
        target.insert(QStringLiteral("stateKey"), QStringLiteral("p"));
        target.insert(QStringLiteral("namespace"), QStringLiteral("stable"));
        target.insert(QStringLiteral("canManage"), true);
        manager.applySnapshotForTest(kRoom, true, { target });
        manager.uploadImagesToPack(QStringLiteral("S"),
            { QUrl::fromLocalFile(QStringLiteral("/tmp/a.png")) });
        Q_EMIT client.stickerPackUploaded(client.lastRoomUploadOp,
                                          QStringLiteral("mxc://example.org/private"));
        Q_EMIT client.stickerPackAddFinished(client.lastRoomUploadOp, false,
                                             QStringLiteral("network"), QString());
        QCOMPARE(manager.failedUploadCount(), 1);
        // Another account becomes active in the same manager.
        client.userId = QStringLiteral("@other:example.org");
        const int before = client.roomUploadCalls;
        manager.retryFailedUploads();
        QCOMPARE(client.roomUploadCalls, before);
        QCOMPARE(manager.failedUploadCount(), 0);
        // And a sign-out drops them outright.
        client.userId = QStringLiteral("@me:example.org");
        manager.applySnapshotForTest(kRoom, true, { target });
        manager.uploadImagesToPack(QStringLiteral("S"),
            { QUrl::fromLocalFile(QStringLiteral("/tmp/b.png")) });
        Q_EMIT client.stickerPackAddFinished(client.lastRoomUploadOp, false,
                                             QStringLiteral("network"), QString());
        QCOMPARE(manager.failedUploadCount(), 1);
        Q_EMIT client.loggedOut();
        QCOMPARE(manager.failedUploadCount(), 0);
    }

    void anAccountSwitchKeepsLocalPackChoicesAndForgetAccountDropsThem()
    {
        // Review S2 + recheck: the app switches accounts IN PLACE and the
        // client emits loggedOut on every switch, so loggedOut must never
        // delete local choices; only forgetAccount(userId), called from the
        // real sign-out and removal paths, does.
        QTemporaryDir dir;
        const QString state = dir.filePath(QStringLiteral("local.ini"));
        FakeClient client;
        StickerPackManager manager;
        manager.setLocalStatePath(state);
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        manager.setPackHidden(QStringLiteral("user"), true);
        manager.recordCustomUse(image(QStringLiteral("wave"), kMxcB, false, true));

        // A in-place switch A -> B -> A: loggedOut fires each time.
        Q_EMIT client.loggedOut();
        client.userId = QStringLiteral("@b:example.org");
        QVERIFY(!manager.isPackHidden(QStringLiteral("user")));  // B has none
        Q_EMIT client.loggedOut();
        client.userId = QStringLiteral("@me:example.org");
        QVERIFY(manager.isPackHidden(QStringLiteral("user")));
        QCOMPARE(manager.customRecents().size(), 1);

        // B's choices are independent, and removing B (a background account)
        // leaves A's alone.
        manager.forgetAccount(QStringLiteral("@b:example.org"));
        QVERIFY(manager.isPackHidden(QStringLiteral("user")));

        // Signing A out removes A's, from memory and from the file.
        manager.forgetAccount(QStringLiteral("@me:example.org"));
        QVERIFY(!manager.isPackHidden(QStringLiteral("user")));
        QVERIFY(manager.customRecents().isEmpty());
        QSettings file(state, QSettings::IniFormat);
        QVERIFY(file.childGroups().isEmpty());
    }

    void shortcodeErrorsSayWhy()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        const QString user = QStringLiteral("user");

        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("kitten")).isEmpty());
        // Renaming to itself is not a clash.
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("cat")).isEmpty());
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"), QString())
                    .contains(QStringLiteral("name")));
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("two words"))
                    .contains(QStringLiteral("letters")));
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral(":cat:"))
                    .contains(QStringLiteral("letters")));
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("caf\u00e9"))
                    .contains(QStringLiteral("letters")));
        // Matrix v1.19 allows these; the old 64-char / no-`__` / no-leading
        // separator rules were Lightning's own and refused valid names
        // (PACK-PRO-006, PACK-LTN-007).
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("-cat")).isEmpty());
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("a__b")).isEmpty());
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QString(65, QLatin1Char('a'))).isEmpty());
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QString(100, QLatin1Char('a'))).isEmpty());
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QString(101, QLatin1Char('a')))
                    .contains(QStringLiteral("100")));
        // Another image in the pack: exact and case-sensitive, so `WAVE` is
        // a different name from `wave` (PACK-PRO-005).
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("wave"))
                    .contains(QStringLiteral("already")));
        QVERIFY(manager.shortcodeError(user, QStringLiteral("cat"),
                                       QStringLiteral("WAVE")).isEmpty());
        // The same name in ANOTHER pack is fine.
        QVERIFY(manager.shortcodeError(QStringLiteral("room:!r:example.org:"),
                                       QStringLiteral("dog"),
                                       QStringLiteral("cat")).isEmpty());
        QCOMPARE(manager.sanitizeShortcode(QStringLiteral("Cat Face!")),
                 QStringLiteral("Cat_Face"));
    }

    void customEmojiInAMessageAreFoundAndOnlyIfTheyAreSafe()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());

        const QString html = QStringLiteral(
            "hi <img data-mx-emoticon src=\"mxc://example.org/new1\" "
            "alt=\":party_parrot:\" height=\"32\"> and "
            "<img src=\"mxc://example.org/plain\" alt=\"not emoji\"> and "
            "<img data-mx-emoticon src=\"https://evil.example/x.png\" alt=\":x:\"> "
            "<img data-mx-emoticon src='mxc://example.org/new2' title=':wave:'> "
            "<img data-mx-emoticon src=\"mxc://example.org/new1\" alt=\":dup:\"> "
            "<img data-mx-emoticon src=\"%1\" alt=\":cat:\">").arg(kMxcA);
        const QVariantList found = manager.emoticonsInHtml(html);
        QCOMPARE(found.size(), 3);
        QCOMPARE(found[0].toMap().value(QStringLiteral("url")).toString(),
                 QStringLiteral("mxc://example.org/new1"));
        QCOMPARE(found[0].toMap().value(QStringLiteral("shortcode")).toString(),
                 QStringLiteral("party_parrot"));
        QCOMPARE(found[1].toMap().value(QStringLiteral("shortcode")).toString(),
                 QStringLiteral("wave"));
        QVERIFY(!found[0].toMap().value(QStringLiteral("saved")).toBool());
        // kMxcA is already in the account's pack.
        QVERIFY(found[2].toMap().value(QStringLiteral("saved")).toBool());
        QVERIFY(manager.emoticonsInHtml(QStringLiteral("<b>plain</b>")).isEmpty());
        QVERIFY(manager.emoticonsInHtml(QString()).isEmpty());
    }

    void savingEmojiAddsThemAsEmoticonsOneAtATimeAndReportsOnce()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        QSignalSpy saved(&manager, &StickerPackManager::saveFinished);

        const QVariantList found = manager.emoticonsInHtml(QStringLiteral(
            "<img data-mx-emoticon src=\"mxc://example.org/n1\" alt=\":one:\">"
            "<img data-mx-emoticon src=\"mxc://example.org/n2\" alt=\":two:\">"
            "<img data-mx-emoticon src=\"%1\" alt=\":cat:\">").arg(kMxcA));
        manager.saveEmoticons(found);
        QVERIFY(manager.transferring());
        QCOMPARE(manager.transferKind(), QStringLiteral("emoji"));
        QCOMPARE(manager.transferTotal(), 3);
        QCOMPARE(client.saveCalls, 1);
        QCOMPARE(client.saveUsage, QStringLiteral("emoticon"));
        QCOMPARE(client.saveUrl, QStringLiteral("mxc://example.org/n1"));
        // Nothing else may start while the job runs.
        manager.saveEmoticons(found);
        QCOMPARE(client.saveCalls, 1);

        Q_EMIT client.stickerPackAddFinished(client.lastSaveOp, true, QString(),
                                             QStringLiteral("one"));
        QCOMPARE(client.saveCalls, 2);
        QCOMPARE(client.saveUrl, QStringLiteral("mxc://example.org/n2"));
        QCOMPARE(finished.count(), 0);
        Q_EMIT client.stickerPackAddFinished(client.lastSaveOp, false,
                                             QStringLiteral("duplicate"),
                                             QString());
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("emoji"));
        QVERIFY(finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(3).toInt(), 1);   // added
        QCOMPARE(finished.first().at(4).toInt(), 2);   // skipped: dup + saved
        QCOMPARE(finished.first().at(5).toInt(), 0);   // failed
        QCOMPARE(finished.first().at(6).toString(), QStringLiteral("one"));
        QVERIFY(!manager.transferring());
        // The per-image notice is for single saves; a batch speaks once.
        QCOMPARE(saved.count(), 0);
        // And the pack was re-read exactly because something was added.
        QVERIFY(client.fetchCalls >= 1);
    }

    void aFullPackEndsTheBatchInsteadOfTryingEveryRemainingImage()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        manager.saveEmoticons(manager.emoticonsInHtml(QStringLiteral(
            "<img data-mx-emoticon src=\"mxc://example.org/n1\" alt=\":one:\">"
            "<img data-mx-emoticon src=\"mxc://example.org/n2\" alt=\":two:\">"
            "<img data-mx-emoticon src=\"mxc://example.org/n3\" alt=\":three:\">")));
        Q_EMIT client.stickerPackAddFinished(client.lastSaveOp, false,
                                             QStringLiteral("pack_full"),
                                             QString());
        QCOMPARE(client.saveCalls, 1);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("pack_full"));
        QCOMPARE(finished.first().at(5).toInt(), 3);
        QVERIFY(manager.transferMessage(QStringLiteral("emoji"), false,
                                        QStringLiteral("pack_full"), 0, 0, 3,
                                        QString())
                    .contains(QStringLiteral("full")));
    }

    void exportingAPackFetchesEveryImageAndWritesAReadableZip()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        QTemporaryDir dir;
        const QString zipPath = dir.filePath(QStringLiteral("pack.zip"));

        const QByteArray png = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(20, 'x');
        manager.exportPack(QStringLiteral("user"), QUrl::fromLocalFile(zipPath));
        QVERIFY(manager.transferring());
        QCOMPARE(manager.transferKind(), QStringLiteral("export"));
        QCOMPARE(client.mxcFetches.size(), 2);   // both images, in flight together
        QCOMPARE(client.mxcFetches[0].second, kMxcA);
        // A pack nobody asked about is not exported, and a second job waits.
        manager.exportPack(QStringLiteral("room:!r:example.org:"),
                           QUrl::fromLocalFile(dir.filePath(QStringLiteral("x.zip"))));
        QCOMPARE(client.mxcFetches.size(), 2);

        Q_EMIT client.mediaReady(client.mxcFetches[0].first, kMxcA, 0, png,
                                 QStringLiteral("image/png"), QString());
        QCOMPARE(finished.count(), 0);
        Q_EMIT client.mediaReady(client.mxcFetches[1].first, kMxcB, 0, png,
                                 QStringLiteral("image/png"), QString());
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(3).toInt(), 2);
        QVERIFY(QFileInfo(zipPath).size() > 100);

        QTemporaryDir work;
        const auto read = stickers::archive::readArchive(zipPath, work.path());
        QCOMPARE(int(read.error), int(stickers::archive::ReadError::None));
        QCOMPARE(read.pack.name, QStringLiteral("Your pack"));
        QCOMPARE(read.pack.images.size(), 2);
        QCOMPARE(read.pack.images[0].shortcode, QStringLiteral("cat"));
        QCOMPARE(read.pack.images[0].usage, QStringLiteral("sticker"));
        QCOMPARE(read.pack.images[1].shortcode, QStringLiteral("wave"));
        QCOMPARE(read.pack.images[1].usage, QStringLiteral("emoticon"));
    }

    void anExportIsAFrozenSnapshotWhateverSyncDoesMeanwhile()
    {
        // PACK-LTN-035: the old export re-read the LIVE model on every batch
        // and paired bytes with whatever row now sat at that index, so a sync
        // that reordered or removed images mid-download put one image's bytes
        // under another's name (or indexed past the allocation).
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        auto img = [](const QString &code, int n) {
            return image(code, QStringLiteral("mxc://example.org/%1").arg(n), true, false);
        };
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"), QStringLiteral("Mine"),
                   { img(QStringLiteral("a"), 1), img(QStringLiteral("b"), 2),
                     img(QStringLiteral("c"), 3), img(QStringLiteral("d"), 4) }) });
        QTemporaryDir dir;
        const QString zipPath = dir.filePath(QStringLiteral("frozen.zip"));
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        manager.exportPack(QStringLiteral("user"), QUrl::fromLocalFile(zipPath));
        QCOMPARE(client.mxcFetches.size(), 3);  // three in flight

        // Sync lands: reordered, one removed, one added, renamed pack.
        manager.applySnapshotForTest(kRoom, false,
            { pack(QStringLiteral("user"), QStringLiteral("user"), QStringLiteral("Renamed"),
                   { img(QStringLiteral("z"), 9), img(QStringLiteral("d"), 4),
                     img(QStringLiteral("b"), 2) }) });

        auto bytesFor = [](const QString &mxc) {
            return QByteArray("\x89PNG\r\n\x1A\n", 8) + mxc.toUtf8() + QByteArray(16, 'x');
        };
        // Answer every fetch, in reverse order, by the mxc it asked for.
        for (int round = 0; round < 3 && finished.isEmpty(); ++round) {
            const auto fetches = client.mxcFetches;
            for (int i = fetches.size() - 1; i >= 0; --i)
                Q_EMIT client.mediaReady(fetches[i].first, fetches[i].second, 0,
                                         bytesFor(fetches[i].second),
                                         QStringLiteral("image/png"), QString());
        }
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().at(1).toBool());
        QTemporaryDir work;
        const auto read = stickers::archive::readArchive(zipPath, work.path());
        QCOMPARE(int(read.error), int(stickers::archive::ReadError::None));
        QCOMPARE(read.pack.name, QStringLiteral("Mine"));
        QStringList codes;
        for (const auto &entry : read.pack.images) {
            codes << entry.shortcode;
            QFile f(entry.path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const int n = QStringList{ "a", "b", "c", "d" }.indexOf(entry.shortcode) + 1;
            QCOMPARE(f.readAll(), bytesFor(QStringLiteral("mxc://example.org/%1").arg(n)));
        }
        QCOMPARE(codes, (QStringList{ "a", "b", "c", "d" }));
    }

    void anExportWhereSomeImagesFailStillReportsHonestly()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        QTemporaryDir dir;
        const QString zipPath = dir.filePath(QStringLiteral("pack.zip"));
        const QByteArray png = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(20, 'x');
        manager.exportPack(QStringLiteral("user"), QUrl::fromLocalFile(zipPath));
        Q_EMIT client.mediaReady(client.mxcFetches[0].first, kMxcA, 0, png,
                                 QStringLiteral("image/png"), QString());
        Q_EMIT client.mediaFailed(client.mxcFetches[1].first, kMxcB, 0,
                                  QStringLiteral("network"));
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(3).toInt(), 1);
        QCOMPARE(finished.first().at(5).toInt(), 1);
        QVERIFY(manager.transferMessage(QStringLiteral("export"), true, QString(),
                                        1, 0, 1, QString())
                    .contains(QStringLiteral("could not be downloaded")));

        // Nothing fetched: no file, and it says so.
        finished.clear();
        const QString none = dir.filePath(QStringLiteral("none.zip"));
        manager.exportPack(QStringLiteral("user"), QUrl::fromLocalFile(none));
        Q_EMIT client.mediaFailed(client.mxcFetches[2].first, kMxcA, 0, QString());
        Q_EMIT client.mediaFailed(client.mxcFetches[3].first, kMxcB, 0, QString());
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("fetch_failed"));
        QVERIFY(!QFileInfo::exists(none));
    }

    void anSvgThatAPackPointsAtIsNeverWrittenIntoAnExport()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        QTemporaryDir dir;
        const QString zipPath = dir.filePath(QStringLiteral("pack.zip"));
        const QByteArray png = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(20, 'x');
        const QByteArray svg = QByteArrayLiteral(
            "<svg xmlns='http://www.w3.org/2000/svg'><script/></svg>");
        manager.exportPack(QStringLiteral("user"), QUrl::fromLocalFile(zipPath));
        Q_EMIT client.mediaReady(client.mxcFetches[0].first, kMxcA, 0, svg,
                                 QStringLiteral("image/png"), QString());
        Q_EMIT client.mediaReady(client.mxcFetches[1].first, kMxcB, 0, png,
                                 QStringLiteral("image/png"), QString());
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(3).toInt(), 1);
        QCOMPARE(finished.first().at(4).toInt(), 1);   // skipped
        QFile f(zipPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("<svg"));
    }

    void importingAPackUploadsEachNewImageWithItsUsageAndSkipsWhatIsThere()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);

        const QByteArray png = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(20, 'x');
        stickers::archive::Pack pack;
        pack.name = QStringLiteral("Friends");
        pack.images = {
            {QStringLiteral("new_one"), QStringLiteral("One"), {}, QStringLiteral("emoticon"), png, {}},
            {QStringLiteral("cat"), QStringLiteral("Cat"), {}, QStringLiteral("sticker"), png, {}},
            {QStringLiteral("new_two"), QStringLiteral("Two"), {}, QStringLiteral("sticker"), png, {}},
        };
        QTemporaryDir dir;
        const QString zipPath = dir.filePath(QStringLiteral("in.zip"));
        {
            QFile f(zipPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(stickers::archive::buildArchive(pack));
        }
        manager.importPack(QUrl::fromLocalFile(zipPath));
        QVERIFY(manager.transferring());
        QTRY_COMPARE_WITH_TIMEOUT(client.uploadCalls, 1, 10000);
        QCOMPARE(client.uploadShortcode, QStringLiteral("new_one"));
        QCOMPARE(client.uploadUsage, QStringLiteral("emoticon"));
        QVERIFY(QFileInfo::exists(client.uploadPath));
        Q_EMIT client.stickerPackAddFinished(client.lastUploadOp, true, QString(),
                                             QStringLiteral("new_one"));
        QCOMPARE(client.uploadCalls, 2);
        QCOMPARE(client.uploadShortcode, QStringLiteral("new_two"));
        QCOMPARE(client.uploadUsage, QStringLiteral("sticker"));
        const QString lastPath = client.uploadPath;
        Q_EMIT client.stickerPackAddFinished(client.lastUploadOp, false,
                                             QStringLiteral("upload_failed"),
                                             QString());
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("import"));
        QVERIFY(finished.first().at(1).toBool());      // something was added
        QCOMPARE(finished.first().at(3).toInt(), 1);   // added
        QCOMPARE(finished.first().at(4).toInt(), 1);   // "cat" was already there
        QCOMPARE(finished.first().at(5).toInt(), 1);   // one upload failed
        QCOMPARE(finished.first().at(6).toString(), QStringLiteral("Friends"));
        // The extracted files are gone with the job.
        QVERIFY(!QFileInfo::exists(lastPath));
        QVERIFY(!manager.transferring());
    }

    void aHostileOrBrokenPackIsRefusedWithAReasonAndUploadsNothing()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("junk.zip"));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not a zip file");
        }
        manager.importPack(QUrl::fromLocalFile(path));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("unreadable"));
        QCOMPARE(client.uploadCalls, 0);
        QVERIFY(!manager.transferring());
        QVERIFY(manager.transferMessage(QStringLiteral("import"), false,
                                        QStringLiteral("unreadable"), 0, 0, 0,
                                        QString())
                    .contains(QStringLiteral("not a readable")));
    }

    void signingOutDropsAJobInFlightAndItsAnswers()
    {
        FakeClient client;
        StickerPackManager manager;
        manager.setClient(&client);
        manager.applySnapshotForTest(kRoom, false, twoPacks());
        QSignalSpy finished(&manager, &StickerPackManager::transferFinished);
        QTemporaryDir dir;
        manager.exportPack(QStringLiteral("user"),
                           QUrl::fromLocalFile(dir.filePath(QStringLiteral("p.zip"))));
        QVERIFY(manager.transferring());
        const auto first = client.mxcFetches[0];
        Q_EMIT client.loggedOut();
        QVERIFY(!manager.transferring());
        const QByteArray png = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(20, 'x');
        Q_EMIT client.mediaReady(first.first, first.second, 0, png,
                                 QStringLiteral("image/png"), QString());
        QCOMPARE(finished.count(), 0);
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("p.zip"))));
    }
};

QTEST_MAIN(StickerPacksTest)
#include "StickerPacksTest.moc"
