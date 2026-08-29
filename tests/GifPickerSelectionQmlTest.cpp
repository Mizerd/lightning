// v0.7 regression (live bug): clicking a favorite GIF sent the FIRST
// TRENDING item — the picker resolved the clicked row against gif.results
// instead of the model the user was looking at. This suite drives the real
// GifPicker.qml with the real GifSearchController/favorites models and
// proves the chosen record is the exact provider-qualified favorite, with
// no dependence on (or substitution from) the browse results model.
//
// v0.7 follow-up: reviewing a report on GIFs sent from a thread reply not
// matching what was clicked found three more surfaces of the SAME
// underlying defect — selection identity resolved from mutable, shared,
// asynchronously-replaced state instead of an immutable snapshot taken at
// activation time:
//   - a keyboard-highlighted grid.currentIndex surviving a search response
//     that replaces the grid's contents, so Return could resolve a
//     completely different item than the one the user actually highlighted
//     (staleCurrentIndexAfterModelResetCannotSendWrongItem);
//   - the search field's Return handler sending row 0 of whatever the grid
//     currently held while the just-typed query was still debounced — i.e.
//     the PREVIOUS query's (or trending's) first result, never what was
//     typed (searchFieldEnterWithPendingDebounceSendsNothing);
//   - the room composer and thread panel each owning an independent
//     GifPicker instance, both bound to the ONE shared app.gif controller,
//     with nothing enforcing that only one is ever open — so a search
//     performed in either could silently swap what the other was showing
//     (openingOnePickerClosesTheOther).
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifResponseParser.h"
#include "gif/GifResultModel.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "gif/GifTransport.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

QVariantMap favoriteFixture(const QString &provider, const QString &id)
{
    // The validated https provider-CDN shape GifStoredModel accepts.
    const QString host = provider == QStringLiteral("giphy")
        ? QStringLiteral("media.giphy.com")
        : QStringLiteral("static.klipy.com");
    QVariantMap m;
    m.insert(QStringLiteral("provider"), provider);
    m.insert(QStringLiteral("gifId"), id);
    m.insert(QStringLiteral("title"), QStringLiteral("fixture %1").arg(id));
    m.insert(QStringLiteral("gifUrl"),
             QStringLiteral("https://%1/%2/original.gif").arg(host, id));
    m.insert(QStringLiteral("previewUrl"),
             QStringLiteral("https://%1/%2/preview.gif").arg(host, id));
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}

// Records issued URLs and lets the test complete requests on demand —
// mirrors tests/GifSearchControllerTest.cpp's FakeGifTransport. Needed here
// (rather than AppController::MockBackend's real gif transport) because
// MockMatrixClient never implements gifGet()/supportsGifProvider(), so
// app.gif is permanently Offline under MockBackend and can never be driven
// through an actual Loading -> Ready transition. A raw GifSearchController
// with this fake transport can be.
class FakeGifTransport : public GifTransport
{
    Q_OBJECT
public:
    bool up = true;
    quint64 next = 100;
    QList<QPair<quint64, QString>> issued; // (opId, url)

    bool available() const override { return up; }
    quint64 get(const QString &url) override
    {
        if (!up)
            return 0;
        const quint64 op = next++;
        issued.append({ op, url });
        return op;
    }
    void complete(quint64 op, bool ok, int status, const QByteArray &body,
                  const QString &category)
    {
        Q_EMIT finished(op, ok, status, body, category);
    }
    quint64 lastOp() const { return issued.isEmpty() ? 0 : issued.last().first; }
};

QByteArray giphyBody(const QStringList &ids)
{
    QByteArray items;
    for (int i = 0; i < ids.size(); ++i) {
        if (i)
            items += ",";
        items += "{\"id\":\"" + ids[i].toUtf8() + "\",\"rating\":\"g\","
                 "\"title\":\"t\",\"images\":{\"original\":{"
                 "\"url\":\"https://media.giphy.com/media/" + ids[i].toUtf8()
              + "/giphy.gif\",\"width\":\"100\",\"height\":\"100\",\"size\":\"10\"}}}";
    }
    return "{\"data\":[" + items + "],\"pagination\":{\"total_count\":"
        + QByteArray::number(ids.size())
        + ",\"count\":" + QByteArray::number(ids.size()) + ",\"offset\":0}}";
}

// A GifResultModel row with an intentionally EMPTY previewUrl/stillUrl,
// injected directly via GifResultModel::reset()/append() rather than
// through the real provider-JSON parser. Two tests below render a real,
// shown GridView to exercise real currentIndex/keyboard behaviour, and
// AnimatedImage/Image bound to an empty source never attempts a load —
// whereas a real "https://media.giphy.com/..." row (as produced by
// giphyBody() above, which IS safe for the headless, non-rendering
// GifSearchControllerTest.cpp) starts a real async network fetch that this
// offline/sandboxed test process can never complete. That fetch's
// in-flight state on Qt's own image-loader thread pool then races the
// window/engine teardown at the end of the test function and reliably
// SEGFAULTs inside QQuickAnimatedImage::~QQuickAnimatedImage() ->
// QObject::deleteLater() — a Qt Quick teardown fragility hit only by
// actually rendering provider-hosted image URLs offscreen, not a defect in
// the code under test. previewUrl/stillUrl content is irrelevant to what
// these tests assert (row identity / currentIndex); provider URL parsing
// and validation are already covered by GifResponseParserTest.cpp and
// GifSearchControllerTest.cpp (headless, no rendering).
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

