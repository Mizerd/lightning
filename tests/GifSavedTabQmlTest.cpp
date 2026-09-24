// GIF picker Saved tab, resize grip and tab layout, driven through the real
// qml/GifPicker.qml and GifSearchController:
//  1. A provider favourite renders as a tile on the Saved tab, and an empty
//     Saved tab says it is empty. (GifPickerSelectionQmlTest covers only the
//     local half of the merge; GifCollectionsTest has no QML engine.)
//  2. Dragging the resize grip must not scroll the timeline behind it: the
//     picker is not modal, and a DragHandler grabs without accepting, so the
//     press keeps walking the hit list. Real mouse events over a real
//     Flickable.
//  3. The Saved/Recent segment sits flush right while GIPHY/KLIPY stay left,
//     asserted as geometry at two picker widths.

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
#include "gif/GifTransport.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

// The https provider-CDN shape GifStoredModel accepts (as in
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
    // previewUrl/stillUrl are left empty: real provider URLs start network
    // fetches that race window teardown (see
    // GifPickerSelectionQmlTest::safeResult). Only row identity matters here.
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}

// A browse-grid row with empty preview/still URLs, for the same reason.
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

// A transport that hands out op ids and answers only when told, so the
// controller can be parked in Loading (or pushed into an error) with no
// network or timer.
class FakeGifTransport : public GifTransport
{
    Q_OBJECT
public:
    bool available() const override { return true; }
    quint64 get(const QString &) override { return ++m_next; }
    void fail(quint64 op, const QString &category)
    {
        Q_EMIT finished(op, false, 0, QByteArray(), category);
    }
    quint64 lastOp() const { return m_next; }

private:
    quint64 m_next = 100;
};

// GifPicker.qml reads app.gif and app.settings.gifAutoplay and nothing else.
class FakeGifSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int gifAutoplay MEMBER gifAutoplay)
public:
    explicit FakeGifSettings(QObject *parent = nullptr) : QObject(parent) {}
    int gifAutoplay = 2; // Never: keep AnimatedImage decoding out of a headless test

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

