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
#include <QFile>
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

    // A DELEGATE THAT DISABLES ITSELF DISABLES ITS OWN BUTTONS.
    //
    // `QQuickItem::enabled` propagates to children. Writing `enabled: false`
    // on a delegate root to stop the ROW being clickable therefore also
    // disables every control inside it — and this shipped twice in one
    // round, making rule removal and the whole per-image half of pack
    // editing unreachable while every controller-level test passed, because
    // those call the controller directly (§16's recorded lesson: a policy
    // test that invokes the policy function proves nothing about whether
    // production ever reaches it).
    //
    // A TEXT SCAN, and deliberately so: the delegates this catches are only
    // built when a model supplies rows, so an instantiation test would need
    // a live backend for each one. The `found` guard is what stops it
    // sweeping nothing when a file is renamed (the mutation-check lesson).
    void noInteractiveDelegateDisablesItself()
    {
        static constexpr const char *kFiles[] = {
            "PolicyListDialog.qml", "StickerPackEditor.qml",
            "ForwardSelectionDialog.qml", "MediaBrowserRow.qml",
            "MediaBrowserTile.qml", "EmojiCompletionPopup.qml",
        };
        int scanned = 0;
        QStringList offenders;
        for (const char *name : kFiles) {
            QFile file(QStringLiteral(QML_DIR "/") + QString::fromUtf8(name));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            // COMMENTS ARE STRIPPED FIRST. Every one of these files now
            // carries a comment saying "NOT `enabled: false`" explaining why
            // — and a scan that trips on the explanation of the rule it
            // enforces is a scan nobody can satisfy.
            QString src;
            const QStringList lines =
                QString::fromUtf8(file.readAll()).split(u'\n');
            for (const QString &line : lines) {
                const QString trimmed = line.trimmed();
                if (trimmed.startsWith(QStringLiteral("//")))
                    continue;
                src += line;
                src += u'\n';
            }
            ++scanned;
            // Every delegate block in the file, taken from `delegate:` to the
            // end of the file — a coarse bound, which is fine: what matters
            // is whether an `enabled: false` and an interactive control share
            // one delegate.
            int at = src.indexOf(QStringLiteral("delegate:"));
            while (at >= 0) {
                const int next =
                    src.indexOf(QStringLiteral("delegate:"), at + 1);
                const QString block =
                    src.mid(at, (next < 0 ? src.size() : next) - at);
                const bool disables =
                    block.contains(QStringLiteral("enabled: false"));
                // A CheckBox alone is the legitimate case — an indicator
                // whose row owns the click, with no interactive descendant.
                const bool hasControl =
                    block.contains(QStringLiteral("AppButton"))
                    || block.contains(QStringLiteral("AppTextField"))
                    || block.contains(QStringLiteral("IconButton"));
                if (disables && hasControl) {
                    offenders << QString::fromUtf8(name);
                }
                at = next;
            }
        }
        QVERIFY2(scanned >= 4,
                 "the scan found almost no files — a rename has made it "
                 "sweep nothing, which passes for the wrong reason");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a delegate sets `enabled: false` while containing a "
                     "control: enabled PROPAGATES, so that control is dead. "
                     "Offenders: %1").arg(offenders.join(u", "))));
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
