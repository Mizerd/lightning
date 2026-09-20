// v0.7.2: the audio card's embedded cover-art box must keep the ARTWORK'S
// aspect at every size.
//
// The box exists so a square album cover renders square (a fixed ratio used
// to crop the top and bottom off most covers). The first version of it
// bounded only the HEIGHT:
//
//     implicitHeight: Math.min(width * ih / iw, 420)   // width = full card
//
// which is exactly the aspect match it was meant to provide, right up until
// the cap engages. A TALL cover then gets a box that is still full card
// width but only 420 tall, and PreserveAspectFit paints the artwork small
// and centred with dead card surface down both sides — "doesn't fit
// normally". Bounding BOTH axes and deriving the width from the capped
// height keeps box and artwork the same shape.
//
// This drives the real qml/AudioPlayerCard.qml with real images, so it
// measures the shipped geometry rather than a copy of the expression.

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

    // A solid image of an exact size, written to disk so the QML Image
    // reports honest implicitWidth/implicitHeight.
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

    // Instantiates the production card with a plain file:// artwork URL.
    // artworkSource is a plain string property, so this exercises the box
    // geometry without needing a decrypted media payload.
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

        // The Image loads asynchronously; the box's size follows its implicit
        // size, so wait for a non-fallback shape rather than a fixed delay.
        // Spun by hand rather than with QTRY_VERIFY: that macro returns void
        // on timeout, which this helper cannot do. A timeout here is not
        // failed here either — the caller's assertions report it in terms of
        // the geometry actually under test.
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

    // A square cover in a card wide enough for it: box is square.
    void aSquareCoverGetsASquareBox()
    {
        Card card = makeCard(makeImage(QStringLiteral("sq.png"), 600, 600), 360);
        QVERIFY(card.item != nullptr);
        QVERIFY2(card.box != nullptr, "the cover art box was not created");
        QTRY_VERIFY(card.box->width() > 1);
        QCOMPARE(card.box->width(), card.box->height());
        // 360 card - 6px margin each side.
        QCOMPARE(card.box->width(), 348.0);
    }

    // THE REGRESSION. A tall cover exceeds the 420 edge cap. Height-only
    // capping left the box full width (348) at 420 tall — ratio 1.21 against
    // the artwork's 3.0. Both axes must be bounded so the shape survives.
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

    // A wide cover is bounded by the card, not by the cap, and still matches.
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

    // No artwork: the box takes no space at all, so a plain audio row stays
    // compact.
    void noArtworkLeavesNoBox()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);
        QVERIFY(card.box != nullptr);
        QVERIFY(!card.box->isVisible());
        QCOMPARE(card.box->height(), 0.0);
    }

    // A POSITION FLOORS, A TOTAL ROUNDS, AND THEY ARE NOT ONE CLOCK.
    //
    // The collapsed summary line rounds (`embedDurationText`) and this card
    // floored BOTH numbers, so a 25.7 s voice message read "0:26" on the
    // line and "0:25" on the card that line opens. Rounding both — the
    // first attempt at this, and it shipped — fixed that and broke the
    // elapsed clock, which then claimed time that had not passed and would
    // reach the total half a second before the audio ended.
    //
    // 25700 and 25400 straddle the boundary in both directions, so neither
    // half can pass by luck, and each is asserted against BOTH functions so
    // a fixture cannot go green by calling the one it wants.
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

        // The total: 25.7 s of audio IS 26 seconds long, which is what the
        // summary line that opens this card says.
        QCOMPARE(call("formatDuration", 25700), QStringLiteral("0:26"));
        QCOMPARE(call("formatDuration", 25400), QStringLiteral("0:25"));
        // The position: at 25.7 s you have not reached 0:26.
        QCOMPARE(call("formatPosition", 25700), QStringLiteral("0:25"));
        QCOMPARE(call("formatPosition", 25400), QStringLiteral("0:25"));
        // Neither may leak past 59 into a bare "0:60".
        QCOMPARE(call("formatDuration", 59600), QStringLiteral("1:00"));
        QCOMPARE(call("formatPosition", 60000), QStringLiteral("1:00"));
        QCOMPARE(call("formatDuration", 0), QStringLiteral("0:00"));
        QCOMPARE(call("formatPosition", 0), QStringLiteral("0:00"));
    }

    // A RECEIVED VOICE MESSAGE'S WAVEFORM WAS A SOLID BLOCK, and every test
    // over this card passed while it was.
    //
    // The delegate read `Math.min(1, wf[at])` against buckets that
    // rust/src/timeline.rs normalises to 0..=100 (downsample_waveform) and
    // RustTimelineIngest.cpp preserves by dropping anything outside that
    // range. So every bucket of amplitude >= 1 clamped to full height and
    // only a literal zero showed the 0.12 floor: the "real MSC3245
    // waveform" this card advertises could not draw a waveform at all.
    //
    // Asserted as SHAPE, not as specific heights: bars must DIFFER from one
    // another, the loud one must beat the quiet one, and none may fill the
    // strip. A test pinning exact pixels would pass on a block of any
    // uniform height.
    void theWaveformDrawsItsBucketsInsteadOfClampingThemToABlock()
    {
        Card card = makeCard(QUrl(), 360);
        QVERIFY(card.item != nullptr);

        // Ascending buckets across the 0..=100 range the ingest produces.
        QVariantList wf;
        for (int amp : { 0, 5, 12, 25, 40, 55, 70, 85, 100 })
            wf.append(amp);
        card.item->setProperty("isVoice", true);
        card.item->setProperty("waveform", wf);

        QQuickItem *row = card.item->findChild<QQuickItem *>(
            QStringLiteral("audioWaveRow"));
        QVERIFY2(row != nullptr, "the waveform row was not created");
        QTRY_VERIFY(row->isVisible() && row->width() > 1);

        // findChild cannot reach Repeater delegates; walk the Row's own
        // children instead.
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

        // COUNT THE DISTINCT HEIGHTS, and do not merely check that the bars
        // differ. Under the clamp they DO differ: bucket 0 still lands on
        // the 0.12 floor while every other bucket saturates to 1.0, so a
        // min-versus-max assertion passes on the broken code. It did, on the
        // first version of this case. Nine ascending buckets must produce a
        // spread of heights, not two.
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
        // The loudest bucket is 100, which IS the full strip; the quietest
        // must sit on the 0.12 floor and nowhere near it.
        QVERIFY2(lo < strip * 0.3,
                 qPrintable(QStringLiteral(
                     "the quietest bar is %1px of a %2px strip")
                                .arg(lo).arg(strip)));
    }
};

QTEST_MAIN(AudioCoverArtAspectTest)
#include "AudioCoverArtAspectTest.moc"
