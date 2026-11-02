
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
    QList<RecordedFetch> roomFetches;
    QList<QPair<QString, QString>> roomWrites;
    quint64 lastRoomWriteOp = 0;
};

}

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
        QCOMPARE(banners.bannerFor(alice), QString());

        banners.request(alice);
        QCOMPARE(client.fetches.size(), 1);
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
                                            QString(),false);
        QVERIFY(!banners.supported());
        QCOMPARE(supported.count(), 1);

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

        Q_EMIT client.loggedOut();
        Q_EMIT client.profileBannerReceived(
            opId, alice, QStringLiteral("mxc://example.org/old"), true);
        QCOMPARE(banners.bannerFor(alice), QString());
        QVERIFY(banners.supported());
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
        banners.setOwnBanner(QStringLiteral("/tmp/other.png"));
        QCOMPARE(client.writes.size(), 1);

        Q_EMIT client.profileBannerSet(client.lastWriteOp, false, QString(),
                                       QStringLiteral("too_large"));
        QVERIFY(!banners.busy());
        QCOMPARE(banners.lastError(), QStringLiteral("too_large"));
        QCOMPARE(banners.ownBanner(), QString());

        banners.setOwnBanner(QStringLiteral("/tmp/banner.png"));
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/new"),
                                       QString());
        QCOMPARE(banners.ownBanner(), QStringLiteral("mxc://example.org/new"));
        QVERIFY(banners.lastError().isEmpty());

        banners.clearOwnBanner();
        QCOMPARE(client.writes.last(), QString());
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true, QString(),
                                       QString());
        QCOMPARE(banners.ownBanner(), QString());
    }

    void aFileUrlIsConvertedForTheCurrentPlatform()
    {
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

        banners.setOwnBanner(path);
        QCOMPARE(client.writes.last(), path);
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/b"),
                                       QString());

        banners.setOwnBanner(QStringLiteral("file://server/share/x.png"));
        QCOMPARE(client.writes.size(), 3);
        QCOMPARE(client.writes.last(), QStringLiteral("//server/share/x.png"));
        Q_EMIT client.profileBannerSet(client.lastWriteOp, true,
                                       QStringLiteral("mxc://example.org/c"),
                                       QString());

        banners.setOwnBanner(QStringLiteral("https://example.org/x.png"));
        QCOMPARE(client.writes.size(), 3);

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

    void aRoomIsAskedAboutOnceAndItsPermissionIsNeverGuessed()
    {
        FakeBannerClient client;
        ProfileBannerManager banners;
        banners.setClient(&client);
        const QString room = QStringLiteral("!space:example.org");
        QVERIFY(banners.roomBannersAvailable());

        QCOMPARE(banners.roomBannerFor(room), QString());
        QVERIFY(!banners.canSetRoomBanner(room));

        banners.requestRoom(room);
        QCOMPARE(client.roomFetches.size(), 1);
        banners.requestRoom(room);
        QCOMPARE(client.roomFetches.size(), 1);

        QSignalSpy revisions(&banners, &ProfileBannerManager::revisionChanged);
        Q_EMIT client.roomBannerReceived(client.roomFetches.first().opId, room,
                                         QStringLiteral("mxc://example.org/b"),
true);
        QCOMPARE(banners.roomBannerFor(room),
                 QStringLiteral("mxc://example.org/b"));
        QVERIFY(banners.canSetRoomBanner(room));
        QCOMPARE(revisions.count(), 1);
    }

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
        Q_EMIT client.roomBannerReceived(client.roomFetches.at(1).opId, room,
                                         QStringLiteral("mxc://example.org/c"),
false);
        QCOMPARE(banners.roomBannerFor(room),
                 QStringLiteral("mxc://example.org/c"));
        QVERIFY(!banners.canSetRoomBanner(room));

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

        Q_EMIT client.loggedOut();
        Q_EMIT client.roomBannerReceived(opId, room,
                                         QStringLiteral("mxc://example.org/x"),
                                         true);
        QCOMPARE(banners.roomBannerFor(room), QString());
        QVERIFY(!banners.canSetRoomBanner(room));
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
        QCOMPARE(banners.roomBannerFor(room), QString());
        banners.setRoomBanner(room, QStringLiteral("/tmp/other.png"));
        QCOMPARE(client.roomWrites.size(), 1);

        Q_EMIT client.roomBannerSet(client.lastRoomWriteOp, room, false,
                                    QString(), QStringLiteral("forbidden"));
        QVERIFY(!banners.busy());
        QCOMPARE(banners.lastError(), QStringLiteral("forbidden"));
        QCOMPARE(banners.roomBannerFor(room), QString());

        banners.setRoomBanner(room, QStringLiteral("/tmp/banner.png"));
        Q_EMIT client.roomBannerSet(client.lastRoomWriteOp, room, true,
                                    QStringLiteral("mxc://example.org/new"),
                                    QString());
        QCOMPARE(banners.roomBannerFor(room),
                 QStringLiteral("mxc://example.org/new"));
        QVERIFY(banners.lastError().isEmpty());

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
