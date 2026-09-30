// Behavioural suite for qml/TopicText.qml, the room and Space topic renderer.
//
// Reported: "you cannot select the text on room information tab ... when
// trying to copy paste something from there its just impossible", and the
// topic's URLs were plain text where Element links them. Until this component
// the Overview topic was a Label: no selection, no links. Pinned against the
// real component inside a Flickable, driven with real mouse and key events:
//   * a URL in the topic (http(s) and a bare www. host) is a link, and a click
//     on it reaches MediaManager::openWebUrl; a Matrix room link opens in-app;
//   * a drag selects and opens nothing, even when it starts on a link;
//   * after a click, Ctrl+A and Ctrl+C copy the whole topic;
//   * no scheme but http(s) is ever a link, and markup in a topic is text;
//   * a wheel over the topic still scrolls the panel around it.
// The last case pins that room info and Space Home render their topics with it.

#include <QtTest/QtTest>

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>

#include <memory>

#include "models/LinkPreviewController.h"

class TopicTextQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Harness {
        // Declared first so it is destroyed last: the warning lambda captures
        // it and the engine can still emit during teardown.
        QStringList warnings;
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<LinkPreviewController> linkPreviews;
        std::unique_ptr<QObject> app;
        std::unique_ptr<QQuickWindow> window;
        // Destroyed before the engine and the `app` its bindings reach.
        std::unique_ptr<QObject> rootOwner;
        QQuickItem *flick = nullptr;
        QQuickItem *topic = nullptr;

        QObject *media() const
        {
            return app->property("media").value<QObject *>();
        }
        int webOpens() const { return media()->property("count").toInt(); }
        QString lastWeb() const { return media()->property("last").toString(); }
        int matrixOpens() const { return app->property("matrixCount").toInt(); }
        QString lastMatrix() const
        {
            return app->property("matrixLast").toString();
        }
    };

    bool build(Harness &h, const QString &topic)
    {
        h.engine = std::make_unique<QQmlEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        // `app` stand-in: the real linkifier, and recorders where the app would
        // launch a browser or navigate.
        h.linkPreviews = std::make_unique<LinkPreviewController>();
        QQmlComponent appComponent(h.engine.get());
        appComponent.setData(R"(
import QtQuick
QtObject {
    property QtObject linkPreviews: null
    property int matrixCount: 0
    property string matrixLast: ""
    function openMatrixLink(link) { matrixCount += 1; matrixLast = String(link) }
    property QtObject media: QtObject {
        property int count: 0
        property string last: ""
        function openWebUrl(url) { count += 1; last = String(url) }
    }
}
)", QUrl(QStringLiteral("qrc:/topictext-app.qml")));
        h.app.reset(appComponent.create());
        if (!h.app) {
            qWarning("%s", qPrintable(appComponent.errorString()));
            return false;
        }
        h.app->setProperty("linkPreviews",
                           QVariant::fromValue<QObject *>(h.linkPreviews.get()));
        h.engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    h.app.get());

        QQmlComponent component(h.engine.get());
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    objectName: "flick"
    width: 420
    height: 300
    contentWidth: width
    contentHeight: col.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    Column {
        id: col
        width: parent.width
        TopicText {
            objectName: "topic"
            width: parent.width
        }
        // Room for the panel to scroll.
        Item { width: 1; height: 2000 }
    }
}
)", QUrl(QStringLiteral("qrc:/topictext.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        h.rootOwner.reset(component.create());
        h.flick = qobject_cast<QQuickItem *>(h.rootOwner.get());
        if (!h.flick)
            return false;
        h.topic = h.flick->findChild<QQuickItem *>(QStringLiteral("topic"));
        if (!h.topic)
            return false;
        h.topic->setProperty("plainText", topic);

        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(420, 300);
        h.flick->setParentItem(h.window->contentItem());
        h.window->show();
        h.window->requestActivate();
        if (!QTest::qWaitForWindowActive(h.window.get()))
            return false;
        QCoreApplication::processEvents();
        return h.topic->height() > 0;
    }

    // Window coordinates of the first point whose link is `href`, or (-1, -1).
    static QPoint linkPoint(const Harness &h, const QString &href)
    {
        for (qreal y = 2; y < h.topic->height(); y += 3) {
            for (qreal x = 0; x < h.topic->width(); x += 2) {
                QString at;
                QMetaObject::invokeMethod(h.topic, "linkAt",
                                          Q_RETURN_ARG(QString, at),
                                          Q_ARG(qreal, x), Q_ARG(qreal, y));
                if (at == href) {
                    // A little inside the anchor, not on its first pixel.
                    return h.topic->mapToScene(QPointF(x + 4, y)).toPoint();
                }
            }
        }
        return { -1, -1 };
    }

    static int linkPixels(const Harness &h)
    {
        int found = 0;
        for (qreal y = 2; y < h.topic->height(); y += 3) {
            for (qreal x = 0; x < h.topic->width(); x += 3) {
                QString at;
                QMetaObject::invokeMethod(h.topic, "linkAt",
                                          Q_RETURN_ARG(QString, at),
                                          Q_ARG(qreal, x), Q_ARG(qreal, y));
                if (!at.isEmpty())
                    ++found;
            }
        }
        return found;
    }

    static QString selectedText(const Harness &h)
    {
        return h.topic->property("selectedText").toString();
    }

    static void drag(QQuickWindow *window, QPoint from, QPoint to)
    {
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(window, from + (to - from) * step / 8);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QCoreApplication::processEvents();
    }

    static QString readQml(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return QString::fromUtf8(file.readAll());
    }

