// Profile banners (MSC4427 over MSC4133 extended profile fields).
//
// The Rust half decides the wire format and is covered in rust/src/banner.rs;
// this is the policy half. What it pins is the honesty: three different facts
// — "no banner", "not asked yet" and "this homeserver does not do extended
// profiles" — all render as nothing, and the third latches so the client stops
// asking a question it has already been told it cannot ask.

#include "matrix/MockMatrixClient.h"
#include "profile/ProfileBannerManager.h"

#include <QDir>
#include <QSignalSpy>
#include <QUrl>
#include <QtTest>

namespace {

struct RecordedFetch {
    QString userId;
    quint64 opId = 0;
};

class FakeBannerClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsProfileBanners() const override { return supports; }
    void fetchProfileBanner(const QString &userId, quint64 opId) override
    {
        fetches.append({ userId, opId });
    }
    void setProfileBanner(const QString &localPath, quint64 opId) override
    {
        writes.append(localPath);
        lastWriteOp = opId;
    }
    QString currentUserId() const override { return self; }

    // --- Room / Space banners -------------------------------------------
    bool supportsRoomBanners() const override { return supportsRooms; }
    void fetchRoomBanner(const QString &roomId, quint64 opId) override
    {
        roomFetches.append({ roomId, opId });
    }
    void setRoomBanner(const QString &roomId, const QString &localPath,
                       quint64 opId) override
    {
        roomWrites.append({ roomId, localPath });
        lastRoomWriteOp = opId;
    }

    bool supports = true;
    bool supportsRooms = true;
    QString self = QStringLiteral("@me:example.org");
    QList<RecordedFetch> fetches;
    QStringList writes;
    quint64 lastWriteOp = 0;
    QList<RecordedFetch> roomFetches;   // userId field carries the room id
    QList<QPair<QString, QString>> roomWrites;
    quint64 lastRoomWriteOp = 0;
};

} // namespace

class ProfileBannerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void anUnknownUserRendersNothingAndIsAskedExactlyOnce()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        QVERIFY(banners.available());

        const QString alice = QStringLiteral("@alice:example.org");
        // Not asked yet is not "no banner"; both are "".
        QCOMPARE(banners.bannerFor(alice), QString());

        banners.request(alice);
        QCOMPARE(client.fetches.size(), 1);
        // A profile card that opens, closes and opens again must not cost a
        // second request.
        banners.request(alice);
        banners.request(alice);
        QCOMPARE(client.fetches.size(), 1);

        QSignalSpy revisions(&banners, &ProfileBannerManager::revisionChanged);
        Q_EMIT client.profileBannerReceived(
            client.fetches.first().opId, alice,
            QStringLiteral("mxc://example.org/banner"), true);
        QCOMPARE(banners.bannerFor(alice),
                 QStringLiteral("mxc://example.org/banner"));
        QCOMPARE(revisions.count(), 1);

        // A user the server answered EMPTY for is now known to have none, and
        // that answer is not a change worth repainting for a second time.
        const QString bob = QStringLiteral("@bob:example.org");
        banners.request(bob);
        QCOMPARE(client.fetches.size(), 2);
        Q_EMIT client.profileBannerReceived(client.fetches.last().opId, bob,
                                            QString(), true);
        QCOMPARE(banners.bannerFor(bob), QString());
        QCOMPARE(revisions.count(), 1);
    }

    void anUnsupportedServerLatchesAndStopsAsking()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        QSignalSpy supported(&banners, &ProfileBannerManager::supportedChanged);
        QVERIFY(banners.supported());

        banners.request(QStringLiteral("@alice:example.org"));
        QCOMPARE(client.fetches.size(), 1);
        Q_EMIT client.profileBannerReceived(client.fetches.first().opId,
                                            QStringLiteral("@alice:example.org"),
                                            QString(), /*supported=*/false);
        QVERIFY(!banners.supported());
        QCOMPARE(supported.count(), 1);

        // Every further request would ask the same question and get the same
        // answer, so it is not sent.
        banners.request(QStringLiteral("@bob:example.org"));
        QCOMPARE(client.fetches.size(), 1);
    }

    void aStaleAnswerFromAPreviousSessionIsDropped()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        const QString alice = QStringLiteral("@alice:example.org");
        banners.request(alice);
        const quint64 opId = client.fetches.first().opId;

        // Sign-out drops the cache AND the in-flight table: a banner belongs
        // to the account that fetched it.
        Q_EMIT client.loggedOut();
        Q_EMIT client.profileBannerReceived(
            opId, alice, QStringLiteral("mxc://example.org/old"), true);
        QCOMPARE(banners.bannerFor(alice), QString());
        // The latch is per session too — the next account may be on a server
        // that does implement extended profiles.
        QVERIFY(banners.supported());
        // And the user can be asked about again under the new session.
        banners.request(alice);
        QCOMPARE(client.fetches.size(), 2);
    }

    void aWriteIsAppliedOnlyWhenTheServerAcknowledgesIt()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        QVERIFY(!banners.busy());

        banners.setOwnBanner(QStringLiteral("/tmp/banner.png"));
        QCOMPARE(client.writes, QStringList{ QStringLiteral("/tmp/banner.png") });
        QVERIFY(banners.busy());
        // One write at a time: a second press while the first is in flight
        // must not start another upload.
        banners.setOwnBanner(QStringLiteral("/tmp/other.png"));
        QCOMPARE(client.writes.size(), 1);

        // A refusal reports and changes NOTHING.
        Q_EMIT client.profileBannerSet(client.lastWriteOp, false, QString(),
                                       QStringLiteral("too_large"));
        QVERIFY(!banners.busy());
        QCOMPARE(banners.lastError(), QStringLiteral("too_large"));
        QCOMPARE(banners.ownBanner(), QString());

        // An acknowledged write is authoritative for our own user and needs
        // no round trip to re-read.
        banners.setOwnBanner(QStringLiteral("/tmp/banner.png"));
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/new"),
                                       QString());
        QCOMPARE(banners.ownBanner(), QStringLiteral("mxc://example.org/new"));
        QVERIFY(banners.lastError().isEmpty());

        // Clearing is the same path with an empty path.
        banners.clearOwnBanner();
        QCOMPARE(client.writes.last(), QString());
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true, QString(),
                                       QString());
        QCOMPARE(banners.ownBanner(), QString());
    }

    void aFileUrlIsConvertedForTheCurrentPlatform()
    {
        // A file the user picks reaches QML as a URL. Stripping "file://" by
        // hand is wrong on Windows — file:///C:/x.png becomes /C:/x.png, a
        // leading slash before the drive letter — so the conversion happens
        // here, once, where no caller can get it wrong.
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);

        const QString path = QDir::toNativeSeparators(
            QStringLiteral("/tmp/banner.png"));
        banners.setOwnBanner(QUrl::fromLocalFile(path).toString());
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.last(), path);
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/a"),
                                       QString());

        // A plain path still passes through untouched.
        banners.setOwnBanner(path);
        QCOMPARE(client.writes.last(), path);
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/b"),
                                       QString());

        // A UNC path IS a local file to QUrl, and to Windows, so it is
        // converted rather than refused — file://server/share/x.png becomes
        // //server/share/x.png. Asserting a refusal here was my mistake, not
        // the code's.
        banners.setOwnBanner(QStringLiteral("file://server/share/x.png"));
        QCOMPARE(client.writes.size(), 3);
        QCOMPARE(client.writes.last(), QStringLiteral("//server/share/x.png"));
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/c"),
                                       QString());

        // A URL that is not a file at all IS refused, rather than handed to
        // the uploader as though it were a path — it would report a missing
        // file, and the reason would look like the user's fault.
        banners.setOwnBanner(QStringLiteral("https://example.org/x.png"));
        QCOMPARE(client.writes.size(), 3);

        // ...but a Windows drive path is NOT a scheme, however much it looks
        // like one: QUrl("C:/x.png").scheme() is "c". It must pass through.
        banners.setOwnBanner(QStringLiteral("C:/Users/x/banner.png"));
        QCOMPARE(client.writes.size(), 4);
        QCOMPARE(client.writes.last(), QStringLiteral("C:/Users/x/banner.png"));
    }

    void abackendWithoutBannersOffersNothing()
    {
        FakeBannerClient client;
        client.supports = false;
        ProfileBannerManager banners;
        banners.setClient(&client);
        QVERIFY(!banners.available());
        banners.request(QStringLiteral("@alice:example.org"));
        banners.setOwnBanner(QStringLiteral("/tmp/banner.png"));
        banners.clearOwnBanner();
        QCOMPARE(client.fetches.size(), 0);
        QCOMPARE(client.writes.size(), 0);
    }

    // ── Room / Space banners ────────────────────────────────────────────
    //
    // The room half had NO coverage at all before 2026-09-08. It is the half
    // the Space settings dialog and Space Home render, it is a custom state
    // event with its OWN required power level, and "nobody has asked yet" and
    // "this account may not change it" are the two facts a control must not
    // guess at.

    void aRoomIsAskedAboutOnceAndItsPermissionIsNeverGuessed()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        const QString room = QStringLiteral("!space:example.org");
        QVERIFY(banners.roomBannersAvailable());

        // Before an answer: no banner, and the control is NOT offered.
        QCOMPARE(banners.roomBannerFor(room), QString());
        QVERIFY(!banners.canSetRoomBanner(room));

        banners.requestRoom(room);
        QCOMPARE(client.roomFetches.size(), 1);
        // A settings dialog that opens, closes and opens again must not cost
        // a second request through requestRoom().
        banners.requestRoom(room);
        QCOMPARE(client.roomFetches.size(), 1);

        QSignalSpy revisions(&banners, &ProfileBannerManager::revisionChanged);
        Q_EMIT client.roomBannerReceived(client.roomFetches.first().opId, room,
                                         QStringLiteral("mxc://example.org/b"),
                                         /*canSet=*/true);
        QCOMPARE(banners.roomBannerFor(room),
                 QStringLiteral("mxc://example.org/b"));
        QVERIFY(banners.canSetRoomBanner(room));
        QCOMPARE(revisions.count(), 1);
    }

    // A REFRESH IS THE ONLY WAY A REMOTE CHANGE CAN EVER ARRIVE. Sliding sync
    // delivers only the state types Lightning names in required_state, and
    // page.codeberg.everypizza.room.banner is not one of them (see
    // rust/src/banner.rs), so nothing tells this client the banner moved. The
    // Space settings dialog re-reads on every open for exactly that reason —
    // and refreshRoom() has to actually ask, including for a room already in
    // the cache and including once the cache is full.
    void refreshingARoomAsksAgainAndTheCacheCapDoesNotBlockIt()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        const QString room = QStringLiteral("!space:example.org");

        banners.requestRoom(room);
        QCOMPARE(client.roomFetches.size(), 1);
        Q_EMIT client.roomBannerReceived(client.roomFetches.first().opId, room,
                                         QStringLiteral("mxc://example.org/b"),
                                         true);
        banners.refreshRoom(room);
        QCOMPARE(client.roomFetches.size(), 2);
        // The remote change lands, permission included.
        Q_EMIT client.roomBannerReceived(client.roomFetches.at(1).opId, room,
                                         QStringLiteral("mxc://example.org/c"),
                                         /*canSet=*/false);
        QCOMPARE(banners.roomBannerFor(room),
                 QStringLiteral("mxc://example.org/c"));
        QVERIFY(!banners.canSetRoomBanner(room));

        // Fill the bound with OTHER rooms. The cap exists to stop the cache
        // growing without limit; a room already in it is not growth, and
        // refusing to re-read one would silently freeze that Space's banner
        // for the rest of the session.
        for (int i = 0; i < 512; ++i) {
            const QString other =
                QStringLiteral("!filler%1:example.org").arg(i);
            banners.requestRoom(other);
            if (!client.roomFetches.isEmpty()
                && client.roomFetches.last().userId == other) {
                Q_EMIT client.roomBannerReceived(
                    client.roomFetches.last().opId, other, QString(), false);
            }
        }
        const int before = client.roomFetches.size();
        banners.refreshRoom(room);
        QCOMPARE(client.roomFetches.size(), before + 1);
    }

    void aStaleRoomAnswerFromAPreviousSessionIsDropped()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        const QString room = QStringLiteral("!space:example.org");
        banners.requestRoom(room);
        const quint64 opId = client.roomFetches.first().opId;

        // A Space banner belongs to the account that read it, and so does the
        // permission that came with it.
        Q_EMIT client.loggedOut();
        Q_EMIT client.roomBannerReceived(opId, room,
                                         QStringLiteral("mxc://example.org/x"),
                                         true);
        QCOMPARE(banners.roomBannerFor(room), QString());
        QVERIFY(!banners.canSetRoomBanner(room));
        // ...and the room may be asked about again under the new session.
        banners.requestRoom(room);
        QCOMPARE(client.roomFetches.size(), 2);
    }

    void aRoomWriteIsAppliedOnlyWhenTheServerAcknowledgesIt()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        const QString room = QStringLiteral("!space:example.org");

        banners.setRoomBanner(room, QStringLiteral("/tmp/banner.png"));
        QCOMPARE(client.roomWrites.size(), 1);
        QVERIFY(banners.busy());
        // Nothing is applied optimistically.
        QCOMPARE(banners.roomBannerFor(room), QString());
        // A second write while one is in flight is refused rather than queued.
        banners.setRoomBanner(room, QStringLiteral("/tmp/other.png"));
        QCOMPARE(client.roomWrites.size(), 1);

        Q_EMIT client.roomBannerSet(client.lastRoomWriteOp, room, false,
                                    QString(), QStringLiteral("forbidden"));
        QVERIFY(!banners.busy());
        QCOMPARE(banners.lastError(), QStringLiteral("forbidden"));
        QCOMPARE(banners.roomBannerFor(room), QString());

        // The acknowledged write IS authoritative — no round trip needed.
        banners.setRoomBanner(room, QStringLiteral("/tmp/banner.png"));
        Q_EMIT client.roomBannerSet(client.lastRoomWriteOp, room, true,
                                    QStringLiteral("mxc://example.org/new"),
                                    QString());
        QCOMPARE(banners.roomBannerFor(room),
                 QStringLiteral("mxc://example.org/new"));
        QVERIFY(banners.lastError().isEmpty());

        // An empty path IS the clear, and it is dispatched as one.
        banners.clearRoomBanner(room);
        QCOMPARE(client.roomWrites.size(), 3);
        QCOMPARE(client.roomWrites.last().second, QString());
        Q_EMIT client.roomBannerSet(client.lastRoomWriteOp, room, true,
                                    QString(), QString());
        QCOMPARE(banners.roomBannerFor(room), QString());
    }

    void abackendWithoutRoomBannersOffersNothing()
    {
        FakeBannerClient client;
        client.supportsRooms = false;
        ProfileBannerManager banners;
        banners.setClient(&client);
        QVERIFY(!banners.roomBannersAvailable());
        const QString room = QStringLiteral("!space:example.org");
        banners.requestRoom(room);
        banners.refreshRoom(room);
        banners.setRoomBanner(room, QStringLiteral("/tmp/banner.png"));
        banners.clearRoomBanner(room);
        QCOMPARE(client.roomFetches.size(), 0);
        QCOMPARE(client.roomWrites.size(), 0);
        QVERIFY(!banners.canSetRoomBanner(room));
    }
};

QTEST_GUILESS_MAIN(ProfileBannerTest)
#include "ProfileBannerTest.moc"
