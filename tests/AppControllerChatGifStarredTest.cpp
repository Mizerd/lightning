// AppController::isChatGifStarred/unstarChatGif's two-tier answer: a fast
// session check (GifStarredStore::isStarredThisSession) backed by a durable
// content-hash check via MediaBridge::cachedFullContentHash (see
// GifStarredStore's "DURABLE STARRED-STATE DESIGN" comment). Drives a real
// AppController with its MediaBridge repointed at a controllable FakeClient
// (MockMatrixClient only serves fixed fixtures). Checks:
//   - the session tier answers immediately for a GIF just starred;
//   - the durable tier answers true for a different mediaKey whose cached
//     bytes are the same content, without persisting that key;
//   - both go false after unstar() and after clearAll(), relying on the same
//     map-before-model ordering
//     GifStarredStoreTest::sessionMapIsAlreadyClearedWhenCountChangedFires
//     pins.

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

#include <QBuffer>
#include <QClipboard>
#include <QMimeData>

namespace {

// An in-memory, always-available secret backend (as in AccountSwitchTest),
// so saveSession()/switchToAccount() need no real Secret Service.
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

// Minimal controllable MatrixClient (as in MediaBridgeTest), swapped into the
// controller's MediaBridge so the durable tier has arbitrary cached bytes to
// hash.
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

    // Inert pure virtuals.
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
        // XDG_DATA_HOME as well as config: GifStarredStore's directory comes
        // from matrix::app_data::appDataBase(), which reads XDG_DATA_HOME, not
        // QSettings. Otherwise the suite writes into the real ~/.local/share
        // and cases depend on each other.
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
        // Declared before the controller so it is destroyed after it:
        // MediaBridge holds a raw pointer to this client.
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