private Q_SLOTS:
    void aTopicUrlIsALinkAndAClickOpensIt()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(
            "Rules: https://example.org/rules and www.example.com/faq.")));

        const QPoint web = linkPoint(h, QStringLiteral("https://example.org/rules"));
        QVERIFY2(web.x() >= 0, "the https URL was not rendered as a link");
        QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier, web);
        QCOMPARE(h.webOpens(), 1);
        QCOMPARE(h.lastWeb(), QStringLiteral("https://example.org/rules"));

        const QPoint bare = linkPoint(h, QStringLiteral("https://www.example.com/faq"));
        QVERIFY2(bare.x() >= 0, "the bare www. host was not rendered as a link");
        QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier, bare);
        QCOMPARE(h.webOpens(), 2);
        QCOMPARE(h.lastWeb(), QStringLiteral("https://www.example.com/faq"));
        QCOMPARE(h.matrixOpens(), 0);
        QCOMPARE(h.warnings, QStringList{});
    }

    // Routed as a message body routes it: in the app, not the browser.
    void aMatrixRoomLinkOpensInTheApp()
    {
        Harness h;
        const QString room = QStringLiteral("https://matrix.to/#/#lightning:example.org");
        QVERIFY(build(h, QStringLiteral("Join us: ") + room));
        const QPoint at = linkPoint(h, room);
        QVERIFY(at.x() >= 0);
        QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(h.matrixOpens(), 1);
        QCOMPARE(h.lastMatrix(), room);
        QCOMPARE(h.webOpens(), 0);
    }

    void aDragSelectsAndOpensNothingEvenFromALink()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(
            "Read https://example.org/rules before posting anything here.")));

        const QPoint plainStart = h.topic->mapToScene(QPointF(2, 6)).toPoint();
        drag(h.window.get(), plainStart, plainStart + QPoint(90, 0));
        QVERIFY2(selectedText(h).size() > 3,
                 qPrintable(QStringLiteral("selected '%1'").arg(selectedText(h))));

        const QPoint onLink = linkPoint(h, QStringLiteral("https://example.org/rules"));
        QVERIFY(onLink.x() >= 0);
        drag(h.window.get(), onLink, onLink + QPoint(80, 0));
        QVERIFY2(!selectedText(h).isEmpty(), "a drag from a link selected nothing");
        QCOMPARE(h.webOpens(), 0);
        // Selecting did not scroll the panel.
        QCOMPARE(h.flick->property("contentY").toReal(), 0.0);
    }

    void selectAllAndCopyYieldTheWholeTopic()
    {
        Harness h;
        const QString topic = QStringLiteral(
            "Welcome. Rules: https://example.org/rules\nBe kind.");
        QVERIFY(build(h, topic));

        // A click on plain text focuses the topic and opens nothing.
        QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier,
                          h.topic->mapToScene(QPointF(2, 6)).toPoint());
        QVERIFY(h.topic->hasActiveFocus());
        QCOMPARE(h.webOpens(), 0);

        QTest::keyClick(h.window.get(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(selectedText(h).size(),
                 h.topic->property("length").toInt());
        QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
        QTest::keyClick(h.window.get(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), topic);
    }

    void noSchemeButWebIsEverALink()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(
            "javascript:alert(1) data:text/html,x file:///etc/passwd "
            "ftp://example.org <a href=\"javascript:alert(2)\">x</a>")));
        QCOMPARE(linkPixels(h), 0);
        // The markup is shown as text, not interpreted.
        QString shown;
        const int start = 0;
        const int end = h.topic->property("length").toInt();
        QMetaObject::invokeMethod(h.topic, "getText", Q_RETURN_ARG(QString, shown),
                                  Q_ARG(int, start), Q_ARG(int, end));
        QVERIFY2(shown.contains(QStringLiteral("<a href=\"javascript:alert(2)\">")),
                 qPrintable(shown));
        QCOMPARE(h.webOpens(), 0);
        QCOMPARE(h.matrixOpens(), 0);
    }

    void aWheelOverTheTopicStillScrollsThePanel()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("A topic with https://example.org/rules")));
        const QPointF over = h.topic->mapToScene(QPointF(10, 6));
        QWheelEvent wheel(over, h.window->mapToGlobal(over.toPoint()),
                          QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(h.window.get(), &wheel);
        QTRY_VERIFY(h.flick->property("contentY").toReal() > 0.0);
    }

    // The source half: the reported panel and its Space sibling render their
    // topics with TopicText. On the old code both were Labels.
    void roomInfoAndSpaceHomeRenderTheTopicWithTopicText()
    {
        const QString panel = readQml(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(!panel.isEmpty());
        const qsizetype info = panel.indexOf(
            QStringLiteral("objectName: \"roomInfoTopic\""));
        QVERIFY2(info > 0, "room info has no TopicText for its topic");
        QVERIFY(panel.lastIndexOf(QStringLiteral("TopicText {"), info) > 0);
        QVERIFY(panel.mid(info, 300).contains(
            QStringLiteral("plainText: root.roomData.topic")));

        const QString pane = readQml(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        const qsizetype home = pane.indexOf(
            QStringLiteral("objectName: \"spaceHomeTopic\""));
        QVERIFY2(home > 0, "Space Home has no TopicText for its topic");
        QVERIFY(pane.lastIndexOf(QStringLiteral("TopicText {"), home) > 0);
        QVERIFY(pane.mid(home, 300).contains(
            QStringLiteral("plainText: spaceHome.info.topic")));

        // Links go through the validated gate, never straight to a launcher.
        const QString component = readQml(QStringLiteral("TopicText.qml"));
        QVERIFY(component.contains(QStringLiteral("app.linkPreviews.linkifiedTopic(")));
        QVERIFY(component.contains(QStringLiteral("app.media.openWebUrl(link)")));
        QVERIFY(!component.contains(QStringLiteral("Qt.openUrlExternally")));
    }
};

QTEST_MAIN(TopicTextQmlTest)
#include "TopicTextQmlTest.moc"
