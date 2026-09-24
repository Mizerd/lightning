// The audio card's cover-art box keeps the artwork's aspect at every size.
// Bounding only the height lets a tall cover get a full-width box capped at
// 420 tall, with PreserveAspectFit leaving dead space at both sides; both
// axes are bounded and the width derives from the capped height. Drives the
// real qml/AudioPlayerCard.qml with real images.

#include <QtTest/QtTest>

#include <QSet>

#include <QElapsedTimer>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include "app/AppController.h"

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

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
    }

    // A square cover in a wide enough card gets a square box.
    void aSquareCoverGetsASquareBox()
    {
        Card card = makeCard(makeImage(QStringLiteral("sq.png"), 600, 600), 360);
        QVERIFY(card.item != nullptr);
        QVERIFY2(card.box != nullptr, "the cover art box was not created");
        QTRY_VERIFY(card.box->width() > 1);
        QCOMPARE(card.box->width(), card.box->height());
        // 360 card minus 6px margin each side.
        QCOMPARE(card.box->width(), 348.0);
    }

    // A tall cover past the 420 edge cap keeps its aspect: both axes are
    // bounded.
    void aTallCoverKeepsItsAspectWhenTheCapEngages()
    {
        Card card = makeCard(makeImage(QStringLiteral("tall.png"), 400, 1200),
                             360);
        QVERIFY(card.item != nullptr);
        QVERIFY(card.box != nullptr);
        QTRY_VERIFY(card.box->height() > 1);

        // Capped on the long edge...
        QCOMPARE(card.box->height(), 420.0);
        // ...and the short edge follows the artwork, not the card.
        QCOMPARE(card.box->width(), 140.0);
        QVERIFY2(card.box->width() < 348.0,
                 "a full-width box at the height cap does not match a tall "
                 "cover's aspect — the artwork letterboxes with dead surface "
                 "down both sides");

        const qreal ratio = card.box->height() / card.box->width();
        QVERIFY2(qAbs(ratio - 3.0) < 0.02,
                 qPrintable(QStringLiteral("box ratio %1, artwork ratio 3.0")
                                .arg(ratio)));
    }

    // A wide cover is bounded by the card, not the cap, and still matches.
    void aWideCoverIsBoundedByTheCard()
    {
        Card card = makeCard(makeImage(QStringLiteral("wide.png"), 1200, 400),
                             360);
        QVERIFY(card.item != nullptr);
        QVERIFY(card.box != nullptr);
        QTRY_VERIFY(card.box->width() > 1);

        QCOMPARE(card.box->width(), 348.0);
        QCOMPARE(card.box->height(), 116.0);
    }

    // No artwork: the box takes no space.
    void noArtworkLeavesNoBox()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);
        QVERIFY(card.box != nullptr);
        QVERIFY(!card.box->isVisible());
        QCOMPARE(card.box->height(), 0.0);
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
            QStringLiteral("audioWaveRow"));
        QVERIFY2(row != nullptr, "the waveform row was not created");
        QTRY_VERIFY(row->isVisible() && row->width() > 1);

        // findChild cannot reach Repeater delegates; walk the Row's children.
        QList<qreal> heights;
        const auto kids = row->childItems();
        for (QQuickItem *kid : kids) {
            if (kid->height() > 0 && kid->width() > 0 && kid->width() <= 4)
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
