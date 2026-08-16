// v0.6.6 review (M2): AppController::isChatGifStarred/unstarChatGif's
// two-tier design (a fast, exact session-only check backed by
// GifStarredStore::isStarredThisSession, falling back to a durable,
// content-hash check through MediaBridge::cachedFullContentHash — see
// GifStarredStore's "DURABLE STARRED-STATE DESIGN" class comment) had NO
// executable coverage before this file: MediaBridgeTest.cpp and
// GifStarredStoreTest.cpp each test their own class in isolation, and
// neither exercises the AppController glue that actually answers the
// question the timeline hover star asks. This drives a real AppController
// (mock backend) with its MediaBridge repointed at a small controllable
// FakeClient (MockMatrixClient's fetchMedia only serves fixed fixtures in
// screenshot-demo mode, unsuitable for arbitrary GIF bytes here), proving:
//   - the session-hit tier answers immediately for a GIF just starred in
//     this run;
//   - the durable tier answers true for a COMPLETELY DIFFERENT mediaKey
//     whose cached full bytes happen to be the same starred content — never
//     persisting that second mediaKey anywhere;
//   - the durable answer goes false again after unstar();
//   - the durable answer goes false again after clearAll() — the same
//     map-before-model ordering guarantee GifStarredStoreTest.cpp's
//     sessionMapIsAlreadyClearedWhenCountChangedFires pins for the OLD
//     session-only answer must still hold for the NEW two-tier one, since
//     unstarChatGif/isChatGifStarred both read isStarredThisSession() first.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"
#include "storage/SecretStore.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

// Mirrors AccountSwitchTest.cpp's own FakeSecretStore: an in-memory,
// always-available secret backend so saveSession()/switchToAccount() do not
// depend on a real Secret Service/libsecret being reachable in this test
// environment.
class FakeSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit FakeSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }

    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    { return m_values.value(userId + QLatin1Char('/') + key); }
    bool deleteSecret(const QString &userId, const QString &key) override
    { return m_values.remove(userId + QLatin1Char('/') + key) > 0; }
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

// Minimal controllable MatrixClient (mirrors MediaBridgeTest.cpp's own
// FakeClient) — swapped into a real AppController's MediaBridge after
// construction so the durable tier's MediaBridge::cachedFullContentHash has
// real, arbitrary, controllable cached bytes to hash, without depending on
// MockMatrixClient's screenshot-demo fixture set.
class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    struct Fetch { quint64 opId; QString key; int kind; };
    QList<Fetch> fetches;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int kind, int) override
    {
        const quint64 op = nextOp++;
        fetches.append({ op, mediaKey, kind });
        return op;
    }
    quint64 fetchMxcThumbnail(const QString &, int, int) override { return 0; }

    void succeed(quint64 opId, const QByteArray &bytes)
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes,
                          QStringLiteral("image/gif"), QString());
    }

    // Pure virtuals (inert — never exercised by this suite).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@fake:example.org"); }
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

const QString kAlice = QStringLiteral("@alice:one.example");
const QString kHsOne = QStringLiteral("https://one.example");

} // namespace

class AppControllerChatGifStarredTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        // XDG_DATA_HOME too, not just CONFIG: GifStarredStore's directory
        // comes from matrix::app_data::starredGifsDir() -> accountRoot() ->
        // appDataBase(), which reads XDG_DATA_HOME — NOT from QSettings.
        // Without this the suite writes a real starred-GIF store into the
        // developer's actual ~/.local/share, beside their real account, and
        // the persisted index makes a later case in this same file depend
        // on whether an earlier one finished cleanly. Mirrors
        // AccountSwitchTest's own isolation.
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("app-controller-chat-gif-starred-test"));
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void twoTierAnswerSessionHitDurableHitFalseAfterUnstarAndClearAll()
    {
        // Declared BEFORE the controller so it is destroyed AFTER it:
        // MediaBridge holds a raw pointer to this client, and reversing
        // the order leaves that pointer dangling through ~AppController.
        FakeClient fake;
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QTRY_VERIFY(app.gif()->starredStore()->isOpen());

        app.mediaBridge()->setClient(&fake);

        // A minimal real GIF (magic + logical-screen-descriptor width/height
        // — gif::validateGifBytes' smallest accepted shape).
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);

        // --- Tier 1: session hit, immediately after starring. ---
        QSignalSpy starFinished(app.gif()->starredStore(),
                                &GifStarredStore::starFinished);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QCOMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-original")));

        // --- Tier 2: durable hit for a COMPLETELY DIFFERENT mediaKey (a
        // second message carrying the identical GIF bytes) that the session
        // map has never seen — only reachable once MediaBridge has that
        // key's full bytes cached, exactly like an ordinary inline preview
        // fetch would produce.
        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));
        app.mediaBridge()->mediaSource(QStringLiteral("mk-duplicate"),
                                       QStringLiteral("full"));
        QCOMPARE(fake.fetches.size(), 1);
        fake.succeed(fake.fetches.first().opId, gif);
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        // Still never entered the session map — the durable tier answered,
        // not the fast one.
        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));

        // --- False after unstar(): removing the ONLY starred hash must
        // flip BOTH the session-tier row and the durable-tier row false,
        // since they both resolve to the same content hash.
        app.unstarChatGif(QStringLiteral("mk-original"));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));

        // --- False after clearAll(): re-star, confirm the durable tier
        // sees it again, then Clear All (Settings -> Privacy & security)
        // and confirm it drops back to false — the same map-before-model
        // countChanged ordering GifStarredStoreTest.cpp's
        // sessionMapIsAlreadyClearedWhenCountChangedFires pins for the
        // session-only answer must hold here too, since unstarChatGif/
        // isChatGifStarred both consult isStarredThisSession() first.
        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        app.gif()->starredStore()->clearAll();
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));

        // --- The DURABLE unstar branch, which is the headline of the bug
        // this whole two-tier answer exists to fix ("after a restart you
        // can never unstar from the timeline"). Every unstar above went
        // through the session map, so tier 2's unstar was untested. Star
        // via one key, then unstar via a DIFFERENT key the session map has
        // never seen — only the content-hash tier can resolve it.
        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));

        app.unstarChatGif(QStringLiteral("mk-duplicate"));

        QCOMPARE(app.gif()->starredStore()->count(), 0);
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
    }

    // review H1a: nothing to hash when the store has never starred
    // anything — the durable tier must never even ask MediaBridge.
    void neverStarredAnswersFalseWithoutHashingAnything()
    {
        // Declared BEFORE the controller so it is destroyed AFTER it:
        // MediaBridge holds a raw pointer to this client, and reversing
        // the order leaves that pointer dangling through ~AppController.
        FakeClient fake;
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_VERIFY(app.gif()->starredStore()->isOpen());
        QCOMPARE(app.gif()->starredStore()->count(), 0);

        app.mediaBridge()->setClient(&fake);
        app.mediaBridge()->mediaSource(QStringLiteral("mk"), QStringLiteral("full"));
        fake.succeed(fake.fetches.first().opId,
                     QByteArray("GIF89a\x10\x00\x10\x00", 10));

        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk")));
        QCOMPARE(app.mediaBridge()->healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(0));
    }

    // v0.7.x: the star-fetch RESULT must be claimed, not assumed.
    //
    // MediaBridge::fetchFullForStar is media-generic and gained a second
    // caller when forwarding landed (ForwardController re-fetches the source
    // payload so it can re-upload it). The handler in AppController::setClient
    // used to act on EVERY answer, so every forwarded image would have
    // written its decrypted bytes into this account's on-disk saved-media
    // store, appeared in the Saved tab, consumed the 200-item budget, and
    // rendered the row's star as filled — none of which the user asked for,
    // and which §6 forbids outside the explicit-export exception in §7.
    void aFetchNotRequestedByStarChatGifWritesNothing()
    {
        FakeClient fake;
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_VERIFY(app.gif()->starredStore()->isOpen());
        app.mediaBridge()->setClient(&fake);

        const int before = app.gif()->starredStore()->count();
        QSignalSpy starFinished(app.gif()->starredStore(),
                                &GifStarredStore::starFinished);

        // A fetch this account never asked to star — exactly what a forward
        // issues through the same media-generic entry point.
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.mediaBridge()->fetchFullForStar(QStringLiteral("mk-forwarded"));
        QVERIFY(!fake.fetches.isEmpty());
        fake.succeed(fake.fetches.last().opId, gif);
        QCoreApplication::processEvents();

        QCOMPARE(app.gif()->starredStore()->count(), before);
        QCOMPARE(starFinished.count(), 0);

        // ...while a fetch the account DID request still stars normally.
        app.starChatGif(QStringLiteral("mk-wanted"));
        QVERIFY(fake.fetches.size() >= 2);
        fake.succeed(fake.fetches.last().opId, gif);
        QCoreApplication::processEvents();
        QTRY_COMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

};

QTEST_GUILESS_MAIN(AppControllerChatGifStarredTest)
#include "AppControllerChatGifStarredTest.moc"
