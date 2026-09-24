// Which picture the image viewer opens, and which list it pages through.
// Driven through the real component in a real window:
//
//   1. A caller with a list and an index (`openAt`, the Media tab) gets
//      exactly that item and pages within exactly that list.
//   2. A lookup by key that misses the loaded timeline (`openFor`) opens what
//      was asked for, alone, never a neighbour. Media history reaches media
//      the timeline has not loaded, so a miss is expected there.

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/TimelineModel.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    ImageViewerOverlay { id: viewer }

    // The three call shapes the production hosts use, verbatim:
    // TimelinePane's delegate/thread entry point, and the Media tab's.
    function openFor(mediaKey, httpUrl) { viewer.openFor(mediaKey, httpUrl) }
    function openAt(list, index) { viewer.openAt(list, index) }
    function showAt(index) { viewer.showAt(index) }

    function isOpen() { return viewer.opened }
    // The Flickable the wheel handlers pan. Reached by objectName so the
    // case reads the REAL viewport rather than a number the scene keeps.
    function flickOf() {
        return viewer.contentItem
            ? findFlick(viewer.contentItem) : null
    }
    function findFlick(item) {
        if (!item)
            return null
        if (item.objectName === "imageViewerFlick")
            return item
        for (var i = 0; i < item.children.length; ++i) {
            var hit = findFlick(item.children[i])
            if (hit)
                return hit
        }
        return null
    }
    function contentX() { var f = flickOf(); return f ? f.contentX : -1 }
    function contentY() { var f = flickOf(); return f ? f.contentY : -1 }
    function pannable() { var f = flickOf(); return f ? f.interactive : false }
    // A Flickable's own wheel handling is ANIMATED, so a sample taken right
    // after a notch reads a position that is still travelling.
    function settled() {
        var f = flickOf()
        return f ? (!f.moving && !f.flicking && !f.dragging) : false
    }
    function zoomNow() { return viewer.zoom }
    function setZoom(z) { viewer.zoom = z }
    // The production entry point the Image's onStatusChanged uses.
    function fitTo(w, h) { viewer.fitImage(w, h); viewer.zoom = 4.0 }
    // Park the viewport in the MIDDLE of its range, so a move in either
    // direction is observable and the case does not depend on wherever the
    // previous one happened to leave it.
    function contentW() { var f = flickOf(); return f ? f.contentWidth : -1 }
    function contentH() { var f = flickOf(); return f ? f.contentHeight : -1 }
    function centreContent() {
        var f = flickOf()
        if (!f) return false
        f.contentX = Math.max(0, (f.contentWidth - f.width) / 2)
        f.contentY = Math.max(0, (f.contentHeight - f.height) / 2)
        return f.contentX > 1 && f.contentY > 1
    }
    function entryCount() { return viewer.entries.length }
    function currentKey() {
        return viewer.current ? (viewer.current.mediaKey || "") : ""
    }
    function close() { viewer.close() }
}
)QML";

QVariantMap historyEntry(const QString &key)
{
    // The exact shape MediaHistoryModel::imageEntries() produces.
    return QVariantMap{
        { QStringLiteral("row"), 0 },
        { QStringLiteral("mediaKey"), key },
        { QStringLiteral("filename"), key + QStringLiteral(".png") },
        { QStringLiteral("sender"), QStringLiteral("@alice:mock.local") },
        { QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc() },
        { QStringLiteral("mime"), QStringLiteral("image/png") },
        { QStringLiteral("httpUrl"), QUrl{} },
        { QStringLiteral("isImage"), true },
        { QStringLiteral("isVideo"), false },
        { QStringLiteral("isVisual"), true },
        { QStringLiteral("thumbAvailable"), false },
        { QStringLiteral("size"), qint64(10) },
    };
}

} // namespace

class MediaViewerNavigationQmlTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QVariant call(const char *name)
    {
        QVariant out;
        const bool ok = QMetaObject::invokeMethod(
            m_root, name, Q_RETURN_ARG(QVariant, out));
        if (!ok)
            return {};
        return out;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("media-viewer-navigation-qml-test"));

        m_controller = new AppController(AppController::MockBackend);
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(5000));
        QTRY_VERIFY(m_controller->loggedIn());
        const QString room = QStringLiteral("!general:mock.local");
        m_controller->openRoom(room);
        // Seed images through the mock's send path, so there is a loaded list
        // a wrong fallback could substitute from.
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->sendImage(room, QStringLiteral("/fixtures/one.png"));
        mock->sendImage(room, QStringLiteral("/fixtures/two.png"));
        mock->sendImage(room, QStringLiteral("/fixtures/three.png"));
        QTRY_VERIFY(m_controller->timeline()->imageEntries().size() >= 3);

        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                     m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("mediaviewerscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    void cleanup() { QMetaObject::invokeMethod(m_root, "close"); }

    // The Media tab's call: the viewer opens the given index in the given list
    // and pages inside that list, never the loaded timeline.
    void openingAtAnExplicitIndexOpensThatItemAndPagesThatList()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")),
                                       historyEntry(QStringLiteral("$c")) };
        QVERIFY2(QMetaObject::invokeMethod(
                     m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
                     Q_ARG(QVariant, QVariant(1))),
                 "the viewer has no way to be given a list and an index");
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        QCOMPARE(call("entryCount").toInt(), 3);

        // Next/previous stay inside the handed-over list.
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(2)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(0)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        // Navigation wraps. Index 3 on three entries lands on index 0, which
        // was also the start, so the next checks go past that coincidence.
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(3)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(4)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        // Backwards past the first wraps to the last. JS `%` keeps the
        // dividend's sign (-1 % 3 is -1), which is why showAt normalises twice.
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(0)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(-1)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
    }

    // An index the list cannot hold opens nothing, never something nearby.
    void anImpossibleIndexOpensNothing()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$only")) };
        QMetaObject::invokeMethod(m_root, "openAt",
                                  Q_ARG(QVariant, QVariant(history)),
                                  Q_ARG(QVariant, QVariant(7)));
        QVERIFY(!call("isOpen").toBool());
        QMetaObject::invokeMethod(m_root, "openAt",
                                  Q_ARG(QVariant, QVariant(QVariantList{})),
                                  Q_ARG(QVariant, QVariant(0)));
        QVERIFY(!call("isOpen").toBool());
    }

    // The timeline's call on a miss: the room has loaded images, the key is
    // not among them, and the viewer opens that key and nothing else.
    void aKeyThatIsNotInTheLoadedTimelineNeverOpensADifferentPicture()
    {
        // The fixture must hold images, or a substituting fallback could not
        // show itself.
        const QVariantList loaded = m_controller->timeline()->imageEntries();
        QVERIFY2(loaded.size() >= 2,
                 "the fixture room has too few images for the old fallback to "
                 "have had anything to substitute");

        QMetaObject::invokeMethod(
            m_root, "openFor", Q_ARG(QVariant, QStringLiteral("$never-loaded")),
            Q_ARG(QVariant, QVariant(QUrl())));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$never-loaded"));
        // Nothing to page to: the truthful state for a key the loaded list
        // does not contain.
        QCOMPARE(call("entryCount").toInt(), 1);
    }

    // On a hit, the key opens inside the timeline's list, so previous/next
    // still walk the room.
    void aKeyThatIsInTheLoadedTimelineStillOpensInsideThatList()
    {
        const QVariantList loaded = m_controller->timeline()->imageEntries();
        QVERIFY(loaded.size() >= 2);
        const QVariantMap first = loaded.first().toMap();
        const QString key = first.value(QStringLiteral("mediaKey")).toString();
        const QUrl url = first.value(QStringLiteral("httpUrl")).toUrl();
        // Seeded rows may be addressed by key or URL; openFor accepts either,
        // as the delegate calls it.
        QMetaObject::invokeMethod(m_root, "openFor", Q_ARG(QVariant, key),
                                  Q_ARG(QVariant, QVariant(url)));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("entryCount").toInt(), int(loaded.size()));
    }

    // The viewer's keys (Left/Right, Up/Down/Space, +/-/0/F, Escape) reach it:
    // the Popup must set `focus: true` (it defaults to false, and a
    // FocusScope inside a popup without active focus never gets it).
    void theViewersKeyboardReachesIt()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")),
                                       historyEntry(QStringLiteral("$c")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(1))));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));

        QTest::keyClick(m_window, Qt::Key_Right);
        QVERIFY2(call("currentKey").toString() == QStringLiteral("$c"),
                 "Right did nothing: the popup never takes active focus, so "
                 "no key the viewer declares can reach it");
        QTest::keyClick(m_window, Qt::Key_Left);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        // Down and Space are "next", Up is "previous".
        QTest::keyClick(m_window, Qt::Key_Down);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        QTest::keyClick(m_window, Qt::Key_Up);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        // ...and it wraps at the end as at the start.
        QTest::keyClick(m_window, Qt::Key_Right);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QTest::keyClick(m_window, Qt::Key_Left);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));

        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY2(!call("isOpen").toBool(),
                 "Escape did not close the viewer, and the decision to take "
                 "click-to-close off the picture rests on it");
    }

    // A thumbnail click selects rather than closing the viewer: a TapHandler
    // on the default DragThreshold policy takes no exclusive grab, so the
    // scrim's close handler would fire on the same press. The strip's handlers
    // use WithinBounds.
    void clickingAThumbnailSelectsItRatherThanClosingTheViewer()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")),
                                       historyEntry(QStringLiteral("$c")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));

        auto *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY2(strip, "the thumbnail strip is gone, so this case is "
                        "testing nothing");
        // The strip fades in (180 ms) with `visible: opacity > 0`; wait for it,
        // or the click lands on the scrim.
        QTRY_VERIFY(strip->isVisible());
        QTRY_VERIFY(strip->opacity() > 0.99);
        auto *content = strip->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QQuickItem *third = nullptr;
        const auto thumbs = content->childItems();
        for (QQuickItem *thumb : thumbs) {
            const QVariant idx = thumb->property("index");
            if (idx.isValid() && idx.toInt() == 2) {
                third = thumb;
                break;
            }
        }
        QVERIFY2(third, "no delegate for the third thumbnail");
        // Its real geometry, mapped to the window.
        const QPointF centre = third->mapToScene(
            QPointF(third->width() / 2, third->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QCoreApplication::processEvents();

        const bool stillOpen = call("isOpen").toBool();
        const QString key = call("currentKey").toString();
        QVERIFY2(stillOpen,
                 qPrintable(QStringLiteral(
                     "clicking a thumbnail closed the viewer (click at %1,%2 "
                     "inside a %3x%4 thumbnail; current key after the click: "
                     "\"%5\"). The scrim's close handler fired on the same "
                     "press as the thumbnail's own.")
                     .arg(centre.x()).arg(centre.y())
                     .arg(third->width()).arg(third->height()).arg(key)));
        QCOMPARE(key, QStringLiteral("$c"));
    }

    // A tap on the picture zooms and does not close: `imageTap` uses
    // `gesturePolicy: WithinBounds` and takes the exclusive grab, so the
    // scrim's close handler never sees it. A text scan cannot show that.
    void clickingThePictureZoomsItRatherThanClosingTheViewer()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());
        // A picture with a real drawn size, so the tap lands inside the band
        // `imageTap` checks rather than on the margin (where closing is right).
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "fitTo", Q_ARG(QVariant, QVariant(4000)),
            Q_ARG(QVariant, QVariant(4000))));
        QCoreApplication::processEvents();
        QTRY_VERIFY(call("pannable").toBool());
        QVERIFY(QMetaObject::invokeMethod(m_root, "setZoom",
                                          Q_ARG(QVariant, QVariant(1.0))));
        QCoreApplication::processEvents();
        QTRY_COMPARE(call("zoomNow").toReal(), 1.0);

        const QPoint centre(m_window->width() / 2, m_window->height() / 2);
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre);
        QCoreApplication::processEvents();

        QVERIFY2(call("isOpen").toBool(),
                 "clicking the picture closed the viewer: the scrim's close "
                 "handler fired on the same press as imageTap, so the "
                 "click-to-zoom gesture cannot work at all");
        QTRY_VERIFY2(call("zoomNow").toReal() > 1.0,
                     "clicking the picture did not zoom it");
        // ...and clicking again returns to fit rather than closing.
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre);
        QCoreApplication::processEvents();
        QVERIFY(call("isOpen").toBool());
        QTRY_COMPARE(call("zoomNow").toReal(), 1.0);
    }

    // A miss between two 48 px thumbnails does nothing: the strip's own
    // swallow surface takes the press rather than letting the scrim close.
    void aMissBetweenTwoThumbnailsDoesNothingRatherThanClosing()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")),
                                       historyEntry(QStringLiteral("$c")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());

        auto *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->isVisible());
        QTRY_VERIFY(strip->opacity() > 0.99);
        auto *content = strip->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QQuickItem *first = nullptr;
        QQuickItem *second = nullptr;
        for (QQuickItem *thumb : content->childItems()) {
            const QVariant idx = thumb->property("index");
            if (!idx.isValid())
                continue;
            if (idx.toInt() == 0)
                first = thumb;
            else if (idx.toInt() == 1)
                second = thumb;
        }
        QVERIFY(first && second);
        // The real gap between two delegates.
        const QPointF a = first->mapToScene(
            QPointF(first->width(), first->height() / 2));
        const QPointF b = second->mapToScene(QPointF(0, second->height() / 2));
        QVERIFY2(b.x() - a.x() >= 2,
                 "the thumbnails are flush, so this case has no gap to aim at");
        const QPoint miss(int((a.x() + b.x()) / 2), int(a.y()));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, miss);
        QCoreApplication::processEvents();

        QVERIFY2(call("isOpen").toBool(),
                 "a click in the gap between two thumbnails closed the "
                 "viewer");
        // ...and it selected nothing.
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
    }


    // One diagonal wheel event (both deltas) moves each axis exactly once. A
    // second WheelHandler for the horizontal axis would move each axis twice;
    // a zoomed picture's interactive Flickable already handles horizontal-only
    // events. That horizontal behaviour is not asserted here: a synthesized
    // click on `imageTap` leaves its exclusive grab behind after the popup
    // closes, and later wheel events in this process never reach the
    // Flickable.
    void oneWheelEventMovesEachAxisExactlyOnce()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());

        // A picture larger than the viewport both ways, parked mid-range so a
        // move either way is observable.
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "fitTo", Q_ARG(QVariant, QVariant(4000)),
            Q_ARG(QVariant, QVariant(4000))));
        QCoreApplication::processEvents();
        QTRY_VERIFY2(call("pannable").toBool(),
                     "the fixture image never became pannable, so this case "
                     "cannot tell a refused pan from nothing to pan to");
        QTRY_VERIFY(call("settled").toBool());
        QVERIFY2(call("centreContent").toBool(),
                 "the viewport has no room to move in both directions");
        QCoreApplication::processEvents();

        const QPointF centre(m_window->width() / 2.0,
                             m_window->height() / 2.0);
        const qreal x0 = call("contentX").toReal();
        const qreal y0 = call("contentY").toReal();
        // pixelDelta on both axes, as a trackpad sends in one event.
        QTest::wheelEvent(m_window, centre, QPoint(0, 0), QPoint(-40, -40));
        QCoreApplication::processEvents();
        QTRY_COMPARE(call("contentX").toReal(), x0 + 40.0);
        QCOMPARE(call("contentY").toReal(), y0 + 40.0);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MediaViewerNavigationQmlTest)
#include "MediaViewerNavigationQmlTest.moc"
