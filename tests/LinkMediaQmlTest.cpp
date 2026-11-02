#include <QtTest/QtTest>

#include <QDesktopServices>

#include <memory>

#include <QBuffer>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaImageProvider.h"
#include "models/LinkPreviewController.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

const QString kImageUrl = QStringLiteral("https://img.example.org/pics/cat.png");
const QString kVideoUrl = QStringLiteral("https://cdn.example.org/v/clip.mp4");

class FakePreviewClient final : public MatrixClient
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

QString pngDataUrl()
{
    QImage image(8, 6, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QStringLiteral("data:image/png;base64,")
        + QString::fromLatin1(bytes.toBase64());
}

const QString kBigVideoUrl = QStringLiteral("https://cdn.example.org/v/film.mp4");
const QString kOtherImageUrl = QStringLiteral("https://img.example.org/pics/dog.png");
const QString kRoomId = QStringLiteral("!links:mock.local");

QVariantMap imageFields()
{
    return {
        { QStringLiteral("previewKind"), QStringLiteral("direct_media") },
        { QStringLiteral("imageSource"), pngDataUrl() },
        { QStringLiteral("imageMime"), QStringLiteral("image/png") },
        { QStringLiteral("imageWidth"), 8 },
        { QStringLiteral("imageHeight"), 6 },
        { QStringLiteral("imageSize"), 90 },
    };
}

QVariantMap videoFields(bool tooLarge = false)
{
    return {
        { QStringLiteral("previewKind"), QStringLiteral("direct_video") },
        { QStringLiteral("videoMime"), QStringLiteral("video/mp4") },
        { QStringLiteral("videoSize"),
          tooLarge ? qint64(900) * 1024 * 1024 : qint64(13002342) },
        { QStringLiteral("mediaTooLarge"), tooLarge },
    };
}

QVariantMap rowState(LinkPreviewController *previews, FakePreviewClient &fake,
                     const QString &url, const QVariantMap &fields,
                     const QString &eventId = QStringLiteral("$row"),
                     bool encrypted = false,
                     const QString &roomId = kRoomId)
{
    const quint64 before = fake.lastOp;
    previews->previewForEvent(roomId, eventId, url, encrypted);
    if (fake.lastOp != before)
        fake.succeed(fake.lastOp, fields);
    return previews->previewForEvent(roomId, eventId, url, encrypted);
}

void usePreviews(AppController &controller, FakePreviewClient &fake)
{
    controller.linkPreviews()->setClient(&fake);
    controller.linkPreviews()->setAutoLoadUnencrypted(true);
}

class UrlSink : public QObject
{
    Q_OBJECT
public:
    QStringList opened;
public Q_SLOTS:
    void open(const QUrl &url) { opened << url.toString(); }
};

QList<QQuickItem *> imagesUnder(QQuickItem *root)
{
    QList<QQuickItem *> out;
    for (QQuickItem *item : root->findChildren<QQuickItem *>()) {
        if (item->inherits("QQuickImage"))
            out << item;
    }
    return out;
}

bool drawsPreviewBytes(QQuickItem *image)
{
    return image->property("source").toUrl().toString(QUrl::FullyEncoded)
        .startsWith(QStringLiteral("image://lightning-media/preview-image"));
}
}

class LinkMediaQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    UrlSink m_browser;

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QObject *host = nullptr;
        QStringList warnings;
    };

    static QVariantMap baseFixture(AppController &controller, const QString &body)
    {
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("eventId"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("itemId"), QStringLiteral("fixture-item"));
        fixture.insert(QStringLiteral("sender"), QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), body);
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("errorKind"), QString{});
        for (const char *flag : { "isImage", "isFile", "isVideo", "isAudio",
                                  "isSticker", "mediaIsVoice", "isThreadRoot",
                                  "mentionsMe", "mentionsRoom", "isLocalEcho",
                                  "mediaSourceAvailable", "mediaThumbAvailable" })
            fixture.insert(QString::fromLatin1(flag), false);
        for (const char *zero : { "mediaDurationMs", "mediaWidth", "mediaHeight",
                                  "mediaSize" })
            fixture.insert(QString::fromLatin1(zero), 0);
        for (const char *empty : { "mediaKey", "mediaFilename", "mediaMimetype",
                                   "replyToEventId" })
            fixture.insert(QString::fromLatin1(empty), QString{});
        fixture.insert(QStringLiteral("mediaUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        return fixture;
    }

    static QObject *host(QQmlEngine *engine, QObject *owner)
    {
        QQmlComponent component(engine);
        component.setData(R"QML(
import QtQuick
QtObject {
    id: pane
    property bool roomEncrypted: false
    property real contentY: 0
    property real height: 10000
    property bool speculativeMediaAllowed: true
    property bool stickToBottom: false
    property bool threadContext: false
    property string transientInteractionOwner: ""
    property string hoveredActionsKey: ""
    property string pinnedActionsKey: ""
    property string openedKey: ""
    property int openCount: 0
    property var openImage: function(mediaKey, httpUrl) {
        pane.openedKey = mediaKey
        pane.openCount += 1
    }
    function stateGroupExpanded(groupId) { return false }
    function toggleStateGroup(groupId) {}
}
)QML",
                          QUrl());
        QObject *object = component.create();
        if (object)
            object->setParent(owner);
        return object;
    }

    bool createDelegate(AppController &controller, const QVariantMap &preview,
                        Delegate &out, bool roomEncrypted = false,
                        bool setPreview = true)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        out.engine->addImageProvider(
            QStringLiteral("lightning-media"),
            new MediaImageProvider(controller.mediaBridge()));
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty(
            "model", baseFixture(controller, preview.value(QStringLiteral("url"))
                                                 .toString()));
        out.host = host(out.engine.get(), out.engine.get());
        if (!out.host)
            return false;
        out.host->setProperty("roomEncrypted", roomEncrypted);
        out.engine->setInitialProperties(
            { { QStringLiteral("timelineView"), QVariant::fromValue(out.host) } });
        QSignalSpy createdSpy(out.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(760, 900);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(700);
        out.window->show();
        if (!QTest::qWaitForWindowExposed(out.window.get()))
            return false;
        if (setPreview)
            out.root->setProperty("preview", QVariant::fromValue(preview));
        QCoreApplication::processEvents();
        return true;
    }

    static QQuickItem *find(Delegate &d, const QString &name)
    {
        return d.root->findChild<QQuickItem *>(name);
    }

    static void settle(Delegate &d)
    {
        QCoreApplication::processEvents();
        if (d.window)
            (void)d.window->grabWindow();
    }

    static bool click(Delegate &d, QQuickItem *item)
    {
        if (!item || !item->isVisible() || item->width() <= 0)
            return false;
        settle(d);
        const QPointF centre = item->mapToScene(
            QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(d.window.get(), Qt::LeftButton, {}, centre.toPoint());
        QCoreApplication::processEvents();
        return true;
    }

    static QString remoteSource(Delegate &d)
    {
        {
            for (QQuickItem *image : imagesUnder(d.root)) {
                const QString src = image->property("source").toUrl().toString();
                if (src.isEmpty()
                    || src.startsWith(QStringLiteral("image://lightning-media/"))
                    || src.startsWith(QStringLiteral("file:")))
                    continue;
                if (src.startsWith(QStringLiteral("qrc:"))
                    || src.startsWith(QStringLiteral("image://")))
                    continue;
                return src;
            }
        }
        return {};
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("link-media-qml-test"));
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &m_browser, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("http"), &m_browser, "open");
    }

    void cleanupTestCase()
    {
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
    }

    void init() { m_browser.opened.clear(); }

    void unrelatedMediaCompletionDoesNotReevaluateLoadedPreviews()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        const QVariantMap row = rowState(controller.linkPreviews(), fake,
                                        kImageUrl, imageFields());
        {
            Delegate direct;
            QVERIFY(createDelegate(controller, row, direct));
            QQuickItem *directCard = find(direct, QStringLiteral("directMediaPreview"));
            QVERIFY(directCard);
            QCOMPARE(directCard->property("previewTick").toInt(), 0);
            Q_EMIT controller.mediaBridge()->mediaCached(
                QStringLiteral("thumb:unrelated-synthetic-media"));
            QCoreApplication::processEvents();
            QCOMPARE(directCard->property("previewTick").toInt(), 0);
        }
        QVariantMap mxcRow = row;
        mxcRow.insert(QStringLiteral("imageSource"),
                      QStringLiteral("mxc://fixture.invalid/preview"));
        {
            Delegate direct;
            QVERIFY(createDelegate(controller, mxcRow, direct));
            QQuickItem *directCard = find(direct, QStringLiteral("directMediaPreview"));
            QVERIFY(directCard);
            Q_EMIT controller.mediaBridge()->mediaCached(
                QStringLiteral("mxcimg:480:mxc://fixture.invalid/preview"));
            QCoreApplication::processEvents();
            QCOMPARE(directCard->property("previewTick").toInt(), 0);
            Q_EMIT controller.mediaBridge()->mediaCached(
                QStringLiteral("mxcimg:512:mxc://fixture.invalid/preview"));
            QCoreApplication::processEvents();
            QCOMPARE(directCard->property("previewTick").toInt(), 1);
        }
        controller.linkPreviews()->setInlineMedia(false);
        Delegate article;
        QVERIFY(createDelegate(controller, mxcRow, article));
        QQuickItem *articleCard = find(article, QStringLiteral("linkPreviewCard"));
        QVERIFY(articleCard);
        QCOMPARE(articleCard->property("previewTick").toInt(), 0);

        Q_EMIT controller.mediaBridge()->mediaCached(
            QStringLiteral("thumb:unrelated-synthetic-media"));
        QCoreApplication::processEvents();
        QCOMPARE(articleCard->property("previewTick").toInt(), 0);
        Q_EMIT controller.mediaBridge()->mediaCached(
            QStringLiteral("mxcimg:480:mxc://fixture.invalid/preview"));
        QCoreApplication::processEvents();
        QCOMPARE(articleCard->property("previewTick").toInt(), 0);
        Q_EMIT controller.mediaBridge()->mediaCached(
            QStringLiteral("mxcimg:512:mxc://fixture.invalid/preview"));
        QCoreApplication::processEvents();
        QCOMPARE(articleCard->property("previewTick").toInt(), 1);
    }

    void aLinkImageOpensTheInAppViewer()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        const QVariantMap row =
            rowState(controller.linkPreviews(), fake, kImageUrl, imageFields());
        Delegate d;
        QVERIFY(createDelegate(controller, row, d));
        QQuickItem *embed = find(d, QStringLiteral("directMediaPreview"));
        QVERIFY2(embed, "a held link image is not drawn as media");
        QVERIFY(embed->isVisible());
        QVERIFY(click(d, embed));
        QCOMPARE(d.host->property("openCount").toInt(), 1);
        QCOMPARE(d.host->property("openedKey").toString(),
                 LinkPreviewController::mediaKeyForUrl(kImageUrl));
        QCOMPARE(m_browser.opened, QStringList{});
        bool sawPreviewBytes = false;
        for (QQuickItem *image : imagesUnder(d.root))
            sawPreviewBytes = sawPreviewBytes || drawsPreviewBytes(image);
        QVERIFY2(sawPreviewBytes,
                 "no Image carries the preview-bytes source: the image scan is blind");
        QQuickItem *open = find(d, QStringLiteral("directMediaOpenInBrowser"));
        QVERIFY(open);
        QCOMPARE(remoteSource(d), QString());
        QCOMPARE(d.warnings, QStringList{});
    }

    void aVideoLinkFetchesNothingUntilPlay()
    {
        QStringList outcomes;
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setSupportsMediaBridgeForTest(true);
        const QString key = LinkPreviewController::mediaKeyForUrl(kVideoUrl);
        connect(controller.mediaBridge(), &MediaBridge::mediaFetchFailed,
                controller.mediaBridge(),
                [&outcomes, key](const QString &cacheKey, const QString &category) {
                    if (cacheKey == QStringLiteral("full:") + key)
                        outcomes << category;
                });
        connect(controller.mediaBridge(), &MediaBridge::mediaCached,
                controller.mediaBridge(),
                [&outcomes, key](const QString &cacheKey) {
                    if (cacheKey == QStringLiteral("full:") + key)
                        outcomes << QStringLiteral("cached");
                });

        Delegate d;
        QVERIFY(createDelegate(
            controller,
            rowState(controller.linkPreviews(), fake, kVideoUrl, videoFields()), d));
        QQuickItem *card = find(d, QStringLiteral("linkMediaCard"));
        QVERIFY2(card, "a video link is not drawn as a video card");
        QVERIFY(!find(d, QStringLiteral("videoPlayerCard")));
        auto *loader = find(d, QStringLiteral("linkMediaPlayerLoader"));
        QVERIFY(loader);
        QCOMPARE(loader->property("active").toBool(), false);
        QCOMPARE(controller.mediaBridge()->healthSnapshot()
                     .value(QStringLiteral("inflight")).toLongLong(), 0);
        QTest::qWait(50);
        QCOMPARE(outcomes, QStringList{});
        QVERIFY(find(d, QStringLiteral("linkMediaPlayAffordance"))->isVisible());
        QVERIFY(!find(d, QStringLiteral("linkMediaTooLarge"))->isVisible());
        const QString chip = find(d, QStringLiteral("linkMediaChip"))
                                 ->property("text").toString();
        QVERIFY2(chip.contains(QStringLiteral("12.4")),
                 qPrintable(QStringLiteral("chip: %1").arg(chip)));

        // Play: the player takes the cover's box, and only now is the video
        // asked for (the mock backend has no link fetch, so it answers
        // "unavailable" once the resolver hands it the URL).
        const QSizeF before(card->width(), card->height());
        QVERIFY(click(d, find(d, QStringLiteral("linkMediaActivate"))));
        QTRY_VERIFY(find(d, QStringLiteral("videoPlayerCard")));
        QCOMPARE(m_browser.opened, QStringList{});
        auto *player = find(d, QStringLiteral("videoPlayerCard"));
        QCOMPARE(QSizeF(player->width(), player->height()), before);
        QCOMPARE(player->property("mediaKey").toString(), key);
        QTRY_COMPARE(outcomes, QStringList{ QStringLiteral("unavailable") });
        QCOMPARE(remoteSource(d), QString());
    }

    void anOverCapVideoSaysSoAndOffersNoPlayer()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        {
            Delegate d;
            QVERIFY(createDelegate(
                controller,
                rowState(controller.linkPreviews(), fake, kBigVideoUrl,
                         videoFields(true), QStringLiteral("$big")),
                d));
            QVERIFY(find(d, QStringLiteral("linkMediaTooLarge"))->isVisible());
            QVERIFY(!find(d, QStringLiteral("linkMediaPlayAffordance"))->isVisible());
            QVERIFY(click(d, find(d, QStringLiteral("linkMediaActivate"))));
            QCOMPARE(m_browser.opened, QStringList{ kBigVideoUrl });
            QVERIFY(!find(d, QStringLiteral("videoPlayerCard")));
            m_browser.opened.clear();
        }
        Delegate d;
        QVERIFY(createDelegate(
            controller,
            rowState(controller.linkPreviews(), fake, kVideoUrl, videoFields()), d));
        QVERIFY(click(d, find(d, QStringLiteral("linkMediaActivate"))));
        QTRY_VERIFY(find(d, QStringLiteral("videoPlayerCard")));
        const QString key = LinkPreviewController::mediaKeyForUrl(kVideoUrl);
        Q_EMIT controller.mediaBridge()->mediaFetchFailed(
            QStringLiteral("full:") + key, QStringLiteral("too_large"));
        QTRY_VERIFY(!find(d, QStringLiteral("videoPlayerCard")));
        QVERIFY(find(d, QStringLiteral("linkMediaTooLarge"))->isVisible());
        QVERIFY(!find(d, QStringLiteral("linkMediaPlayAffordance"))->isVisible());
    }

    void inlineMediaOffIsAPlainCardThatNamesTheKind()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        const QVariantMap video = rowState(controller.linkPreviews(), fake,
                                           kVideoUrl, videoFields(),
                                           QStringLiteral("$v"));
        const QVariantMap image = rowState(controller.linkPreviews(), fake,
                                           kImageUrl, imageFields(),
                                           QStringLiteral("$i"));
        controller.linkPreviews()->setInlineMedia(false);
        for (const QVariantMap &preview : { video, image }) {
            Delegate d;
            QVERIFY(createDelegate(controller, preview, d));
            QVERIFY(find(d, QStringLiteral("linkPreviewCard")));
            QVERIFY(!find(d, QStringLiteral("linkMediaCard")));
            QVERIFY(!find(d, QStringLiteral("directMediaPreview")));
            auto *summary = find(d, QStringLiteral("linkPreviewMediaSummary"));
            QVERIFY(summary);
            const QString text = summary->property("text").toString();
            const bool video = preview.value(QStringLiteral("mediaKind")).toString()
                == QLatin1String("video");
            QVERIFY2(text.startsWith(video ? QStringLiteral("Video")
                                           : QStringLiteral("Image"))
                         && text.contains(preview.value(QStringLiteral("fileName"))
                                              .toString()),
                     qPrintable(text));
            const QList<QQuickItem *> images = imagesUnder(d.root);
            QVERIFY(!images.isEmpty());
            for (QQuickItem *image : images)
                QVERIFY2(!drawsPreviewBytes(image),
                         qPrintable(QStringLiteral(
                             "inline media off still loads the link's picture: %1")
                                        .arg(image->property("source").toUrl()
                                                 .toString())));
            QCOMPARE(remoteSource(d), QString());
        }
    }

    void collapsedEmbedsNameTheKindAndBuildNoMedia()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        controller.settings()->setCollapseEmbeds(true);
        const auto restore = qScopeGuard(
            [&controller] { controller.settings()->setCollapseEmbeds(false); });
        Delegate d;
        QVERIFY(createDelegate(
            controller,
            rowState(controller.linkPreviews(), fake, kVideoUrl, videoFields()), d));
        auto *row = find(d, QStringLiteral("collapsedEmbedRow"));
        QVERIFY(row);
        QCOMPARE(row->property("kindLabel").toString(), QStringLiteral("Video"));
        QCOMPARE(row->property("iconName").toString(), QStringLiteral("videocam"));
        QVERIFY(row->property("detailText").toString()
                    .contains(QStringLiteral("clip.mp4")));
        QVERIFY(!find(d, QStringLiteral("linkMediaCard")));
    }

    void aClickAfterEvictionOpensTheBrowser()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        LinkPreviewController *previews = controller.linkPreviews();
        Delegate d;
        QVERIFY(createDelegate(controller,
                               rowState(previews, fake, kImageUrl, imageFields()),
                               d));
        QVERIFY(find(d, QStringLiteral("directMediaPreview")));
        previews->setHeldBytesBudget(1);
        rowState(previews, fake, kOtherImageUrl, imageFields(),
                 QStringLiteral("$other"));
        QVERIFY(!previews->linkMediaAvailable(
            LinkPreviewController::mediaKeyForUrl(kImageUrl)));
        QVERIFY(click(d, find(d, QStringLiteral("directMediaPreview"))));
        QCOMPARE(d.host->property("openCount").toInt(), 0);
        QCOMPARE(m_browser.opened, QStringList{ kImageUrl });
    }

    void aCardFromAnotherRoomSaysWhatPlayCosts()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        usePreviews(controller, fake);
        LinkPreviewController *previews = controller.linkPreviews();
        previews->setAllowEncrypted(false);
        const QVariantMap openRow = rowState(previews, fake, kVideoUrl,
                                             videoFields(), QStringLiteral("$open"));
        {
            Delegate open;
            QVERIFY(createDelegate(controller, openRow, open));
            QVERIFY(!find(open, QStringLiteral("linkMediaConsentNotice"))
                         ->isVisible());
        }
        const QString secretRoom = QStringLiteral("!secret:mock.local");
        controller.setCurrentRoomId(secretRoom);
        const QString rowKey = QStringLiteral("fixture-item");
        Delegate d;
        QVERIFY(createDelegate(controller,
                               { { QStringLiteral("url"), kVideoUrl } }, d,
true,false));
        const QVariantMap own =
            previews->previewForEvent(secretRoom, rowKey, kVideoUrl, true);
        QCOMPARE(own.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loaded"));
        QCOMPARE(own.value(QStringLiteral("mediaAllowed")).toBool(), false);
        auto *notice = find(d, QStringLiteral("linkMediaConsentNotice"));
        QVERIFY2(notice && notice->isVisible(),
                 "Play would contact the site with nothing said first");
        const QString text = notice->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("cdn.example.org"))
                     && text.contains(QStringLiteral("IP")),
                 qPrintable(text));
        QCOMPARE(fake.requestedUrls, QStringList{ kVideoUrl });

        QVERIFY(click(d, find(d, QStringLiteral("linkMediaActivate"))));
        QTRY_VERIFY(find(d, QStringLiteral("videoPlayerCard")));
        QVERIFY2(previews->previewForEvent(secretRoom, rowKey, kVideoUrl, true)
                     .value(QStringLiteral("mediaAllowed")).toBool(),
                 "Play did not record consent on the row that was pressed");
        QCOMPARE(fake.requestedUrls, QStringList{ kVideoUrl });
        QCOMPARE(m_browser.opened, QStringList{});
    }

    void theViewerShowsALinkImageFromTheBridge()
    {
        FakePreviewClient fake;
        AppController controller(AppController::MockBackend);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setSupportsMediaBridgeForTest(true);
        LinkPreviewController *previews = controller.linkPreviews();
        previews->setClient(&fake);
        previews->setAutoLoadUnencrypted(true);
        previews->previewForEvent(QStringLiteral("!r:mock.local"),
                                  QStringLiteral("$e"), kImageUrl, false);
        QCOMPARE(fake.requestedUrls, QStringList{ kImageUrl });
        fake.succeed(fake.lastOp, {
            { QStringLiteral("previewKind"), QStringLiteral("direct_media") },
            { QStringLiteral("imageSource"), pngDataUrl() },
            { QStringLiteral("imageMime"), QStringLiteral("image/png") },
            { QStringLiteral("imageWidth"), 8 },
            { QStringLiteral("imageHeight"), 6 },
        });
        const QString key = LinkPreviewController::mediaKeyForUrl(kImageUrl);

        QQmlEngine engine;
        engine.addImageProvider(QStringLiteral("lightning-media"),
                                new MediaImageProvider(controller.mediaBridge()));
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 900; height: 700; visible: true
    ImageViewerOverlay { id: viewer }
    function openFor(key) { viewer.openFor(key, "") }
    function findNamed(item, name) {
        if (!item) return null
        if (item.objectName === name) return item
        for (var i = 0; i < item.children.length; ++i) {
            var hit = findNamed(item.children[i], name)
            if (hit) return hit
        }
        return null
    }
    function named(name) {
        var hit = findNamed(viewer.contentItem, name)
        return hit ? hit : findNamed(viewer.background, name)
    }
    function staticSource() {
        var i = named("viewerStaticImage"); return i ? i.source.toString() : ""
    }
    function staticReady() {
        var i = named("viewerStaticImage"); return i ? i.status === Image.Ready : false
    }
    function subtitle() { var l = named("viewerSubtitle"); return l ? l.text : "" }
    function browserOffered() {
        var b = named("viewerOpenInBrowserButton"); return b ? b.visible : false
    }
    function linkUrl() { return viewer.currentLinkUrl }
}
)QML",
                          QUrl(QStringLiteral("linkviewerscene.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto call = [&root](const char *name) {
            QVariant out;
            QMetaObject::invokeMethod(root.get(), name, Q_RETURN_ARG(QVariant, out));
            return out;
        };
        QMetaObject::invokeMethod(root.get(), "openFor", Q_ARG(QVariant, key));
        QCOMPARE(call("linkUrl").toString(), kImageUrl);
        QTRY_VERIFY(call("staticReady").toBool());
        QVERIFY2(call("staticSource").toString()
                     .startsWith(QStringLiteral("image://lightning-media/")),
                 qPrintable(call("staticSource").toString()));
        QCOMPARE(call("subtitle").toString(), QStringLiteral("img.example.org"));
        QVERIFY(call("browserOffered").toBool());
        QCOMPARE(fake.requestedUrls.size(), 1);
        previews->setClient(nullptr);
    }
};

QTEST_MAIN(LinkMediaQmlTest)
#include "LinkMediaQmlTest.moc"
