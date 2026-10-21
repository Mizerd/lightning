// C43: the voice preview pill must not change width, and so must not move its
// play button, when playback starts and the time label grows from "m:ss" to
// "m:ss / m:ss". Loads the REAL qml/VoicePreviewBar.qml offscreen and measures
// it; the mock backend supplies `app` (settings.mediaVolume).

#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>

#include "app/AppController.h"

class VoicePreviewWidthQmlTest : public QObject
{
    Q_OBJECT

    struct Bar {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QObject> root;
        QQuickItem *item = nullptr;
        QQuickItem *label = nullptr;
    };

    static Bar makeBar(bool compact, int durationMs)
    {
        Bar bar;
        bar.engine = std::make_unique<QQmlApplicationEngine>();
        auto *controller = new AppController(AppController::MockBackend, false,
                                             bar.engine.get());
        bar.engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                      controller);
        QQmlComponent component(bar.engine.get());
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
VoicePreviewBar { objectName: "bar" }
)"), QUrl(QStringLiteral("qrc:/voicePreviewWidth.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return bar;
        }
        bar.root.reset(component.create(bar.engine->rootContext()));
        bar.item = qobject_cast<QQuickItem *>(bar.root.get());
        if (!bar.item)
            return bar;
        bar.item->setProperty("compact", compact);
        bar.item->setProperty("durationMs", durationMs);
        bar.label = bar.item->findChild<QQuickItem *>(
            QStringLiteral("voicePreviewTime"));
        return bar;
    }

private Q_SLOTS:
    void theWidthDoesNotFollowThePlaybackLabel_data()
    {
        QTest::addColumn<bool>("compact");
        QTest::addColumn<int>("durationMs");
        QTest::newRow("regular short") << false << 6000;
        QTest::newRow("regular minutes") << false << 125000;
        QTest::newRow("compact short") << true << 6000;
        QTest::newRow("compact minutes") << true << 599000;
    }

    void theWidthDoesNotFollowThePlaybackLabel()
    {
        QFETCH(bool, compact);
        QFETCH(int, durationMs);
        Bar bar = makeBar(compact, durationMs);
        QVERIFY(bar.item != nullptr);
        QVERIFY(bar.label != nullptr);
        QTRY_VERIFY(bar.item->implicitWidth() > 0);

        const qreal idle = bar.item->implicitWidth();
        const QString idleText = bar.label->property("text").toString();
        const qreal idleContent = bar.label->property("contentWidth").toReal();
        QVERIFY2(!idleText.contains(QLatin1Char('/')), qPrintable(idleText));

        // What playback shows, in the widest digits: the label is bound to
        // MediaPlayer.position, which a test cannot drive, so state the text
        // it would produce. (Breaking the binding is fine here, it is a probe.)
        const int total = int(std::round(durationMs / 1000.0));
        auto clock = [](int s) {
            return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10,
                                                           QLatin1Char('0'));
        };
        const QString playing = QStringLiteral("8:88 / ") + clock(total);
        bar.label->setProperty("text", playing);
        QTest::qWait(60); // let the layout polish run
        // Non-vacuity: the text really is wider than the idle one.
        QVERIFY2(bar.label->property("contentWidth").toReal() > idleContent,
                 "probe sanity: the playing text must be the wider one");
        QCOMPARE(bar.item->implicitWidth(), idle);
        QVERIFY2(bar.label->property("contentWidth").toReal()
                     <= bar.label->width() + 0.5,
                 "the playing label overflows the slot that was reserved");
    }
};

QTEST_MAIN(VoicePreviewWidthQmlTest)
#include "VoicePreviewWidthQmlTest.moc"
