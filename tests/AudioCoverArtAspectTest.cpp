
#include <QtTest/QtTest>

#include <QSet>

#include <QElapsedTimer>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include <QSettings>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "media/MediaBridge.h"
#include "media/MediaImageProvider.h"

class AudioCoverArtAspectTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QUrl makeImage(const QString &name, int w, int h)
    {
        QImage image(w, h, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        const QString path = m_dir.filePath(name);
        return image.save(path, "PNG") ? QUrl::fromLocalFile(path) : QUrl{};
    }

    struct Card {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QObject> root;
        QQuickItem *item = nullptr;
        QQuickItem *box = nullptr;
        std::unique_ptr<QQuickWindow> window;
    };

    Card makeCard(const QUrl &artwork, qreal cardWidth)
    {
        Card card;
        card.engine = std::make_unique<QQmlApplicationEngine>();
        auto *controller = new AppController(AppController::MockBackend,
                                             false, card.engine.get());
        card.engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                       controller);
        card.engine->addImageProvider(
            QStringLiteral("lightning-media"),
            new MediaImageProvider(controller->mediaBridge()));

        QQmlComponent component(card.engine.get());
        component.setData(R"(
import QtQuick
import MatrixClient
AudioPlayerCard { objectName: "card" }
)", QUrl(QStringLiteral("qrc:/audioCoverArtAspect.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return card;
        }
        card.root.reset(component.create(card.engine->rootContext()));
        card.item = qobject_cast<QQuickItem *>(card.root.get());
        if (!card.item)
            return card;

        card.window = std::make_unique<QQuickWindow>();
        card.window->resize(int(cardWidth) + 40, 600);
        card.item->setParentItem(card.window->contentItem());
        card.window->show();
        card.item->setWidth(cardWidth);
        card.item->setProperty("artworkSource", artwork.toString());
        card.box = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioCoverArtBox"));

        QQuickItem *image = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioCoverArtwork"));
        if (image) {
            QElapsedTimer elapsed;
            elapsed.start();
            while (elapsed.elapsed() < 5000
                   && !(image->implicitWidth() > 0
                        && image->implicitHeight() > 0)) {
                QTest::qWait(20);
            }
        }
        return card;
    }

    void setLarge(Card &card, bool large)
    {
        auto *controller = card.engine->rootContext()
            ->contextProperty(QStringLiteral("app")).value<QObject *>();
        auto *settings = controller->findChild<SettingsManager *>();
        QVERIFY(settings);
        settings->setAlbumArtStyle(large ? QStringLiteral("card") : QStringLiteral("compact"));
        QTest::qWait(50);
    }

    static AppController *controllerOf(const Card &card)
    {
        return qobject_cast<AppController *>(card.engine->rootContext()
            ->contextProperty(QStringLiteral("app")).value<QObject *>());
    }

    static QQuickItem *coverBox(const Card &card)
    {
        return card.item->findChild<QQuickItem *>(
            QStringLiteral("audioCoverArtBox"));
    }

    static QRectF sceneRectOf(QQuickItem *item)
    {
        return QRectF(item->mapToScene(QPointF(0, 0)), item->size());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("audio-cover-art-aspect-test"));
        QSettings().clear();
    }

    void thePlayButtonNeverCoversTheCoverAndTheSettingSwitchesLayout()
    {
        Card card = makeCard(makeImage(QStringLiteral("g.png"), 600, 600), 360);
        QVERIFY(card.item != nullptr);
        card.item->setProperty("isVoice", false);
        auto *play = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioPlayPauseButton"));
        auto *tile = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioCoverArtBox"));
        QVERIFY(play && tile);

        setLarge(card, true);
        QTRY_VERIFY(tile->width() > 80);
        QCOMPARE(tile->width(), tile->height());
        QVERIFY2(!sceneRectOf(play).intersects(sceneRectOf(tile)),
                 "the play button covers the large cover");
        const qreal withLarge = card.item->implicitHeight();
        QVERIFY2(withLarge <= tile->height() + 12 + 4,
                 "the card is taller than its cover");

        setLarge(card, false);
        QTRY_COMPARE(tile->width(), 40.0);
        QVERIFY2(!sceneRectOf(play).intersects(sceneRectOf(tile)),
                 "the play button covers the compact tile");
        QVERIFY(card.item->implicitHeight() < withLarge);

        card.item->setProperty("artworkSource", QString());
        setLarge(card, true);
        QTRY_VERIFY(!coverBox(card));

        card.item->setProperty("artworkSource",
            makeImage(QStringLiteral("n.png"), 600, 600).toString());
        card.item->setWidth(240);
        QTRY_VERIFY(coverBox(card));
        QTRY_COMPARE(coverBox(card)->width(), 40.0);
    }

    void anEventCoverStandsInAndABrokenOneLooksLikeNone()
    {
        Card bare = makeCard(QUrl(), 360);
        QVERIFY(bare.item != nullptr);
        bare.item->setProperty("isVoice", false);
        setLarge(bare, true);
        QVERIFY2(!coverBox(bare), "a file with no known cover drew a tile");
        const qreal bareHeight = bare.item->implicitHeight();

        Card good = makeCard(QUrl(), 360);
        QVERIFY(good.item != nullptr);
        good.item->setProperty("isVoice", false);
        setLarge(good, true);
        good.item->setProperty("wireCoverSource",
            makeImage(QStringLiteral("wire.png"), 500, 500).toString());
        auto *large = good.item->findChild<QQuickItem *>(
            QStringLiteral("audioCoverArtBox"));
        QVERIFY(large);
        QTRY_VERIFY(large->width() > 80);
        QVERIFY(good.item->property("artworkSource").toString().endsWith(
            QStringLiteral("wire.png")));

        const QString junk = m_dir.filePath(QStringLiteral("junk.png"));
        {
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not a picture");
        }
        Card broken = makeCard(QUrl(), 360);
        QVERIFY(broken.item != nullptr);
        broken.item->setProperty("isVoice", false);
        setLarge(broken, true);
        broken.item->setProperty("wireCoverSource",
                                 QUrl::fromLocalFile(junk).toString());
        QTRY_VERIFY(broken.item->property("wireCoverFailed").toBool());
        QTRY_VERIFY(!coverBox(broken));
        QCOMPARE(broken.item->property("artworkSource").toString(), QString());
        QCOMPARE(broken.item->implicitHeight(), bareHeight);
    }

    void everyCoverIsTheSameFixedTileAndTheRowKeepsItsHeight()
    {
        Card bare = makeCard(QUrl(), 360);
        QVERIFY(bare.item != nullptr);
        bare.item->setProperty("isVoice", false);
        setLarge(bare, false);
        QVERIFY(!coverBox(bare));
        const qreal rowHeight = bare.item->implicitHeight();
        QVERIFY(rowHeight > 0);

        const struct { const char *name; int w; int h; } covers[] = {
            { "sq.png", 600, 600 }, { "tall.png", 400, 1200 },
            { "wide.png", 1200, 400 } };
        for (const auto &cover : covers) {
            Card card = makeCard(makeImage(QString::fromLatin1(cover.name),
                                           cover.w, cover.h), 360);
            QVERIFY(card.item != nullptr);
            card.item->setProperty("isVoice", false);
            setLarge(card, false);
            QVERIFY2(card.box != nullptr, "the cover art tile was not created");
            QTRY_VERIFY(card.box->width() > 1);
            QCOMPARE(card.box->width(), 40.0);
            QCOMPARE(card.box->height(), 40.0);
            QCOMPARE(card.item->implicitHeight(), rowHeight);
            auto *image = card.item->findChild<QQuickItem *>(
                QStringLiteral("audioCoverArtwork"));
            QVERIFY(image);
            QCOMPARE(image->property("fillMode").toInt(),
                     int(2));
        }
    }

    void theArtIsCutToItsTilesRoundedCorners()
    {
        const QColor ground(250, 250, 250);
        QImage red(600, 400, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        for (const QString style : { QStringLiteral("compact"),
                                     QStringLiteral("card"),
                                     QStringLiteral("large") }) {
            for (const bool inBubble : { false, true }) {
                const QString where = style
                    + (inBubble ? QStringLiteral(" in a bubble") : QString());
                Card card = makeCard(QUrl(), 360);
                QVERIFY(card.item != nullptr);
                card.window->setColor(ground);
                card.window->resize(420, 600);
                card.item->setPosition(QPointF(20, 20));
                card.item->setProperty("isVoice", false);
                card.item->setProperty("inBubble", inBubble);
                AppController *controller = controllerOf(card);
                QVERIFY(controller && controller->mediaBridge());
                controller->findChild<SettingsManager *>()
                    ->setAlbumArtStyle(style);
                card.item->setProperty("artworkSource",
                    controller->mediaBridge()->audioArtworkSource(
                        QStringLiteral("mk-corner"), QVariant::fromValue(red)));
                QTRY_VERIFY(coverBox(card));
                auto *image = card.item->findChild<QQuickItem *>(
                    QStringLiteral("audioCoverArtwork"));
                QVERIFY(image);
                QTRY_COMPARE(image->property("status").toInt(), 1);
                QTRY_COMPARE(image->opacity(), 1.0);
                QTest::qWait(50);

                const QRectF box = sceneRectOf(coverBox(card));
                const QImage frame = card.window->grabWindow();
                const qreal dpr = frame.devicePixelRatio();
                auto at = [&](qreal x, qreal y) {
                    return QColor(frame.pixel(int(x * dpr), int(y * dpr)));
                };
                QVERIFY2(at(box.center().x(), box.center().y()).red() > 200
                             && at(box.center().x(), box.center().y()).green() < 60,
                         qPrintable(where + QStringLiteral(": no art drawn")));
                const QPointF corners[] = {
                    box.topLeft(), box.topRight() - QPointF(1, 0),
                    box.bottomLeft() - QPointF(0, 1),
                    box.bottomRight() - QPointF(1, 1) };
                for (const QPointF &c : corners) {
                    const QColor px = at(c.x(), c.y());
                    QVERIFY2(px.red() - px.green() < 40,
                             qPrintable(QStringLiteral("%1: art outside the "
                                 "rounded tile at (%2,%3): %4")
                                 .arg(where).arg(c.x()).arg(c.y())
                                 .arg(px.name())));
                }
            }
        }
    }

    void aVoiceMessageHasNoCoverTile()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);
        card.item->setProperty("isVoice", true);
        card.item->setProperty("artworkSource",
            makeImage(QStringLiteral("v.png"), 100, 100).toString());
        card.item->setProperty("thumbAvailable", true);
        QTest::qWait(50);
        QVERIFY(!coverBox(card));
    }

    void aPositionFloorsAndATotalRounds()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);

        const auto call = [&card](const char *fn, int ms) {
            QVariant out;
            const bool called = QMetaObject::invokeMethod(
                card.item, fn, Qt::DirectConnection,
                Q_RETURN_ARG(QVariant, out), Q_ARG(QVariant, ms));
            return called ? out.toString() : QStringLiteral("<not called>");
        };

        QCOMPARE(call("formatDuration", 25700), QStringLiteral("0:26"));
        QCOMPARE(call("formatDuration", 25400), QStringLiteral("0:25"));
        QCOMPARE(call("formatPosition", 25700), QStringLiteral("0:25"));
        QCOMPARE(call("formatPosition", 25400), QStringLiteral("0:25"));
        QCOMPARE(call("formatDuration", 59600), QStringLiteral("1:00"));
        QCOMPARE(call("formatPosition", 60000), QStringLiteral("1:00"));
        QCOMPARE(call("formatDuration", 0), QStringLiteral("0:00"));
        QCOMPARE(call("formatPosition", 0), QStringLiteral("0:00"));
    }

    void theWaveformDrawsItsBucketsInsteadOfClampingThemToABlock()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);

        QVariantList wf;
        for (int amp : { 0, 5, 12, 25, 40, 55, 70, 85, 100 })
            wf.append(amp);
        card.item->setProperty("isVoice", true);
        card.item->setProperty("waveform", wf);

        QQuickItem *row = card.item->findChild<QQuickItem *>(
            QStringLiteral("voiceSeekWave"));
        QVERIFY2(row != nullptr, "the waveform row was not created");
        QTRY_VERIFY(row->isVisible() && row->width() > 1);

        QList<qreal> heights;
        const auto kids = row->childItems();
        for (QQuickItem *kid : kids) {
            if (kid->height() > 0 && kid->width() > 0)
                heights.append(kid->height());
        }
        QVERIFY2(heights.size() >= 4,
                 qPrintable(QStringLiteral("only %1 waveform bars were built")
                                .arg(heights.size())));

        qreal lo = heights.first();
        qreal hi = heights.first();
        for (qreal h : heights) {
            lo = qMin(lo, h);
            hi = qMax(hi, h);
        }
        const qreal strip = row->height();

        QSet<int> distinct;
        for (qreal h : heights)
            distinct.insert(qRound(h * 4.0));
        QVERIFY2(distinct.size() >= 5,
                 qPrintable(QStringLiteral(
                     "nine ascending buckets drew only %1 distinct bar "
                     "heights on a %2px strip (%3..%4) — the 0..=100 values "
                     "were clamped to 1 instead of divided by 100")
                                .arg(distinct.size()).arg(strip)
                                .arg(lo).arg(hi)));
        QVERIFY2(hi <= strip + 0.5,
                 qPrintable(QStringLiteral("a bar is %1px on a %2px strip")
                                .arg(hi).arg(strip)));
        QVERIFY2(lo < strip * 0.3,
                 qPrintable(QStringLiteral(
                     "the quietest bar is %1px of a %2px strip")
                                .arg(lo).arg(strip)));
    }
};

QTEST_MAIN(AudioCoverArtAspectTest)
#include "AudioCoverArtAspectTest.moc"
