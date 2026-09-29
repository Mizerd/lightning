// A link that is itself an image or a video ("Show images and videos from
// links inline"). Pinned here, below QML:
//   * LinkPreviewController resolves a "link:" key only for a preview that
//     LOADED as direct media, only with inline media on, and never a URL the
//     preview already knows is over the cap;
//   * what each kind resolves to: the preview's own bytes, the homeserver's
//     copy, or a fetch of the link;
//   * MediaBridge sends a "link:" key through that resolver and nowhere else,
//     delivers held bytes through its ordinary completion (so the markup
//     refusal applies), gives a link fetch at least the playable timeout
//     class, and learns no persistent size for a link;
//   * preview images are held within a byte budget;
//   * the caption strips bidi overrides; the setting defaults on.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"
#include "models/LinkPreview.h"
#include "models/LinkPreviewController.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    QStringList requestedUrls;
    quint64 lastPreviewOp = 0;

    struct LinkFetch {
        quint64 opId;
        QString url;
        QString key;
        int expect;
        int timeoutClass;
    };
    QList<LinkFetch> linkFetches;
    QStringList mediaFetches; // ordinary fetchMedia keys
    struct MxcFetch {
        quint64 opId;
        QString mxc;
        int width;
        int height;
    };
    QList<MxcFetch> mxcFetches;

    bool supportsUrlPreview() const override { return true; }
    quint64 fetchUrlPreview(const QString &url) override
    {
        requestedUrls.append(url);
        lastPreviewOp = nextOp++;
        return lastPreviewOp;
    }
    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int, int) override
    {
        mediaFetches.append(mediaKey);
        return nextOp++;
    }
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int height) override
    {
        const quint64 op = nextOp++;
        mxcFetches.append({ op, mxc, width, height });
        return op;
    }
    quint64 fetchLinkMedia(const QString &url, const QString &linkKey,
                           int expect, int timeoutClass) override
    {
        const quint64 op = nextOp++;
        linkFetches.append({ op, url, linkKey, expect, timeoutClass });
        return op;
    }

    void succeedPreview(quint64 opId, const QVariantMap &fields)
    {
        Q_EMIT urlPreviewFinished(opId, true, fields, QString());
    }
    void media(quint64 opId, const QString &key, const QByteArray &bytes,
               const QString &mime)
    {
        Q_EMIT mediaReady(opId, key, 0, bytes, mime, QString());
    }
    void mediaFail(quint64 opId, const QString &key, const QString &category)
    {
        Q_EMIT mediaFailed(opId, key, 0, category);
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

QByteArray pngBytes(int w = 8, int h = 6)
{
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return out;
}

QString dataUrl(const QByteArray &bytes, const QString &mime)
{
    return QStringLiteral("data:") + mime + QStringLiteral(";base64,")
        + QString::fromLatin1(bytes.toBase64());
}

QByteArray mp4Bytes()
{
    QByteArray b("\x00\x00\x00\x20" "ftypisom", 12);
    b.append(QByteArray(4096, '\x01'));
    return b;
}

// RIFF/WEBP with a VP8X chunk; `animated` sets the animation flag bit.
QByteArray webpBytes(bool animated)
{
    QByteArray b("RIFF\x00\x00\x00\x00" "WEBP" "VP8X", 16);
    b.append(QByteArray("\x0a\x00\x00\x00", 4)); // chunk size
    b.append(char(animated ? 0x02 : 0x00));        // flags (offset 20)
    b.append(QByteArray(9, '\0'));
    return b;
}

QVariantMap imageFields(const QString &source, const QString &mime,
                        qint64 size = 0, bool tooLarge = false)
{
    return {
        { QStringLiteral("previewKind"), QStringLiteral("direct_media") },
        { QStringLiteral("imageSource"), source },
        { QStringLiteral("imageMime"), mime },
        { QStringLiteral("imageWidth"), 8 },
        { QStringLiteral("imageHeight"), 6 },
        { QStringLiteral("imageSize"), size },
        { QStringLiteral("mediaTooLarge"), tooLarge },
    };
}

QVariantMap videoFields(qint64 size, bool tooLarge = false)
{
    return {
        { QStringLiteral("previewKind"), QStringLiteral("direct_video") },
        { QStringLiteral("videoMime"), QStringLiteral("video/mp4") },
        { QStringLiteral("videoSize"), size },
        { QStringLiteral("mediaTooLarge"), tooLarge },
    };
}

const QString kRoom = QStringLiteral("!room:example.org");

} // namespace

class LinkMediaTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;

    // Loads `url`'s preview into `c` with `fields`, through the real
    // previewFor -> dispatch -> completion path.
    static void load(LinkPreviewController &c, FakeClient &client,
                     const QString &eventId, const QString &url,
                     const QVariantMap &fields)
    {
        c.previewForEvent(kRoom, eventId, url, false);
        client.succeedPreview(client.lastPreviewOp, fields);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("link-media-test"));
    }

    // ---- LinkPreviewController ------------------------------------------

    // The privacy gate: a link's media resolves only once its preview has
    // loaded under the preview policy, and not with inline media off.
    void mediaResolvesOnlyForALoadedPreviewWithInlineMediaOn()
    {
        FakeClient client;
        LinkPreviewController c;
        c.setClient(&client);
        c.setAutoLoadUnencrypted(true);
        const QString url = QStringLiteral("https://img.example.org/cat.png");
        const QString key = LinkPreviewController::mediaKeyForUrl(url);
        QVERIFY(key.startsWith(QStringLiteral("link:")));
        QCOMPARE(key.size(), 5 + 40);

        // Unknown before any preview, and while it is still loading.
        QVERIFY(c.resolveLinkMedia(key).isEmpty());
        c.previewForEvent(kRoom, QStringLiteral("$e1"), url, false);
        QVERIFY(c.resolveLinkMedia(key).isEmpty());

        const QByteArray png = pngBytes();
        client.succeedPreview(client.lastPreviewOp,
                              imageFields(dataUrl(png, QStringLiteral("image/png")),
                                          QStringLiteral("image/png")));
        const QVariantMap target = c.resolveLinkMedia(key);
        QCOMPARE(target.value(QStringLiteral("bytes")).toByteArray(), png);
        QCOMPARE(target.value(QStringLiteral("mime")).toString(),
                 QStringLiteral("image/png"));
        // Never a URL for bytes already in hand: no second contact.
        QVERIFY(!target.contains(QStringLiteral("url")));

        c.setInlineMedia(false);
        QVERIFY2(c.resolveLinkMedia(key).isEmpty(),
                 "inline media off must fetch nothing");
        c.setInlineMedia(true);
        QVERIFY(!c.resolveLinkMedia(key).isEmpty());

        // An ordinary page is not a media key at all.
        const QString page = QStringLiteral("https://example.org/article");
        load(c, client, QStringLiteral("$e2"), page,
             { { QStringLiteral("previewKind"), QStringLiteral("metadata") },
               { QStringLiteral("title"), QStringLiteral("An article") } });
        QVERIFY(c.resolveLinkMedia(
                     LinkPreviewController::mediaKeyForUrl(page)).isEmpty());
        QCOMPARE(c.mediaKeyCount(), 1);

        // Sign-out forgets every key.
        c.clear();
        QVERIFY(c.resolveLinkMedia(key).isEmpty());
        QCOMPARE(c.mediaKeyCount(), 0);
    }

    void eachKindResolvesToWhatItMayFetch()
    {
        FakeClient client;
        LinkPreviewController c;
        c.setClient(&client);
        c.setAutoLoadUnencrypted(true);

        const QString video = QStringLiteral("https://cdn.example.org/clip.mp4");
        load(c, client, QStringLiteral("$v"), video, videoFields(12 * 1024 * 1024));
        const QVariantMap v =
            c.resolveLinkMedia(LinkPreviewController::mediaKeyForUrl(video));
        QCOMPARE(v.value(QStringLiteral("url")).toString(), video);
        QCOMPARE(v.value(QStringLiteral("expect")).toInt(), 1);

        const QString huge = QStringLiteral("https://cdn.example.org/film.mp4");
        load(c, client, QStringLiteral("$h"), huge,
             videoFields(900LL * 1024 * 1024, true));
        QVERIFY2(c.resolveLinkMedia(LinkPreviewController::mediaKeyForUrl(huge))
                     .isEmpty(),
                 "a video known to be over the cap must not be fetched");

        const QString server = QStringLiteral("https://img.example.org/s.jpg");
        load(c, client, QStringLiteral("$s"), server,
             imageFields(QStringLiteral("mxc://example.org/abc"),
                         QStringLiteral("image/jpeg")));
        const QVariantMap s =
            c.resolveLinkMedia(LinkPreviewController::mediaKeyForUrl(server));
        QCOMPARE(s.value(QStringLiteral("mxc")).toString(),
                 QStringLiteral("mxc://example.org/abc"));
        QVERIFY(!s.contains(QStringLiteral("url")));

        const QString large = QStringLiteral("https://img.example.org/big.jpg");
        load(c, client, QStringLiteral("$l"), large,
             imageFields(QString(), QStringLiteral("image/jpeg"), 9 * 1024 * 1024));
        const QVariantMap l =
            c.resolveLinkMedia(LinkPreviewController::mediaKeyForUrl(large));
        QCOMPARE(l.value(QStringLiteral("url")).toString(), large);
        QCOMPARE(l.value(QStringLiteral("expect")).toInt(), 0);

        // A data: source that is not exactly what Rust builds is refused.
        const QString odd = QStringLiteral("https://img.example.org/odd.png");
        load(c, client, QStringLiteral("$o"), odd,
             imageFields(dataUrl(pngBytes(), QStringLiteral("image/svg+xml")),
                         QStringLiteral("image/png")));
        QVERIFY(c.resolveLinkMedia(LinkPreviewController::mediaKeyForUrl(odd))
                    .isEmpty());
    }

    void theRowStateDescribesTheMedia()
    {
        FakeClient client;
        LinkPreviewController c;
        c.setClient(&client);
        c.setAutoLoadUnencrypted(true);
        const QString url = QStringLiteral("https://cdn.example.org/v/clip%20one.webm");
        c.previewForEvent(kRoom, QStringLiteral("$v"), url, false);
        client.succeedPreview(client.lastPreviewOp, videoFields(4096));
        const QVariantMap state =
            c.previewForEvent(kRoom, QStringLiteral("$v"), url, false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loaded"));
        QVERIFY(state.value(QStringLiteral("isDirectMedia")).toBool());
        QCOMPARE(state.value(QStringLiteral("mediaKind")).toString(),
                 QStringLiteral("video"));
        QCOMPARE(state.value(QStringLiteral("mediaKey")).toString(),
                 LinkPreviewController::mediaKeyForUrl(url));
        QCOMPARE(state.value(QStringLiteral("mediaSize")).toLongLong(), 4096);
        QCOMPARE(state.value(QStringLiteral("mediaHeld")).toBool(), false);
        QCOMPARE(state.value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("clip one.webm"));

        // A bidi override cannot make "exe.png" out of "gnp.exe".
        const QString spoof =
            QStringLiteral("https://x.example.org/a%E2%80%AEgnp.exe");
        c.previewForEvent(kRoom, QStringLiteral("$s"), spoof, false);
        client.succeedPreview(client.lastPreviewOp,
                              imageFields(QString(), QStringLiteral("image/png"), 1));
        const QVariantMap s =
            c.previewForEvent(kRoom, QStringLiteral("$s"), spoof, false);
        QCOMPARE(s.value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("agnp.exe"));
        QCOMPARE(c.viewerEntry(LinkPreviewController::mediaKeyForUrl(spoof))
                     .value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("agnp.exe"));
        QCOMPARE(c.viewerEntry(LinkPreviewController::mediaKeyForUrl(spoof))
                     .value(QStringLiteral("host")).toString(),
                 QStringLiteral("x.example.org"));
        QVERIFY(c.viewerEntry(QStringLiteral("link:unknown")).isEmpty());

        // Line and paragraph separators and the Arabic letter mark go too.
        const QString seps =
            QStringLiteral("https://x.example.org/a%E2%80%A8b%E2%80%A9c%D8%9Cd.png");
        c.previewForEvent(kRoom, QStringLiteral("$p"), seps, false);
        client.succeedPreview(client.lastPreviewOp,
                              imageFields(QString(), QStringLiteral("image/png"), 1));
        QCOMPARE(c.previewForEvent(kRoom, QStringLiteral("$p"), seps, false)
                     .value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("abcd.png"));

        // A long name is clipped between code points, never inside a
        // surrogate pair: 78 letters, then an emoji straddling the cut.
        const QString longName = QStringLiteral("https://x.example.org/")
            + QString(78, QLatin1Char('a'))
            + QStringLiteral("%F0%9F%98%80bbbbbbbb.png");
        c.previewForEvent(kRoom, QStringLiteral("$long"), longName, false);
        client.succeedPreview(client.lastPreviewOp,
                              imageFields(QString(), QStringLiteral("image/png"), 1));
        const QString clipped =
            c.previewForEvent(kRoom, QStringLiteral("$long"), longName, false)
                .value(QStringLiteral("fileName")).toString();
        QCOMPARE(clipped, QString(78, QLatin1Char('a')) + QChar(0x2026));
        for (qsizetype i = 0; i < clipped.size(); ++i)
            QVERIFY2(!clipped.at(i).isSurrogate(), "a lone surrogate was left");
    }

    // Loaded is per URL, but contacting the site again is per row: a card
    // another room loaded is shown, and Play/View there needs this row's
    // own consent.
    void aCardFromAnotherRoomNeedsThisRowsConsent()
    {
        FakeClient client;
        LinkPreviewController c;
        c.setClient(&client);
        c.setAutoLoadUnencrypted(true);
        c.setAllowEncrypted(false);
        const QString url = QStringLiteral("https://cdn.example.org/clip.mp4");
        load(c, client, QStringLiteral("$open"), url, videoFields(4096));
        const QVariantMap open =
            c.previewForEvent(kRoom, QStringLiteral("$open"), url, false);
        QCOMPARE(open.value(QStringLiteral("mediaAllowed")).toBool(), true);

        const QString secret = QStringLiteral("!secret:example.org");
        const QVariantMap before =
            c.previewForEvent(secret, QStringLiteral("$enc"), url, true);
        QCOMPARE(before.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loaded"));
        QVERIFY2(!before.value(QStringLiteral("mediaAllowed")).toBool(),
                 "an encrypted row may not contact the site on another room's say-so");
        c.requestPreviewForEvent(secret, QStringLiteral("$enc"));
        QVERIFY(c.previewForEvent(secret, QStringLiteral("$enc"), url, true)
                    .value(QStringLiteral("mediaAllowed")).toBool());
        // Consent for a loaded URL fetches nothing again.
        QCOMPARE(client.requestedUrls, QStringList{ url });
    }

    // The inline-media switch is presentation: it must not make rows ask for
    // their previews again (policyChanged re-dispatches evicted ones).
    void theInlineSwitchDoesNotReaskPreviews()
    {
        LinkPreviewController c;
        QSignalSpy policy(&c, &LinkPreviewController::policyChanged);
        QSignalSpy inlineSpy(&c, &LinkPreviewController::inlineMediaChanged);
        c.setInlineMedia(false);
        c.setInlineMedia(true);
        QCOMPARE(policy.count(), 0);
        QCOMPARE(inlineSpy.count(), 2);
    }

    // Preview images are held as base64 text; the cache must stay within its
    // byte budget, dropping the oldest image first and never the newest.
    void heldPreviewImagesStayWithinTheirBudget()
    {
        FakeClient client;
        LinkPreviewController c;
        c.setClient(&client);
        c.setAutoLoadUnencrypted(true);
        const QString source =
            dataUrl(QByteArray(300 * 1024, 'x'), QStringLiteral("image/png"));
        const qint64 each = qint64(source.size()) * 2;
        c.setHeldBytesBudget(each * 2 + each / 2);

        QStringList urls;
        for (int i = 0; i < 4; ++i) {
            const QString url =
                QStringLiteral("https://img.example.org/%1.png").arg(i);
            urls << url;
            load(c, client, QStringLiteral("$e%1").arg(i), url,
                 imageFields(source, QStringLiteral("image/png")));
        }
        QVERIFY2(c.heldBytes() <= each * 2 + each / 2,
                 qPrintable(QStringLiteral("held %1 bytes over a %2 budget")
                                .arg(c.heldBytes()).arg(each * 2 + each / 2)));
        QVERIFY(c.heldBytes() > 0);
        // The newest survives; the oldest went, and its media key with it.
        QVERIFY(!c.resolveLinkMedia(
                     LinkPreviewController::mediaKeyForUrl(urls.last())).isEmpty());
        QVERIFY(c.resolveLinkMedia(
                    LinkPreviewController::mediaKeyForUrl(urls.first())).isEmpty());
        QCOMPARE(c.mediaKeyCount(), 2);
        // The evicted row is still drawn; availability is what its click asks
        // before choosing the viewer or the browser.
        QVERIFY(c.linkMediaAvailable(
            LinkPreviewController::mediaKeyForUrl(urls.last())));
        QVERIFY2(!c.linkMediaAvailable(
                     LinkPreviewController::mediaKeyForUrl(urls.first())),
                 "an evicted link must not claim a viewer it cannot fill");
        c.clear();
        QCOMPARE(c.heldBytes(), 0);
    }

    // ---- MediaBridge ----------------------------------------------------

    void aLinkKeyFetchesThroughTheResolverOnly()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString key = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://cdn.example.org/clip.mp4"));

        // Without a resolver nothing is fetched and the key fails.
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QCOMPARE(bridge.mediaSource(key, QStringLiteral("full")), QString());
        QVERIFY(client.linkFetches.isEmpty());
        QVERIFY(client.mediaFetches.isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("unavailable"));
        bridge.retry(QStringLiteral("full:") + key);

        QVariantMap answer;
        bridge.setLinkMediaResolver([&answer](const QString &) { return answer; });
        // A refusal is the same: no fetch.
        QCOMPARE(bridge.mediaSource(key, QStringLiteral("full")), QString());
        QVERIFY(client.linkFetches.isEmpty());
        bridge.retry(QStringLiteral("full:") + key);

        answer = { { QStringLiteral("url"),
                     QStringLiteral("https://cdn.example.org/clip.mp4") },
                   { QStringLiteral("expect"), 1 } };
        QCOMPARE(bridge.playableSource(key), QString());
        QCOMPARE(client.linkFetches.size(), 1);
        QCOMPARE(client.linkFetches.at(0).key, key);
        QCOMPARE(client.linkFetches.at(0).expect, 1);
        QVERIFY2(client.linkFetches.at(0).timeoutClass >= 1,
                 "a link fetch must not get the 40 s class");
        // Never the Matrix media path.
        QVERIFY(client.mediaFetches.isEmpty());

        // A viewer (class 0) request is raised too.
        const QString imageKey = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://img.example.org/big.jpg"));
        answer = { { QStringLiteral("url"),
                     QStringLiteral("https://img.example.org/big.jpg") },
                   { QStringLiteral("expect"), 0 } };
        bridge.mediaSource(imageKey, QStringLiteral("full"));
        QCOMPARE(client.linkFetches.size(), 2);
        QCOMPARE(client.linkFetches.at(1).expect, 0);
        QVERIFY(client.linkFetches.at(1).timeoutClass >= 1);

        // Thumbnail classes never fetch a link, and are not failures either
        // (the viewer's strip asks for one on every open).
        const int before = client.linkFetches.size();
        const int failuresBefore = failed.count();
        bridge.mediaSource(key, QStringLiteral("thumb"));
        bridge.mediaSource(key, QStringLiteral("list_thumb"));
        QCOMPARE(client.linkFetches.size(), before);
        QCOMPARE(failed.count(), failuresBefore);
    }

    // Held bytes complete through the ordinary path, so every check there
    // applies, and no request leaves the process.
    void heldLinkBytesCompleteThroughTheOrdinaryPath()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QByteArray png = pngBytes();
        QByteArray held = png;
        bridge.setLinkMediaResolver([&held](const QString &) {
            return QVariantMap{ { QStringLiteral("bytes"), held },
                                { QStringLiteral("mime"),
                                  QStringLiteral("image/png") } };
        });
        const QString key = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://img.example.org/cat.png"));
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        QCOMPARE(bridge.mediaSource(key, QStringLiteral("full")), QString());
        QTRY_COMPARE(cached.count(), 1);
        QCOMPARE(cached.at(0).at(0).toString(), QStringLiteral("full:") + key);
        QVERIFY(bridge.mediaSource(key, QStringLiteral("full"))
                    .startsWith(QStringLiteral("image://lightning-media/")));
        QVERIFY(client.linkFetches.isEmpty());
        QVERIFY(client.mxcFetches.isEmpty());
        QVERIFY(client.mediaFetches.isEmpty());

        // Markup is refused even from held bytes.
        held = QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
        const QString svgKey = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://img.example.org/x.png"));
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.mediaSource(svgKey, QStringLiteral("full"));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
        QCOMPARE(bridge.cachedSource(QStringLiteral("full:") + svgKey), QString());

        // Sign-out before the queued completion: nothing lands.
        held = png;
        const QString lateKey = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://img.example.org/late.png"));
        bridge.mediaSource(lateKey, QStringLiteral("full"));
        bridge.clear();
        QTest::qWait(50);
        QCOMPARE(bridge.cachedSource(QStringLiteral("full:") + lateKey), QString());
    }

    // A server-route preview: the viewer gets the homeserver's original, and
    // the site is not contacted.
    void aServerRouteImageFetchesTheHomeserverCopy()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setLinkMediaResolver([](const QString &) {
            return QVariantMap{ { QStringLiteral("mxc"),
                                  QStringLiteral("mxc://example.org/abc") } };
        });
        const QString key = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://img.example.org/s.jpg"));
        bridge.mediaSource(key, QStringLiteral("full"));
        QCOMPARE(client.mxcFetches.size(), 1);
        QCOMPARE(client.mxcFetches.at(0).mxc, QStringLiteral("mxc://example.org/abc"));
        QCOMPARE(client.mxcFetches.at(0).width, 0);
        QCOMPARE(client.mxcFetches.at(0).height, 0);
        QVERIFY(client.linkFetches.isEmpty());
        // The answer arrives under the mxc key and still lands on the link key.
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        client.media(client.mxcFetches.at(0).opId,
                     QStringLiteral("mxc://example.org/abc"), pngBytes(),
                     QStringLiteral("image/jpeg"));
        QCOMPARE(cached.count(), 1);
        QCOMPARE(cached.at(0).at(0).toString(), QStringLiteral("full:") + key);
    }

    // Played sizes persist per account for Matrix media; a link must leave no
    // such record.
    void aPlayedLinkLeavesNoLearnedSize()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setLinkMediaResolver([](const QString &) {
            return QVariantMap{ { QStringLiteral("url"),
                                  QStringLiteral("https://cdn.example.org/c.mp4") },
                                { QStringLiteral("expect"), 1 } };
        });
        QSignalSpy learned(&bridge, &MediaBridge::playableSizeLearned);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        const QString key = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://cdn.example.org/c.mp4"));
        bridge.playableSource(key);
        QCOMPARE(client.linkFetches.size(), 1);
        client.media(client.linkFetches.at(0).opId, key, mp4Bytes(),
                     QStringLiteral("video/mp4"));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(bridge.playableSource(key).startsWith(QStringLiteral("file:")));
        QCOMPARE(learned.count(), 0);

        // Control: the same payload for a Matrix event is learned.
        bridge.playableSource(QStringLiteral("$event:example.org"));
        QCOMPARE(client.mediaFetches.size(), 1);
        client.media(client.nextOp - 1, QStringLiteral("$event:example.org"),
                     mp4Bytes(), QStringLiteral("video/mp4"));
        QCOMPARE(learned.count(), 1);
    }

    // "too_large" reaches the card by its category, so it can say so.
    void anOverCapFetchFailsAsTooLarge()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setLinkMediaResolver([](const QString &) {
            return QVariantMap{ { QStringLiteral("url"),
                                  QStringLiteral("https://cdn.example.org/c.mp4") },
                                { QStringLiteral("expect"), 1 } };
        });
        const QString key = LinkPreviewController::mediaKeyForUrl(
            QStringLiteral("https://cdn.example.org/c.mp4"));
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.playableSource(key);
        client.mediaFail(client.linkFetches.at(0).opId, key,
                         QStringLiteral("too_large"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("full:") + key);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("too_large"));
    }

    // Link previews of animated WebP play like GIFs; a still WebP stays still.
    void aPreviewWebpAnimatesOnlyWhenItIsAnimated()
    {
        MediaBridge bridge;
        const QString animated = bridge.previewAnimatedSource(
            dataUrl(webpBytes(true), QStringLiteral("image/webp")),
            QStringLiteral("image/webp"));
        QVERIFY2(animated.startsWith(QStringLiteral("file:")),
                 "an animated WebP preview has no animation source");
        QVERIFY(QFileInfo::exists(QUrl(animated).toLocalFile()));
        QCOMPARE(bridge.previewAnimatedSource(
                     dataUrl(webpBytes(false), QStringLiteral("image/webp")),
                     QStringLiteral("image/webp")),
                 QString());
        // The label must match the data: prefix.
        QCOMPARE(bridge.previewAnimatedSource(
                     dataUrl(webpBytes(true), QStringLiteral("image/gif")),
                     QStringLiteral("image/webp")),
                 QString());
    }

    // ---- Settings -------------------------------------------------------

    void theSettingDefaultsOnAndPersists()
    {
        // Restored however the case ends, or the next run reads "false".
        const auto restore = qScopeGuard([] {
            SettingsManager settings;
            settings.setShowLinkMediaInline(true);
        });
        {
            SettingsManager settings;
            QCOMPARE(settings.showLinkMediaInline(), true);
            // The preview switches it rides on stay off by default.
            QCOMPARE(settings.autoLoadLinkPreviews(), false);
            QCOMPARE(settings.loadPreviewsInEncryptedRooms(), false);
            QSignalSpy spy(&settings, &SettingsManager::showLinkMediaInlineChanged);
            settings.setShowLinkMediaInline(false);
            settings.setShowLinkMediaInline(false);
            QCOMPARE(spy.count(), 1);
        }
        SettingsManager reread;
        QCOMPARE(reread.showLinkMediaInline(), false);
    }
};

// Guiless, like media-bridge-test: no decoder may be constructed here.
QTEST_GUILESS_MAIN(LinkMediaTest)
#include "LinkMediaTest.moc"
