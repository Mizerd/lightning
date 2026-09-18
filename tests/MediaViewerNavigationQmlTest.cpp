// WHICH PICTURE THE IMAGE VIEWER OPENS, AND WHICH LIST IT PAGES THROUGH.
//
// Reported 2026-09-06: "Opening room information and selecting the media tab
// shows media sent previously in this room. However, when I click on the
// images, it always opens the last one."
//
// The cause was a list mismatch with a substituting fallback on top of it.
// `ImageViewerOverlay.openFor(mediaKey, httpUrl)` built its list from
// `app.timeline.imageEntries()` — the images the open TIMELINE has paginated
// — and searched it for the key it was handed. The Media tab is
// MediaHistoryModel, a separate backwards walk that exists precisely to reach
// media the timeline has never loaded, so the search missed by construction,
// and then:
//
//     if (currentIndex === -1 && entries.length > 0)
//         currentIndex = entries.length - 1
//
// turned "I could not find that" into "here is something else". The user
// clicked one picture and got another.
//
// Two properties are pinned here, and both are behavioural rather than
// structural — they are driven through the real component in a real window:
//
//   1. A caller that knows the list AND the index gets exactly that item, and
//      pages within exactly that list (`openAt`). This is the Media tab.
//   2. A lookup that MISSES opens what was asked for, alone, and never a
//      neighbour (`openFor`). This is the timeline's own entry point.
//
// On the unfixed tree case 1 cannot even dispatch (there is no `openAt`) and
// case 2 opens a different picture.

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
        // The old fallback substituted THE LAST ENTRY OF THE LOADED LIST, so
        // a room with no loaded images could not have exhibited the defect at
        // all. Seed a couple through the mock's own send path so the list the
        // viewer would have substituted from actually exists.
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

    // THE MEDIA TAB'S CALL. The viewer opens the index it is given, in the
    // list it is given, and pages inside that list — never inside the loaded
    // timeline, which is a different set of pictures.
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
        // NAVIGATION WRAPS, as of 2026-09-17. This used to read "out of
        // bounds is refused rather than clamped onto a neighbour", and it
        // stayed green through the change that made `showAt` wrap — by
        // arithmetic coincidence, not because the contract held. With three
        // entries at index 0, `3 % 3 == 0 == currentIndex`, so the early
        // return fired and `$a` was still correct for the wrong reason.
        //
        // Both directions are pinned now, and past the coincidence, so the
        // wrap is actually asserted rather than accidentally survived.
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(3)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(4)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        // Back to the first, then backwards past it: the last entry, not
        // nothing. JS `%` keeps the DIVIDEND's sign, so -1 % 3 is -1 rather
        // than 2 — which is why showAt normalises twice, and why asserting
        // this direction is worth the two lines.
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(0)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(-1)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
    }

    // An index the list cannot hold opens nothing at all. Opening "something
    // near it" is the exact behaviour this whole suite exists to forbid.
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

    // THE TIMELINE'S CALL, ON A MISS. The room has loaded images; the key
    // asked for is not one of them. The viewer must open THAT key and nothing
    // else — on the unfixed tree it opened the last loaded image instead.
    void aKeyThatIsNotInTheLoadedTimelineNeverOpensADifferentPicture()
    {
        // The fixture room must actually hold images, or this case proves
        // nothing: the substituting fallback needed a non-empty list.
        const QVariantList loaded = m_controller->timeline()->imageEntries();
        QVERIFY2(loaded.size() >= 2,
                 "the fixture room has too few images for the old fallback to "
                 "have had anything to substitute");

        QMetaObject::invokeMethod(
            m_root, "openFor", Q_ARG(QVariant, QStringLiteral("$never-loaded")),
            Q_ARG(QVariant, QVariant(QUrl())));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$never-loaded"));
        // And there is nothing to page to, which is the truthful state for a
        // row the loaded list does not contain.
        QCOMPARE(call("entryCount").toInt(), 1);
    }

    // The hit path is unchanged: a key the timeline DOES hold opens that row
    // inside the timeline's list, so previous/next still walk the room.
    void aKeyThatIsInTheLoadedTimelineStillOpensInsideThatList()
    {
        const QVariantList loaded = m_controller->timeline()->imageEntries();
        QVERIFY(loaded.size() >= 2);
        const QVariantMap first = loaded.first().toMap();
        const QString key = first.value(QStringLiteral("mediaKey")).toString();
        const QUrl url = first.value(QStringLiteral("httpUrl")).toUrl();
        // The mock's seeded rows may be addressed by key or by URL depending
        // on the backend; openFor accepts either, exactly as the delegate
        // calls it.
        QMetaObject::invokeMethod(m_root, "openFor", Q_ARG(QVariant, key),
                                  Q_ARG(QVariant, QVariant(url)));
        QVERIFY(call("isOpen").toBool());
        QCOMPARE(call("entryCount").toInt(), int(loaded.size()));
    }

    // ── 2026-09-18: the viewer had no keyboard at all ────────────────────
    //
    // Every key this overlay declares — Left/Right, Up/Down/Space,
    // +/-/0/F, and Escape through `closePolicy: Popup.CloseOnEscape` — was
    // dead, because `Popup.focus` defaults to FALSE and this one never set
    // it. `contentItem: FocusScope { focus: true }` cannot help: a focus
    // scope inside a popup that never takes active focus never becomes the
    // active focus item either. The sibling VideoViewerOverlay, written to
    // the same pattern, sets `focus: true` and works.
    //
    // Found on a real build: clicking the next ARROW advanced the counter
    // 1 -> 2 -> 3 while Right, Down, Space, plus and Escape all left it at
    // "3 of 5", with the window's X input focus confirmed and Ctrl+K proving
    // in the same session that keys reached the application.
    //
    // Escape is the half that matters most: the round that took
    // click-to-close off the picture justified it by "closing is still
    // instant everywhere else — the scrim, Escape, the close button", and
    // Escape was not one of them.
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
        // Down and Space are "next", Up is "previous" — one axis is not
        // enough for someone who has just been scrolling a list.
        QTest::keyClick(m_window, Qt::Key_Down);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        QTest::keyClick(m_window, Qt::Key_Up);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$b"));
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));
        // ...and it wraps, at the end as at the start.
        QTest::keyClick(m_window, Qt::Key_Right);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
        QTest::keyClick(m_window, Qt::Key_Left);
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$c"));

        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY2(!call("isOpen").toBool(),
                 "Escape did not close the viewer, and the decision to take "
                 "click-to-close off the picture rests on it");
    }

    // ── 2026-09-18: a thumbnail click CLOSED the viewer ──────────────────
    //
    // The strip is the one piece of viewer chrome built out of bare
    // TapHandlers, and a TapHandler on its default `DragThreshold` policy
    // never takes an exclusive grab. Handlers are non-exclusive across
    // subtrees, so the scrim's close handler fired on the same press: the
    // picture was selected and the viewer shut underneath it. The strip's
    // own swallow handler — added so a miss BETWEEN two 48px thumbnails
    // would do nothing rather than close — swallowed nothing for the same
    // reason.
    //
    // The toolbar was never affected (a Control's MouseArea accepts the
    // press) and neither was the picture (`imageTap` already asks for
    // WithinBounds), which is exactly why this survived: the two surfaces a
    // reviewer would try both worked.
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
        // The strip fades in over 180ms and carries `visible: opacity > 0`,
        // so a click sent before that lands on the scrim and closes the
        // viewer for a reason that has nothing to do with the defect. Wait
        // for the state the user is actually clicking in.
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
        // Its REAL geometry, mapped to the window — never a fabricated point.
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

    // ── The PICTURE's own tap: it zooms, and it does NOT close ──────────
    //
    // The headline of the click-to-zoom round, and it had no behavioural
    // coverage at all — the only other gate reads this file as TEXT and
    // asserts the band check exists, which cannot see a second handler
    // firing on the same press. An audit raised it because the thumbnail
    // fix's own commit message claims "a TapHandler never suppresses a
    // handler on an ancestor", which would make this impossible. That claim
    // was wrong: `imageTap` asks for `gesturePolicy: WithinBounds`, takes
    // the exclusive grab, and the scrim's close handler never sees the tap.
    // This case is what keeps the two facts from drifting apart again.
    void clickingThePictureZoomsItRatherThanClosingTheViewer()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());
        // A picture with a real drawn size, so the tap lands INSIDE the band
        // `imageTap` checks rather than on the holder's margin — where
        // closing is the correct answer and this case would prove nothing.
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

    // The strip's own swallow surface, which exists because the targets are
    // 48px and a MISS between two of them is the likeliest click in the whole
    // viewer. It had the same defect as the thumbnails themselves — a bare
    // TapHandler that swallowed nothing — so a near miss closed the viewer.
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
        // The real gap between two real delegates — 4px of strip, and the
        // only part of it that is not a thumbnail.
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
        // ...and it selected nothing either.
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
    }


    // ── ONE WHEEL EVENT MOVES EACH AXIS ONCE ────────────────────────────
    //
    // I nearly shipped a second WheelHandler here. `WheelHandler.orientation`
    // is a single `Qt::Orientation` and DEFAULTS TO VERTICAL, so the one
    // handler's `angleDelta.x` branch never sees a horizontal-ONLY event —
    // which reads like a missing feature. Measured with this case run alone,
    // it is not: a zoomed picture makes the Flickable `interactive` and
    // QQuickFlickable handles that axis itself, moving contentX 710 -> 781.8
    // on one notch.
    //
    // That measurement is deliberately NOT asserted, and the reason belongs
    // here: a synthesized click on `imageTap` — which holds an EXCLUSIVE
    // grab, that being exactly why the picture zooms without the scrim
    // closing the viewer — leaves the grab behind when the popup closes, and
    // the Flickable never sees another wheel event for the life of the
    // process. Bisected to that one case; a flush click and a pointer move
    // both failed to clear it. An assertion that passes alone and fails in
    // the suite is worse than no assertion.
    //
    // What IS asserted is the property the second handler would have broken:
    // a DIAGONAL event is ONE event carrying both deltas, and a handler pair
    // that each applied the pair moves each axis TWICE. Proven — with that
    // second handler added, contentX moves 80 where the contract is 40.
    void oneWheelEventMovesEachAxisExactlyOnce()
    {
        const QVariantList history = { historyEntry(QStringLiteral("$a")),
                                       historyEntry(QStringLiteral("$b")) };
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QVariant(history)),
            Q_ARG(QVariant, QVariant(0))));
        QVERIFY(call("isOpen").toBool());

        // A picture bigger than the viewport in BOTH directions, parked in
        // the middle of its range so a move either way is observable.
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
        // pixelDelta on both axes: what a trackpad sends, as ONE event.
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