// Minimal stand-in for AppController's QML-facing surface: GifPicker.qml
// only ever reads app.gif and app.settings.gifAutoplay (grep confirms
// there is nothing else), so a real AppController — whose GIF transport is
// architecturally offline under MockBackend — is not required to drive it
// deterministically through real search timing.
class FakeGifSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int gifAutoplay MEMBER gifAutoplay)
public:
    explicit FakeGifSettings(QObject *parent = nullptr) : QObject(parent) {}
    int gifAutoplay = 2; // Never — keep AnimatedImage decode out of a headless test

    // v0.6.7: AnchoredPopup reads the remembered picker size on every open and
    // writes it back after a resize drag. These must exist on the fake, and
    // must behave like SettingsManager's (0 = never resized), or the popup's
    // onAboutToShow would raise a TypeError — which the warning assertions in
    // this suite would then catch as a failure.
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

// A minimal real ApplicationWindow scene hosting two independent GifPicker
// instances, exactly as MessageComposerBar.qml (target: "room") and
// ThreadPanel.qml (target: "thread") each do — needed for
// openingOnePickerClosesTheOther(), which drives the REAL Popup
// open()/close() lifecycle (aboutToShow/closed) and therefore needs a real
// window + Overlay, unlike the other tests here which bypass that by
// reparenting a picker's bare contentItem.
const char *kTwoPickerScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    GifPicker {
        id: roomPicker
        objectName: "roomPicker"
        target: "room"
    }
    GifPicker {
        id: threadPicker
        objectName: "threadPicker"
        target: "thread"
    }
}
)QML";

// v0.6.7 anchoring scene: a composer-shaped anchor — a wide bar pinned to the
// bottom of the window, exactly what MessageComposerBar's card is. The picker
// is parented to it and pinned by its bottom-right corner, so the contract
// under test is expressed purely in the ANCHOR's coordinates: the picker's
// right edge meets the anchor's right edge and its bottom sits one gap above
// the anchor's top, at every window size.
//
// A 40px button will not do here: the picker is clamped to never exceed its
// anchor's width ("on the right go no further than the text box").
const char *kAnchoredPickerScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

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

// v0.6.7 review (H1): the scene that catches an anchor moving because an
// ANCESTOR moved. The anchor bar lives inside a fixed-width trailing panel, so
// a window resize slides the whole panel while the bar's own x within it never
// changes — the ThreadPanel shape (a fixed 340px item at the end of a
// RowLayout).
//
// Three successive placement schemes got this wrong before the popup was
// simply parented to its anchor and left to Qt's own positioner.
const char *kAncestorMoveScene = R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            objectName: "roomColumn"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Item {
            id: sidePanel
            objectName: "sidePanel"
            Layout.preferredWidth: 340
            Layout.fillHeight: true

            Rectangle {
                id: panelComposer
                objectName: "panelComposer"
                color: "#202030"
                height: 50
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
            }
        }
    }

    GifPicker {
        id: picker
        objectName: "picker"
        target: "room"
        anchorItem: panelComposer
    }
}
)QML";

// v0.6.7 review (round-4 follow-up): the bare-`anchorPoint` path — no
// anchorItem — which is how the reaction popovers open (at a point inside a
// scrolling message row, with no stable item to hold). It is a high-traffic
// surface that no round had ever exercised, and it takes the one code path
// the placement bindings deliberately do NOT drive.
const char *kPointAnchoredScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    GifPicker {
        id: picker
        objectName: "picker"
        target: "room"
        // Deliberately no anchorItem: placed once from this point, then only
        // ever clamped.
        anchorPoint: Qt.point(860, 300)
    }
}
)QML";

} // namespace

class GifPickerSelectionQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("gif-picker-selection-test"));
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // v0.6.7: the Favorites section became the Saved tab, and the visible
    // model is the merged GifSavedModel rather than GifFavoritesModel
    // directly. The invariant under test is unchanged and is the whole reason
    // choose() resolves against activeModel: a row must be resolved against
    // the model the user is LOOKING AT, never the browse grid.
    void savedClickChoosesExactSavedRowNotTrending()
    {
        AppController controller(AppController::MockBackend);
        auto *gif = controller.gif();
        QVERIFY(gif != nullptr);

        // Two saved provider GIFs from different providers, deliberately NOT
        // present in the browse results model (which stays empty — the live
        // bug substituted its first row).
        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("aaa1"))));
        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("klipy"), QStringLiteral("bbb2"))));
        QCOMPARE(gif->favorites()->rowCount(), 2);
        QCOMPARE(gif->results()->rowCount(), 0);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("saved"));

        // Click row 1 of the VISIBLE saved grid — read through the merged
        // model the picker is actually bound to, not through the favorites
        // store behind it (with no local rows the two agree, but resolving
        // against the visible model is exactly the invariant here). Saved
        // rows prepend newest first, so verify by identity, never position.
        const QVariantMap expected = gif->saved()->get(1);
        QVERIFY(!expected.value(QStringLiteral("gifId")).toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(picker, "choose",
                                          Q_ARG(QVariant, 1)));

        QCOMPARE(chosen.count(), 1);
        const QVariantMap result = chosen.at(0).at(0).toMap();
        QCOMPARE(result.value(QStringLiteral("provider")).toString(),
                 expected.value(QStringLiteral("provider")).toString());
        QCOMPARE(result.value(QStringLiteral("gifId")).toString(),
                 expected.value(QStringLiteral("gifId")).toString());
        QCOMPARE(result.value(QStringLiteral("gifUrl")).toString(),
                 expected.value(QStringLiteral("gifUrl")).toString());
        // And the identity is provider-qualified — never a bare id that
        // could collide across GIPHY and KLIPY.
        QVERIFY(!result.value(QStringLiteral("provider")).toString().isEmpty());

        // An out-of-range or unidentifiable row chooses NOTHING (no
        // index-zero fallback, no substitution).
        QVERIFY(QMetaObject::invokeMethod(picker, "choose",
                                          Q_ARG(QVariant, 99)));
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(warnings, QStringList{});
    }

    void staleCurrentIndexAfterModelResetCannotSendWrongItem()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        // A "search response" lands with three rows. Injected directly via
        // the model (see safeResult()) rather than through
        // searchNow()/a fake transport/the real parser: GifResultModel's
        // reset() emits the exact same modelReset() this test's fix reacts
        // to regardless of who calls it, and doing it this way keeps every
        // rendered row's image source empty (network-safe) — the debounce
        // -> transport -> parser wiring itself is already covered by
        // GifSearchControllerTest.cpp.
        gif.results()->reset({ safeResult(QStringLiteral("giphy"), QStringLiteral("cat1")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("cat2")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("cat3")) });
        QCOMPARE(gif.results()->count(), 3);

        auto *gridObj =
            picker->findChild<QObject *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);

        // The user keyboard-highlights the last row ("cat3") without
        // pressing Return yet.
        QQmlProperty::write(gridObj, QStringLiteral("currentIndex"), 2);
        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("currentIndex"))
                     .toInt(),
                 2);

        // A debounced search for something else lands before Return is
        // pressed, replacing the grid with entirely different content that
        // still happens to have a row at the same position.
        gif.results()->reset({ safeResult(QStringLiteral("giphy"), QStringLiteral("dog1")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("dog2")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("dog3")) });
        QCOMPARE(gif.results()->count(), 3);
        QVERIFY(gif.results()->get(2).value(QStringLiteral("gifId")).toString()
                != QStringLiteral("cat3"));

        // The stale highlight must NOT survive: row 2 now means "dog3", a
        // completely different item than what the user was looking at when
        // they highlighted row 2. Without this invalidation, Return's own
        // `if (currentIndex >= 0) picker.choose(currentIndex)` guard would
        // happily resolve and send "dog3" under the user's belief they
        // were sending "cat3". Fails before the fix: currentIndex stays 2.
        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("currentIndex"))
                     .toInt(),
                 -1);
        QCOMPARE(warnings, QStringList{});
    }

    void searchFieldEnterWithPendingDebounceSendsNothing()
    {
        FakeGifTransport transport;
        GifSearchController gif;
        // A long debounce so the test's Return keypress deterministically
        // races ahead of it — exactly the window the live bug hit.
        gif.setDebounceMs(60'000);
        gif.setApiKey(QStringLiteral("giphy"), QStringLiteral("GKEY"));
        gif.setTransport(&transport);
        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        // Seed the grid with a "trending" result first — what a "send row
        // 0" implementation would fall back to if it ignored the pending
        // debounce. Injected directly via the model (see safeResult()'s
        // comment) so the rendered tile's image source stays empty/network
        // -safe; showTrending()/setQueryText() debounce timing itself is
        // exercised below through the real transport.
        gif.results()->reset({ safeResult(QStringLiteral("giphy"), QStringLiteral("trend1")) });
        QCOMPARE(gif.results()->count(), 1);

        auto *searchFieldItem = picker->findChild<QQuickItem *>(
            QStringLiteral("gifSearchField"));
        QVERIFY(searchFieldItem != nullptr);
        QVERIFY(QMetaObject::invokeMethod(searchFieldItem,
                                          "forceActiveFocus"));
        searchFieldItem->setProperty("text", QStringLiteral("dogs"));

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        const int issuedCountBeforeEnter = transport.issued.size();
        QTest::keyClick(&window, Qt::Key_Return);
        QCoreApplication::processEvents();

        // Nothing was sent — the debounce (60s) has not fired, and no
        // response for "dogs" exists yet, so there is no safe row to send.
        QCOMPARE(chosen.count(), 0);
        // Return flushed the query immediately rather than waiting out the
        // debounce: a new request for "dogs" was issued right away.
        QVERIFY(transport.issued.size() > issuedCountBeforeEnter);
        QVERIFY(transport.issued.last().second.contains(QStringLiteral("dogs")));

        // Completing that flushed request afterward must not retroactively
        // send anything either — Return only moved focus into the grid, it
        // never queued a pending choose(). An empty response body keeps
        // this safe to render (see safeResult()'s comment): the point here
        // is that NOTHING gets auto-sent on completion, not what the
        // result set contains.
        transport.complete(transport.lastOp(), true, 200,
                           giphyBody({}), QStringLiteral("ok"));
        QCOMPARE(chosen.count(), 0);
        QCOMPARE(warnings, QStringList{});
    }

    void openingOnePickerClosesTheOther()
    {
        AppController controller(AppController::MockBackend);
        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kTwoPickerScene),
                          QUrl(QStringLiteral("twopickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCoreApplication::processEvents();

        auto *roomPicker =
            root->findChild<QObject *>(QStringLiteral("roomPicker"));
        auto *threadPicker =
            root->findChild<QObject *>(QStringLiteral("threadPicker"));
        QVERIFY(roomPicker != nullptr);
        QVERIFY(threadPicker != nullptr);

        QVERIFY(QMetaObject::invokeMethod(roomPicker, "open"));
        QTRY_VERIFY(roomPicker->property("opened").toBool());
        QVERIFY(!threadPicker->property("opened").toBool());

        // Opening the thread picker while the room one is still open must
        // close the room one — the documented-but-previously-unimplemented
        // "the active target closes the other picker" invariant (see the
        // comment at the top of qml/GifPicker.qml). Before the fix this
        // never happened: both stayed open, sharing one live results grid.
        QVERIFY(QMetaObject::invokeMethod(threadPicker, "open"));
        QTRY_VERIFY(threadPicker->property("opened").toBool());
        QTRY_VERIFY(!roomPicker->property("opened").toBool());

        // And it works symmetrically back the other way.
        QVERIFY(QMetaObject::invokeMethod(roomPicker, "open"));
        QTRY_VERIFY(roomPicker->property("opened").toBool());
        QTRY_VERIFY(!threadPicker->property("opened").toBool());

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // v0.6.6 live-bug fix, still guarded after the v0.6.7 rework:
    // `gif.starredStore.model()` in GifPicker.qml's activeModel binding called
    // a plain, non-Q_INVOKABLE C++ method — QML cannot call that, the binding
    // throws a TypeError, Qt's QQmlBinding::update catches it and (silently)
    // leaves activeModel at its PREVIOUS value. Since activeModel starts out
    // bound to gif.results, selecting the local tab left the grid still
    // showing trending GIPHY results — while the tab's header/footer/
    // search-field visibility all correctly switched, because those bind on
    // the tab state directly, never through the throwing expression.
    // Text-scanning contract tests can only see the call form; only a real
    // engine evaluates the branch. This test drives the real engine, the real
    // GifStarredStore and the real merged model, and asserts both the model
    // identity AND that no QML warning (i.e. no caught binding exception) was
    // ever emitted — a text scan can never prove that on its own.
    void savedTabBindsTheMergedModelNotResults()
    {
        QTemporaryDir starredDir;
        QVERIFY(starredDir.isValid());
        GifSearchController gif;
        // v0.6.7: provider favorites persist to the shared QSettings store,
        // so a controller constructed here loads whatever an EARLIER case in
        // this binary saved. That did not matter while the local tab rendered
        // GifStarredModel alone; now that the Saved tab is the merged list,
        // leftover favorites would land in the very count asserted below.
        // Start from an empty provider group so the merged list is exactly
        // the local group.
        gif.favorites()->clearAll();
        gif.openStarredStoreFor(starredDir.path());
        QVERIFY(gif.starredStore()->isOpen());
        QCOMPARE(gif.saved()->count(), 0);

        // A minimal real GIF: magic + logical-screen-descriptor width/height
        // — the smallest shape gif::validateGifBytes accepts (mirrors
        // GifStarredStoreTest.cpp's makeGif()).
        QByteArray starredBytes = QByteArrayLiteral("GIF89a");
        starredBytes.append(char(10)); starredBytes.append(char(0));
        starredBytes.append(char(10)); starredBytes.append(char(0));
        QSignalSpy starFinished(gif.starredStore(),
                                &GifStarredStore::starFinished);
        gif.starredStore()->starBytes(QStringLiteral("mk1"), starredBytes);
        QCOMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
        QCOMPARE(gif.starredStore()->count(), 1);

        // The browse results model stays populated with UNRELATED trending
        // content — exactly what the bug left rendered on the Starred tab.
        // TWO rows, deliberately: with one row each, the grid's count would
        // be 1 in both the fixed and the broken state, so the count
        // assertion below would pass against the bug and only the identity
        // assertions would catch it.
        gif.results()->reset(
            { safeResult(QStringLiteral("giphy"), QStringLiteral("trend1")),
              safeResult(QStringLiteral("giphy"), QStringLiteral("trend2")) });
        QCOMPARE(gif.results()->count(), 2);

        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        // Select the Saved tab exactly like the nav strip's onActivated does.
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("saved"));
        QVERIFY(!QQmlProperty::read(picker, QStringLiteral("providerTab"))
                     .toBool());

        auto *gridObj =
            picker->findChild<QObject *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);

        // activeModel (and the grid bound to it) must be the merged SAVED
        // model — never gif.results (trending).
        QObject *activeModel =
            QQmlProperty::read(picker, QStringLiteral("activeModel"))
                .value<QObject *>();
        QCOMPARE(activeModel, static_cast<QObject *>(gif.saved()));
        QVERIFY(activeModel != static_cast<QObject *>(gif.results()));

        QObject *gridModel =
            QQmlProperty::read(gridObj, QStringLiteral("model")).value<QObject *>();
        QCOMPARE(gridModel, static_cast<QObject *>(gif.saved()));

        // The grid actually renders the ONE locally-saved row, not the TWO
        // trending rows the bug left it showing — a count assertion that
        // discriminates on its own, not only alongside the identity checks.
        // No provider favorites exist here, so the merged list is exactly the
        // local group.
        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        // No caught binding exception anywhere in this sequence — the
        // regression's actual failure mode (a swallowed TypeError) would
        // otherwise show up here even if some other code path happened to
        // still produce the right model.
        QCOMPARE(warnings, QStringList{});
    }

    // v0.6.7: the picker is PARENTED to its anchor, so its position is
    // expressed in the anchor's own coordinates and Qt's popup positioner
    // keeps the two rigid through any ancestor movement. The contract is
    // therefore a geometric relationship that must hold at EVERY window size:
    // right edges flush, bottom one gap above the anchor's top.
    //
    // Three earlier schemes computed an absolute overlay position instead and
    // each broke a different case — drift, lag, then a 400px ancestor error.
    void pickerIsPinnedToItsAnchorAtEveryWindowSize()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAnchoredPickerScene),
                          QUrl(QStringLiteral("anchoredpickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        auto *bar = root->findChild<QQuickItem *>(QStringLiteral("composerBar"));
        QVERIFY(picker != nullptr && bar != nullptr);

        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        // The anchor IS the parent — that is the whole mechanism.
        QCOMPARE(picker->property("parent").value<QQuickItem *>(), bar);

        const qreal gap = picker->property("anchorGap").toReal();
        const auto pinned = [&](const char *whenLabel) {
            const qreal x = picker->property("x").toReal();
            const qreal y = picker->property("y").toReal();
            const qreal w = picker->property("width").toReal();
            const qreal h = picker->property("height").toReal();
            // Right edges flush, and never past the anchor.
            QVERIFY2(qAbs((x + w) - bar->width()) < 1.5,
                     qPrintable(QStringLiteral("%1: right edge %2 vs anchor %3")
                                    .arg(QString::fromUtf8(whenLabel))
                                    .arg(x + w).arg(bar->width())));
            QVERIFY2(x + w <= bar->width() + 0.5,
                     qPrintable(QStringLiteral("%1: overhangs the anchor")
                                    .arg(QString::fromUtf8(whenLabel))));
            // Sits on top of the anchor with exactly the hairline gap.
            QVERIFY2(qAbs((y + h) + gap) < 1.5,
                     qPrintable(QStringLiteral("%1: bottom %2, expected %3")
                                    .arg(QString::fromUtf8(whenLabel))
                                    .arg(y + h).arg(-gap)));
        };

        pinned("initial");
        const qreal barWidthBefore = bar->width();

        window->resize(1300, 900);
        QTRY_COMPARE(int(window->width()), 1300);
        QTRY_VERIFY2(bar->width() > barWidthBefore + 300,
                     "anchor did not grow: scene no longer exercises a resize");
        pinned("after grow");

        window->resize(620, 520);
        QTRY_COMPARE(int(window->width()), 620);
        QTRY_VERIFY(bar->width() < barWidthBefore);
        pinned("after shrink");
        // Shrinking below the picker's own default forces the width clamp —
        // the picker must follow the anchor down rather than overhang it.
        QVERIFY(picker->property("width").toReal() <= bar->width() + 0.5);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // v0.6.7 review (H1): the anchor moves because its CONTAINER moved, not
    // because its own x changed. Every scheme that computed an absolute
    // position from mapToItem() missed this — the named dependencies never
    // fire — and it is the production ThreadPanel shape. Parenting the popup
    // to the anchor makes it structurally impossible: the popup's coordinates
    // are the anchor's, so it cannot be left behind by an ancestor at all.
    void pickerFollowsAnAnchorMovedByItsAncestor()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAncestorMoveScene),
                          QUrl(QStringLiteral("ancestormovescene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        auto *bar = root->findChild<QQuickItem *>(QStringLiteral("panelComposer"));
        auto *sidePanel = root->findChild<QQuickItem *>(QStringLiteral("sidePanel"));
        QVERIFY(picker != nullptr && bar != nullptr && sidePanel != nullptr);
        auto *overlayForScene = window->contentItem();
        QVERIFY(overlayForScene != nullptr);

        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        const qreal panelXBefore = sidePanel->x();
        const qreal barLocalXBefore = bar->x();
        const qreal barSceneXBefore =
            bar->mapToItem(overlayForScene, QPointF(0, 0)).x();
        const qreal pickerXBefore = picker->property("x").toReal();

        window->resize(1300, 900);
        QTRY_COMPARE(int(window->width()), 1300);
        // Premise: the panel slid by the full delta and the bar did NOT move
        // inside it. If this stops holding the scene has drifted and the case
        // no longer covers what it claims.
        QTRY_VERIFY2(sidePanel->x() > panelXBefore + 300,
                     qPrintable(QStringLiteral("panel did not slide: %1 -> %2")
                                    .arg(panelXBefore).arg(sidePanel->x())));
        QCOMPARE(bar->x(), barLocalXBefore);
        QVERIFY(bar->mapToItem(overlayForScene, QPointF(0, 0)).x()
                > barSceneXBefore + 300);

        // The picker's coordinates are the anchor's, so they did not need to
        // change at all — and the relationship still holds exactly.
        QCOMPARE(picker->property("x").toReal(), pickerXBefore);
        const qreal x = picker->property("x").toReal();
        const qreal w = picker->property("width").toReal();
        QVERIFY(qAbs((x + w) - bar->width()) < 1.5);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // v0.6.7 review (round-4 follow-up): a popup with no anchorItem is placed
    // ONCE from its point and afterwards only clamped — never re-placed. That
    // distinction is the whole reason the placement Binding tests
    // `anchorItem !== null`: the captured point is already stale by the time
    // the window changes, so re-placing against it would slide an
    // edge-clamped popover somewhere arbitrary, or flip one that opened above
    // its anchor to below.
    void pointAnchoredPopupIsPlacedOnceThenOnlyClamped()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kPointAnchoredScene),
                          QUrl(QStringLiteral("pointanchoredscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        QVERIFY(picker != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());
        auto *overlay = picker->property("parent").value<QQuickItem *>();
        QVERIFY(overlay != nullptr);

        // Placed from the point, clamped inside the window: the anchor is at
        // x=860 in a 900px window, so a centred placement would overflow and
        // is pulled back to the right margin.
        const qreal w = picker->property("width").toReal();
        const qreal openedX = picker->property("x").toReal();
        const qreal rightLimit = overlay->width() - w - 8; // AppTheme.spacingS
        QTRY_COMPARE(picker->property("x").toReal(), rightLimit);

        // GROWING the window must NOT move it. This is the regression the
        // anchorItem test in the Binding guards: a re-place would now fit the
        // centred position (860 - w/2) and shift it there.
        window->resize(1400, 900);
        QTRY_COMPARE(int(window->width()), 1400);
        QTRY_COMPARE(int(overlay->width()), 1400);
        QCoreApplication::processEvents();
        const qreal centredIfReplaced = 860 - w / 2;
        QVERIFY2(qAbs(centredIfReplaced - openedX) > 50,
                 "scene no longer distinguishes clamp from re-place");
        QCOMPARE(picker->property("x").toReal(), openedX);

        // SHRINKING must clamp it back inside — the one correction it does get.
        window->resize(500, 700);
        QTRY_COMPARE(int(overlay->width()), 500);
        QTRY_VERIFY(picker->property("x").toReal()
                    + picker->property("width").toReal()
                    <= overlay->width());
        QVERIFY(picker->property("x").toReal() >= 8);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // v0.6.7: the resize cycle end to end, without a synthetic pointer — the
    // grip's only uncovered part is then the two-line inverted arithmetic.
    //
    // There is no "detach" any more: the placement bindings pin the
    // bottom-right corner to the anchor, so a bigger size grows the popup up
    // and to the LEFT on its own. That is why the grip moved to the top-left
    // corner, and it is what "snapped in place" means — the picker cannot
    // drift away from the composer even while being resized.
    void resizeGrowsFromThePinnedCornerAndPersists()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAnchoredPickerScene),
                          QUrl(QStringLiteral("anchoredpickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        auto *bar = root->findChild<QQuickItem *>(QStringLiteral("composerBar"));
        QVERIFY(picker != nullptr && bar != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        // Sized as a SHARE of the anchor, so the expected width is derived
        // from the live anchor rather than hardcoded — that is the behaviour
        // under test, not an incidental number.
        const qreal fraction = picker->property("widthFraction").toReal();
        QVERIFY2(qAbs(picker->property("width").toReal()
                      - bar->width() * fraction) < 1.5,
                 qPrintable(QStringLiteral("auto width %1 vs share %2")
                                .arg(picker->property("width").toReal())
                                .arg(bar->width() * fraction)));
        const qreal gap = picker->property("anchorGap").toReal();
        const qreal rightEdgeBefore = picker->property("x").toReal()
                                      + picker->property("width").toReal();

        // Grow. The right edge and the bottom must not budge — only the top
        // and the left move outward.
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 460),
                                          Q_ARG(QVariant, 600)));
        QTRY_COMPARE(picker->property("width").toReal(), 460.0);
        QCOMPARE(picker->property("height").toReal(), 600.0);
        QCOMPARE(picker->property("x").toReal()
                     + picker->property("width").toReal(), rightEdgeBefore);
        QVERIFY(qAbs((picker->property("y").toReal()
                      + picker->property("height").toReal()) + gap) < 1.5);

        // Below the component minimum is refused. Read from the picker's own
        // minWidth/minHeight rather than hardcoded: the claim under test is
        // "the floor is honoured", and the floor legitimately moves when the
        // popup's item carries chrome the panel does not (2026-08-28: the
        // resize grab band, which is inset from the visible panel so a press
        // aimed at the corner grip cannot land outside the popup).
        const qreal floorW = picker->property("minWidth").toReal();
        const qreal floorH = picker->property("minHeight").toReal();
        QVERIFY(floorW > 0 && floorH > 0);
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 50),
                                          Q_ARG(QVariant, 50)));
        QCOMPARE(picker->property("width").toReal(), floorW);
        QCOMPARE(picker->property("height").toReal(), floorH);

        // Wider than the anchor is refused — "no further than the text box".
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 99999),
                                          Q_ARG(QVariant, 99999)));
        QCOMPARE(picker->property("width").toReal(), bar->width());
        QCOMPARE(picker->property("x").toReal(), 0.0);
        // And never taller than the room above the anchor.
        QVERIFY(picker->property("height").toReal()
                <= window->height() - bar->height());

        // Settle on a real size and end the drag. What is remembered is a
        // SHARE (per mille of the anchor), not a pixel count — which is what
        // lets it scale with the window and transfer to the emoji picker.
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 420),
                                          Q_ARG(QVariant, 560)));
        QVERIFY(QMetaObject::invokeMethod(picker, "endResize"));

        auto *settings = fakeApp.property("settings").value<QObject *>();
        QVERIFY(settings != nullptr);
        int storedShare = 0;
        // Both pickers persist under the SAME id, which is the sync between
        // them: there is no second value to keep in step.
        QVERIFY(QMetaObject::invokeMethod(settings, "pickerWidthShare",
                                          Q_RETURN_ARG(int, storedShare),
                                          Q_ARG(QString, QStringLiteral("picker"))));
        const int expectedShare = qRound(420.0 / bar->width() * 1000);
        QCOMPARE(storedShare, expectedShare);

        // Reopening restores that share and stays pinned.
        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QTRY_VERIFY(!picker->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());
        const qreal restored = picker->property("width").toReal();
        QVERIFY2(qAbs(restored - 420.0) < 2.0,
                 qPrintable(QStringLiteral("restored width %1").arg(restored)));
        QVERIFY(qAbs((picker->property("x").toReal() + restored) - bar->width()) < 1.5);

        // And the share tracks the window: a wider anchor gives a wider
        // picker without the user touching anything.
        window->resize(1500, 900);
        QTRY_VERIFY(bar->width() > 1200);
        QTRY_VERIFY2(picker->property("width").toReal() > restored + 100,
                     "picker did not scale up with the window");

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // v0.6.7 review (H1/N9): the behavioural guard for the Recent-tab star.
    //
    // GifStoredModel answers FavoriteRole with a constant `true` — "stored ==
    // favorited" — and GifRecentModel does not override it. The picker read
    // that role to drive its star, so every Recent tile rendered as saved,
    // announced "Remove from saved GIFs", and then called toggleFavorite()
    // which INSERTS: the control said Remove and did Save. The source-scan
    // pin in GifPickerRedesignContractTest catches the shape; this drives a
    // real picker with a real controller and reads the rendered delegate's
    // own `saved` property, which is what the user actually sees.
    //
    // It fails on the unfixed tree: `saved` was
    // `tile.provider === "local" || tile.favorite`, and tile.favorite is
    // unconditionally true for a recents row, so the first assertion below
    // would read true.
    void recentTileIsNotSavedMerelyBecauseItWasSent()
    {
        GifSearchController gif;
        // Recents and favorites share the process QSettings, so start from a
        // known-empty provider group (see savedTabBindsTheMergedModelNot-
        // Results for the same isolation note).
        gif.favorites()->clearAll();
        gif.recent()->clearAll();

        const QVariantMap sent =
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("sent1"));
        gif.recordSent(sent);
        QCOMPARE(gif.recent()->rowCount(), 1);
        QCOMPARE(gif.favorites()->rowCount(), 0);

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        // A real ApplicationWindow so Overlay.overlay resolves and the opened
        // popup gets real geometry — a GridView with no height creates no
        // delegates, and this test has to read one.
        QQmlComponent component(&engine);
        component.setData(QByteArray(kAnchoredPickerScene),
                          QUrl(QStringLiteral("anchoredpickerscene.qml")));
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
                            QStringLiteral("recent"));
        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        QQuickItem *tile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tile),
                        Q_ARG(int, 0))
                    && tile != nullptr);

        // A GIF that was merely SENT is not saved.
        QVERIFY2(!QQmlProperty::read(tile, QStringLiteral("saved")).toBool(),
                 "a recents row reported itself as saved");

        // Saving it flips the SAME tile live — which also proves the revision
        // counter actually re-evaluates the binding, since isSaved() is a
        // plain call that establishes no dependency of its own.
        QVERIFY(gif.toggleFavorite(sent));
        QTRY_VERIFY2(QQmlProperty::read(tile, QStringLiteral("saved")).toBool(),
                     "tile did not pick up the new saved state");

        // And unsaving flips it back.
        QVERIFY(!gif.toggleFavorite(sent));
        QTRY_VERIFY(!QQmlProperty::read(tile, QStringLiteral("saved")).toBool());

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    // Regression: one user activation must produce exactly one send. Before
    // the fix, nothing stopped a second activation reaching choose() while
    // the popup was still visually closing — MouseArea delivers a fast
    // double-click as TWO separate "clicked" signals, and Popup.close()
    // only starts an exit transition, it does not synchronously tear the
    // content down. The `activated` one-shot latch (qml/GifPicker.qml)
    // gates choose() itself, so it covers every activation surface (mouse
    // AND the grid's Return/Enter keyboard path) with one guard rather than
    // one per input device.
    //
    // Uses the same minimal harness as staleCurrentIndexAfterModelResetCan-
    // notSendWrongItem/searchFieldEnterWithPendingDebounceSendsNothing above
    // (a raw GifSearchController + FakeGifApp, default provider tab, results
    // injected directly) rather than the two-picker
    // ApplicationWindow/kTwoPickerScene harness openingOnePickerClosesThe-
    // Other uses: that combination — a real Popup open()/close()/reopen
    // cycle together with a switch to the saved list — segfaulted deep in
    // Qt's own event-posting machinery
    // (QCoreApplicationPrivate::lockThreadPostEventList) in a way that
    // survived waiting for the close transition to finish before reopening,
    // and is not a combination any OTHER test in this file exercises. The
    // guard itself only needs choose() to be called twice in a row with no
    // event-loop turn between them — reachable without a real popup
    // lifecycle or the saved list at all.
    void secondActivationBeforeCloseCompletesSendsExactlyOne()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        gif.results()->reset(
            { safeResult(QStringLiteral("giphy"),
                        QStringLiteral("dupactivation1")) });
        QCOMPARE(gif.results()->count(), 1);

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        // Two activations of the SAME row, back to back, with no event-loop
        // turn between them — the worst case the race allows.
        QVERIFY(QMetaObject::invokeMethod(picker, "choose", Q_ARG(QVariant, 0)));
        QVERIFY(QMetaObject::invokeMethod(picker, "choose", Q_ARG(QVariant, 0)));
        QCOMPARE(chosen.count(), 1);
        const QVariantMap result = chosen.at(0).at(0).toMap();
        QCOMPARE(result.value(QStringLiteral("provider")).toString(),
                 QStringLiteral("giphy"));
        QCOMPARE(result.value(QStringLiteral("gifId")).toString(),
                 QStringLiteral("dupactivation1"));
        QCOMPARE(warnings, QStringList{});
    }

    // NOT COVERED, and a PRE-EXISTING CRASH worth its own pass.
    //
    // The latch's RELEASE path (onAboutToShow's `activated = false`) has no
    // automated coverage, because the only scene that exercises it segfaults
    // for reasons that have nothing to do with this checkpoint.
    //
    // Reproduction: drive a two-picker scene matching production
    // (MessageComposerBar.qml and ThreadPanel.qml each instantiate one
    // GifPicker), then open() -> tab = "saved" (the "favorites" section,
    // before the v0.6.7 rework) -> choose() -> close() -> reopen(). That
    // crashes in
    // QCoreApplicationPrivate::lockThreadPostEventList, and waiting for the
    // close transition to finish first does not avoid it.
    //
    // It is NOT caused by the double-activation guard. Verified empirically:
    // with origin/main's qml/GifPicker.qml swapped in, the same sequence
    // still segfaults, while secondActivationBeforeCloseCompletes... fails
    // as expected without the guard. So the crash is a pre-existing
    // fragility in GifPicker.qml's real Popup/saved-list lifecycle, reachable
    // by an ordinary user sequence, and it survived this long only because
    // nothing had ever driven that sequence.
    //
    // A test asserting the crash was deliberately NOT committed: a red suite
    // helps nobody. Fixing the lifecycle, and then covering the latch reset,
    // belongs in a dedicated checkpoint.

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(GifPickerSelectionQmlTest)
#include "GifPickerSelectionQmlTest.moc"
