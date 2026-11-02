#include <QtTest/QtTest>

#include <QTemporaryDir>
#include <QImage>
#include <QBuffer>
#include <QRegularExpression>
#include "media/MediaImageProvider.h"

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "models/MessageComposer.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaPlaybackController.h"
#include "media/MediaVisibilityStore.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
class ReplyMediaClient final : public MockMatrixClient
{
public:
    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &, int, int) override { return ++lastOp; }
    quint64 lastOp = 0;
};
}

class MediaPlaceholderQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QStringList warnings;
    };

    static QVariantMap baseFixture(AppController &controller)
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
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), QString{});
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("errorKind"), QString{});
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("isVideo"), false);
        fixture.insert(QStringLiteral("isAudio"), false);
        fixture.insert(QStringLiteral("isSticker"), false);
        fixture.insert(QStringLiteral("mediaIsVoice"), false);
        fixture.insert(QStringLiteral("mediaDurationMs"), 0);
        fixture.insert(QStringLiteral("mediaWidth"), 0);
        fixture.insert(QStringLiteral("mediaHeight"), 0);
        fixture.insert(QStringLiteral("mediaSize"), 0);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), false);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), false);
        fixture.insert(QStringLiteral("mediaKey"), QString{});
        fixture.insert(QStringLiteral("mediaFilename"), QString{});
        fixture.insert(QStringLiteral("mediaUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaMimetype"), QString{});
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("isThreadRoot"), false);
        fixture.insert(QStringLiteral("mentionsMe"), false);
        fixture.insert(QStringLiteral("mentionsRoom"), false);
        fixture.insert(QStringLiteral("isLocalEcho"), false);
        return fixture;
    }

    static void collectItems(QQuickItem *item, const QString &name,
                             QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name)
            out.append(item);
        const auto children = item->childItems();
        for (QQuickItem *child : children)
            collectItems(child, name, out);
    }

    static QVariantMap galleryItem(const QString &key, const QString &kind,
                                   const QString &filename)
    {
        QVariantMap item;
        item.insert(QStringLiteral("mediaKey"), key);
        item.insert(QStringLiteral("kind"), kind);
        item.insert(QStringLiteral("filename"), filename);
        item.insert(QStringLiteral("mimetype"),
                    kind == QLatin1String("image") ? QStringLiteral("image/png")
                                                   : QStringLiteral("application/pdf"));
        item.insert(QStringLiteral("size"), 1024);
        item.insert(QStringLiteral("width"), 1280);
        item.insert(QStringLiteral("height"), 720);
        item.insert(QStringLiteral("durationMs"), 0);
        item.insert(QStringLiteral("thumbAvailable"), false);
        return item;
    }

    bool createDelegate(AppController &controller, const QVariantMap &fixture,
                        Delegate &out, const QVariantMap &initial = {})
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->addImageProvider(QStringLiteral("lightning-media"),
                                    new MediaImageProvider(controller.mediaBridge()));
        if (!initial.isEmpty())
            out.engine->setInitialProperties(initial);
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
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
        out.window->resize(700, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(640);
        out.window->show();
        QCoreApplication::processEvents();
        return true;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("media-placeholder-qml-test"));
    }

    void imageWithKnownDimensionsReservesAspectBox()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 800);
        fixture.insert(QStringLiteral("mediaHeight"), 600);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("photo.jpg"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("photo.jpg"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *skeleton = d.root->findChild<QQuickItem *>(
            QStringLiteral("imageSkeleton"));
        QVERIFY(skeleton != nullptr);
        QVERIFY(skeleton->isVisible());
        QVERIFY(qAbs(skeleton->width() - 360.0) < 1.0);
        QVERIFY(qAbs(skeleton->height() - 270.0) < 1.0);
        QCOMPARE(d.warnings, QStringList{});
    }

    void imageReleasesItsSourceOutsideTheRetentionBandWithoutChangingGeometry()
    {
        AppController controller(AppController::MockBackend);
        QTemporaryDir pictures;
        QVERIFY(pictures.isValid());
        const QString path = pictures.filePath(QStringLiteral("picture.png"));
        QImage pixels(4, 4, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::red);
        QVERIFY(pixels.save(path));
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 320);
        fixture.insert(QStringLiteral("mediaHeight"), 200);
        fixture.insert(QStringLiteral("mediaUrl"), QUrl::fromLocalFile(path));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *image = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageStillImage"));
        QVERIFY(image != nullptr);
        QTRY_VERIFY(!image->property("source").toUrl().isEmpty());
        const QUrl source = image->property("source").toUrl();
        const qreal height = d.root->height();
        QVERIFY(d.root->setProperty("mediaPixmapRetained", false));
        QTRY_VERIFY(image->property("source").toUrl().isEmpty());
        QCOMPARE(d.root->height(), height);
        QVERIFY(d.root->setProperty("mediaPixmapRetained", true));
        QTRY_COMPARE(image->property("source").toUrl(), source);
        QCOMPARE(d.root->height(), height);
    }

    void aGalleryRendersEveryAttachment()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("before.png"));
        fixture.insert(QStringLiteral("body"), QString{});
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("galleryItems"), QVariantList{
            galleryItem(QStringLiteral("$fixture"), QStringLiteral("image"),
                        QStringLiteral("before.png")),
            galleryItem(QStringLiteral("$fixture#item1"), QStringLiteral("image"),
                        QStringLiteral("after.png")),
            galleryItem(QStringLiteral("$fixture#item2"), QStringLiteral("file"),
                        QStringLiteral("notes.pdf")),
        });

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        QTRY_VERIFY(d.root->findChild<QQuickItem *>(
                        QStringLiteral("messageGallery")) != nullptr);
        QVERIFY(d.root->findChild<QQuickItem *>(QStringLiteral("imageMedia"))
                == nullptr);
        QList<QQuickItem *> tiles;
        collectItems(d.root, QStringLiteral("messageGalleryTile"), tiles);
        QCOMPARE(tiles.size(), 2);
        QVERIFY(tiles.at(0)->width() >= 48.0);
        QCOMPARE(tiles.at(0)->width(), tiles.at(0)->height());
        QCOMPARE(tiles.at(0)->width(), tiles.at(1)->width());
        QCOMPARE(tiles.at(0)->y(), tiles.at(1)->y());
        QVERIFY(tiles.at(1)->x() > tiles.at(0)->x());
        QList<QQuickItem *> files;
        collectItems(d.root, QStringLiteral("messageGalleryFile"), files);
        QCOMPARE(files.size(), 1);
        QVERIFY(files.at(0)->height() > 0.0);
        auto *body = d.root->findChild<QQuickItem *>(QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        QVERIFY(!body->isVisible());
        QCOMPARE(d.warnings, QStringList{});
    }

    void aGalleryTileFetchesNothingOutsideTheBandOrWhileHidden()
    {
        AppController controller(AppController::MockBackend);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setSupportsMediaBridgeForTest(true);
        QVERIFY(controller.mediaBridge()->supported());
        const auto misses = [&controller] {
            return controller.mediaBridge()->healthSnapshot()
                .value(QStringLiteral("cacheMisses")).toLongLong();
        };

        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("before.png"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("galleryItems"), QVariantList{
            galleryItem(QStringLiteral("$fixture"), QStringLiteral("image"),
                        QStringLiteral("before.png")),
            galleryItem(QStringLiteral("$fixture#item1"), QStringLiteral("image"),
                        QStringLiteral("after.png")),
        });

        const qint64 beforeBand = misses();
        Delegate out;
        QVERIFY(createDelegate(controller, fixture, out,
                               { { QStringLiteral("mediaInBand"), false } }));
        QTRY_VERIFY(out.root->findChild<QQuickItem *>(
                        QStringLiteral("messageGallery")) != nullptr);
        QCoreApplication::processEvents();
        QCOMPARE(misses() - beforeBand, qint64(0));
        out.root->setProperty("mediaInBand", true);
        QTRY_COMPARE(misses() - beforeBand, qint64(2));

        controller.mediaVisibility()->hide(QStringLiteral("$fixture"));
        const qint64 beforeHidden = misses();
        QVariantMap other = fixture;
        other.insert(QStringLiteral("eventId"), QStringLiteral("$fixture2"));
        QVariantList items = fixture.value(QStringLiteral("galleryItems")).toList();
        for (int i = 0; i < items.size(); ++i) {
            QVariantMap item = items.at(i).toMap();
            item.insert(QStringLiteral("mediaKey"),
                        item.value(QStringLiteral("mediaKey")).toString() + QStringLiteral("-h"));
            items[i] = item;
        }
        other.insert(QStringLiteral("galleryItems"), items);
        Delegate hidden;
        QVERIFY(createDelegate(controller, other, hidden));
        QTRY_VERIFY(hidden.root->property("mediaHidden").toBool());
        QCoreApplication::processEvents();
        QCOMPARE(misses() - beforeHidden, qint64(0));
        controller.mediaVisibility()->show(QStringLiteral("$fixture"));
        QTRY_COMPARE(misses() - beforeHidden, qint64(2));
        QCOMPARE(out.warnings, QStringList{});
        QCOMPARE(hidden.warnings, QStringList{});
    }

    void aGalleryRowOffersNoSingleAttachmentActions()
    {
        AppController controller(AppController::MockBackend);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setSupportsMediaBridgeForTest(true);
        QVERIFY(controller.mediaBridge()->supported());
        QVariantMap single = baseFixture(controller);
        single.insert(QStringLiteral("isImage"), true);
        single.insert(QStringLiteral("mediaFilename"), QStringLiteral("before.png"));
        single.insert(QStringLiteral("mediaSourceAvailable"), true);
        single.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));
        QVariantMap gallery = single;
        gallery.insert(QStringLiteral("galleryItems"), QVariantList{
            galleryItem(QStringLiteral("$fixture"), QStringLiteral("image"),
                        QStringLiteral("before.png")),
            galleryItem(QStringLiteral("$fixture#item1"), QStringLiteral("image"),
                        QStringLiteral("after.png")),
        });
        const auto menuItemVisible = [&](const QVariantMap &fixture,
                                         const QString &name) {
            Delegate d;
            if (!createDelegate(controller, fixture, d))
                return QStringLiteral("no delegate");
            QMetaObject::invokeMethod(d.root, "openContextMenu",
                                      Q_ARG(QVariant, 10), Q_ARG(QVariant, 10),
                                      Q_ARG(QVariant, false));
            auto *menu = d.root->findChild<QObject *>(
                QStringLiteral("messageContextMenu"));
            if (!menu)
                return QStringLiteral("no menu");
            for (int waited = 0; !menu->property("opened").toBool()
                                 && waited < kSignalTimeoutMs; waited += 20)
                QTest::qWait(20);
            if (!menu->property("opened").toBool())
                return QStringLiteral("menu never opened");
            auto *item = menu->findChild<QObject *>(name);
            if (!item)
                return QStringLiteral("no item");
            return item->property("visible").toBool() ? QStringLiteral("visible")
                                                      : QStringLiteral("hidden");
        };
        for (const QString &name : { QStringLiteral("forwardMessageMenuItem"),
                                     QStringLiteral("saveMediaMenuItem"),
                                     QStringLiteral("copyImageMenuItem") }) {
            QCOMPARE(menuItemVisible(single, name), QStringLiteral("visible"));
            QCOMPARE(menuItemVisible(gallery, name), QStringLiteral("hidden"));
        }

        const auto selectable = [&](const QVariantMap &fixture) {
            Delegate d;
            if (!createDelegate(controller, fixture, d))
                return QStringLiteral("no delegate");
            return d.root->property("rowSelectable").toBool()
                ? QStringLiteral("selectable") : QStringLiteral("not selectable");
        };
        QCOMPARE(selectable(single), QStringLiteral("selectable"));
        QCOMPARE(selectable(gallery), QStringLiteral("not selectable"));
    }

    void replyThumbnailReleasesAndReloadsWithoutSourceBindingLoops_data()
    {
        QTest::addColumn<bool>("decodeError");
        QTest::newRow("ready") << false;
        QTest::newRow("decode-error") << true;
    }

    void replyThumbnailReleasesAndReloadsWithoutSourceBindingLoops()
    {
        QFETCH(bool, decodeError);
        QTest::failOnWarning(QRegularExpression(QStringLiteral("QQuickPixmap:.*")));
        ReplyMediaClient client;
        AppController controller(AppController::MockBackend);
        controller.mediaBridge()->setClient(&client);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("body"), QStringLiteral("reply"));
        fixture.insert(QStringLiteral("replyToEventId"), QStringLiteral("$target"));
        fixture.insert(QStringLiteral("replyToMediaKey"), QStringLiteral("$target"));
        fixture.insert(QStringLiteral("replyToSender"), QStringLiteral("Sender"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *image = d.root->findChild<QQuickItem *>(QStringLiteral("replyQuoteThumbnail"));
        QVERIFY(image);
        QTRY_VERIFY(client.lastOp > 0);
        QImage pixels(8, 8, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::red);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(pixels.save(&buffer, "PNG"));
        if (decodeError)
            bytes = QByteArrayLiteral("invalid raster fixture");
        Q_EMIT client.mediaReady(client.lastOp, QStringLiteral("$target"), 1,
                                bytes, QStringLiteral("image/png"), QString{});
        const int expectedStatus = decodeError ? 3 : 1;
        QTRY_COMPARE(image->property("status").toInt(), expectedStatus);
        const QUrl source = image->property("source").toUrl();
        QVERIFY(!source.isEmpty());
        d.root->setVisible(false);
        QVERIFY(d.root->setProperty("mediaPixmapRetained", false));
        QTRY_VERIFY(image->property("source").toUrl().isEmpty());
        QVERIFY(d.root->setProperty("mediaPixmapRetained", true));
        d.root->setVisible(true);
        QTRY_COMPARE(image->property("source").toUrl(), source);
        QTRY_COMPARE(image->property("status").toInt(), expectedStatus);
        QVERIFY(d.root->setProperty("mediaPixmapRetained", false));
        QTRY_VERIFY(image->property("source").toUrl().isEmpty());
        for (const QString &warning : d.warnings)
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")), qPrintable(warning));
    }

    void aReplyQuoteLabelsAWordlessTargetByKind()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("body"), QStringLiteral("nice"));
        fixture.insert(QStringLiteral("replyToEventId"), QStringLiteral("$g"));
        fixture.insert(QStringLiteral("replyToSender"), QStringLiteral("Seikm"));
        fixture.insert(QStringLiteral("replyToPreview"), QString{});
        const auto quoteText = [&](const QString &kind, int count) {
            QVariantMap f = fixture;
            f.insert(QStringLiteral("replyToKind"), kind);
            f.insert(QStringLiteral("replyToCount"), count);
            Delegate d;
            if (!createDelegate(controller, f, d))
                return QStringLiteral("<no delegate>");
            auto *label = d.root->findChild<QQuickItem *>(
                QStringLiteral("replyQuoteBody"));
            if (!d.warnings.isEmpty())
                return QStringLiteral("<warnings> ") + d.warnings.join(QLatin1Char('|'));
            return label ? label->property("text").toString()
                         : QStringLiteral("<no label>");
        };
        const QString two = quoteText(QStringLiteral("image"), 2);
        QVERIFY2(two.startsWith(QStringLiteral("2 image")), qPrintable(two));
        const QString three = quoteText(QStringLiteral("file"), 3);
        QVERIFY2(three.startsWith(QStringLiteral("3 attachment")), qPrintable(three));
        QCOMPARE(quoteText(QStringLiteral("image"), 0), QStringLiteral("Image"));
        QCOMPARE(quoteText(QString{}, 0),
                 QStringLiteral("(original message not loaded)"));
    }

    void imageWithoutDimensionsReservesBoundedDefault()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("photo.jpg"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *skeleton = d.root->findChild<QQuickItem *>(
            QStringLiteral("imageSkeleton"));
        QVERIFY(skeleton != nullptr);
        QVERIFY(skeleton->isVisible());
        QVERIFY(skeleton->width() >= 240.0);
        QVERIFY(skeleton->height() >= 120.0);
        QVERIFY(skeleton->height() <= 320.0);
        QCOMPARE(d.warnings, QStringList{});
    }

    void videoReservesThumbnailGeometryWithDuration()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaDurationMs"), 83000);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        const qreal maxW = video->property("maxW").toReal();
        const qreal minControlW = video->property("minControlW").toReal();
        const qreal ratio = 720.0 / 1280.0;
        qreal expectedW = qMin(1280.0, maxW);
        if (expectedW * ratio > 400.0)
            expectedW = 400.0 / ratio;
        expectedW = qMax(minControlW, expectedW);
        QVERIFY(qAbs(video->implicitWidth() - expectedW) < 1.0);
        QVERIFY(qAbs(video->implicitHeight()
                     - qMin(400.0, expectedW * ratio)) < 1.0);
        QVERIFY(video->implicitWidth() >= 400.0);
        bool foundDuration = false;
        const auto labels = video->findChildren<QQuickItem *>();
        for (auto *child : labels) {
            if (child->property("text").toString() == QStringLiteral("1:23"))
                foundDuration = true;
        }
        QVERIFY(foundDuration);
        QCOMPARE(d.warnings, QStringList{});
    }

    void videoWithoutThumbnailShowsStyledPlaceholder()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        auto *placeholder = video->findChild<QQuickItem *>(
            QStringLiteral("videoNoThumbPlaceholder"));
        QVERIFY(placeholder != nullptr);
        QVERIFY(placeholder->isVisible());
        bool foundName = false;
        const auto children = placeholder->findChildren<QQuickItem *>();
        for (auto *child : children) {
            if (child->property("text").toString()
                == QStringLiteral("clip.mp4"))
                foundName = true;
        }
        QVERIFY(foundName);
        QCOMPARE(d.warnings, QStringList{});
    }

    void thePlaceholderNameDoesNotRunThroughThePlayButton()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaDurationMs"), 90000);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("F3-no-thumbnail-8MB.mp4"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("F3-no-thumbnail-8MB.mp4"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        auto *name = video->findChild<QQuickItem *>(
            QStringLiteral("videoPlaceholderName"));
        auto *glyph = video->findChild<QQuickItem *>(
            QStringLiteral("videoCoverGlyph"));
        QVERIFY(name != nullptr && name->isVisible());
        QVERIFY(glyph != nullptr);
        QQuickItem *circle = glyph->parentItem();
        const QRectF n(name->mapToItem(video, QPointF(0, 0)),
                       QSizeF(name->width(), name->height()));
        const QRectF c(circle->mapToItem(video, QPointF(0, 0)),
                       QSizeF(circle->width(), circle->height()));
        QVERIFY2(!n.intersects(c),
                 qPrintable(QStringLiteral("name %1,%2 %3x%4 overlaps the play "
                                           "circle %5,%6 %7x%8")
                     .arg(n.x()).arg(n.y()).arg(n.width()).arg(n.height())
                     .arg(c.x()).arg(c.y()).arg(c.width()).arg(c.height())));
        QVERIFY(n.bottom() <= video->height());
        QCOMPARE(d.warnings, QStringList{});
    }

    void portraitVideoControlsRemainReachable()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 720);
        fixture.insert(QStringLiteral("mediaHeight"), 1280);
        fixture.insert(QStringLiteral("mediaDurationMs"), 66000);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"),
                       QStringLiteral("fixture-video"));
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        QVERIFY(video->implicitWidth() >= 260.0 - 0.5);
        QVERIFY(video->implicitHeight() <= 440.0 + 0.5);

        QVERIFY(video->setProperty("playerActive", true));
        QCoreApplication::processEvents();
        auto *bar = video->findChild<QQuickItem *>(
            QStringLiteral("videoControlBar"));
        QVERIFY(bar != nullptr);
        auto *card = video->findChild<QQuickItem *>(
            QStringLiteral("videoPlayerCard"));
        QVERIFY(card != nullptr);
        QVERIFY(bar->property("compact").toBool());
        auto *seek = bar->findChild<QQuickItem *>(
            QStringLiteral("videoSeekSlider"));
        QVERIFY(seek != nullptr);
        QVERIFY2(seek->width() >= 120.0,
                 qPrintable(QStringLiteral("the seek track is %1 px")
                                .arg(seek->width())));
        const QStringList required = {
            QStringLiteral("videoPlayPauseButton"),
            QStringLiteral("videoSeekSlider"),
            QStringLiteral("videoTimeLabel"),
            QStringLiteral("videoExpandButton"),
            QStringLiteral("videoCardCloseButton"),
            QStringLiteral("videoMuteButton"),
        };
        for (const QString &name : required) {
            auto *control = card->findChild<QQuickItem *>(name);
            QVERIFY2(control, qPrintable(name));
            QVERIFY2(control->isVisible(), qPrintable(name));
            const QPointF topLeft =
                control->mapToItem(video, QPointF(0, 0));
            const QPointF bottomRight = control->mapToItem(
                video, QPointF(control->width(), control->height()));
            QVERIFY2(topLeft.x() >= -0.5, qPrintable(name));
            QVERIFY2(bottomRight.x() <= video->width() + 0.5,
                     qPrintable(name));
            QVERIFY2(bottomRight.y() <= video->height() + 0.5,
                     qPrintable(name));
        }
        auto *speed = bar->findChild<QQuickItem *>(
            QStringLiteral("videoSpeedButton"));
        QVERIFY(speed);
        QVERIFY2(!speed->isVisible(),
                 "speed is what yields to the overflow menu in a tight card");
        auto *barClose = bar->findChild<QQuickItem *>(
            QStringLiteral("videoCloseButton"));
        QVERIFY(barClose);
        QVERIFY(!barClose->isVisible());

        auto *volume = bar->findChild<QQuickItem *>(
            QStringLiteral("videoMuteButton"));
        QVERIFY(volume != nullptr);
        auto *slider = volume->findChild<QQuickItem *>(
            QStringLiteral("videoVolumeSlider"));
        QVERIFY2(slider != nullptr,
                 "the tight card's volume control carries no level slider, "
                 "so volume can still only be toggled on or off");
    }

    // The player's bar shows the event's declared length until the player
    // reports its own, and "–:–" when neither is known; "0:00 / 0:00" read as
    // an empty clip. The mock bridge cannot fetch, so the player never loads
    // and the declared length is all the bar has.
    void theVideoBarShowsTheDeclaredLengthOrAnUnknownOne()
    {
        const QList<QPair<int, QString>> cases = {
            { 83000, QStringLiteral("0:00 / 1:23") },
            { 0, QStringLiteral("0:00 / \u2013:\u2013") },
        };
        for (const auto &c : cases) {
            AppController controller(AppController::MockBackend);
            QVariantMap fixture = baseFixture(controller);
            fixture.insert(QStringLiteral("isVideo"), true);
            fixture.insert(QStringLiteral("mediaWidth"), 1280);
            fixture.insert(QStringLiteral("mediaHeight"), 720);
            fixture.insert(QStringLiteral("mediaDurationMs"), c.first);
            fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
            fixture.insert(QStringLiteral("mediaKey"),
                           QStringLiteral("fixture-video"));
            fixture.insert(QStringLiteral("mediaFilename"),
                           QStringLiteral("clip.mp4"));
            fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

            Delegate d;
            QVERIFY(createDelegate(controller, fixture, d));
            auto *video = d.root->findChild<QQuickItem *>(
                QStringLiteral("videoMedia"));
            QVERIFY(video != nullptr);
            QVERIFY(video->setProperty("playerActive", true));
            QTRY_VERIFY(video->findChild<QQuickItem *>(
                            QStringLiteral("videoTimeLabel")) != nullptr);
            auto *label = video->findChild<QQuickItem *>(
                QStringLiteral("videoTimeLabel"));
            QTRY_COMPARE(label->property("text").toString(), c.second);
            QCOMPARE(d.warnings, QStringList{});
        }
    }

    void endedVideoReturnsToTheCoverInsteadOfABlackCard()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"),
                       QStringLiteral("fixture-video-ended"));
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);

        QVERIFY(video->setProperty("playerActive", true));
        QCoreApplication::processEvents();
        auto *card = video->findChild<QQuickItem *>(
            QStringLiteral("videoPlayerCard"));
        QVERIFY(card != nullptr);

        QVERIFY2(QMetaObject::invokeMethod(card, "handleEndOfMedia"),
                 "VideoPlayerCard has no handleEndOfMedia() to drive — "
                 "EndOfMedia is not handled at all");

        QTRY_VERIFY(!video->property("playerActive").toBool());
        QTRY_VERIFY(video->findChild<QQuickItem *>(
                        QStringLiteral("videoPlayerCard")) == nullptr);
    }

    void theVideoCoverOffersResumeAndReplay()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        const QString key = QStringLiteral("$video-resume");
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaDurationMs"), 180000);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), key);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        auto *chip = video->findChild<QQuickItem *>(
            QStringLiteral("videoResumeChip"));
        auto *glyph = video->findChild<QQuickItem *>(
            QStringLiteral("videoCoverGlyph"));
        QVERIFY2(chip && glyph, "the cover has no resume chip or glyph");
        QVERIFY(!chip->isVisible());
        QCOMPARE(glyph->property("name").toString(),
                 QStringLiteral("play_arrow"));

        MediaPlaybackController *playback = controller.playback();
        playback->rememberVideoPosition(key, 55400, 180000);
        QTRY_VERIFY(chip->isVisible());
        bool saysWhere = false;
        for (auto *child : chip->findChildren<QQuickItem *>()) {
            if (child->property("text").toString().contains(
                    QStringLiteral("0:55")))
                saysWhere = true;
        }
        QVERIFY2(saysWhere, "the resume chip does not name 0:55");

        playback->markVideoFinished(key);
        QTRY_VERIFY(!chip->isVisible());
        QCOMPARE(glyph->property("name").toString(),
                 QStringLiteral("refresh"));

        playback->forgetResumePositions();
        QTRY_COMPARE(glyph->property("name").toString(),
                     QStringLiteral("play_arrow"));
        QCOMPARE(d.warnings, QStringList{});
    }

    void theVideoCoverIsAKeyboardButtonThatSaysWhatPlayDoes()
    {
        AppController controller(AppController::MockBackend);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setSupportsMediaBridgeForTest(true);
        QVariantMap fixture = baseFixture(controller);
        const QString key = QStringLiteral("$video-keys");
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaDurationMs"), 180000);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), key);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        QVERIFY2(video->activeFocusOnTab(),
                 "the video cover is not a Tab stop");
        const auto name = [video] {
            return QQmlProperty(video, QStringLiteral("Accessible.name"),
                                qmlContext(video)).read().toString();
        };
        QCOMPARE(name(), QStringLiteral("Play clip.mp4"));
        MediaPlaybackController *playback = controller.playback();
        playback->rememberVideoPosition(key, 55400, 180000);
        QTRY_COMPARE(name(), QStringLiteral("Resume clip.mp4 from 0:55"));
        playback->markVideoFinished(key);
        QTRY_COMPARE(name(), QStringLiteral("Replay clip.mp4"));

        video->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(video->hasActiveFocus());
        QTest::keyClick(d.window.get(), Qt::Key_Space);
        QTRY_VERIFY2(video->property("playerActive").toBool(),
                     "Space on the focused cover did not start the video");
        QTRY_VERIFY(video->findChild<QQuickItem *>(
                        QStringLiteral("videoControlBar")) != nullptr);
        auto *bar = video->findChild<QQuickItem *>(
            QStringLiteral("videoControlBar"));
        QTRY_VERIFY2(bar->hasActiveFocus(),
                     "a player opened by Space left focus on the hidden "
                     "cover");

        auto *close = bar->findChild<QQuickItem *>(
            QStringLiteral("videoCloseButton"));
        QVERIFY(close != nullptr);
        QVERIFY(close->isVisible());
        close->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(close->hasActiveFocus());
        QTest::keyClick(d.window.get(), Qt::Key_Space);
        QTRY_VERIFY(!video->property("playerActive").toBool());
        QTRY_VERIFY2(video->hasActiveFocus(),
                     "closing the player from the keyboard lost focus");
    }

    void aVideoWhoseFetchCannotStartOffersRetry()
    {
        AppController controller(AppController::MockBackend);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        // Outside demo mode the mock answers every fetch with op id 0.
        mock->setSupportsMediaBridgeForTest(true);
        MediaBridge *bridge = controller.mediaBridge();
        QVERIFY(bridge->supported());
        const QString key = QStringLiteral("fixture-video");
        const QString fullKey = QStringLiteral("full:") + key;
        const auto dispatchFailures = [bridge] {
            return bridge->healthSnapshot()
                .value(QStringLiteral("failed")).toLongLong();
        };

        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaDurationMs"), 11000);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaSize"), 0);
        fixture.insert(QStringLiteral("mediaKey"), key);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        QCoreApplication::processEvents();
        QVERIFY2(bridge->failureCategory(fullKey).isEmpty(),
                 "the cover fetched the payload before Play");

        const auto card = [video] {
            return video->findChild<QQuickItem *>(
                QStringLiteral("videoPlayerCard"));
        };
        const auto checkFailedPanel = [&card]() -> bool {
            QQuickItem *c = card();
            if (!c)
                return false;
            auto *panel = c->findChild<QQuickItem *>(
                QStringLiteral("videoFailedPanel"));
            auto *retry = c->findChild<QQuickItem *>(
                QStringLiteral("videoRetryButton"));
            auto *spinner = c->findChild<QQuickItem *>(
                QStringLiteral("videoCardSpinner"));
            return panel && panel->isVisible() && retry && retry->isVisible()
                && spinner && !spinner->isVisible();
        };

        const qint64 before = dispatchFailures();
        QVERIFY(video->setProperty("playerActive", true));
        QTRY_VERIFY(card() != nullptr);
        QCOMPARE(dispatchFailures() - before, qint64(1));
        QTRY_COMPARE(card()->property("fetchState").toString(),
                     QStringLiteral("failed"));
        QVERIFY(checkFailedPanel());

        QVERIFY(video->setProperty("playerActive", false));
        QTRY_VERIFY(card() == nullptr);
        QVERIFY(!bridge->failureCategory(fullKey).isEmpty());
        const qint64 blocked = dispatchFailures();
        QVERIFY(video->setProperty("playerActive", true));
        QTRY_VERIFY(card() != nullptr);
        QTRY_COMPARE(card()->property("fetchState").toString(),
                     QStringLiteral("failed"));
        QCOMPARE(dispatchFailures() - blocked, qint64(0));
        QVERIFY(checkFailedPanel());

        // 3. Retry dispatches again (and fails again on the mock).
        auto *retry = card()->findChild<QQuickItem *>(
            QStringLiteral("videoRetryButton"));
        QVERIFY(retry != nullptr);
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QCOMPARE(dispatchFailures() - blocked, qint64(1));
        QTRY_COMPARE(card()->property("fetchState").toString(),
                     QStringLiteral("failed"));
        QCOMPARE(d.warnings, QStringList{});
    }

    void aFailedThumbnailDoesNotBlockPlay()
    {
        AppController controller(AppController::MockBackend);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setSupportsMediaBridgeForTest(true);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 1280);
        fixture.insert(QStringLiteral("mediaHeight"), 720);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), true);
        fixture.insert(QStringLiteral("mediaSize"), 0);
        fixture.insert(QStringLiteral("mediaKey"),
                       QStringLiteral("fixture-video-nothumb"));
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = d.root->findChild<QQuickItem *>(
            QStringLiteral("videoMedia"));
        QVERIFY(video != nullptr);
        QCoreApplication::processEvents();
        QVERIFY(video->setProperty("bridgeFailed", true));
        QVERIFY(QMetaObject::invokeMethod(video, "activate"));
        QVERIFY2(video->property("playerActive").toBool(),
                 "the failed thumbnail intercepted Play");
        QTRY_VERIFY(video->findChild<QQuickItem *>(
                        QStringLiteral("videoPlayerCard")) != nullptr);
        auto *retry = video->findChild<QQuickItem *>(
            QStringLiteral("videoThumbRetryButton"));
        QVERIFY(retry != nullptr);
        QVERIFY2(!retry->isVisible(),
                 "the poster retry stays over a playing video");

        // Back on the cover, the retry is the poster's own action: it asks
        // for the poster again (the mock refuses at once, so it fails
        // again) without opening the player.
        QVERIFY(video->setProperty("playerActive", false));
        QVERIFY2(retry->isVisible(),
                 "a failed poster offers no way to retry it");
        QSignalSpy refetched(controller.mediaBridge(),
                             &MediaBridge::mediaFetchFailed);
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QTRY_VERIFY(!refetched.isEmpty());
        QCOMPARE(refetched.first().at(0).toString(),
                 QStringLiteral("thumb:fixture-video-nothumb"));
        QVERIFY(!video->property("playerActive").toBool());
        QCOMPARE(d.warnings, QStringList{});
    }

    void audioAndVoiceRowsStayCompact()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isAudio"), true);
        fixture.insert(QStringLiteral("mediaDurationMs"), 4000);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("song.ogg"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("song.ogg"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *audio = d.root->findChild<QQuickItem *>(
            QStringLiteral("audioMedia"));
        QVERIFY(audio != nullptr);
        QVERIFY(audio->implicitHeight() >= 44.0);
        QVERIFY(audio->implicitHeight() < 90.0);
        QCOMPARE(d.warnings, QStringList{});

        QVariantMap voice = fixture;
        voice.insert(QStringLiteral("mediaIsVoice"), true);
        Delegate v;
        QVERIFY(createDelegate(controller, voice, v));
        auto *voiceRow = v.root->findChild<QQuickItem *>(
            QStringLiteral("audioMedia"));
        QVERIFY(voiceRow != nullptr);
        bool voiceLabel = false;
        for (auto *child : voiceRow->findChildren<QQuickItem *>()) {
            if (child->property("text").toString()
                == QStringLiteral("Voice message"))
                voiceLabel = true;
        }
        QVERIFY(voiceLabel);
        QCOMPARE(v.warnings, QStringList{});
    }

    void stickerReservesBoundedTransparentBox()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isSticker"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 512);
        fixture.insert(QStringLiteral("mediaHeight"), 512);
        fixture.insert(QStringLiteral("body"), QStringLiteral("party"));
        fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("party"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *sticker = d.root->findChild<QQuickItem *>(
            QStringLiteral("stickerMedia"));
        QVERIFY(sticker != nullptr);
        QVERIFY(qAbs(sticker->implicitWidth() - 180.0) < 1.0);
        QVERIFY(qAbs(sticker->implicitHeight() - 180.0) < 1.0);
        QVERIFY(!sticker->metaObject()->className()
                     || sticker->property("color").isValid() == false);
        QCOMPARE(d.warnings, QStringList{});
    }

    void anUnlabelledStickerStillReachesTheAnimatedPath()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isSticker"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 256);
        fixture.insert(QStringLiteral("mediaHeight"), 256);
        fixture.insert(QStringLiteral("body"), QStringLiteral("wave"));
        fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("wave"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$sticker"));
        fixture.insert(QStringLiteral("mediaMimetype"), QString{});

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *sticker = d.root->findChild<QQuickItem *>(
            QStringLiteral("stickerMedia"));
        QVERIFY(sticker != nullptr);
        auto *animated = d.root->findChild<QQuickItem *>(
            QStringLiteral("stickerAnimatedMedia"));
        QVERIFY2(animated != nullptr,
                 "a sticker with no AnimatedImage can never animate");
        QVERIFY(sticker->property("maybeAnimated").isValid());
        QCOMPARE(sticker->property("maybeAnimated").toBool(), true);
        QCOMPARE(animated->property("animating").toBool(), false);
        QCOMPARE(d.warnings, QStringList{});
    }

    void aStickerThatDeclaresAStillFormatDoesNotAskForAnAnimation()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isSticker"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 256);
        fixture.insert(QStringLiteral("mediaHeight"), 256);
        fixture.insert(QStringLiteral("body"), QStringLiteral("still"));
        fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("still"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$still"));
        fixture.insert(QStringLiteral("mediaMimetype"),
                       QStringLiteral("image/png"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *sticker = d.root->findChild<QQuickItem *>(
            QStringLiteral("stickerMedia"));
        QVERIFY(sticker != nullptr);
        QVERIFY2(sticker->property("maybeAnimated").isValid(),
                 "the sticker delegate has no animation policy to test");
        QCOMPARE(sticker->property("maybeAnimated").toBool(), false);
        QCOMPARE(d.warnings, QStringList{});
    }

    void anSvgWithoutASenderThumbnailIsAFileCard()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaMimetype"),
                       QStringLiteral("image/svg+xml"));
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("logo.svg"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("logo.svg"));
        fixture.insert(QStringLiteral("mediaSize"), 2048);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        QVERIFY(d.root->findChild<QQuickItem *>(QStringLiteral("fileCard")));
        QVERIFY(!d.root->findChild<QQuickItem *>(QStringLiteral("imageMedia")));
        QCOMPARE(d.warnings, QStringList{});
    }

    void anSvgWithASenderThumbnailShowsItAndActivatingSaves()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaMimetype"),
                       QStringLiteral("image/svg+xml"));
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("logo.svg"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("logo.svg"));
        fixture.insert(QStringLiteral("mediaWidth"), 800);
        fixture.insert(QStringLiteral("mediaHeight"), 600);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("mediaUrl"),
                       QUrl(QStringLiteral("https://example.org/logo.svg")));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        QVERIFY(!d.root->findChild<QQuickItem *>(QStringLiteral("fileCard")));
        auto *image =
            d.root->findChild<QQuickItem *>(QStringLiteral("imageMedia"));
        QVERIFY(image != nullptr);
        // The mock backend has no media bridge, so this is the HTTP path.
        QVERIFY(image->property("resolvedSource").toString().isEmpty());

        QQmlComponent component(d.engine.get());
        component.setData(R"QML(
import QtQuick
QtObject {
    property bool roomEncrypted: false
    property bool isDirectRoom: false
    property real contentY: 0
    property real height: 10000
    property bool speculativeMediaAllowed: true
    property bool stickToBottom: false
    property bool threadContext: false
    property string transientInteractionOwner: ""
    property string hoveredActionsKey: ""
    property string pinnedActionsKey: ""
    property string savedKey: ""
    property string savedName: ""
    property string openedKey: ""
    function stateGroupExpanded(groupId) { return false }
    function toggleStateGroup(groupId) {}
    function saveMedia(mediaKey, filename) {
        savedKey = mediaKey
        savedName = filename
    }
    function openImage(mediaKey, httpUrl) { openedKey = mediaKey }
}
)QML",
                          QUrl());
        QObject *pane = component.create();
        QVERIFY2(pane, qPrintable(component.errorString()));
        pane->setParent(d.root);
        d.root->setProperty("timelineView", QVariant::fromValue(pane));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(image, "activate"));
        QCOMPARE(pane->property("savedKey").toString(),
                 QStringLiteral("$fixture"));
        QCOMPARE(pane->property("savedName").toString(),
                 QStringLiteral("logo.svg"));
        QVERIFY(pane->property("openedKey").toString().isEmpty());
    }

    void fileWithBodyEqualToFilenameSuppressesDuplicate()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isFile"), true);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("report.zip"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("report.zip"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        QVERIFY(!body->isVisible());
        QCOMPARE(d.warnings, QStringList{});

        QVariantMap captioned = fixture;
        captioned.insert(QStringLiteral("body"),
                         QStringLiteral("Here is the Q3 report"));
        Delegate c;
        QVERIFY(createDelegate(controller, captioned, c));
        auto *captionBody = c.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(captionBody != nullptr);
        QVERIFY(captionBody->isVisible());
        QCOMPARE(c.warnings, QStringList{});
    }

    void anEmoteReadsAsAnActionInEveryLayout()
    {
        AppController controller(AppController::MockBackend);
        for (int layout = 0; layout <= 2; ++layout) {
            controller.settings()->setMessageLayout(layout);
            QVariantMap fixture = baseFixture(controller);
            fixture.insert(QStringLiteral("eventType"), 1);
            fixture.insert(QStringLiteral("senderDisplayName"),
                           QStringLiteral("Alice <b>"));
            fixture.insert(QStringLiteral("body"),
                           QStringLiteral("waves hello"));
            fixture.insert(QStringLiteral("showSenderIdentity"), false);
            Delegate d;
            QVERIFY(createDelegate(controller, fixture, d));
            auto *body = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(body != nullptr);
            QVERIFY(body->isVisible());
            const QString text = body->property("text").toString();
            QVERIFY2(text.contains(QStringLiteral("* Alice &lt;b&gt; ")),
                     qPrintable(QStringLiteral("layout %1: %2")
                                    .arg(layout).arg(text)));
            QVERIFY(text.contains(QStringLiteral("waves hello")));
            QVERIFY(body->property("font").value<QFont>().italic());

            QVariantMap plain = fixture;
            plain.insert(QStringLiteral("eventType"), 0);
            Delegate t;
            QVERIFY(createDelegate(controller, plain, t));
            auto *plainBody = t.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(plainBody != nullptr);
            QVERIFY(!plainBody->property("text").toString()
                         .contains(QStringLiteral("* Alice")));
            QVERIFY(!plainBody->property("font").value<QFont>().italic());
        }
        controller.settings()->setMessageLayout(0);
    }

    void anImageCaptionShowsUnderPlainAndEncryptedMedia()
    {
        AppController controller(AppController::MockBackend);
        for (const bool encrypted : {false, true}) {
            QVariantMap fixture = baseFixture(controller);
            fixture.insert(QStringLiteral("isImage"), true);
            fixture.insert(QStringLiteral("isEncrypted"), encrypted);
            fixture.insert(QStringLiteral("isDecrypted"), encrypted);
            fixture.insert(QStringLiteral("mediaWidth"), 800);
            fixture.insert(QStringLiteral("mediaHeight"), 600);
            fixture.insert(QStringLiteral("mediaFilename"),
                           QStringLiteral("IMG_2041.jpg"));
            fixture.insert(QStringLiteral("body"),
                           QStringLiteral("Sunset over the harbour"));
            fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
            fixture.insert(QStringLiteral("mediaKey"),
                           QStringLiteral("$fixture"));
            Delegate d;
            QVERIFY(createDelegate(controller, fixture, d));
            auto *body = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(body != nullptr);
            QVERIFY2(body->isVisible(),
                     encrypted ? "encrypted caption hidden"
                               : "plain caption hidden");
            QVERIFY(body->property("text").toString()
                        .contains(QStringLiteral("Sunset over the harbour")));
            auto *image = d.root->findChild<QQuickItem *>(
                QStringLiteral("imageMedia"));
            QVERIFY(image != nullptr);
            QVERIFY2(body->mapToScene(QPointF(0, 0)).y()
                         >= image->mapToScene(QPointF(0, image->height())).y()
                             - 1.0,
                     "the caption must sit under the picture");

            QVariantMap echo = fixture;
            echo.insert(QStringLiteral("body"), QStringLiteral("IMG_2041.jpg"));
            Delegate e;
            QVERIFY(createDelegate(controller, echo, e));
            auto *echoBody = e.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(echoBody != nullptr);
            QVERIFY(!echoBody->isVisible());
        }
    }

    void doubleClickOnTheEmptyPartOfABubbleStartsAReply()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QString id;
        auto *tl = controller.timeline();
        for (int row = 0; row < tl->rowCount() && id.isEmpty(); ++row) {
            const QModelIndex idx = tl->index(row, 0);
            const QString candidate =
                tl->data(idx, TimelineModel::EventIdRole).toString();
            if (!candidate.isEmpty() && !candidate.startsWith(QLatin1String("local:"))
                && !tl->data(idx, TimelineModel::RedactedRole).toBool()
                && !tl->data(idx, TimelineModel::IsVirtualRole).toBool()
                && !tl->messagePermalink(candidate).isEmpty())
                id = candidate;
        }
        QVERIFY2(!id.isEmpty(), "the mock room has no repliable event");

        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("eventId"), id);
        fixture.insert(QStringLiteral("body"), QStringLiteral("hi there"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(bubble && body);
        QVERIFY(body->isVisible());
        const QPointF textPoint =
            body->mapToScene(QPointF(body->width() / 2, body->height() / 2));
        const QPointF emptyPoint = body->mapToScene(
            QPointF(body->width() + 120, body->height() / 2));
        QVERIFY2(bubble->mapFromScene(emptyPoint).x() < bubble->width(),
                 "the probe point must still be inside the bubble");

        QTest::mouseDClick(d.window.get(), Qt::LeftButton, {},
                           textPoint.toPoint());
        QTest::qWait(50);
        QCOMPARE(controller.composer()->replyingToEventId(), QString());

        QTest::mouseDClick(d.window.get(), Qt::LeftButton, {},
                           emptyPoint.toPoint());
        QTRY_COMPARE_WITH_TIMEOUT(controller.composer()->replyingToEventId(),
                                  id, kSignalTimeoutMs);
        controller.composer()->clearReplyTarget();

        QVariantMap gone = fixture;
        gone.insert(QStringLiteral("redacted"), true);
        Delegate g;
        QVERIFY(createDelegate(controller, gone, g));
        auto *goneBody = g.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(goneBody != nullptr);
        QTest::mouseDClick(g.window.get(), Qt::LeftButton, {},
                           goneBody->mapToScene(QPointF(goneBody->width() + 120,
                                                        goneBody->height() / 2))
                               .toPoint());
        QTest::qWait(50);
        QCOMPARE(controller.composer()->replyingToEventId(), QString());

        QVariantMap quoting = fixture;
        quoting.insert(QStringLiteral("replyToEventId"),
                       QStringLiteral("$other"));
        quoting.insert(QStringLiteral("replyToSender"), QStringLiteral("Bob"));
        quoting.insert(QStringLiteral("replyToPreview"),
                       QStringLiteral("earlier words"));
        Delegate q;
        QVERIFY(createDelegate(controller, quoting, q));
        auto *quote = q.root->findChild<QQuickItem *>(
            QStringLiteral("replyNavigationTarget"));
        QVERIFY(quote != nullptr);
        QVERIFY(quote->isVisible());
        QTest::mouseDClick(q.window.get(), Qt::LeftButton, {},
                           quote->mapToScene(QPointF(quote->width() - 6,
                                                     quote->height() / 2))
                               .toPoint());
        QTest::qWait(50);
        QCOMPARE(controller.composer()->replyingToEventId(), QString());

        Delegate b;
        QVERIFY(createDelegate(controller, fixture, b));
        auto *barLoader = b.root->findChild<QQuickItem *>(
            QStringLiteral("messageActionBarLoader"));
        auto *bubbleB = b.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(barLoader && bubbleB);
        QVERIFY(QQmlProperty::write(barLoader, QStringLiteral("active"), true));
        QVERIFY(QQmlProperty::write(barLoader, QStringLiteral("visible"), true));
        QTest::qWait(50);
        QVERIFY(barLoader->width() > 0 && barLoader->height() > 0);
        const QPointF barPoint = barLoader->mapToScene(
            QPointF(barLoader->width() / 2, barLoader->height() / 2));
        const QPointF inBubble = bubbleB->mapFromScene(barPoint);
        QVERIFY2(bubbleB->contains(inBubble),
                 "the action bar must overlap the bubble for this to mean anything");
        QTest::mouseDClick(b.window.get(), Qt::LeftButton, {},
                           barPoint.toPoint());
        QTest::qWait(50);
        QCOMPARE(controller.composer()->replyingToEventId(), QString());
    }

    void formattedBodyRendersInsteadOfRawMarkdown()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral(
                           "[@bob:example.org](https://matrix.to/#/@bob:example.org) hi"));
        fixture.insert(QStringLiteral("formattedBody"),
                       QStringLiteral(
                           "<a href=\"mention:@bob:example.org\">@bob</a> hi"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        const QString shown = body->property("text").toString();
        QVERIFY(shown.contains(QStringLiteral("@bob")));
        QVERIFY(shown.contains(QStringLiteral("mention:@bob:example.org")));
        QVERIFY(!shown.contains(QStringLiteral("](")));
        QCOMPARE(d.warnings, QStringList{});
    }

    void decryptingSkeletonOnlyForRecoverableFailures()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isEncrypted"), true);
        fixture.insert(QStringLiteral("undecryptable"), true);
        fixture.insert(QStringLiteral("errorKind"), QString{});
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("[unable to decrypt yet]"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *skeleton = d.root->findChild<QQuickItem *>(
            QStringLiteral("decryptingSkeleton"));
        QVERIFY(skeleton != nullptr);
        QVERIFY(skeleton->isVisible());
        QVERIFY(skeleton->implicitHeight() > 0.0);
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        QVERIFY(!body->isVisible());
        QCOMPARE(d.warnings, QStringList{});

        QVariantMap withheld = fixture;
        withheld.insert(QStringLiteral("errorKind"),
                        QStringLiteral("withheld"));
        Delegate w;
        QVERIFY(createDelegate(controller, withheld, w));
        auto *staticSkeleton = w.root->findChild<QQuickItem *>(
            QStringLiteral("decryptingSkeleton"));
        QVERIFY(staticSkeleton != nullptr);
        QVERIFY(!staticSkeleton->isVisible());
        auto *staticBody = w.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(staticBody != nullptr);
        QVERIFY(staticBody->isVisible());
        QCOMPARE(w.warnings, QStringList{});
    }

    void skeletonPrimitiveAnimatesOnlyWhenEligible()
    {
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("Skeleton"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *skeleton = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(skeleton != nullptr);
        QQuickWindow window;
        window.resize(300, 80);
        skeleton->setParentItem(window.contentItem());
        skeleton->setSize(QSizeF(220, 40));
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(skeleton->property("animating").toBool());

        skeleton->setProperty("active", false);
        QVERIFY(!skeleton->property("animating").toBool());
        skeleton->setProperty("active", true);
        QVERIFY(skeleton->property("animating").toBool());

        auto *theme = engine.singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY(theme != nullptr);
        theme->setProperty("reducedMotion", true);
        QVERIFY(!skeleton->property("animating").toBool());
        theme->setProperty("reducedMotion", false);
        QVERIFY(skeleton->property("animating").toBool());

        skeleton->setProperty("circle", true);
        QCOMPARE(skeleton->property("radius").toReal(), 20.0);
        QCOMPARE(warnings, QStringList{});
    }

    void hidingAnImageKeepsItsExactReservedGeometry()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 800);
        fixture.insert(QStringLiteral("mediaHeight"), 600);
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("photo.jpg"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("photo.jpg"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *skeleton =
            d.root->findChild<QQuickItem *>(QStringLiteral("imageSkeleton"));
        QVERIFY(skeleton != nullptr);
        const qreal boxWidth = skeleton->width();
        const qreal boxHeight = skeleton->height();
        QVERIFY(qAbs(boxWidth - 360.0) < 1.0);
        QVERIFY(qAbs(boxHeight - 270.0) < 1.0);
        const qreal rowHeight = d.root->height();
        QVERIFY(rowHeight > 0);
        QVERIFY(d.root->findChild<QQuickItem *>(
                    QStringLiteral("mediaHiddenPlaceholder")) == nullptr);

        controller.mediaVisibility()->hide(QStringLiteral("$fixture"));
        QCoreApplication::processEvents();
        d.root->polish();
        QCoreApplication::processEvents();

        QCOMPARE(d.root->property("mediaHidden").toBool(), true);
        auto *placeholder = d.root->findChild<QQuickItem *>(
            QStringLiteral("mediaHiddenPlaceholder"));
        QVERIFY2(placeholder != nullptr,
                 "hiding produced no placeholder, so the media box is empty");
        QVERIFY(placeholder->isVisible());
        QVERIFY2(qAbs(placeholder->width() - boxWidth) < 1.0,
                 qPrintable(QStringLiteral("placeholder is %1 wide, box was %2")
                                .arg(placeholder->width()).arg(boxWidth)));
        QVERIFY2(qAbs(placeholder->height() - boxHeight) < 1.0,
                 qPrintable(QStringLiteral("placeholder is %1 tall, box was %2")
                                .arg(placeholder->height()).arg(boxHeight)));
        QVERIFY2(qAbs(d.root->height() - rowHeight) < 1.0,
                 qPrintable(QStringLiteral("the row changed height from %1 to "
                                           "%2, so the timeline jumped")
                                .arg(rowHeight).arg(d.root->height())));
        QVERIFY(!skeleton->isVisible());
        auto *label = d.root->findChild<QQuickItem *>(
            QStringLiteral("mediaShowImageLabel"));
        QVERIFY(label != nullptr);
        QCOMPARE(label->property("text").toString(), QStringLiteral("Show image"));
        QCOMPARE(d.warnings, QStringList{});
    }

    void revealingAnImageRestoresTheNormalMediaPath()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 640);
        fixture.insert(QStringLiteral("mediaHeight"), 480);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        controller.mediaVisibility()->hide(QStringLiteral("$fixture"));
        QCoreApplication::processEvents();
        QCOMPARE(d.root->property("mediaHidden").toBool(), true);

        controller.mediaVisibility()->show(QStringLiteral("$fixture"));
        QCoreApplication::processEvents();
        d.root->polish();
        QCoreApplication::processEvents();
        QCOMPARE(d.root->property("mediaHidden").toBool(), false);
        auto *stale = d.root->findChild<QQuickItem *>(
            QStringLiteral("mediaHiddenPlaceholder"));
        QVERIFY2(stale == nullptr || !stale->isVisible(),
                 "the placeholder outlived the reveal and still covers the "
                 "image");
        auto *skeleton =
            d.root->findChild<QQuickItem *>(QStringLiteral("imageSkeleton"));
        QVERIFY(skeleton != nullptr);
        QVERIFY2(skeleton->isVisible(),
                 "revealing did not put the row back on the ordinary media "
                 "path");
        QCOMPARE(d.warnings, QStringList{});
    }

    void aRebuiltRowComesBackHidden()
    {
        AppController controller(AppController::MockBackend);
        controller.mediaVisibility()->hide(QStringLiteral("$fixture"));

        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaWidth"), 400);
        fixture.insert(QStringLiteral("mediaHeight"), 400);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));

        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        QVERIFY2(d.root->property("mediaHidden").toBool(),
                 "a freshly created row does not know the image was hidden, so "
                 "the state is lost to delegate recycling");
        QVERIFY(d.root->findChild<QQuickItem *>(
                    QStringLiteral("mediaHiddenPlaceholder")) != nullptr);

        QVariantMap other = baseFixture(controller);
        other.insert(QStringLiteral("isImage"), true);
        other.insert(QStringLiteral("mediaWidth"), 400);
        other.insert(QStringLiteral("mediaHeight"), 400);
        other.insert(QStringLiteral("mediaSourceAvailable"), true);
        other.insert(QStringLiteral("mediaKey"), QStringLiteral("$another"));
        other.insert(QStringLiteral("eventId"), QStringLiteral("$another"));
        Delegate e;
        QVERIFY(createDelegate(controller, other, e));
        QVERIFY(!e.root->property("mediaHidden").toBool());
    }

    void stickersAreHideableAndVideosAreNot()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap sticker = baseFixture(controller);
        sticker.insert(QStringLiteral("isSticker"), true);
        sticker.insert(QStringLiteral("mediaWidth"), 160);
        sticker.insert(QStringLiteral("mediaHeight"), 160);
        sticker.insert(QStringLiteral("mediaSourceAvailable"), true);
        sticker.insert(QStringLiteral("mediaKey"), QStringLiteral("$sticker"));
        Delegate s;
        QVERIFY(createDelegate(controller, sticker, s));
        QVERIFY(s.root->property("mediaHideable").toBool());
        const qreal before = s.root->height();
        controller.mediaVisibility()->hide(QStringLiteral("$sticker"));
        QCoreApplication::processEvents();
        QVERIFY(s.root->findChild<QQuickItem *>(
                    QStringLiteral("mediaHiddenPlaceholder")) != nullptr);
        QVERIFY2(qAbs(s.root->height() - before) < 1.0,
                 "hiding a sticker moved the timeline");

        QVariantMap video = baseFixture(controller);
        video.insert(QStringLiteral("isVideo"), true);
        video.insert(QStringLiteral("mediaWidth"), 1280);
        video.insert(QStringLiteral("mediaHeight"), 720);
        video.insert(QStringLiteral("mediaSourceAvailable"), true);
        video.insert(QStringLiteral("mediaKey"), QStringLiteral("$video"));
        Delegate v;
        QVERIFY(createDelegate(controller, video, v));
        QVERIFY2(!v.root->property("mediaHideable").toBool(),
                 "a video row offers a control that was deliberately scoped to "
                 "images and stickers");
    }

    void theHiddenSetIsBoundedAndEvictsTheOldest()
    {
        MediaVisibilityStore store;
        for (int i = 0; i < MediaVisibilityStore::kMaxHidden + 10; ++i)
            store.hide(QStringLiteral("$k%1").arg(i));
        QCOMPARE(store.hiddenCount(), MediaVisibilityStore::kMaxHidden);
        QVERIFY2(!store.isHidden(QStringLiteral("$k0")),
                 "the cap refused the newest hide instead of releasing the "
                 "oldest");
        QVERIFY(store.isHidden(
            QStringLiteral("$k%1").arg(MediaVisibilityStore::kMaxHidden + 9)));
        store.clear();
        QCOMPARE(store.hiddenCount(), 0);
        store.hide(QString());
        QCOMPARE(store.hiddenCount(), 0);
        QVERIFY(!store.isHidden(QString()));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MediaPlaceholderQmlTest)
#include "MediaPlaceholderQmlTest.moc"
