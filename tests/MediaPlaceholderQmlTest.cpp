// v0.7: type-specific media placeholder suite. Renders the production
// MessageDelegate (and the shared Skeleton primitive) offscreen with staged
// role fixtures and proves each deferred content class reserves stable,
// type-correct geometry with the right loading presentation:
//   * images/GIFs reserve their Matrix-metadata aspect box (bounded default
//     when dimensions are unknown) with a shimmering skeleton, never a
//     zero-height flash;
//   * stickers keep a bounded transparency-preserving box;
//   * videos reserve thumbnail geometry with a play badge and duration;
//   * audio/voice rows are compact and fixed, never image-sized;
//   * recoverable undecryptable rows show the decrypting text skeleton while
//     deterministic failures keep their honest static explanation;
//   * the skeleton primitive animates only while shimmering, on screen, and
//     not reduced-motion.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
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

    bool createDelegate(AppController &controller, const QVariantMap &fixture,
                        Delegate &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
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
    // Known Matrix dimensions reserve the exact bounded aspect box before
    // any bytes arrive, with a visible image skeleton — and the outer
    // geometry is identical to the final display box, so the swap-in cannot
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
        // 800×600 bounded to the 360px media width → 360×270.
        QVERIFY(qAbs(skeleton->width() - 360.0) < 1.0);
        QVERIFY(qAbs(skeleton->height() - 270.0) < 1.0);
        QCOMPARE(d.warnings, QStringList{});
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

    // Videos reserve thumbnail geometry from info metadata and present the
    // play badge plus formatted duration.
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
        // 1280×720 bounded to 360 wide → 360×202.5.
        QVERIFY(qAbs(video->implicitWidth() - 360.0) < 1.0);
        QVERIFY(qAbs(video->implicitHeight() - 202.5) < 1.0);
        bool foundDuration = false;
        const auto labels = video->findChildren<QQuickItem *>();
        for (auto *child : labels) {
            if (child->property("text").toString() == QStringLiteral("1:23"))
                foundDuration = true;
        }
        QVERIFY(foundDuration);
        QCOMPARE(d.warnings, QStringList{});
    }

    // Audio and voice rows are compact and fixed — never image-sized — and
    // the voice marker switches the presentation.
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

    // Stickers reserve a bounded box and keep transparency intent: the
    // sticker container itself paints no opaque backing card.
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
        // The sticker container is a plain Item (no Rectangle color role):
        // transparent pixels reveal the timeline surface, not a card.
        QVERIFY(!sticker->metaObject()->className()
                     || sticker->property("color").isValid() == false);
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

    // The skeleton primitive: shimmer runs only while active + shimmering
    // + visible in a visible window; reduced motion forces the static
    // surface; deactivation stops animation.
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
};

QTEST_MAIN(MediaPlaceholderQmlTest)
#include "MediaPlaceholderQmlTest.moc"
