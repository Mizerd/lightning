
#include <QtTest/QtTest>

#include <cmath>

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
    function resizeTo(w, h) { win.width = w; win.height = h }
    function currentIndex() { return viewer.currentIndex }
    // The right-click menu is a child of the overlay, found by objectName.
    // A Popup's default property is contentData, not data.
    function menuObject() {
        for (var i = 0; i < viewer.contentData.length; ++i) {
            var d = viewer.contentData[i]
            if (d && d.objectName === "imageViewerContextMenu")
                return d
        }
        return null
    }
    function menuOpened() {
        var m = menuObject()
        return m ? m.opened : false
    }
    function menuFound() { return menuObject() !== null }
    function closeMenu() {
        var m = menuObject()
        if (m)
            m.close()
    }
}
)QML";

QVariantMap historyEntry(const QString &key)
{
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

QVariantList manyEntries(int n)
{
    QVariantList list;
    for (int i = 0; i < n; ++i)
        list << historyEntry(QStringLiteral("$e%1").arg(i));
    return list;
}

}

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

    void theStripStartsWhereTheCurrentThumbnailIs()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "resizeTo", Q_ARG(QVariant, QVariant(700)),
            Q_ARG(QVariant, QVariant(700))));
        QTRY_COMPARE(m_window->width(), 700);
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(40))),
            Q_ARG(QVariant, QVariant(39))));
        QVERIFY(call("isOpen").toBool());
        QQuickItem *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->opacity() > 0.99);
        const qreal cx = strip->property("contentX").toReal();
        const qreal cell = strip->property("cell").toReal();
        QVERIFY2(strip->property("contentWidth").toReal()
                     > strip->width() + 1,
                 "the fixture set does not overflow the strip");
        QVERIFY2(cx + strip->width() >= 39 * cell + 48 - 1,
                 qPrintable(QStringLiteral(
                     "strip left at the start (contentX=%1) with the "
                     "current thumbnail off to the right").arg(cx)));
    }

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

        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(2)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(0)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(3)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(4)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(0)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(-1)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
    }

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

    void aKeyThatIsNotInTheLoadedTimelineNeverOpensADifferentPicture()
    {
        const QVariantList loaded = m_controller->timeline()->imageEntries();
        QVERIFY2(loaded.size() >= 2,
                 "the fixture room has too few images for the old fallback to "
                 "have had anything to substitute");

        QMetaObject::invokeMethod(
            m_root, "openFor", Q_ARG(QVariant, QStringLiteral("$never-loaded")),
            Q_ARG(QVariant, QVariant(QUrl())));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$never-loaded"));
        QCOMPARE(call("entryCount").toInt(), 1);
    }

    void aKeyThatIsInTheLoadedTimelineStillOpensInsideThatList()
    {
        const QVariantList loaded = m_controller->timeline()->imageEntries();
        QVERIFY(loaded.size() >= 2);
        const QVariantMap first = loaded.first().toMap();
        const QString key = first.value(QStringLiteral("mediaKey")).toString();
        const QUrl url = first.value(QStringLiteral("httpUrl")).toUrl();
        QMetaObject::invokeMethod(m_root, "openFor", Q_ARG(QVariant, key),
                                  Q_ARG(QVariant, QVariant(url)));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("entryCount").toInt(), int(loaded.size()));
    }

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
        QTest::keyClick(m_window, Qt::Key_Down);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        QTest::keyClick(m_window, Qt::Key_Up);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        QTest::keyClick(m_window, Qt::Key_Right);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QTest::keyClick(m_window, Qt::Key_Left);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));

        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY2(!call("isOpen").toBool(),
                 "Escape did not close the viewer, and the decision to take "
                 "click-to-close off the picture rests on it");
    }

    void homeEndAndPageKeysNavigate()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(26))),
            Q_ARG(QVariant, QVariant(10))));
        QVERIFY(call("isOpen").toBool());
        QTest::keyClick(m_window, Qt::Key_End);
        QCOMPARE(call("currentIndex").toInt(), 25);
        QTest::keyClick(m_window, Qt::Key_Home);
        QCOMPARE(call("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_PageDown);
        const int afterPage = call("currentIndex").toInt();
        QVERIFY2(afterPage > 1, "Page Down did not move");
        QTest::keyClick(m_window, Qt::Key_PageUp);
        QCOMPARE(call("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_PageUp);
        QCOMPARE(call("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_End);
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QCOMPARE(call("currentIndex").toInt(), 25);
    }

    void theContextMenuOpensFromTheKeyboard()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(3))),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());
        QVERIFY2(call("menuFound").toBool(),
                 "the harness cannot find the context menu");
        QVERIFY(!call("menuOpened").toBool());
        QTest::keyClick(m_window, Qt::Key_Menu);
        QTRY_VERIFY2(call("menuOpened").toBool(), "Menu key opened nothing");
        QMetaObject::invokeMethod(m_root, "closeMenu");
        QTRY_VERIFY(!call("menuOpened").toBool());
        QTest::keyClick(m_window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY2(call("menuOpened").toBool(),
                     "Shift+F10 opened nothing");
        QMetaObject::invokeMethod(m_root, "closeMenu");
        QTRY_VERIFY(!call("menuOpened").toBool());
    }

    void theWheelOverTheStripScrollsTheStrip()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(40))),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());
        QQuickItem *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->opacity() > 0.99);
        const qreal cell = strip->property("cell").toReal();
        const qreal x0 = strip->property("contentX").toReal();
        const qreal zoom0 = call("zoomNow").toReal();
        const QPointF at = strip->mapToScene(
            QPointF(strip->width() / 2, strip->height() / 2));
        QTest::wheelEvent(m_window, at, QPoint(0, -120));
        QCoreApplication::processEvents();
        QTRY_COMPARE(strip->property("contentX").toReal(), x0 + cell);
        QCOMPARE(call("zoomNow").toReal(), zoom0);
        QTest::wheelEvent(m_window, at, QPoint(0, 120));
        QCoreApplication::processEvents();
        QTRY_COMPARE(strip->property("contentX").toReal(), x0);
    }

    void touchpadPixelDeltasMoveTheStripByWholeCells()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(40))),
            Q_ARG(QVariant, QVariant(0))));
        QQuickItem *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->opacity() > 0.99);
        const qreal cell = strip->property("cell").toReal();
        const QPointF at = strip->mapToScene(
            QPointF(strip->width() / 2, strip->height() / 2));
        for (int i = 0; i < 7; ++i) {
            QTest::wheelEvent(m_window, at, QPoint(0, 0), QPoint(0, -20));
            QCoreApplication::processEvents();
            const qreal x = strip->property("contentX").toReal();
            QVERIFY2(qFuzzyIsNull(std::fmod(x, cell)),
                     qPrintable(QStringLiteral("contentX=%1 is not a whole "
                                               "number of cells").arg(x)));
        }
        QCOMPARE(strip->property("contentX").toReal(), 2 * cell);
    }

    void ctrlWheelOverTheStripStillZooms()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(40))),
            Q_ARG(QVariant, QVariant(0))));
        QQuickItem *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->opacity() > 0.99);
        const qreal x0 = strip->property("contentX").toReal();
        const qreal zoom0 = call("zoomNow").toReal();
        const QPointF at = strip->mapToScene(
            QPointF(strip->width() / 2, strip->height() / 2));
        QTest::wheelEvent(m_window, at, QPoint(0, 120), QPoint(0, 0),
                          Qt::ControlModifier);
        QCoreApplication::processEvents();
        QTRY_VERIFY(call("zoomNow").toReal() != zoom0);
        QCOMPARE(strip->property("contentX").toReal(), x0);
    }

    void aResizeKeepsAScrolledStripPosition()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "resizeTo", Q_ARG(QVariant, QVariant(900)),
            Q_ARG(QVariant, QVariant(700))));
        QTRY_COMPARE(m_window->width(), 900);
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(40))),
            Q_ARG(QVariant, QVariant(0))));
        QQuickItem *strip = m_window->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->opacity() > 0.99);
        const qreal cell = strip->property("cell").toReal();
        const QPointF at = strip->mapToScene(
            QPointF(strip->width() / 2, strip->height() / 2));
        for (int i = 0; i < 5; ++i)
            QTest::wheelEvent(m_window, at, QPoint(0, -120));
        QCoreApplication::processEvents();
        QTRY_COMPARE(strip->property("contentX").toReal(), 5 * cell);
        QTRY_VERIFY(!strip->property("moving").toBool());
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "resizeTo", Q_ARG(QVariant, QVariant(700)),
            Q_ARG(QVariant, QVariant(700))));
        QTRY_COMPARE(m_window->width(), 700);
        QCoreApplication::processEvents();
        QVERIFY2(strip->property("width").toReal() < 800,
                 "the resize did not change the strip's width");
        QCOMPARE(strip->property("contentX").toReal(), 5 * cell);
    }

    void shiftWheelFallsBackToVerticalWhenThereIsNoSidewaysOverflow()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(2))),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "fitTo", Q_ARG(QVariant, QVariant(200)),
            Q_ARG(QVariant, QVariant(8000))));
        QCoreApplication::processEvents();
        QTRY_VERIFY(call("pannable").toBool());
        QTRY_VERIFY(call("settled").toBool());
        QTRY_VERIFY(call("contentH").toReal() > m_window->height());
        QVERIFY2(call("contentW").toReal() <= m_window->width() + 1,
                 "the fixture overflows sideways too");
        QMetaObject::invokeMethod(m_root, "centreContent");
        QCoreApplication::processEvents();
        const qreal y0 = call("contentY").toReal();
        QTest::wheelEvent(m_window, QPointF(m_window->width() / 2.0,
                                            m_window->height() / 3.0),
                          QPoint(0, -120), QPoint(0, 0), Qt::ShiftModifier);
        QCoreApplication::processEvents();
        QTRY_VERIFY(call("contentY").toReal() != y0);
    }

    void shiftWheelPansHorizontally()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(manyEntries(2))),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "fitTo", Q_ARG(QVariant, QVariant(4000)),
            Q_ARG(QVariant, QVariant(4000))));
        QCoreApplication::processEvents();
        QTRY_VERIFY(call("pannable").toBool());
        QTRY_VERIFY(call("settled").toBool());
        QVERIFY(call("centreContent").toBool());
        QCoreApplication::processEvents();
        const QPointF centre(m_window->width() / 2.0,
                             m_window->height() / 2.0);
        const qreal x0 = call("contentX").toReal();
        const qreal y0 = call("contentY").toReal();
        QTest::wheelEvent(m_window, centre, QPoint(0, -120), QPoint(0, 0),
                          Qt::ShiftModifier);
        QCoreApplication::processEvents();
        QTRY_COMPARE(call("contentX").toReal(), x0 + 60.0);
        QCOMPARE(call("contentY").toReal(), y0);
    }

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
        QTRY_VERIFY(strip->isVisible());
        QTRY_VERIFY(strip->opacity() > 0.99);
        auto *content = strip->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const auto findThird = [content]() -> QQuickItem * {
            const auto thumbs = content->childItems();
            for (QQuickItem *thumb : thumbs) {
                const QVariant idx = thumb->property("index");
                if (idx.isValid() && idx.toInt() == 2 && thumb->width() > 0)
                    return thumb;
            }
            return nullptr;
        };
        QQuickItem *third = nullptr;
        QTRY_VERIFY2((third = findThird()) != nullptr,
                     "no delegate for the third thumbnail");
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

    void aTallImageAtFitClearsTheStripAndTheToolbar()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "fitTo", Q_ARG(QVariant, QVariant(600)),
            Q_ARG(QVariant, QVariant(1600))));
        QCoreApplication::processEvents();
        auto *viewer = m_root->findChild<QObject *>(
            QStringLiteral("imageViewerOverlay"));
        auto *strip = m_root->findChild<QQuickItem *>(
            QStringLiteral("viewerThumbnailStrip"));
        auto *flick = m_root->findChild<QQuickItem *>(
            QStringLiteral("imageViewerFlick"));
        QVERIFY(viewer && strip && flick);
        const qreal height = viewer->property("baseHeight").toReal();
        QVERIFY(height > 0);
        const qreal bottom = flick->mapToScene(
            QPointF(0, (flick->height() + height) / 2)).y();
        const qreal stripTop = strip->mapToScene(QPointF(0, 0)).y();
        QVERIFY2(bottom <= stripTop,
                 qPrintable(QStringLiteral("picture ends at %1, strip starts at %2")
                                .arg(bottom).arg(stripTop)));
    }

    void clickingThePictureZoomsItRatherThanClosingTheViewer()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());
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
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre);
        QCoreApplication::processEvents();
        QVERIFY(call("isOpen").toBool());
        QTRY_COMPARE(call("zoomNow").toReal(), 1.0);
    }

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
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
    }

    void oneWheelEventMovesEachAxisExactlyOnce()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());

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
