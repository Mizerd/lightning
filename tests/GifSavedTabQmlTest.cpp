// 2026-08-28 tester round — three reported GIF-picker defects, each driven
// through the REAL qml/GifPicker.qml with the REAL GifSearchController rather
// than asserted from a source scan.
//
// The reports (all against the packaged AppImage):
//
//  1. "Saving GIFs does not work" — the Saved tab is empty while a Recent tile
//     plainly carries a filled star. The filled star means
//     GifFavoritesModel::isFavorite() answered true for that provider id, so
//     the merged GifSavedModel must have had at least one row; and the
//     screenshot shows NO empty-state copy either, which the picker renders
//     whenever gif.saved.count === 0. Both halves of that contradiction are
//     covered here: a PROVIDER FAVOURITE must render as a tile on the Saved
//     tab, and a genuinely empty Saved tab must SAY it is empty.
//
//     Why nothing caught it: GifPickerSelectionQmlTest::savedTabBindsTheMerged-
//     ModelNotResults clears favorites first and exercises only the LOCAL
//     (GifStarredStore) half of the merge, and GifCollectionsTest exercises the
//     C++ model with no QML engine at all. The provider half had never been
//     rendered by any test.
//
//  2. "Resizing the picker grabs the chat behind it" — dragging the corner grip
//     scrolls the timeline underneath. The picker stopped being modal in
//     ecb2604 (correctly: modality was never the press barrier), and that
//     commit's own message notes the grabbed overlay was what had been
//     stopping the timeline from scrolling. PopupResizeGrip drives the resize
//     from a DragHandler, and a pointer handler GRABS a point without
//     ACCEPTING the event, so the press keeps walking the hit list — the exact
//     mechanism that commit documents for the emoji cells. This drives real
//     QTest mouse events at the grip's own centre over a real Flickable and
//     asserts the Flickable did not move.
//
//  3. Tab alignment — the Saved/Recent segment must sit flush RIGHT while
//     GIPHY/KLIPY stay left. Asserted as geometry (right edges within a pixel
//     of each other at two different picker widths), never as a pixel offset,
//     because the picker is resizable.

#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifRecentModel.h"
#include "gif/GifResultModel.h"
#include "gif/GifSavedModel.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

// The validated https provider-CDN shape GifStoredModel accepts (mirrors
// GifPickerSelectionQmlTest's favoriteFixture).
QVariantMap favoriteFixture(const QString &provider, const QString &id)
{
    const QString host = provider == QStringLiteral("giphy")
        ? QStringLiteral("media.giphy.com")
        : QStringLiteral("static.klipy.com");
    QVariantMap m;
    m.insert(QStringLiteral("provider"), provider);
    m.insert(QStringLiteral("gifId"), id);
    m.insert(QStringLiteral("title"), QStringLiteral("fixture %1").arg(id));
    m.insert(QStringLiteral("gifUrl"),
             QStringLiteral("https://%1/%2/original.gif").arg(host, id));
    // previewUrl/stillUrl are deliberately LEFT EMPTY: a real provider URL
    // starts an async network fetch this sandboxed process can never finish,
    // and its in-flight state races the window teardown (see the long note in
    // GifPickerSelectionQmlTest::safeResult). Nothing asserted here depends on
    // a decoded image — only on the delegate existing and carrying the row's
    // identity.
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}

// A browse-grid row with deliberately EMPTY preview/still URLs, for the same
// teardown-safety reason as favoriteFixture above.
gif::GifResult safeResult(const QString &provider, const QString &id)
{
    gif::GifResult r;
    r.provider = provider;
    r.id = id;
    r.title = QStringLiteral("fixture %1").arg(id);
    r.rating = QStringLiteral("g");
    r.gifUrl = QStringLiteral("https://media.giphy.com/media/%1/giphy.gif").arg(id);
    r.gifWidth = 100;
    r.gifHeight = 100;
    return r;
}

