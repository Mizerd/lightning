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
        // Out of bounds is refused rather than clamped onto a neighbour.
        QMetaObject::invokeMethod(m_root, "showAt", Q_ARG(QVariant, QVariant(3)));
        QCOMPARE(call("currentKey").toString(), QStringLiteral("$a"));
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

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MediaViewerNavigationQmlTest)
#include "MediaViewerNavigationQmlTest.moc"
