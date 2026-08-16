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
};

QTEST_MAIN(AudioCoverArtAspectTest)
#include "AudioCoverArtAspectTest.moc"
