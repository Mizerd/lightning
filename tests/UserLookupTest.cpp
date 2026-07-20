// v0.5.11: tests for bare-localpart user lookup — candidate derivation from
// the authenticated account's server, exact-profile confirmation before a
// bare-localpart result is offered, typed-full-id behavior, merge
// deduplication with provenance, and stale-result rejection.

#include "matrix/MatrixClient.h"
#include "models/UserLookup.h"
#include "models/UserSearchModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastSearchOp = 0;
    quint64 lastProfileOp = 0;
    QString lastSearchQuery;
    QString lastProfileUser;
    int profileCalls = 0;

    bool supportsRoomManagement() const override { return true; }
    quint64 searchUsers(const QString &query, int) override
    {
        lastSearchQuery = query;
        lastSearchOp = nextOp++;
        return lastSearchOp;
    }
    quint64 fetchUserProfile(const QString &userId) override
    {
        ++profileCalls;
        lastProfileUser = userId;
        lastProfileOp = nextOp++;
        return lastProfileOp;
    }

    // Pure virtuals (inert).
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

struct Rig {
    FakeClient client;
    UserSearchModel model;

    Rig()
    {
        model.setClient(&client);
        model.setDebounceMs(0);
    }

    void search(const QString &query)
    {
        model.setQuery(query);
        QTest::qWait(1); // let the zero-interval debounce fire
    }
    void finishDirectory(const QVariantList &rows, bool ok = true)
    {
        Q_EMIT client.userSearchFinished(client.lastSearchOp, ok, rows, false,
                                         ok ? QString()
                                            : QStringLiteral("network"));
    }
    void confirmProfile(const QString &userId, const QString &displayName)
    {
        Q_EMIT client.userProfileFinished(client.lastProfileOp, true, userId,
                                          displayName, QString(), QString());
    }
    void refuteProfile(const QString &userId)
    {
        Q_EMIT client.userProfileFinished(client.lastProfileOp, false, userId,
                                          QString(), QString(),
                                          QStringLiteral("not_found"));
    }
};

QVariantMap directoryRow(const QString &userId, const QString &displayName)
{
    return { { QStringLiteral("userId"), userId },
             { QStringLiteral("displayName"), displayName },
             { QStringLiteral("avatarUrl"), QString() } };
}

QString sourceAt(const UserSearchModel &model, int row)
{
    return model.data(model.index(row, 0), UserSearchModel::SourceRole)
        .toString();
}

} // namespace

class UserLookupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // v0.7: shared visible-name fallback — localpart before the full MXID.
    void localpartFallbackDerivation()
    {
        using namespace matrix::user_lookup;
        QCOMPARE(localpartOrUserId(QStringLiteral("@matas:matrix.smetonis.net")),
                 QStringLiteral("matas"));
        QCOMPARE(localpartOrUserId(QStringLiteral("@a:x:8448")),
                 QStringLiteral("a"));
        // No localpart derivable → the original value is returned verbatim.
        QCOMPARE(localpartOrUserId(QStringLiteral("@:server")),
                 QStringLiteral("@:server"));
        QCOMPARE(localpartOrUserId(QStringLiteral("plain-name")),
                 QStringLiteral("plain-name"));
        QCOMPARE(localpartOrUserId(QString()), QString());
    }

    void candidateDerivation()
    {
        using namespace matrix::user_lookup;
        const QString server = QStringLiteral("example.org");

        QCOMPARE(exactCandidate(QStringLiteral("admin"), server),
                 QStringLiteral("@admin:example.org"));
        QCOMPARE(exactCandidate(QStringLiteral("@admin"), server),
                 QStringLiteral("@admin:example.org"));
        QCOMPARE(exactCandidate(QStringLiteral("admin:other.example"), server),
                 QStringLiteral("@admin:other.example"));
        QCOMPARE(exactCandidate(QStringLiteral("@admin:other.example"), server),
                 QStringLiteral("@admin:other.example"));
        QCOMPARE(exactCandidate(QStringLiteral("  admin  "), server),
                 QStringLiteral("@admin:example.org"));
        // Port-carrying server names survive.
        QCOMPARE(exactCandidate(QStringLiteral("@a:server.example:8448"), server),
                 QStringLiteral("@a:server.example:8448"));

        // Invalid localparts / servers never fabricate an id.
        QVERIFY(exactCandidate(QStringLiteral("has space"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("bad!char"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("@"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("a:bad_server!"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("admin"), QString()).isEmpty());

        QCOMPARE(serverNameFromUserId(QStringLiteral("@me:example.org")),
                 QStringLiteral("example.org"));
        QVERIFY(serverNameFromUserId(QStringLiteral("nonsense")).isEmpty());

        QVERIFY(queryNamesServer(QStringLiteral("@a:b.org")));
        QVERIFY(queryNamesServer(QStringLiteral("a:b.org")));
        QVERIFY(!queryNamesServer(QStringLiteral("@admin")));
        QVERIFY(!queryNamesServer(QStringLiteral("admin")));
    }

    void bareLocalpartUsesAccountServerAndNeedsConfirmation()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        // The candidate server comes from the authenticated account, never
        // from a hardcoded name.
        QCOMPARE(rig.client.lastProfileUser, QStringLiteral("@admin:example.org"));

        rig.finishDirectory({});
        // Unconfirmed bare-localpart candidate: not offered.
        QCOMPARE(rig.model.rowCount(), 0);
        QCOMPARE(rig.model.state(), QStringLiteral("loading")); // profile pending

        rig.confirmProfile(QStringLiteral("@admin:example.org"),
                           QStringLiteral("Admin"));
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@admin:example.org"));
        QCOMPARE(rig.model.displayNameAt(0), QStringLiteral("Admin"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_local"));
        QCOMPARE(rig.model.state(), QStringLiteral("results"));
    }

    void refutedLocalpartIsNeverInvented()
    {
        Rig rig;
        rig.search(QStringLiteral("ghost"));
        rig.finishDirectory({});
        rig.refuteProfile(QStringLiteral("@ghost:example.org"));
        QCOMPARE(rig.model.rowCount(), 0);
        QCOMPARE(rig.model.state(), QStringLiteral("no_results"));
    }

    void typedFullIdIsOfferedImmediatelyAndEnriched()
    {
        Rig rig;
        rig.search(QStringLiteral("@carol:federated.example"));
        rig.finishDirectory({});
        // Full typed ids stay usable for federated invites even before (or
        // without) profile confirmation.
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@carol:federated.example"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_mxid"));
        QVERIFY(rig.model.displayNameAt(0).isEmpty());

        rig.confirmProfile(QStringLiteral("@carol:federated.example"),
                           QStringLiteral("Carol"));
        QCOMPARE(rig.model.displayNameAt(0), QStringLiteral("Carol"));
    }

    void localpartWithServerButNoAtIsExactMxid()
    {
        Rig rig;
        rig.search(QStringLiteral("admin:other.example"));
        QCOMPARE(rig.client.lastProfileUser, QStringLiteral("@admin:other.example"));
        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_mxid"));
    }

    void duplicateDirectoryAndExactResultIsMerged()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        rig.confirmProfile(QStringLiteral("@admin:example.org"), QString());
        rig.finishDirectory({ directoryRow(QStringLiteral("@admin:example.org"),
                                           QStringLiteral("Admin From Directory")),
                              directoryRow(QStringLiteral("@adminson:example.org"),
                                           QStringLiteral("Adminson")) });
        QCOMPARE(rig.model.rowCount(), 2);
        // The exact row keeps its provenance and gains the directory's
        // display name.
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@admin:example.org"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_local"));
        QCOMPARE(rig.model.displayNameAt(0), QStringLiteral("Admin From Directory"));
        QCOMPARE(sourceAt(rig.model, 1), QStringLiteral("directory"));
    }

    void invalidLocalpartDispatchesNoProfileLookup()
    {
        Rig rig;
        rig.search(QStringLiteral("has space"));
        QCOMPARE(rig.client.profileCalls, 0);
        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 0);
        QCOMPARE(rig.model.state(), QStringLiteral("no_results"));
    }

    void staleProfileResultIsRejected()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        const quint64 firstProfileOp = rig.client.lastProfileOp;
        rig.search(QStringLiteral("bob"));

        // The old lookup confirming now must not surface the old candidate.
        Q_EMIT rig.client.userProfileFinished(firstProfileOp, true,
                                              QStringLiteral("@admin:example.org"),
                                              QStringLiteral("Admin"), QString(),
                                              QString());
        rig.finishDirectory({});
        for (int i = 0; i < rig.model.rowCount(); ++i)
            QVERIFY(rig.model.userIdAt(i) != QStringLiteral("@admin:example.org"));

        rig.confirmProfile(QStringLiteral("@bob:example.org"), QStringLiteral("Bob"));
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@bob:example.org"));
    }

    void ownUserIsNeverACandidate()
    {
        Rig rig;
        rig.search(QStringLiteral("me")); // @me:example.org == own account
        QCOMPARE(rig.client.profileCalls, 0);
    }

    void failedDirectoryWithConfirmedExactStillShowsResult()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        rig.finishDirectory({}, false);
        rig.confirmProfile(QStringLiteral("@admin:example.org"),
                           QStringLiteral("Admin"));
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.state(), QStringLiteral("results"));
    }

    void mergeIsPureAndDeduplicates()
    {
        UserSearchModel::Result exact;
        exact.userId = QStringLiteral("@a:x.org");
        exact.source = QStringLiteral("exact_local");
        UserSearchModel::Result dupe;
        dupe.userId = QStringLiteral("@a:x.org");
        dupe.displayName = QStringLiteral("A");
        UserSearchModel::Result self;
        self.userId = QStringLiteral("@own:x.org");

        const auto merged = UserSearchModel::mergeResults(
            { exact }, { dupe, self }, QStringLiteral("@own:x.org"));
        QCOMPARE(merged.size(), 1);
        QCOMPARE(merged.first().source, QStringLiteral("exact_local"));
        QCOMPARE(merged.first().displayName, QStringLiteral("A"));
    }
};

QTEST_GUILESS_MAIN(UserLookupTest)
#include "UserLookupTest.moc"
