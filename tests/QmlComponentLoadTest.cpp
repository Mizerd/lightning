// EVERY COMPONENT IN THIS LIST MUST ACTUALLY LOAD.
//
// A load-time QML error is invisible to everything else this repository has.
// `qmlformat` parses syntax and does not check that a property exists;
// `qmlcachegen` compiles the file without instantiating it; and every
// contract suite that reads a `.qml` file sees SOURCE TEXT, which cannot
// show that `font.families` (a C++ QFont API, absent from the QML font value
// type) makes a component unavailable and cascades into every parent — the
// failure that took four QML suites down at once.
//
// CallStage got its own gate after that round (CallUiContractTest::
// theCallStageComponentActuallyLoads). This is the same gate, generalised, so
// a new file does not have to remember to invent one.
//
// # Adding a component
//
// Put its name in kComponents. If it cannot load standalone — it needs a
// required property, or a parent, or it is a delegate — say so in
// kNotLoadable with the reason, rather than quietly leaving it out. An
// omission and a deliberate exclusion look identical in a list, which is how
// something stops being covered without anyone deciding that it should.
//
// # This proves loading, not correctness
//
// A component that loads can still be laid out wrong, and a failure inside a
// `Loader`'s `sourceComponent` leaves the ROOT loading fine. This catches the
// class of error where the component is simply unavailable.

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "app/AppController.h"
#include "auth/AuthManager.h"

namespace {

// Components that must load with nothing but `app` in context.
constexpr const char *kComponents[] = {
    // v0.9.0 additions — the reason this suite exists now.
    "StickerPackEditor",     // pack CRUD dialog
    "QrLoginDialog",         // MSC4108 sign-in-another-device
    "PolicyListDialog",      // Mjolnir-style moderation lists
    "ShareLocationDialog",   // m.location send
    "CallPipWindow",         // the floating call window
    "MediaBrowser",          // room media/files/links over all history
    "ForwardSelectionDialog",
    "EmojiCompletionPopup",
    // Long-standing surfaces with the same exposure. Cheap to cover, and
    // each one is a file somebody will edit without running its own suite.
    "CallStage",
    "SettingsScreen",
    "RoomInfoPanel",
    "GifPicker",
    "StickerPicker",
    "EmojiPicker",
    "QuickSwitcher",
    "MessageSearchDialog",
    "ThemeEditorDialog",
    "IncomingCallPrompt",
    "CallHeaderBar",
    "ActivityCenterPanel",
    "JumpToDateDialog",
    "WidgetOpenSheet",
};

// Deliberately NOT loaded standalone, each with the reason. Kept here rather
// than omitted so that "not covered" is a decision on the record.
struct Excluded { const char *name; const char *why; };
constexpr Excluded kNotLoadable[] = {
    { "MessageDelegate", "a timeline delegate: required properties come from "
                         "the model, and TimelinePaneQmlTest drives it for real" },
    { "MediaBrowserTile", "a GridView delegate with required properties" },
    { "MediaBrowserRow", "a ListView delegate with required properties" },
    { "RoomDelegate", "a room-list delegate with required properties" },
    { "CallParticipantTile", "a call-grid delegate; CallUiContractTest covers it" },
    { "Main", "the application window; StartupSessionTest loads it" },
};

} // namespace

class QmlComponentLoadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void everyListedComponentLoads_data()
    {
        QTest::addColumn<QString>("component");
        for (const char *name : kComponents)
            QTest::newRow(name) << QString::fromUtf8(name);
    }

    void everyListedComponentLoads()
    {
        QFETCH(QString, component);

        // A logged-in mock controller: most of these read `app.<something>`
        // in a creation-time binding, and a null model there is its own
        // class of load failure.
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"), component);
        if (createdSpy.isEmpty())
            QVERIFY2(createdSpy.wait(8000),
                     qPrintable(component + QStringLiteral(
                         " never finished loading")));

        // objectCreated carries a NULL object when the component failed, so
        // THIS is the assertion — the spy having fired is not enough.
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY2(root != nullptr,
                 qPrintable(component + QStringLiteral(
                     ".qml failed to load — the qWarning above names the "
                     "property or type that does not exist")));
    }

    // The exclusions are a list of DECISIONS, and a decision with no reason
    // is an omission wearing a decision's clothes.
    void everyExclusionCarriesItsReason()
    {
        for (const Excluded &e : kNotLoadable) {
            QVERIFY2(e.why != nullptr && qstrlen(e.why) > 20,
                     qPrintable(QStringLiteral(
                         "%1 is excluded without a real reason")
                         .arg(QString::fromUtf8(e.name))));
        }
    }
};

QTEST_MAIN(QmlComponentLoadTest)
#include "QmlComponentLoadTest.moc"
