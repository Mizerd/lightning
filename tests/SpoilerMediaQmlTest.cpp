#include <QtTest/QtTest>

#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "media/MediaImageProvider.h"
#include "models/TimelineModel.h"

class SpoilerMediaQmlTest : public QObject
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
        const QVariantMap values{
            { QStringLiteral("isVirtual"), false },
            { QStringLiteral("isStateActivity"), false },
            { QStringLiteral("stateGroupEntries"), QVariantList{} },
            { QStringLiteral("showSenderIdentity"), true },
            { QStringLiteral("eventId"), QStringLiteral("$spoiler") },
            { QStringLiteral("itemId"), QStringLiteral("spoiler-item") },
            { QStringLiteral("sender"), QStringLiteral("@fixture:mock.local") },
            { QStringLiteral("senderDisplayName"), QStringLiteral("Fixture") },
            { QStringLiteral("senderInitials"), QStringLiteral("F") },
            { QStringLiteral("body"), QString{} },
            { QStringLiteral("eventType"), 0 },
            { QStringLiteral("status"), 0 },
            { QStringLiteral("isOwn"), false },
            { QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc() },
            { QStringLiteral("redacted"), false },
            { QStringLiteral("edited"), false },
            { QStringLiteral("isEncrypted"), false },
            { QStringLiteral("isDecrypted"), false },
            { QStringLiteral("undecryptable"), false },
            { QStringLiteral("errorKind"), QString{} },
            { QStringLiteral("isImage"), false },
            { QStringLiteral("isFile"), false },
            { QStringLiteral("isVideo"), false },
            { QStringLiteral("isAudio"), false },
            { QStringLiteral("isSticker"), false },
            { QStringLiteral("mediaIsVoice"), false },
            { QStringLiteral("mediaDurationMs"), 0 },
            { QStringLiteral("mediaWidth"), 320 },
            { QStringLiteral("mediaHeight"), 200 },
            { QStringLiteral("mediaSize"), 0 },
            { QStringLiteral("mediaSourceAvailable"), false },
            { QStringLiteral("mediaThumbAvailable"), false },
            { QStringLiteral("mediaKey"), QString{} },
            { QStringLiteral("mediaFilename"), QStringLiteral("picture.png") },
            { QStringLiteral("mediaUrl"), QUrl{} },
            { QStringLiteral("mediaThumbUrl"), QUrl{} },
            { QStringLiteral("mediaMimetype"), QStringLiteral("image/png") },
            { QStringLiteral("reactions"), QVariantList{} },
            { QStringLiteral("replyToEventId"), QString{} },
            { QStringLiteral("isThreadRoot"), false },
            { QStringLiteral("mentionsMe"), false },
            { QStringLiteral("mentionsRoom"), false },
            { QStringLiteral("isLocalEcho"), false },
            { QStringLiteral("mediaSpoiler"), false },
            { QStringLiteral("mediaSpoilerReason"), QString{} },
        };
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            fixture.insert(it.key(), it.value());
        return fixture;
    }

    QVariantMap imageFixture(AppController &controller, bool spoiler)
    {
        const QString path = m_pictures.filePath(QStringLiteral("picture.png"));
        if (!QFile::exists(path)) {
            QImage pixels(320, 200, QImage::Format_ARGB32_Premultiplied);
            pixels.fill(Qt::red);
            pixels.save(path);
        }
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("mediaUrl"), QUrl::fromLocalFile(path));
        fixture.insert(QStringLiteral("mediaSpoiler"), spoiler);
        return fixture;
    }

    bool createDelegate(AppController &controller, const QVariantMap &fixture,
                        Delegate &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->addImageProvider(QStringLiteral("lightning-media"),
                                    new MediaImageProvider(controller.mediaBridge()));
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(out.engine.get(), &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty() && !createdSpy.wait(3000))
            return false;
        out.root = qobject_cast<QQuickItem *>(createdSpy.at(0).at(0).value<QObject *>());
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

    static QQuickItem *find(Delegate &d, const char *name)
    {
        return d.root->findChild<QQuickItem *>(QLatin1String(name));
    }

    static QPoint centreOf(QQuickItem *item)
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_pictures.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("spoiler-media-qml-test"));
    }

    void aPlainImageHasNoCover()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller, false), d));
        auto *image = find(d, "messageStillImage");
        QVERIFY(image);
        QTRY_VERIFY(!image->property("source").toUrl().isEmpty());
        QVERIFY(!find(d, "spoilerCover"));
        QCOMPARE(d.warnings, QStringList{});
    }

    void aSpoilerImageIsCoveredAndOnlyATinyDecodeExists()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = imageFixture(controller, true);
        fixture.insert(QStringLiteral("mediaSpoilerReason"), QStringLiteral("finale"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *cover = find(d, "spoilerCover");
        QVERIFY(cover);
        QVERIFY(cover->isVisible());
        auto *image = find(d, "messageStillImage");
        QVERIFY(image);
        QVERIFY(image->property("source").toUrl().isEmpty());
        QVERIFY(!image->isVisible());
        auto *smear = find(d, "spoilerSmear");
        QVERIFY(smear);
        QTRY_COMPARE(smear->property("status").toInt(), 1);
        const QSize decoded = smear->property("sourceSize").toSize();
        QVERIFY2(decoded.width() <= 32 && decoded.height() <= 32,
                 qPrintable(QStringLiteral("%1x%2").arg(decoded.width()).arg(decoded.height())));
        QVERIFY(smear->property("implicitWidth").toReal() <= 32);
        auto *reason = find(d, "spoilerReason");
        QVERIFY(reason && reason->isVisible());
        QCOMPARE(d.warnings, QStringList{});
    }

    void strongerBlurDecodesFewerPixels()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setSpoilerBlurStrength(0);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller, true), d));
        auto *smear = find(d, "spoilerSmear");
        QVERIFY(smear);
        const int weak = smear->property("sourceSize").toSize().width();
        controller.settings()->setSpoilerBlurStrength(100);
        QTRY_VERIFY(smear->property("sourceSize").toSize().width() < weak);
        QCOMPARE(controller.settings()->spoilerBlurStrength(), 100);
        controller.settings()->setSpoilerBlurStrength(70);
    }

    void aClickRevealsInsteadOfOpeningTheViewer()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller, true), d));
        auto *cover = find(d, "spoilerCover");
        QVERIFY(cover);
        QTest::mouseClick(d.window.get(), Qt::LeftButton, {}, centreOf(cover));
        QTRY_VERIFY(!find(d, "spoilerCover"));
        auto *image = find(d, "messageStillImage");
        QTRY_VERIFY(!image->property("source").toUrl().isEmpty());
        QVERIFY(image->isVisible());
        QCOMPARE(d.root->property("spoilerHidden").toBool(), false);
    }

    void enterRevealsFromTheKeyboard()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller, true), d));
        auto *cover = find(d, "spoilerCover");
        QVERIFY(cover);
        QVERIFY(cover->property("activeFocusOnTab").toBool());
        cover->forceActiveFocus();
        QTRY_VERIFY(cover->hasActiveFocus());
        QTest::keyClick(d.window.get(), Qt::Key_Return);
        QTRY_VERIFY(!find(d, "spoilerCover"));
    }

    void aSpoilerVideoIsCoveredAndTheFirstActivationOnlyReveals()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isVideo"), true);
        fixture.insert(QStringLiteral("mediaMimetype"), QStringLiteral("video/mp4"));
        fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("clip.mp4"));
        fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), true);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$spoiler"));
        fixture.insert(QStringLiteral("mediaSpoiler"), true);
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        auto *video = find(d, "videoMedia");
        QVERIFY(video);
        auto *cover = find(d, "spoilerCover");
        QVERIFY(cover);
        QVERIFY(!cover->property("activeFocusOnTab").toBool());
        QVERIFY(QMetaObject::invokeMethod(video, "activate"));
        QTRY_VERIFY(!find(d, "spoilerCover"));
        QCOMPARE(video->property("playerActive").toBool(), false);
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_pictures;
};

QTEST_MAIN(SpoilerMediaQmlTest)
#include "SpoilerMediaQmlTest.moc"
