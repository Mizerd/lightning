
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

}

class AppControllerChatGifStarredTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
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

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);

        QSignalSpy starFinished(app.gif()->starredStore(),
                                &GifStarredStore::starFinished);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QCOMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-original")));

        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));
        app.mediaBridge()->mediaSource(QStringLiteral("mk-duplicate"),
                                       QStringLiteral("full"));
        QCOMPARE(fake.fetches.size(), 1);
        fake.succeed(fake.fetches.first().opId, gif);
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        QVERIFY(!app.gif()->starredStore()->isStarredThisSession(
            QStringLiteral("mk-duplicate")));

        app.unstarChatGif(QStringLiteral("mk-original"));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));

        app.gif()->starredStore()->starBytes(QStringLiteral("mk-original"), gif);
        QVERIFY(app.isChatGifStarred(QStringLiteral("mk-duplicate")));
        app.gif()->starredStore()->clearAll();
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-original")));
        QVERIFY(!app.isChatGifStarred(QStringLiteral("mk-duplicate")));

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

    void neverStarredAnswersFalseWithoutHashingAnything()
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

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.mediaBridge()->fetchFullForStar(QStringLiteral("mk-forwarded"));
        QVERIFY(!fake.fetches.isEmpty());
        fake.succeed(fake.fetches.last().opId, gif);
        QCoreApplication::processEvents();

        QCOMPARE(app.gif()->starredStore()->count(), before);
        QCOMPARE(starFinished.count(), 0);

        app.starChatGif(QStringLiteral("mk-wanted"));
        QVERIFY(fake.fetches.size() >= 2);
        fake.succeed(fake.fetches.last().opId, gif);
        QCoreApplication::processEvents();
        QTRY_COMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
    }

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
        QCOMPARE(app.gif()->starredStore()->count(), starsBefore);
        QCOMPARE(starFinished.count(), 0);
        const QMimeData *mime =
            QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime);
        QVERIFY(mime->hasImage());
        QCOMPARE(mime->data(QStringLiteral("image/png")), png);
    }

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
        QVERIFY(!copied.at(0).at(0).toBool());
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

};

QTEST_MAIN(AppControllerChatGifStarredTest)
#include "AppControllerChatGifStarredTest.moc"