// GifPicker.qml reads app.gif and app.settings.gifAutoplay and nothing else.
class FakeGifSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int gifAutoplay MEMBER gifAutoplay)
public:
    explicit FakeGifSettings(QObject *parent = nullptr) : QObject(parent) {}
    int gifAutoplay = 2; // Never — keep AnimatedImage decode out of a headless test

    Q_INVOKABLE int pickerWidthShare(const QString &id) const
    { return m_sizes.value(id + QStringLiteral("/w"), 0); }
    Q_INVOKABLE int pickerHeightShare(const QString &id) const
    { return m_sizes.value(id + QStringLiteral("/h"), 0); }
    Q_INVOKABLE void setPickerShare(const QString &id, int w, int h)
    {
        m_sizes.insert(id + QStringLiteral("/w"), w);
        m_sizes.insert(id + QStringLiteral("/h"), h);
    }

private:
    QHash<QString, int> m_sizes;
};

class FakeGifApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(GifSearchController *gif READ gif CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
public:
    explicit FakeGifApp(GifSearchController *g, QObject *parent = nullptr)
        : QObject(parent), m_gif(g), m_settings(new FakeGifSettings(this))
    {
    }
    GifSearchController *gif() const { return m_gif; }
    QObject *settings() const { return m_settings; }

private:
    GifSearchController *m_gif;
    FakeGifSettings *m_settings;
};

// A composer-shaped anchor at the bottom of a real window, with a REAL
// Flickable filling the room above it — the timeline the picker floats over.
// The Flickable is what report 2 is about: it must not move while the grip is
// being dragged.
const char *kTimelineScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    // Anything the picker fails to consume lands here, which is what the
    // report calls "the chat behind it".
    property int pressesBehind: 0

    Flickable {
        id: fakeTimeline
        objectName: "fakeTimeline"
        anchors.fill: parent
        contentWidth: width
        contentHeight: 8000
        interactive: true
        Rectangle {
            width: fakeTimeline.width
            height: 8000
            color: "#101018"
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: win.pressesBehind++
            }
        }
    }

    Rectangle {
        id: composerBar
        objectName: "composerBar"
        color: "#202030"
        height: 60
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
    }

    GifPicker {
        id: picker
        objectName: "picker"
        target: "room"
        anchorItem: composerBar
    }
}
)QML";

} // namespace

class GifSavedTabQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;

    // Loads kTimelineScene with `gif` bound as app.gif and opens the picker.
    // Returns the scene root; `warnings` collects every engine warning so a
    // swallowed binding TypeError (the v0.6.6 failure mode this whole area
    // keeps reproducing) shows up as a test failure rather than as an empty
    // panel nobody can explain.
    QObject *openPicker(QQmlApplicationEngine &engine, FakeGifApp &fakeApp,
                        QStringList &warnings, QQuickWindow **windowOut,
                        QObject **pickerOut)
    {
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        auto *component = new QQmlComponent(&engine);
        component->setData(QByteArray(kTimelineScene),
                           QUrl(QStringLiteral("giftimelinescene.qml")));
        QObject *root = component->create();
        if (!root) {
            qWarning("%s", qPrintable(component->errorString()));
            return nullptr;
        }
        auto *window = qobject_cast<QQuickWindow *>(root);
        if (!window || !QTest::qWaitForWindowExposed(window))
            return nullptr;
        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        if (!picker)
            return nullptr;
        if (!QMetaObject::invokeMethod(picker, "open"))
            return nullptr;
        *windowOut = window;
        *pickerOut = picker;
        return root;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("gif-saved-tab-test"));
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // REPORT 1 — THE ACTUAL DEFECT, and the only assertion here that fails on
    // the unfixed tree under the dev shell's own Qt.
    //
    // QConcatenateTablesProxyModel does not forward its sources' role names on
    // every Qt this project ships against (measured with an identical probe
    // and identical source models):
    //
    //   Qt 6.11.1, nix dev shell        -> the sources' roles PLUS Qt's 6 defaults
    //   Qt 6.8.2, debian:13.6-slim      -> Qt's 6 defaults and NOTHING else
    //                                      (the deb/rpm/flatpak/AppImage base)
    //
    // GifPicker.qml's tile resolves thirteen REQUIRED properties by role name,
    // and a required property the model cannot supply makes QQmlDelegateModel
    // refuse to build the delegate — so the packaged Saved tab drew no tiles
    // while count() was correct, and a correct non-zero count is also what
    // kept the "No saved GIFs yet" overlay empty. A blank panel saying
    // nothing: the report exactly.
    //
    // Asserting EQUALITY, not containment, is what gives this teeth on 6.11:
    // the unfixed 6.11 answer is a strict superset (21 entries against 15), so
    // an "are the names present?" check would pass there and the defect would
    // stay invisible until the next release build.
    void savedModelAnswersTheSameRoleTableAsItsSources()
    {
        GifSearchController gif;
        const QHash<int, QByteArray> shared = GifResultModel().roleNames();
        QCOMPARE(gif.favorites()->roleNames(), shared);
        QCOMPARE(gif.starredStore()->model()->roleNames(), shared);
        // The merged view must speak the SAME table — same names, same
        // numbers — or the numeric role the base class forwards unremapped
        // means something different at each end.
        QCOMPARE(gif.saved()->roleNames(), shared);
        // Spelled out for the roles the delegate actually requires, so a
        // future trim of GifResultModel's table cannot quietly drop one.
        for (const QByteArray &name :
             { QByteArrayLiteral("provider"), QByteArrayLiteral("gifId"),
               QByteArrayLiteral("title"), QByteArrayLiteral("rating"),
               QByteArrayLiteral("previewUrl"), QByteArrayLiteral("stillUrl"),
               QByteArrayLiteral("gifUrl"), QByteArrayLiteral("gifWidth"),
               QByteArrayLiteral("gifHeight"), QByteArrayLiteral("previewWidth"),
               QByteArrayLiteral("previewHeight"), QByteArrayLiteral("favorite"),
               QByteArrayLiteral("gifBytes") }) {
            QVERIFY2(gif.saved()->roleNames().values().contains(name),
                     qPrintable(QStringLiteral(
                         "the merged Saved model does not expose the '%1' role "
                         "the picker's tile requires")
                                    .arg(QString::fromUtf8(name))));
        }
    }

    // REPORT 1a. A star pressed on a GIPHY/KLIPY tile is a provider bookmark
    // in GifFavoritesModel — one of the two sources GifSavedModel merges. It
    // must render as a real tile on the Saved tab.
    //
    // The starred (local byte) store is deliberately NOT opened here: that is
    // the ordinary state of a session where the user has only ever saved
    // provider GIFs, and it is the half no existing test covers.
    void savedTabRendersAProviderFavourite()
    {
        GifSearchController gif;
        // Favorites persist to the process QSettings, so start from a known
        // empty group: a leftover row from another case would make the count
        // assertion below pass for the wrong reason.
        gif.favorites()->clearAll();
        QCOMPARE(gif.saved()->count(), 0);

        const QVariantMap fixture =
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("fav1"));
        QVERIFY(gif.toggleFavorite(fixture));
        QCOMPARE(gif.favorites()->rowCount(), 1);
        // The C++ merge is the precondition, not the thing under test — if
        // this fails the defect is in GifSavedModel, not in the picker.
        QCOMPARE(gif.saved()->count(), 1);

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());

        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("saved"));

        // activeModel must BE the merged model — a binding that threw would
        // silently leave it at gif.results (Qt swallows the exception).
        QObject *activeModel =
            QQmlProperty::read(picker, QStringLiteral("activeModel"))
                .value<QObject *>();
        QCOMPARE(activeModel, static_cast<QObject *>(gif.saved()));

        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        // A count is not a rendered row: read the delegate itself and check it
        // carries the favourite's own identity.
        QQuickItem *tile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tile),
                        Q_ARG(int, 0))
                    && tile != nullptr);
        QCOMPARE(QQmlProperty::read(tile, QStringLiteral("provider")).toString(),
                 QStringLiteral("giphy"));
        QCOMPARE(QQmlProperty::read(tile, QStringLiteral("gifId")).toString(),
                 QStringLiteral("fav1"));
        // And the tile knows it is saved, so its star reads as saved on the
        // one tab where every row is saved by definition.
        QVERIFY(QQmlProperty::read(tile, QStringLiteral("saved")).toBool());

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // REPORT 1b. The screenshot shows an empty Saved tab with NO copy on it.
    // The picker owns an empty state for exactly this case, so its absence is
    // itself a defect — and the same missing evidence is what makes "saving
    // does not work" indistinguishable from "nothing has been saved yet".
    void savedTabSaysSoWhenNothingIsSaved()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();
        QCOMPARE(gif.saved()->count(), 0);

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());

        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();

        auto *overlayText =
            picker->findChild<QQuickItem *>(QStringLiteral("gifStateOverlayText"));
        QVERIFY2(overlayText != nullptr,
                 "the picker has no reachable state-overlay label");
        auto *overlay =
            picker->findChild<QQuickItem *>(QStringLiteral("gifStateOverlay"));
        QVERIFY(overlay != nullptr);

        const QString text =
            QQmlProperty::read(overlayText, QStringLiteral("text")).toString();
        QVERIFY2(text.contains(QStringLiteral("No saved GIFs yet")),
                 qPrintable(QStringLiteral("empty Saved tab said: '%1'").arg(text)));
        QVERIFY2(overlay->isVisible(),
                 "the empty-state overlay is not visible on an empty Saved tab");
        QVERIFY(overlay->width() > 0 && overlay->height() > 0);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // REPORT 1c. The reporter's ACTUAL sequence, which the two cases above
    // skip: browse a provider (the grid fills with trending), star a tile
    // WHILE the picker is open, and only then look at Saved. Both earlier
    // cases populate the store before the picker exists, so neither exercises
    // the live rowsInserted path through the merge proxy, nor a GridView whose
    // model is swapped from a big populated one to a one-row one.
    void savedTabFillsAfterBrowsingAndStarringWithThePickerOpen()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();
        gif.recent()->clearAll();

        // The browse grid the user was looking at.
        QList<gif::GifResult> trending;
        for (int i = 0; i < 12; ++i)
            trending.append(safeResult(QStringLiteral("giphy"),
                                       QStringLiteral("trend%1").arg(i)));
        gif.results()->reset(trending);
        QCOMPARE(gif.results()->count(), 12);
        // And a recents list, exactly as the screenshot shows.
        gif.recordSent(
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("sent1")));
        QCOMPARE(gif.recent()->rowCount(), 1);

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());
        // The picker opens on the active PROVIDER tab, showing the grid.
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("giphy"));
        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 12);

        // Recent, then the star on a tile — the picker's own toggleSaved(),
        // not a controller call, so the QML path is what is exercised.
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("recent"));
        QCoreApplication::processEvents();
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);
        QQuickItem *recentTile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, recentTile),
                        Q_ARG(int, 0))
                    && recentTile != nullptr);
        QVERIFY(!QQmlProperty::read(recentTile, QStringLiteral("saved")).toBool());
        QVariant snapshot;
        QVERIFY(QMetaObject::invokeMethod(recentTile, "snapshot",
                                          Q_RETURN_ARG(QVariant, snapshot)));
        QVERIFY(QMetaObject::invokeMethod(picker, "toggleSaved",
                                          Q_ARG(QVariant, snapshot)));
        // The star fills — this is what the screenshot shows.
        QTRY_VERIFY(QQmlProperty::read(recentTile, QStringLiteral("saved")).toBool());
        QCOMPARE(gif.favorites()->rowCount(), 1);

        // Now the Saved tab must show that exact GIF.
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();
        QCOMPARE(gif.saved()->count(), 1);
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);
        QQuickItem *savedTile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, savedTile),
                        Q_ARG(int, 0))
                    && savedTile != nullptr);
        QCOMPARE(
            QQmlProperty::read(savedTile, QStringLiteral("gifId")).toString(),
            QStringLiteral("sent1"));

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // REPORT 1d. The same list, but reached through the REAL AppController
    // rather than a test double for `app` — the one remaining difference
    // between every existing saved-tab test and the packaged build the report
    // came from.
    void savedTabRendersThroughTheRealAppController()
    {
        AppController controller(AppController::MockBackend);
        auto *gif = controller.gif();
        QVERIFY(gif != nullptr);
        gif->favorites()->clearAll();
        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("klipy"), QStringLiteral("real1"))));
        QCOMPARE(gif->saved()->count(), 1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kTimelineScene),
                          QUrl(QStringLiteral("giftimelinescene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        QVERIFY(picker != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();
        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);
        QQuickItem *tile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tile),
                        Q_ARG(int, 0))
                    && tile != nullptr);
        QCOMPARE(QQmlProperty::read(tile, QStringLiteral("gifId")).toString(),
                 QStringLiteral("real1"));

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // REPORT 2. A real press-drag-release on the resize grip, over a real
    // interactive Flickable. Two things must both hold: the picker resizes,
    // and the Flickable behind it does not move a pixel.
    //
    // Run on the SAVED tab: on a provider tab the grip's hit area deliberately
    // overlaps the search field's rounded corner, and a TextField accepts the
    // press itself — which would mask a missing barrier. The local lists have
    // no search field, so the press lands on the picker's chrome, which is
    // exactly where a leak escapes.
    void draggingTheResizeGripDoesNotScrollTheTimeline_data()
    {
        QTest::addColumn<QString>("tab");
        // The local list has no search field, so the grip's press lands on the
        // picker's bare chrome — where a missing barrier escapes.
        QTest::newRow("saved tab") << QStringLiteral("saved");
        // A provider tab puts the search field under the grip's hit area. The
        // grip's own comment claims a plain click still reaches the field and
        // only a real drag resizes; this is the case that checks it.
        QTest::newRow("provider tab") << QStringLiteral("giphy");
    }

    void draggingTheResizeGripDoesNotScrollTheTimeline()
    {
        QFETCH(QString, tab);
        GifSearchController gif;
        gif.favorites()->clearAll();

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());
        QQmlProperty::write(picker, QStringLiteral("tab"), tab);
        QCoreApplication::processEvents();

        auto *timeline =
            root->findChild<QQuickItem *>(QStringLiteral("fakeTimeline"));
        QVERIFY(timeline != nullptr);
        // Park the timeline mid-content so a drag in EITHER direction can move
        // it — starting at 0 with StopAtBounds would hide a downward steal.
        QQmlProperty::write(timeline, QStringLiteral("contentY"), 400.0);
        QCoreApplication::processEvents();
        const qreal timelineBefore =
            QQmlProperty::read(timeline, QStringLiteral("contentY")).toReal();
        QCOMPARE(timelineBefore, 400.0);

        auto *grip =
            picker->findChild<QQuickItem *>(QStringLiteral("popupResizeGrip"));
        QVERIFY(grip != nullptr);
        QVERIFY(grip->width() > 0 && grip->height() > 0);

        const qreal widthBefore = picker->property("width").toReal();
        const qreal heightBefore = picker->property("height").toReal();

        const QPoint start =
            grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2))
                .toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        // Well past both the drag threshold and any single-step guard, in
        // several moves — one giant jump can be dropped as a teleport.
        for (int step = 1; step <= 6; ++step) {
            QTest::mouseMove(window, start + QPoint(-10 * step, -12 * step));
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                            start + QPoint(-60, -72));
        QCoreApplication::processEvents();

        const qreal timelineAfter =
            QQmlProperty::read(timeline, QStringLiteral("contentY")).toReal();
        QVERIFY2(qFuzzyCompare(timelineAfter, timelineBefore),
                 qPrintable(QStringLiteral(
                     "the timeline behind the picker moved from %1 to %2 while "
                     "the resize grip was being dragged")
                                .arg(timelineBefore)
                                .arg(timelineAfter)));
        QVERIFY2(!QQmlProperty::read(timeline, QStringLiteral("moving")).toBool(),
                 "the timeline behind the picker was left flicking");
        // The press itself must never have reached the chat at all — the
        // scroll is only the most visible consequence of it leaking.
        QCOMPARE(root->property("pressesBehind").toInt(), 0);

        // And the gesture did what it exists for.
        const qreal widthAfter = picker->property("width").toReal();
        const qreal heightAfter = picker->property("height").toReal();
        QVERIFY2(widthAfter > widthBefore + 20
                     || heightAfter > heightBefore + 20,
                 qPrintable(QStringLiteral("the grip drag did not resize: "
                                           "%1x%2 -> %3x%4")
                                .arg(widthBefore).arg(heightBefore)
                                .arg(widthAfter).arg(heightAfter)));

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // REPORT 2, the part the drag case above cannot reach. The drag case
    // presses at the grip's own centre, which is comfortably inside the panel
    // and has always worked. What the user hits is the EDGE: the grip is the
    // one affordance that invites the pointer onto the picker's outermost
    // corner, and a press outside the popup's item rect both closes the picker
    // and keeps walking down to the chat, which then flicks with the drag.
    //
    // So this presses at a range of offsets around the VISIBLE PANEL CORNER —
    // the thing a person aims at — and requires that a near miss still resizes,
    // still leaves the chat untouched, and does not dismiss the picker.
    //
    // Positive is inside the panel, negative outside it. Each offset gets a
    // FRESH scene, because a leak also closes the picker and would poison the
    // next reading.
    void aNearMissOnTheResizeGripStillResizesThePicker_data()
    {
        QTest::addColumn<int>("offset");
        QTest::newRow("+6 well inside") << 6;
        QTest::newRow("+1 just inside") << 1;
        QTest::newRow("0 on the corner") << 0;
        QTest::newRow("-3 the reported miss") << -3;
        QTest::newRow("-6 the band's edge") << -6;
    }

    void aNearMissOnTheResizeGripStillResizesThePicker()
    {
        QFETCH(int, offset);
        GifSearchController gif;
        gif.favorites()->clearAll();
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();

        auto *timeline =
            root->findChild<QQuickItem *>(QStringLiteral("fakeTimeline"));
        QVERIFY(timeline != nullptr);
        QQmlProperty::write(timeline, QStringLiteral("contentY"), 400.0);
        QCoreApplication::processEvents();

        auto *panel =
            picker->findChild<QQuickItem *>(QStringLiteral("gifPickerPanel"));
        QVERIFY2(panel != nullptr, "the picker's visible panel must be findable "
                                   "— the offsets here are measured from ITS "
                                   "corner, not from the popup's item rect");
        const QPointF origin = panel->mapToScene(QPointF(0, 0));
        const qreal widthBefore = picker->property("width").toReal();

        const QPoint start = (origin + QPointF(offset, offset)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        for (int i = 1; i <= 6; ++i) {
            QTest::mouseMove(window, start + QPoint(-10 * i, -12 * i));
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                            start + QPoint(-60, -72));
        QCoreApplication::processEvents();

        const QString where =
            QStringLiteral("%1px from the panel corner").arg(offset);
        QVERIFY2(root->property("pressesBehind").toInt() == 0,
                 qPrintable(QStringLiteral("%1: the press reached the chat")
                                .arg(where)));
        QCOMPARE(QQmlProperty::read(timeline, QStringLiteral("contentY")).toReal(),
                 400.0);
        QVERIFY2(picker->property("opened").toBool(),
                 qPrintable(QStringLiteral("%1: the picker was dismissed by a "
                                           "press meant for its resize grip")
                                .arg(where)));
        QVERIFY2(picker->property("width").toReal() > widthBefore + 20,
                 qPrintable(QStringLiteral("%1: the drag did not resize "
                                           "(%2 -> %3)")
                                .arg(where).arg(widthBefore)
                                .arg(picker->property("width").toReal())));

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void gripGeometryDiagnostic()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();

        auto *grip =
            picker->findChild<QQuickItem *>(QStringLiteral("popupResizeGrip"));
        QVERIFY(grip != nullptr);
        auto *popupItem =
            picker->property("contentItem").value<QQuickItem *>();
        QVERIFY(popupItem != nullptr);
        QQuickItem *popupRoot = popupItem->parentItem();
        QVERIFY(popupRoot != nullptr);
        const QPointF gripScene = grip->mapToScene(QPointF(0, 0));
        const QPointF popupScene = popupRoot->mapToScene(QPointF(0, 0));
        auto *panel =
            picker->findChild<QQuickItem *>(QStringLiteral("gifPickerPanel"));
        QVERIFY(panel != nullptr);
        const QPointF panelScene = panel->mapToScene(QPointF(0, 0));
        qInfo("panel scene=(%g,%g) %gx%g", panelScene.x(), panelScene.y(),
              panel->width(), panel->height());
        qInfo("padding=%g  popupItem scene=(%g,%g) %gx%g  grip scene=(%g,%g) %gx%g "
              " arcCentre=%g outerRadius=%g",
              picker->property("padding").toReal(),
              popupScene.x(), popupScene.y(), popupRoot->width(), popupRoot->height(),
              gripScene.x(), gripScene.y(), grip->width(), grip->height(),
              grip->property("arcCentre").toReal(),
              grip->property("outerRadius").toReal());

        auto *timeline =
            root->findChild<QQuickItem *>(QStringLiteral("fakeTimeline"));
        QVERIFY(timeline != nullptr);
        delete root;

        // Map the failure region: one fresh scene per offset, because a leak
        // also closes the picker.
        for (int d : { 6, 4, 2, 1, 0, -1, -2, -3, -6 }) {
            GifSearchController g2;
            g2.favorites()->clearAll();
            FakeGifApp app2(&g2);
            QQmlApplicationEngine e2;
            QStringList w2;
            QQuickWindow *win2 = nullptr;
            QObject *p2 = nullptr;
            QObject *r2 = openPicker(e2, app2, w2, &win2, &p2);
            QVERIFY(r2 != nullptr);
            QTRY_VERIFY(p2->property("opened").toBool());
            QQmlProperty::write(p2, QStringLiteral("tab"),
                                QStringLiteral("saved"));
            QCoreApplication::processEvents();
            auto *t2 = r2->findChild<QQuickItem *>(QStringLiteral("fakeTimeline"));
            QQmlProperty::write(t2, QStringLiteral("contentY"), 400.0);
            // Measured from the VISIBLE PANEL corner, which is what the user
            // aims at — not from the popup's item rect, which the insets make
            // a different thing.
            auto *panel2 =
                p2->findChild<QQuickItem *>(QStringLiteral("gifPickerPanel"));
            QVERIFY(panel2 != nullptr);
            const QPointF origin = panel2->mapToScene(QPointF(0, 0));
            const qreal wBefore = p2->property("width").toReal();
            const QPoint s = (origin + QPointF(d, d)).toPoint();
            QTest::mousePress(win2, Qt::LeftButton, Qt::NoModifier, s);
            for (int i = 1; i <= 6; ++i) {
                QTest::mouseMove(win2, s + QPoint(-10 * i, -12 * i));
                QCoreApplication::processEvents();
            }
            QTest::mouseRelease(win2, Qt::LeftButton, Qt::NoModifier,
                                s + QPoint(-60, -72));
            QCoreApplication::processEvents();
            qInfo("offset %+d -> chatMoved=%d pressesBehind=%d opened=%d "
                  "resized=%d",
                  d,
                  QQmlProperty::read(t2, QStringLiteral("contentY")).toReal() != 400.0,
                  r2->property("pressesBehind").toInt(),
                  p2->property("opened").toBool() ? 1 : 0,
                  p2->property("width").toReal() != wBefore);
            delete r2;
        }
    }

    // REPORT 3. The Saved/Recent segment is flush RIGHT in the nav row while
    // the provider segment stays left. Asserted as geometry at TWO widths, so
    // it cannot be satisfied by a hardcoded offset that only holds at the
    // default picker size.
    void listTabsSitFlushRightAtEveryPickerWidth()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());

        auto *providerTabs =
            picker->findChild<QQuickItem *>(QStringLiteral("gifProviderTabs"));
        auto *listTabs =
            picker->findChild<QQuickItem *>(QStringLiteral("gifListTabs"));
        QVERIFY(providerTabs != nullptr && listTabs != nullptr);
        // The row both live in — their common parent — is the reference for
        // "flush right"; reading the picker's width instead would fold in the
        // padding and turn this into an arithmetic assertion about margins.
        QQuickItem *navRow = listTabs->parentItem();
        QVERIFY(navRow != nullptr);
        QCOMPARE(providerTabs->parentItem(), navRow);

        const auto checkFlushRight = [&](const char *what) {
            QCoreApplication::processEvents();
            const qreal listRight = listTabs->x() + listTabs->width();
            QVERIFY2(qAbs(listRight - navRow->width()) < 1.5,
                     qPrintable(QStringLiteral(
                         "%1: list tabs end at %2, nav row is %3 wide")
                                    .arg(QString::fromLatin1(what))
                                    .arg(listRight)
                                    .arg(navRow->width())));
            // The provider strip stays at the left edge, and the two groups do
            // not overlap.
            QVERIFY2(providerTabs->x() < 1.5,
                     qPrintable(QStringLiteral("%1: provider tabs at x=%2")
                                    .arg(QString::fromLatin1(what))
                                    .arg(providerTabs->x())));
            QVERIFY2(providerTabs->x() + providerTabs->width() <= listTabs->x(),
                     qPrintable(QStringLiteral("%1: the two strips overlap")
                                    .arg(QString::fromLatin1(what))));
            // A control whose own width is the surplus is not "flush right" —
            // it is a left-aligned control in a stretched cell, which is what
            // the report shows. Its width must be its content's.
            QVERIFY2(listTabs->width() < navRow->width() * 0.75,
                     qPrintable(QStringLiteral(
                         "%1: list tabs are %2 wide in a %3 row — stretched, "
                         "not right-aligned")
                                    .arg(QString::fromLatin1(what))
                                    .arg(listTabs->width())
                                    .arg(navRow->width())));
        };

        checkFlushRight("default width");

        // Resizing is the whole reason this may not be a fixed offset.
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 640),
                                          Q_ARG(QVariant, 560)));
        QTRY_COMPARE(picker->property("width").toReal(), 640.0);
        checkFlushRight("widened");

        delete root;
        QCOMPARE(warnings, QStringList{});
    }
    // The other half of report 2, and it is REFUTED as a cause: a wheel over
    // the picker does not reach the chat. It was worth measuring because a
    // MouseArea does not handle QEvent::Wheel at all, so the background press
    // sink is no barrier to one — but something else consumes it, at every
    // point on the picker. Kept as a guard, with its control asserted first.
    void aWheelOverThePickerNeverScrollsTheChatBehindIt()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();

        auto *timeline =
            root->findChild<QQuickItem *>(QStringLiteral("fakeTimeline"));
        QVERIFY(timeline != nullptr);
        auto *popupItem = picker->property("contentItem").value<QQuickItem *>();
        QQuickItem *popupRoot = popupItem->parentItem();
        const QPointF origin = popupRoot->mapToScene(QPointF(0, 0));

        const auto wheelAt = [&](const char *what, const QPointF &scenePos) {
            QQmlProperty::write(timeline, QStringLiteral("contentY"), 400.0);
            QCoreApplication::processEvents();
            QWheelEvent ev(scenePos, window->mapToGlobal(scenePos.toPoint()),
                           QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
                           Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &ev);
            // Flickable answers a wheel with an animated movement, so the
            // value only changes once the animation clock has ticked.
            QTest::qWait(250);
            const qreal after =
                QQmlProperty::read(timeline, QStringLiteral("contentY"))
                    .toReal();
            qInfo("wheel over %-22s -> chat contentY 400 -> %g  (moved=%d)",
                  what, after, after != 400.0);
            return after;
        };

        // The control FIRST, and it must MOVE. A wheel probe that cannot make
        // the chat scroll proves nothing about the picker suppressing one —
        // the first version of this passed everywhere including here, because
        // Flickable answers a wheel with an ANIMATED movement and the value
        // had not changed yet when it was read.
        QVERIFY2(wheelAt("the chat itself", origin - QPointF(80, 0)) != 400.0,
                 "the wheel probe cannot scroll the chat even with nothing in "
                 "the way — it is measuring nothing");

        const auto mustNotReachTheChat = [&](const char *what,
                                             const QPointF &at) {
            QVERIFY2(wheelAt(what, at) == 400.0,
                     qPrintable(QStringLiteral(
                         "a wheel over %1 scrolled the chat behind the picker")
                                    .arg(QString::fromLatin1(what))));
        };
        mustNotReachTheChat("the resize grip", origin + QPointF(14, 14));
        mustNotReachTheChat("the header row",
                            origin + QPointF(popupRoot->width() / 2, 24));
        mustNotReachTheChat("the tab strip",
                            origin + QPointF(popupRoot->width() / 2, 64));
        mustNotReachTheChat("the grid centre",
                            origin + QPointF(popupRoot->width() / 2,
                                             popupRoot->height() / 2));
        mustNotReachTheChat("the footer",
                            origin + QPointF(popupRoot->width() / 2,
                                             popupRoot->height() - 12));

        delete root;
    }
};

QTEST_MAIN(GifSavedTabQmlTest)
#include "GifSavedTabQmlTest.moc"
