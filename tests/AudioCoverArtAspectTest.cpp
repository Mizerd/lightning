// The audio card's cover art (design-media B6): a FIXED square thumbnail the
// play button sits on, whatever the artwork's shape, so a row never changes
// height when a cover appears (it used to grow a box up to 420 px tall on
// play and drop it at the end, moving the history twice). Plus the card's
// clocks and waveform shape. Drives the real qml/AudioPlayerCard.qml with
// real images.

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

class AudioCoverArtAspectTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // A solid image of an exact size, on disk, so the QML Image reports true
    // implicit dimensions.
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
        // A window, so the layouts polish and sizes are real.
        std::unique_ptr<QQuickWindow> window;
    };

    // Instantiates the production card with a file:// artwork URL;
    // artworkSource is a plain string, so no decrypted payload is needed.
    Card makeCard(const QUrl &artwork, qreal cardWidth)
    {
        Card card;
        card.engine = std::make_unique<QQmlApplicationEngine>();
        auto *controller = new AppController(AppController::MockBackend,
                                             false, card.engine.get());
        card.engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                       controller);

        QQmlComponent component(card.engine.get());
        component.setData(R"(
import QtQuick
import MatrixClient
AudioPlayerCard { objectName: "card" }
)", QUrl(QStringLiteral("qrc:/audioCoverArtAspect.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return card;  // caller asserts on the null item
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

        // The Image loads asynchronously; wait for a non-fallback shape.
        // Spun by hand because QTRY_VERIFY returns void; a timeout is left to
        // the caller's geometry assertions to report.
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
        settings->setLargeAlbumArt(large);
        QTest::qWait(50);
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

    // The play button never covers the art, in either layout (0.10.2 drew it
    // on top of the 40 px tile), and the setting switches between them.
    void thePlayButtonNeverCoversTheCoverAndTheSettingSwitchesLayout()
    {
        Card card = makeCard(makeImage(QStringLiteral("g.png"), 600, 600), 360);
        QVERIFY(card.item != nullptr);
        card.item->setProperty("isVoice", false);
        auto *play = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioPlayPauseButton"));
        auto *tile = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioCoverArtBox"));
        auto *large = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioLargeCoverBox"));
        QVERIFY(play && tile && large);

        // Default on, with art: the large cover, above the controls.
        setLarge(card, true);
        QTRY_VERIFY(large->isVisible());
        QVERIFY(!tile->isVisible());
        QTRY_VERIFY(large->width() > 100);
        QCOMPARE(large->width(), large->height());
        QVERIFY2(!sceneRectOf(play).intersects(sceneRectOf(large)),
                 "the play button covers the large cover");
        const qreal withLarge = card.item->implicitHeight();

        // Off: the compact tile, beside the button, not under it.
        setLarge(card, false);
        QTRY_VERIFY(tile->isVisible());
        QVERIFY(!large->isVisible());
        QCOMPARE(tile->width(), 40.0);
        QVERIFY2(!sceneRectOf(play).intersects(sceneRectOf(tile)),
                 "the play button covers the compact tile");
        QVERIFY(card.item->implicitHeight() < withLarge);

        // No art at all: compact even with the setting on.
        card.item->setProperty("artworkSource", QString());
        setLarge(card, true);
        QTRY_VERIFY(!large->isVisible());
        QVERIFY(tile->isVisible());
        QVERIFY(!sceneRectOf(play).intersects(sceneRectOf(tile)));
    }

    // Any cover, square, tall or wide, is the same fixed 40 px tile, cropped
    // to fill it; the row is as tall as it was without one (B6).
    void everyCoverIsTheSameFixedTileAndTheRowKeepsItsHeight()
    {
        Card bare = makeCard(QUrl(), 360);
        QVERIFY(bare.item != nullptr);
        bare.item->setProperty("isVoice", false);
        QVERIFY(bare.box != nullptr);
        setLarge(bare, false);
        QVERIFY(bare.box->isVisible());
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
                     int(2) /* Image.PreserveAspectCrop */);
        }
    }

    // A voice message has no tile at all: just the play button.
    void aVoiceMessageHasNoCoverTile()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);
        card.item->setProperty("isVoice", true);
        QVERIFY(card.box != nullptr);
        QVERIFY(!card.box->isVisible());
    }

    // A position floors and a total rounds. The summary line rounds the
    // duration, so the card must too; rounding the elapsed clock as well
    // would claim time that has not passed. 25700 and 25400 straddle the
    // boundary both ways, and each is checked against both functions.
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

        // The total: 25.7 s of audio is 26 seconds, as the summary line says.
        QCOMPARE(call("formatDuration", 25700), QStringLiteral("0:26"));
        QCOMPARE(call("formatDuration", 25400), QStringLiteral("0:25"));
        // The position: at 25.7 s you have not reached 0:26.
        QCOMPARE(call("formatPosition", 25700), QStringLiteral("0:25"));
        QCOMPARE(call("formatPosition", 25400), QStringLiteral("0:25"));
        // Neither leaks past 59 into "0:60".
        QCOMPARE(call("formatDuration", 59600), QStringLiteral("1:00"));
        QCOMPARE(call("formatPosition", 60000), QStringLiteral("1:00"));
        QCOMPARE(call("formatDuration", 0), QStringLiteral("0:00"));
        QCOMPARE(call("formatPosition", 0), QStringLiteral("0:00"));
    }

    // The received waveform draws its buckets rather than a solid block.
    // rust/src/timeline.rs normalises buckets to 0..100 and
    // RustTimelineIngest.cpp keeps that range, so clamping with
    // Math.min(1, v) saturates every non-zero bucket. Asserted as shape: bars
    // differ, loud beats quiet, none fills the strip.
    void theWaveformDrawsItsBucketsInsteadOfClampingThemToABlock()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);

        // Ascending buckets across the ingest's 0..100 range.
        QVariantList wf;
        for (int amp : { 0, 5, 12, 25, 40, 55, 70, 85, 100 })
            wf.append(amp);
        card.item->setProperty("isVoice", true);
        card.item->setProperty("waveform", wf);

        QQuickItem *row = card.item->findChild<QQuickItem *>(
            QStringLiteral("voiceSeekWave"));
        QVERIFY2(row != nullptr, "the waveform row was not created");
        QTRY_VERIFY(row->isVisible() && row->width() > 1);

        // findChild cannot reach Repeater delegates; walk the Row's children.
        // The bars are stretched over the row (no dead space), so they are
        // as wide as the row allows; only the Repeater itself is skipped.
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

        // Count distinct heights: under the clamp bucket 0 still sits on the
        // floor while the rest saturate, so a min-vs-max check alone passes.
        // Nine ascending buckets must give a spread.
        QSet<int> distinct;
        for (qreal h : heights)
            distinct.insert(qRound(h * 4.0));  // quarter-pixel buckets
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
        // The loudest bucket (100) is the full strip; the quietest sits on the
        // 0.12 floor.
        QVERIFY2(lo < strip * 0.3,
                 qPrintable(QStringLiteral(
                     "the quietest bar is %1px of a %2px strip")
                                .arg(lo).arg(strip)));
    }
};

QTEST_MAIN(AudioCoverArtAspectTest)
#include "AudioCoverArtAspectTest.moc"
