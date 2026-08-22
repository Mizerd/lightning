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

    bool supports = true;
    QString self = QStringLiteral("@me:example.org");
    QList<RecordedFetch> fetches;
    QStringList writes;
    quint64 lastWriteOp = 0;
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
};

QTEST_GUILESS_MAIN(ProfileBannerTest)
#include "ProfileBannerTest.moc"
