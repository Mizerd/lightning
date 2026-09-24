// Type-specific media placeholders. Renders the production MessageDelegate
// (and the shared Skeleton) offscreen with staged role fixtures and checks
// that each deferred content class reserves stable, type-correct geometry:
//   * images/GIFs reserve their metadata aspect box (a bounded default when
//     dimensions are unknown) with a shimmering skeleton, never zero height;
//   * stickers keep a bounded transparency-preserving box;
//   * videos reserve thumbnail geometry with a play badge and duration;
//   * audio/voice rows are compact and fixed;
//   * recoverable undecryptable rows show the decrypting skeleton, while
//     deterministic failures keep their static explanation;
//   * the skeleton animates only while shimmering, on screen, and not in
//     reduced-motion mode.
#include <QtTest/QtTest>

#include <QTemporaryDir>


#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaVisibilityStore.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
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

    // A complete role map with safe defaults so the production delegate
    // binds without undefined-property warnings.
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
        // The real model roles always deliver QUrls (possibly empty).
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

    // Repeater delegates are not reachable through findChild; walk the visual
    // tree.
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
    // Isolate the settings store before anything touches it: a real
    // AppController uses a default QSettings resolved from the application
    // identity, which would be the developer's real configuration. Both
    // guards are needed: XDG_CONFIG_HOME set here (QStandardPaths caches its
    // first answer) and an identity that is not the app's.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("media-placeholder-qml-test"));
    }

    // Known Matrix dimensions reserve the exact bounded aspect box before any
    // bytes arrive, identical to the final display box so the swap-in cannot
    // reflow the row.
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
        // 800x600 bounded to the 360 px media width: 360x270.
        QVERIFY(qAbs(skeleton->width() - 360.0) < 1.0);
        QVERIFY(qAbs(skeleton->height() - 270.0) < 1.0);
        QCOMPARE(d.warnings, QStringList{});
    }

    // An MSC4274 gallery renders every attachment: two pictures as equal
    // square tiles side by side and a file as a chip below, with geometry
    // fixed before any byte arrives. The generated `[name: mxc]` body is not
    // shown as a caption.
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
        // The single-picture component for the primary item is not built.
        QVERIFY(d.root->findChild<QQuickItem *>(QStringLiteral("imageMedia"))
                == nullptr);
        QList<QQuickItem *> tiles;
        collectItems(d.root, QStringLiteral("messageGalleryTile"), tiles);
        QCOMPARE(tiles.size(), 2);
        QVERIFY(tiles.at(0)->width() >= 48.0);
        QCOMPARE(tiles.at(0)->width(), tiles.at(0)->height());
        QCOMPARE(tiles.at(0)->width(), tiles.at(1)->width());
        // Side by side, not stacked.
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

    // A gallery tile fetches nothing outside the media band or while hidden,
    // like a single picture; releasing each gate makes both tiles fetch,
    // proving the probe (bridge cache misses) can see a fetch.
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

        // Outside the band.
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

        // Hidden before the row is built, as a recycled row would be.
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

    // A gallery offers no Forward, Save as or Copy image: those carry the
    // row's single media key and would drop the other attachments. A single
    // picture keeps Forward, proving the menu is read at all.
    void aGalleryRowOffersNoSingleAttachmentActions()
    {
        AppController controller(AppController::MockBackend);
        // Save as and Copy image also need a working media bridge, or they
        // are hidden on every row.
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
            // QTRY_* would return from this lambda; wait by hand.
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

        // Nor can a gallery be selected for multi-message forwarding.
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

    // A reply quoting a wordless target labels it by kind (an image, "2
    // images"), keeping "not loaded" for targets of unknown kind.
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
        // Plural wording comes from the catalog ("%n image(s)" is literal with
        // none loaded), so assert the count and noun only.
        const QString two = quoteText(QStringLiteral("image"), 2);
        QVERIFY2(two.startsWith(QStringLiteral("2 image")), qPrintable(two));
        const QString three = quoteText(QStringLiteral("file"), 3);
        QVERIFY2(three.startsWith(QStringLiteral("3 attachment")), qPrintable(three));
        QCOMPARE(quoteText(QStringLiteral("image"), 0), QStringLiteral("Image"));
        QCOMPARE(quoteText(QString{}, 0),
                 QStringLiteral("(original message not loaded)"));
    }

    // Unknown dimensions still reserve a bounded non-zero default box.
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

    // Videos reserve thumbnail geometry from metadata and show the play badge
    // and duration.
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
        // Landscape sizing: ~72% of the content column (280..560), aspect
        // preserved, 400 px height cap; computed from the box's own published
        // bounds.
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
        // Larger than the old flat 360 cap on a 640 px row.
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

    // A video with no usable thumbnail shows a styled placeholder (surface
    // tone, type icon, filename) under the play affordance, never an empty
    // transparent box.
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

    // A portrait video never renders as a narrow strip, and every player
    // control stays reachable at the minimum card width.
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
        // The control-surface floor: at least 260 (or the column) wide,
        // height capped at 440 with letterboxing.
        QVERIFY(video->implicitWidth() >= 260.0 - 0.5);
        QVERIFY(video->implicitHeight() <= 440.0 + 0.5);

        // With the inline player active, every control of the adaptive bar
        // lies inside the card. At 260 px the bar is in tight mode.
        QVERIFY(video->setProperty("playerActive", true));
        QCoreApplication::processEvents();
        auto *bar = video->findChild<QQuickItem *>(
            QStringLiteral("videoControlBar"));
        QVERIFY(bar != nullptr);
        const QStringList required = {
            QStringLiteral("videoPlayPauseButton"),
            QStringLiteral("videoSeekSlider"),
            QStringLiteral("videoTimeLabel"),
            QStringLiteral("videoExpandButton"),
            QStringLiteral("videoCloseButton"),
            // The volume control stays visible at the narrowest card (its
            // level would otherwise only be reachable fullscreen), without
            // pushing the row outside the 260 px card.
            QStringLiteral("videoMuteButton"),
        };
        for (const QString &name : required) {
            auto *control = bar->findChild<QQuickItem *>(name);
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
        // Tight mode collapses only the speed control.
        auto *speed = bar->findChild<QQuickItem *>(
            QStringLiteral("videoSpeedButton"));
        QVERIFY(speed);
        QVERIFY2(!speed->isVisible(),
                 "speed is what yields to the overflow menu in a tight card");

        // The volume control reaches its slider, not just mute.
        auto *volume = bar->findChild<QQuickItem *>(
            QStringLiteral("videoMuteButton"));
        QVERIFY(volume != nullptr);
        auto *slider = volume->findChild<QQuickItem *>(
            QStringLiteral("videoVolumeSlider"));
        QVERIFY2(slider != nullptr,
                 "the tight card's volume control carries no level slider, "
                 "so volume can still only be toggled on or off");
    }

    // Audio and voice rows are compact and fixed, and the voice marker
    // switches the presentation.
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

    // Stickers reserve a bounded box and paint no opaque backing card.
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
        // The sticker container is a plain Item (no colour role): transparent
        // pixels show the timeline surface.
        QVERIFY(!sticker->metaObject()->className()
                     || sticker->property("color").isValid() == false);
        QCOMPARE(d.warnings, QStringList{});
    }

    // A sticker has an animated path, gated by the bytes rather than its
    // label: `info.mimetype` is optional for m.sticker (MSC2545), so an absent
    // mimetype means "unknown, ask" rather than "not a GIF".
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
        // What an MSC2545 sticker without info.mimetype delivers.
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
        // No bytes yet, so the still frame is drawn; the animated item has not
        // taken over on an empty source.
        QCOMPARE(animated->property("animating").toBool(), false);
        QCOMPARE(d.warnings, QStringList{});
    }

    // A sticker declaring a still format is taken at its word only to avoid
    // requesting a second full payload.
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
        // Check the property exists first: an absent property also reads
        // false.
        QVERIFY2(sticker->property("maybeAnimated").isValid(),
                 "the sticker delegate has no animation policy to test");
        QCOMPARE(sticker->property("maybeAnimated").toBool(), false);
        QCOMPARE(d.warnings, QStringList{});
    }

    // A plain file (body defaults to the filename) shows the filename once, in
    // the file card; a distinct caption is still shown.
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
        QVERIFY(!body->isVisible()); // duplicate filename suppressed
        QCOMPARE(d.warnings, QStringList{});

        // A distinct caption is preserved.
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

    // A sanitized formatted body renders as rich content, not the raw markdown
    // of the plain body.
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
        // TextEdit normalizes RichText, so assert on content: the mention and
        // display name are present, the raw markdown is not.
        const QString shown = body->property("text").toString();
        QVERIFY(shown.contains(QStringLiteral("@bob")));
        QVERIFY(shown.contains(QStringLiteral("mention:@bob:example.org")));
        QVERIFY(!shown.contains(QStringLiteral("](")));   // not the raw markdown
        QCOMPARE(d.warnings, QStringList{});
    }

    // Recoverable undecryptable rows show the decrypting skeleton (keys can
    // still arrive); deterministic failures keep the static explanation.
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

    // The skeleton shimmers only while active, shimmering and visible in a
    // visible window; reduced motion forces the static surface.
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

        // Reduced motion wins over everything.
        auto *theme = engine.singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY(theme != nullptr);
        theme->setProperty("reducedMotion", true);
        QVERIFY(!skeleton->property("animating").toBool());
        theme->setProperty("reducedMotion", false);
        QVERIFY(skeleton->property("animating").toBool());

        // Circle mode keeps the circular radius contract for avatars.
        skeleton->setProperty("circle", true);
        QCOMPARE(skeleton->property("radius").toReal(), 20.0);
        QCOMPARE(warnings, QStringList{});
    }

    // Local image hiding keeps the media box's exact reserved rectangle, so
    // nothing above or below moves.
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
        // 800x600 bounded to the 360 px media width: 360x270.
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
        // Same box, same row.
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
        // The skeleton stands down behind the opaque placeholder.
        QVERIFY(!skeleton->isVisible());
        // The placeholder says how to get the picture back.
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
        // Gone or not painted (a Loader destroys its item on its own
        // schedule): nothing covers the picture.
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

    // Hidden state is keyed by media identity in a store, not the delegate,
    // so a rebuilt row (recycling, room re-entry) comes back hidden.
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

        // A different image is unaffected.
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

    // Stickers are hideable (they draw a bitmap); videos are not (their card
    // has its own poster and controls).
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

    // The hidden set is bounded and evicts the oldest entry rather than
    // refusing a new hide.
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
        // An empty key is not an identity and must never be stored.
        store.hide(QString());
        QCOMPARE(store.hiddenCount(), 0);
        QVERIFY(!store.isHidden(QString()));
    }


private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MediaPlaceholderQmlTest)
#include "MediaPlaceholderQmlTest.moc"
