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

    void favoriteClickChoosesExactFavoriteNotTrending()
    {
        AppController controller(AppController::MockBackend);
        auto *gif = controller.gif();
        QVERIFY(gif != nullptr);

        // Two favorites from different providers, deliberately NOT present
        // in the browse results model (which stays empty — the live bug
        // substituted its first row).
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
        QQmlProperty::write(picker, QStringLiteral("section"),
                            QStringLiteral("favorites"));
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("section"))
                     .toString(),
                 QStringLiteral("favorites"));

        // Click row 1 of the VISIBLE favorites grid (the klipy favorite —
        // favorites prepend newest first, so verify by identity, not
        // position assumptions).
        const QVariantMap expected = gif->favorites()->get(1);
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

    // v0.6.6 live-bug fix: `gif.starredStore.model()` in GifPicker.qml's
    // activeModel binding called a plain, non-Q_INVOKABLE C++ method — QML
    // cannot call that, the binding throws a TypeError, Qt's
    // QQmlBinding::update catches it and (silently) leaves activeModel at
    // its PREVIOUS value. Since activeModel starts out (starredTabActive
    // false) bound to gif.results, selecting the Starred tab left the grid
    // still showing trending GIPHY results — while the tab's header/footer/
    // search-field visibility all correctly switched, because those bind on
    // starredTabActive directly, never through the throwing expression.
    // Every previous guard on this (QmlBindingContractTest,
    // GifPickerRedesignContractTest) only text-scanned the QML source for
    // the call; neither ever set starredTabActive on a real, running picker,
    // so the throwing branch was never actually evaluated. This test drives
    // the real engine, the real GifStarredStore, and asserts both the model
    // identity AND that no QML warning (i.e. no caught binding exception)
    // was ever emitted — a text scan can never prove that on its own.
    void starredTabBindsTheStarredModelNotResults()
    {
        QTemporaryDir starredDir;
        QVERIFY(starredDir.isValid());
        GifSearchController gif;
        gif.openStarredStoreFor(starredDir.path());
        QVERIFY(gif.starredStore()->isOpen());

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

        // Select the Starred tab exactly like the provider
        // SegmentedControl's onActivated("starred") branch does.
        QQmlProperty::write(picker, QStringLiteral("starredTabActive"), true);
        QCoreApplication::processEvents();
        QVERIFY(QQmlProperty::read(picker, QStringLiteral("starredTabActive"))
                     .toBool());

        auto *gridObj =
            picker->findChild<QObject *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);

        // activeModel (and the grid bound to it) must be the STARRED model
        // — never gif.results (trending).
        QObject *activeModel =
            QQmlProperty::read(picker, QStringLiteral("activeModel"))
                .value<QObject *>();
        QCOMPARE(activeModel, static_cast<QObject *>(gif.starredStore()->model()));
        QVERIFY(activeModel != static_cast<QObject *>(gif.results()));

        QObject *gridModel =
            QQmlProperty::read(gridObj, QStringLiteral("model")).value<QObject *>();
        QCOMPARE(gridModel, static_cast<QObject *>(gif.starredStore()->model()));

        // The grid actually renders the ONE starred row, not the TWO
        // trending rows the bug left it showing — a count assertion that
        // discriminates on its own, not only alongside the identity checks.
        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        // No caught binding exception anywhere in this sequence — the
        // regression's actual failure mode (a swallowed TypeError) would
        // otherwise show up here even if some other code path happened to
        // still produce the right model.
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
    // (a raw GifSearchController + FakeGifApp, default "browse" section,
    // results injected directly) rather than the two-picker
    // ApplicationWindow/kTwoPickerScene harness openingOnePickerClosesThe-
    // Other uses: that combination — a real Popup open()/close()/reopen
    // cycle together with a "favorites" section switch — segfaulted deep in
    // Qt's own event-posting machinery
    // (QCoreApplicationPrivate::lockThreadPostEventList) in a way that
    // survived waiting for the close transition to finish before reopening,
    // and is not a combination any OTHER test in this file exercises. The
    // guard itself only needs choose() to be called twice in a row with no
    // event-loop turn between them — reachable without a real popup
    // lifecycle or the favorites section at all.
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
    // GifPicker), then open() -> section = "favorites" -> choose() ->
    // close() -> reopen(). That crashes in
    // QCoreApplicationPrivate::lockThreadPostEventList, and waiting for the
    // close transition to finish first does not avoid it.
    //
    // It is NOT caused by the double-activation guard. Verified empirically:
    // with origin/main's qml/GifPicker.qml swapped in, the same sequence
    // still segfaults, while secondActivationBeforeCloseCompletes... fails
    // as expected without the guard. So the crash is a pre-existing
    // fragility in GifPicker.qml's real Popup/favorites lifecycle, reachable
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