        // A minimal real GIF (magic plus logical-screen width/height, the
        // smallest shape gif::validateGifBytes accepts).
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);

        // --- Tier 1: session hit right after starring ---
        QSignalSpy starFinished(app.gif()->starredStore(),
                                &GifStarredStore::starFinished);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QCOMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-original")));

        // --- Tier 2: durable hit for a different mediaKey carrying identical
        // bytes, unseen by the session map, once MediaBridge has that key's
        // full bytes cached (as an inline preview fetch would). ---
        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));
        app.mediaBridge()->mediaSource(QStringLiteral("mk-duplicate"),
                                       QStringLiteral("full"));
        QCOMPARE(fake.fetches.size(), 1);
        fake.succeed(fake.fetches.first().opId, gif);
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        // Still not in the session map: the durable tier answered.
        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));

        // --- unstar(): removing the only starred hash flips both tiers false,
        // since both resolve to the same content hash. ---
        app.unstarChatGif(QStringLiteral("mk-original"));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));

        // --- clearAll() (Settings > Privacy & security): re-star, confirm the
        // durable tier sees it, clear, and confirm false. ---
        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        app.gif()->starredStore()->clearAll();
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));

        // --- Durable unstar: star via one key, unstar via a different key the
        // session map has never seen (the after-restart case); only the
        // content-hash tier can resolve it. ---
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

    // With nothing ever starred, the durable tier never asks MediaBridge.
    void neverStarredAnswersFalseWithoutHashingAnything()
    {
        // Declared before the controller so it is destroyed after it.
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

    // A star-fetch result must be claimed, not assumed:
    // MediaBridge::fetchFullForStar is shared with other callers (e.g.
    // ForwardController), and acting on every answer would write forwarded
    // images' decrypted bytes into the saved store.
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

        // A fetch this account never asked to star, as a forward issues.
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.mediaBridge()->fetchFullForStar(QStringLiteral("mk-forwarded"));
        QVERIFY(!fake.fetches.isEmpty());
        fake.succeed(fake.fetches.last().opId, gif);
        QCoreApplication::processEvents();

        QCOMPARE(app.gif()->starredStore()->count(), before);
        QCOMPARE(starFinished.count(), 0);

        // ...while a requested fetch still stars.
        app.starChatGif(QStringLiteral("mk-wanted"));
        QVERIFY(fake.fetches.size() >= 2);
        fake.succeed(fake.fetches.last().opId, gif);
        QCoreApplication::processEvents();
        QTRY_COMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
    }

    // Copy image rides the same broadcast: a copy fetch must not star and a
    // star fetch must not copy, the clipboard gets both representations, and
    // junk bytes are refused by magic sniffing.
    void copyImagePutsBothRepresentationsOnTheClipboard()
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

        // A real 1x1 PNG so QImage decodes it.
        QImage pixel(1, 1, QImage::Format_ARGB32);
        pixel.fill(Qt::red);
        QByteArray png;
        {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            QVERIFY(pixel.save(&buffer, "PNG"));
        }

        QSignalSpy copied(&app, &AppController::copyImageFinished);
        QSignalSpy starFinished(app.gif()->starredStore(),
                                &GifStarredStore::starFinished);
        const int starsBefore = app.gif()->starredStore()->count();

        app.copyImageToClipboard(QStringLiteral("mk-copy"));
        QVERIFY(!fake.fetches.isEmpty());
        fake.succeed(fake.fetches.last().opId, png);
        QCoreApplication::processEvents();

        QTRY_COMPARE(copied.count(), 1);
        QVERIFY(copied.at(0).at(0).toBool());
        // The copy fetch starred nothing.
        QCOMPARE(app.gif()->starredStore()->count(), starsBefore);
        QCOMPARE(starFinished.count(), 0);
        // The clipboard carries the decoded raster and the original bytes.
        const QMimeData *mime =
            QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime);
        QVERIFY(mime->hasImage());
        QCOMPARE(mime->data(QStringLiteral("image/png")), png);
    }

    // The bridge dedups in-flight fetches by key, so starring and copying the
    // same uncached image yields one broadcast; the handler must service both
    // claims.
    void starAndCopyRacingOnOneKeyBothComplete()
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

        QImage pixel(1, 1, QImage::Format_ARGB32);
        pixel.fill(Qt::blue);
        QByteArray png;
        {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            QVERIFY(pixel.save(&buffer, "PNG"));
        }

        QSignalSpy copied(&app, &AppController::copyImageFinished);
        QSignalSpy starFinished(app.gif()->starredStore(),
                                &GifStarredStore::starFinished);

        app.starChatGif(QStringLiteral("mk-race"));
        const int fetchesAfterStar = fake.fetches.size();
        QVERIFY(fetchesAfterStar >= 1);
        app.copyImageToClipboard(QStringLiteral("mk-race"));
        // Deduped: the copy rides the star's in-flight fetch.
        QCOMPARE(fake.fetches.size(), fetchesAfterStar);

        fake.succeed(fake.fetches.last().opId, png);
        QCoreApplication::processEvents();

        QTRY_COMPARE(copied.count(), 1);
        QVERIFY(copied.at(0).at(0).toBool());
        QTRY_COMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
    }

    void copyImageRefusesNonRasterBytes()
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
        app.mediaBridge()->setClient(&fake);

        QSignalSpy copied(&app, &AppController::copyImageFinished);
        app.copyImageToClipboard(QStringLiteral("mk-svg"));
        QVERIFY(!fake.fetches.isEmpty());
        fake.succeed(fake.fetches.last().opId,
                     QByteArray("<svg onload=alert(1)></svg>"));
        QCoreApplication::processEvents();
        QTRY_COMPARE(copied.count(), 1);
        QVERIFY(!copied.at(0).at(0).toBool()); // refused by magic sniffing
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

};

// QTEST_MAIN (QGuiApplication, offscreen): the copy cases need a real
// clipboard.
QTEST_MAIN(AppControllerChatGifStarredTest)
#include "AppControllerChatGifStarredTest.moc"
