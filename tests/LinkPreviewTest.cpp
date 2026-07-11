// v0.5.11: tests for the safe link-preview backend — URL extraction with
// scheme allow-list, punctuation/parenthesis/code-span handling, userinfo
// rejection, MIME-validated GIF classification, encrypted-room privacy
// gating with explicit consent, per-URL request deduplication, bounded
// caching, retry, sign-out partitioning, and settings defaults.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/LinkPreview.h"
#include "models/LinkPreviewController.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastOp = 0;
    QStringList requestedUrls;

    bool supportsUrlPreview() const override { return true; }
    quint64 fetchUrlPreview(const QString &url) override
    {
        requestedUrls.append(url);
        lastOp = nextOp++;
        return lastOp;
    }

    void succeed(quint64 opId, const QVariantMap &fields)
    {
        Q_EMIT urlPreviewFinished(opId, true, fields, QString());
    }
    void fail(quint64 opId, const QString &category)
    {
        Q_EMIT urlPreviewFinished(opId, false, {}, category);
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

} // namespace

class LinkPreviewTest : public QObject
{
    Q_OBJECT

public:
    LinkPreviewTest() = default;

private:
    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("link-preview-test"));
    }

    // ---- URL extraction --------------------------------------------------

    void extractsFirstHttpsUrl()
    {
        using matrix::link_preview::firstPreviewableUrl;
        QCOMPARE(firstPreviewableUrl(
                     QStringLiteral("look at https://example.org/a and "
                                    "https://example.org/b")),
                 QStringLiteral("https://example.org/a"));
        QCOMPARE(firstPreviewableUrl(QStringLiteral("http://plain.example/x")),
                 QString());
        QCOMPARE(firstPreviewableUrl(QStringLiteral("no links here")),
                 QString());
        QCOMPARE(firstPreviewableUrl(QString()), QString());
    }

    void trailingPunctuationIsTrimmed()
    {
        using matrix::link_preview::firstPreviewableUrl;
        QCOMPARE(firstPreviewableUrl(QStringLiteral("see https://example.org/page.")),
                 QStringLiteral("https://example.org/page"));
        QCOMPARE(firstPreviewableUrl(QStringLiteral("see https://example.org/page!?")),
                 QStringLiteral("https://example.org/page"));
        QCOMPARE(firstPreviewableUrl(
                     QStringLiteral("(see https://example.org/page)")),
                 QStringLiteral("https://example.org/page"));
        // Balanced parentheses inside the URL survive.
        QCOMPARE(firstPreviewableUrl(QStringLiteral(
                     "(https://en.wikipedia.org/wiki/Foo_(bar))")),
                 QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar)"));
    }

    void unsafeSchemesNeverMatch()
    {
        using matrix::link_preview::firstPreviewableUrl;
        QCOMPARE(firstPreviewableUrl(QStringLiteral("javascript:alert(1)")),
                 QString());
        QCOMPARE(firstPreviewableUrl(
                     QStringLiteral("data:text/html;base64,AAAA")),
                 QString());
        QCOMPARE(firstPreviewableUrl(QStringLiteral("file:///etc/passwd")),
                 QString());
        QCOMPARE(firstPreviewableUrl(QStringLiteral("blob:https://x/1")),
                 QString());
        // An unsafe scheme first must not block a later safe URL.
        QCOMPARE(firstPreviewableUrl(QStringLiteral(
                     "javascript:x https://example.org/ok")),
                 QStringLiteral("https://example.org/ok"));
    }

    void userinfoUrlsAreRejected()
    {
        using matrix::link_preview::firstPreviewableUrl;
        QCOMPARE(firstPreviewableUrl(QStringLiteral(
                     "https://user:secret@example.org/private")),
                 QString());
    }

    void codeSpansAreExcluded()
    {
        using matrix::link_preview::firstPreviewableUrl;
        QCOMPARE(firstPreviewableUrl(QStringLiteral(
                     "`https://example.org/in-code` then https://example.org/real")),
                 QStringLiteral("https://example.org/real"));
        QCOMPARE(firstPreviewableUrl(QStringLiteral(
                     "```\nhttps://example.org/block\n``` nothing else")),
                 QString());
    }

    void malformedUrlsAreSkipped()
    {
        using matrix::link_preview::firstPreviewableUrl;
        QCOMPARE(firstPreviewableUrl(QStringLiteral("https://")), QString());
        QCOMPARE(firstPreviewableUrl(QStringLiteral("https://%zz/")), QString());
    }

    void sanitizedHostOmitsEverythingSensitive()
    {
        using matrix::link_preview::sanitizedHost;
        QCOMPARE(sanitizedHost(QStringLiteral(
                     "https://example.org/path?token=SECRET#frag")),
                 QStringLiteral("example.org"));
    }

    // ---- GIF classification ----------------------------------------------

    void gifClassificationTrustsMimeNotSuffix()
    {
        using namespace matrix::link_preview;
        // image.gif that really is a GIF.
        QCOMPARE(classifyGif(QStringLiteral("image/gif"), 1024, 200, 200),
                 GifClass::Gif);
        // notgif.gif served as text/html: not a GIF.
        QCOMPARE(classifyGif(QStringLiteral("text/html"), 1024, 0, 0),
                 GifClass::NotGif);
        // Extensionless URL served as image/gif: a GIF.
        QCOMPARE(classifyGif(QStringLiteral("IMAGE/GIF; charset=binary"),
                             1024, 10, 10),
                 GifClass::Gif);
        QCOMPARE(classifyGif(QString(), 1024, 10, 10), GifClass::NotGif);
    }

    void gifSafetyLimitsApply()
    {
        using namespace matrix::link_preview;
        GifLimits limits;
        limits.maxBytes = 1000;
        limits.maxWidth = 100;
        limits.maxHeight = 100;
        QCOMPARE(classifyGif(QStringLiteral("image/gif"), 2000, 10, 10, limits),
                 GifClass::Oversized);
        QCOMPARE(classifyGif(QStringLiteral("image/gif"), 10, 500, 10, limits),
                 GifClass::Oversized);
        QCOMPARE(classifyGif(QStringLiteral("image/gif"), 10, 10, 500, limits),
                 GifClass::Oversized);
        QCOMPARE(classifyGif(QStringLiteral("image/gif"), 10, 10, 10, limits),
                 GifClass::Gif);
    }

    // ---- Controller policy -----------------------------------------------

    void unencryptedRoomAutoLoads()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        const QVariantMap state = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("https://example.org/a"),
            /*roomEncrypted=*/false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loading"));
        QCOMPARE(client.requestedUrls.size(), 1);

        client.succeed(client.lastOp,
                       { { QStringLiteral("title"), QStringLiteral("Example") } });
        const QVariantMap loaded = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("https://example.org/a"), false);
        QCOMPARE(loaded.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loaded"));
        QCOMPARE(loaded.value(QStringLiteral("title")).toString(),
                 QStringLiteral("Example"));
    }

    void unencryptedAutoLoadSettingIsRespected()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setAutoLoadUnencrypted(false);

        const QVariantMap state = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("https://example.org/a"), false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("requires_action"));
        QCOMPARE(client.requestedUrls.size(), 0);
    }

    void encryptedRoomNeverAutoLoads()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        const QVariantMap state = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("https://example.org/secret"),
            /*roomEncrypted=*/true);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("requires_action"));
        // The sanitized host is available for the consent UI...
        QCOMPARE(state.value(QStringLiteral("host")).toString(),
                 QStringLiteral("example.org"));
        // ...but the homeserver was NOT contacted.
        QCOMPARE(client.requestedUrls.size(), 0);
    }

    void explicitConsentLoadsEncryptedPreview()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        controller.previewFor(QStringLiteral("$1"),
                              QStringLiteral("https://example.org/secret"), true);
        controller.requestPreview(QStringLiteral("$1"));
        QCOMPARE(client.requestedUrls.size(), 1);
    }

    void encryptedOptInSettingEnablesAutoLoad()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setAllowEncrypted(true);

        controller.previewFor(QStringLiteral("$1"),
                              QStringLiteral("https://example.org/x"), true);
        QCOMPARE(client.requestedUrls.size(), 1);
    }

    void duplicateUrlsShareOneRequest()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        QSignalSpy changed(&controller, &LinkPreviewController::previewChanged);

        controller.previewFor(QStringLiteral("$1"),
                              QStringLiteral("https://example.org/a"), false);
        controller.previewFor(QStringLiteral("$2"),
                              QStringLiteral("see https://example.org/a too"),
                              false);
        QCOMPARE(client.requestedUrls.size(), 1);

        client.succeed(client.lastOp, {});
        // Both interested messages are notified.
        QCOMPARE(changed.count(), 2);
    }

    void failureIsRetryableAndDoesNotAffectMessages()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        controller.previewFor(QStringLiteral("$1"),
                              QStringLiteral("https://example.org/a"), false);
        client.fail(client.lastOp, QStringLiteral("network"));

        const QVariantMap failed = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("https://example.org/a"), false);
        QCOMPARE(failed.value(QStringLiteral("state")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(failed.value(QStringLiteral("retryable")).toBool(), true);

        controller.retry(QStringLiteral("$1"));
        QCOMPARE(client.requestedUrls.size(), 2);
    }

    void staleResultAfterSignOutIsRejected()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        QSignalSpy changed(&controller, &LinkPreviewController::previewChanged);

        controller.previewFor(QStringLiteral("$1"),
                              QStringLiteral("https://example.org/a"), false);
        const quint64 preLogoutOp = client.lastOp;
        client.logout();

        client.succeed(preLogoutOp,
                       { { QStringLiteral("title"), QStringLiteral("Old") } });
        QCOMPARE(changed.count(), 0);
        QCOMPARE(controller.cachedUrlCount(), 0);
    }

    void urlCacheIsBounded()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setUrlCacheLimit(2);

        for (int i = 0; i < 4; ++i) {
            controller.previewFor(
                QStringLiteral("$%1").arg(i),
                QStringLiteral("https://example.org/%1").arg(i), false);
            client.succeed(client.lastOp, {});
        }
        QVERIFY(controller.cachedUrlCount() <= 2);
    }

    void messageWithoutUrlHasNoPreview()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        const QVariantMap state = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("just words"), false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("none"));
        QCOMPARE(client.requestedUrls.size(), 0);
    }

    void gifStateIsDerivedFromLoadedMetadata()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        controller.previewFor(QStringLiteral("$1"),
                              QStringLiteral("https://example.org/image.gif"),
                              false);
        client.succeed(client.lastOp,
                       { { QStringLiteral("imageMime"), QStringLiteral("image/gif") },
                         { QStringLiteral("imageMxc"),
                           QStringLiteral("mxc://example.org/abc") },
                         { QStringLiteral("imageSize"), 2048 },
                         { QStringLiteral("imageWidth"), 300 },
                         { QStringLiteral("imageHeight"), 200 } });
        const QVariantMap state = controller.previewFor(
            QStringLiteral("$1"), QStringLiteral("https://example.org/image.gif"),
            false);
        QCOMPARE(state.value(QStringLiteral("isGif")).toBool(), true);
        QCOMPARE(state.value(QStringLiteral("animationExpected")).toBool(), true);
        QCOMPARE(state.value(QStringLiteral("gifOversized")).toBool(), false);
        QCOMPARE(state.value(QStringLiteral("imageMxc")).toString(),
                 QStringLiteral("mxc://example.org/abc"));
    }

    // ---- Settings defaults -------------------------------------------------

    void previewSettingsDefaultsAndNotifications()
    {
        QSettings fresh;
        fresh.clear();
        fresh.sync();
        SettingsManager settings;

        QCOMPARE(settings.autoLoadLinkPreviews(), true);
        // Privacy default: encrypted-room previews are OFF.
        QCOMPARE(settings.loadPreviewsInEncryptedRooms(), false);
        QCOMPARE(settings.animateGifPreviews(), true);

        QSignalSpy encryptedChanged(
            &settings, &SettingsManager::loadPreviewsInEncryptedRoomsChanged);
        settings.setLoadPreviewsInEncryptedRooms(true);
        QCOMPARE(encryptedChanged.count(), 1);
        QCOMPARE(settings.loadPreviewsInEncryptedRooms(), true);
        settings.setLoadPreviewsInEncryptedRooms(false);
    }
};

QTEST_GUILESS_MAIN(LinkPreviewTest)
#include "LinkPreviewTest.moc"