// A composer-shaped anchor at the bottom of a real window with a real
// Flickable above it: the timeline the picker floats over, which must not
// move while the grip is dragged.
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

    // Loads kTimelineScene with `gif` as app.gif and opens the picker.
    // `warnings` collects engine warnings so a swallowed binding TypeError
    // fails the test instead of producing an empty panel.
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

    // The merged saved model must answer exactly its sources' role table.
    // QConcatenateTablesProxyModel forwards source role names on Qt 6.11 but
    // not on 6.8 (the packaged builds), and GifPicker.qml's tile resolves its
    // required properties by name: without them QQmlDelegateModel builds no
    // delegate while count() is right, so the tab is blank with no empty-state
    // text. Equality, not containment, since 6.11 returns a superset.
    void savedModelAnswersTheSameRoleTableAsItsSources()
    {
        GifSearchController gif;
        const QHash<int, QByteArray> shared = GifResultModel().roleNames();
        QCOMPARE(gif.favorites()->roleNames(), shared);
        QCOMPARE(gif.starredStore()->model()->roleNames(), shared);
        // Same names and numbers, or a forwarded numeric role means different
        // things at each end.
        QCOMPARE(gif.saved()->roleNames(), shared);
        // Spelled out for the roles the delegate requires.
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

    // A provider bookmark (GifFavoritesModel, one of GifSavedModel's two
    // sources) renders as a real tile on the Saved tab. The local starred
    // store is deliberately not opened, as when a user has only saved
    // provider GIFs.
    void savedTabRendersAProviderFavourite()
    {
        GifSearchController gif;
        // Favorites persist in process QSettings; start from an empty group.
        gif.favorites()->clearAll();
        QCOMPARE(gif.saved()->count(), 0);

        const QVariantMap fixture =
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("fav1"));
        QVERIFY(gif.toggleFavorite(fixture));
        QCOMPARE(gif.favorites()->rowCount(), 1);
        // Precondition: the C++ merge. A failure here is in GifSavedModel.
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

        // activeModel must be the merged model; a thrown binding would leave it
        // at gif.results silently.
        QObject *activeModel =
            QQmlProperty::read(picker, QStringLiteral("activeModel"))
                .value<QObject *>();
        QCOMPARE(activeModel, static_cast<QObject *>(gif.saved()));

        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        // A count is not a rendered row: check the delegate's identity.
        QQuickItem *tile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tile),
                        Q_ARG(int, 0))
                    && tile != nullptr);
        QCOMPARE(QQmlProperty::read(tile, QStringLiteral("provider")).toString(),
                 QStringLiteral("giphy"));
        QCOMPARE(QQmlProperty::read(tile, QStringLiteral("gifId")).toString(),
                 QStringLiteral("fav1"));
        // The tile knows it is saved, so its star reads as saved.
        QVERIFY(QQmlProperty::read(tile, QStringLiteral("saved")).toBool());

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // An empty Saved tab shows its empty-state text; otherwise "saving does
    // not work" and "nothing saved yet" look the same.
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

    // The overlay spinner (AppBusyIndicator, which leaves visibility to its
    // host) shows only while something is loading; a stopped spinner was drawn
    // over every empty and error message. Both halves are asserted, since each
    // wrong fix passes one of them.
    void theOverlaySpinnerShowsOnlyWhileSomethingIsActuallyLoading()
    {
        GifSearchController gif;
        FakeGifTransport transport;
        gif.setTransport(&transport);
        gif.setApiKey(QStringLiteral("giphy"), QStringLiteral("k"));
        gif.favorites()->clearAll();

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;
        QObject *picker = nullptr;
        QObject *root = openPicker(engine, fakeApp, warnings, &window, &picker);
        QVERIFY(root != nullptr);
        QTRY_VERIFY(picker->property("opened").toBool());

        auto *busy =
            picker->findChild<QQuickItem *>(QStringLiteral("gifStateOverlayBusy"));
        QVERIFY2(busy != nullptr, "the state overlay has no reachable spinner");
        auto *overlayText =
            picker->findChild<QQuickItem *>(QStringLiteral("gifStateOverlayText"));
        QVERIFY(overlayText != nullptr);

        // (a) A real load on a provider tab with no results: spinner shown.
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("giphy"));
        gif.searchNow(QStringLiteral("cats"));
        QTRY_COMPARE(gif.state(),
                     static_cast<int>(GifSearchController::Loading));
        QCoreApplication::processEvents();
        QVERIFY2(QQmlProperty::read(busy, QStringLiteral("running")).toBool(),
                 "the spinner is not running during a real load");
        QVERIFY2(busy->isVisible(),
                 "the spinner is hidden while the picker is loading");
        QCOMPARE(QQmlProperty::read(overlayText, QStringLiteral("text"))
                     .toString(),
                 QString());

        // (b) The request fails: the message shows without a spinner over it.
        transport.fail(transport.lastOp(), QStringLiteral("provider_error"));
        QTRY_VERIFY(!QQmlProperty::read(overlayText, QStringLiteral("text"))
                         .toString().isEmpty());
        QVERIFY(!QQmlProperty::read(busy, QStringLiteral("running")).toBool());
        QVERIFY2(!busy->isVisible(),
                 qPrintable(QStringLiteral(
                     "the dead spinner is still painted over the error text: '%1'")
                        .arg(QQmlProperty::read(overlayText,
                                                QStringLiteral("text")).toString())));

        // (c) A local tab never loads, so no spinner.
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();
        const QString saved =
            QQmlProperty::read(overlayText, QStringLiteral("text")).toString();
        QVERIFY(saved.contains(QStringLiteral("No saved GIFs yet")));
        QVERIFY2(!busy->isVisible(),
                 "the dead spinner is still painted over the empty Saved tab");

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // Browse a provider, star a tile while the picker is open, then view
    // Saved. Exercises the live rowsInserted path through the merge proxy and
    // a GridView model swap from many rows to one.
    void savedTabFillsAfterBrowsingAndStarringWithThePickerOpen()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();
        gif.recent()->clearAll();

        // The browse grid.
        QList<gif::GifResult> trending;
        for (int i = 0; i < 12; ++i)
            trending.append(safeResult(QStringLiteral("giphy"),
                                       QStringLiteral("trend%1").arg(i)));
        gif.results()->reset(trending);
        QCOMPARE(gif.results()->count(), 12);
        // And a recents list.
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
        // The picker opens on the active provider tab.
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("giphy"));
        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(
            QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 12);

        // Recent, then star a tile through the picker's own toggleSaved().
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
        // The star fills.
        QTRY_VERIFY(QQmlProperty::read(recentTile, QStringLiteral("saved")).toBool());
        QCOMPARE(gif.favorites()->rowCount(), 1);

        // The Saved tab shows that GIF.
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

    // The same list through the real AppController instead of a stand-in
    // `app`.
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

    // A real press-drag-release on the resize grip over an interactive
    // Flickable: the picker resizes and the Flickable does not move.
    void draggingTheResizeGripDoesNotScrollTheTimeline_data()
    {
        QTest::addColumn<QString>("tab");
        // The local list has no search field, so the press lands on the
        // picker's bare chrome, where a missing barrier would leak.
        QTest::newRow("saved tab") << QStringLiteral("saved");
        // A provider tab puts the search field under the grip's hit area; a
        // plain click should still reach the field and only a drag resize.
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
        // Park the timeline mid-content so a drag either way could move it.
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
        // Past the drag threshold, in several moves (one big jump can be
        // dropped as a teleport).
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
        // The press never reached the chat at all.
        QCOMPARE(root->property("pressesBehind").toInt(), 0);

        // And the gesture resized the picker.
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

    // A press near the visible panel corner (the grip's outer edge) must still
    // resize, leave the chat alone and not dismiss the picker; a press outside
    // the popup's item rect would do both. Positive offsets are inside the
    // panel; each gets a fresh scene, since a leak closes the picker.
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

        // One fresh scene per offset, since a leak also closes the picker.
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
            // Measured from the visible panel corner, not the popup's item
            // rect (the insets make them differ).
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

    // The Saved/Recent segment is flush right in the nav row and the provider
    // segment stays left, at two picker widths.
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
        // The common parent row is the reference, not the picker's width
        // (which includes padding).
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
            // The provider strip stays left and the groups do not overlap.
            QVERIFY2(providerTabs->x() < 1.5,
                     qPrintable(QStringLiteral("%1: provider tabs at x=%2")
                                    .arg(QString::fromLatin1(what))
                                    .arg(providerTabs->x())));
            QVERIFY2(providerTabs->x() + providerTabs->width() <= listTabs->x(),
                     qPrintable(QStringLiteral("%1: the two strips overlap")
                                    .arg(QString::fromLatin1(what))));
            // Its width must be its content's; a stretched cell with a
            // left-aligned control is not flush right.
            QVERIFY2(listTabs->width() < navRow->width() * 0.75,
                     qPrintable(QStringLiteral(
                         "%1: list tabs are %2 wide in a %3 row — stretched, "
                         "not right-aligned")
                                    .arg(QString::fromLatin1(what))
                                    .arg(listTabs->width())
                                    .arg(navRow->width())));
        };

        checkFlushRight("default width");

        // The picker is resizable, so this cannot be a fixed offset.
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 640),
                                          Q_ARG(QVariant, 560)));
        QTRY_COMPARE(picker->property("width").toReal(), 640.0);
        checkFlushRight("widened");

        delete root;
        QCOMPARE(warnings, QStringList{});
    }
    // A wheel over the picker never scrolls the chat behind it. A MouseArea
    // does not handle wheel events, so the press sink is no barrier, but
    // something consumes the wheel everywhere on the picker. Kept as a guard.
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
            // Flickable animates wheel movement; wait for the clock to tick.
            QTest::qWait(250);
            const qreal after =
                QQmlProperty::read(timeline, QStringLiteral("contentY"))
                    .toReal();
            qInfo("wheel over %-22s -> chat contentY 400 -> %g  (moved=%d)",
                  what, after, after != 400.0);
            return after;
        };

        // The control first: a wheel on the chat itself must move it, or the
        // probe proves nothing.
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
