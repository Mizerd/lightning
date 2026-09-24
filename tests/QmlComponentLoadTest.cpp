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
#include <QScopeGuard>
#include <QSignalSpy>
#include <QFile>
#include <QtTest/QtTest>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"

namespace {

// Components that must load with nothing but `app` in context.
constexpr const char *kComponents[] = {
    // v0.9.0 additions — the reason this suite exists now.
    "StickerPackEditor",     // pack CRUD dialog
    "QrLoginDialog",         // MSC4108 sign-in-another-device
    "PolicyListDialog",      // Mjolnir-style moderation lists
    "AddWidgetDialog",       // widget kind picker
    "MemberProfilePopover",  // carries the policy-list notice and its Connections
    "CallPipWindow",         // the floating call window
    // 2026-09-12: the in-call device menu now carries the microphone LEVEL
    // as well as the device list — a Slider, two Labels and a Layout inside
    // a Menu, which is exactly the shape whose load-time errors nothing else
    // in this repository can see. Its sibling in Settings is listed beside
    // it for the same reason.
    "CallDeviceMenu",
    "CallDeviceSettings",
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
    "EncryptionBrokenPrompt",  // B011: the undecryptable-device card
    "CallHeaderBar",
    "ActivityCenterPanel",
    "JumpToDateDialog",
    "WidgetOpenSheet",
    // 2026-09-18: added with the binding-loop assertion below, which it was
    // the reason for. It had been in neither list — an omission, which is
    // the thing this file's header asks not to happen.
    "HomePane",
    // 2026-09-19: the collapsed-embed summary row. Loads standalone — no
    // required properties, and it reads only AppTheme and Icon.
    "CollapsedEmbedRow",
    // 2026-09-23: the Space Home lobby. Loads standalone — every input is a
    // plain property with an empty default, and it reads no `app`.
    "SpaceLobby",
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
        // A BINDING LOOP IS A LOAD-TIME FACT AND NOTHING ELSE HERE CAN SEE
        // IT. The component loads, the root is non-null, every contract scan
        // over its source text passes — and Qt has ABANDONED one of its
        // bindings, so the property keeps whatever value the aborted
        // evaluation left behind. HomePane shipped one for however long:
        // `displayName` read `activeUserId`, that read evaluated
        // `activeUserId`'s own binding for the first time, and the resulting
        // change handler wrote a dependency of the binding still on the
        // stack.
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
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

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(component + QStringLiteral(": ") + warning));
        }
    }

    // A PANE THAT GREETS THE USER BY NAME MUST NOT LOOP RESOLVING IT.
    //
    // WHY THIS IS NOT THE DATA-DRIVEN CASE ABOVE. That one loads every
    // component against a controller whose ACTIVE ACCOUNT IS EMPTY — the
    // mock login leaves no saved record, so `app.accounts.activeUserId` is
    // "" and a binding on it never changes value. The loop under test needs
    // exactly what production has and that fixture does not: a real active
    // id at the moment the pane is created.
    //
    // THE LOOP. `displayName` falls back to the localpart, so it reads
    // `activeUserId`. `activeUserId` WAS a binding on the manager's
    // property, and its first evaluation therefore happened inside
    // `displayName`'s — moving it from "" to the real id, firing
    // `onActiveUserIdChanged` synchronously, and writing `activeAccount`,
    // which `displayName` had already captured as a dependency. Qt abandons
    // an evaluation whose dependencies move under it, so the greeting kept
    // whatever the aborted pass left behind.
    //
    // The assertion is therefore in two halves: no loop was reported, AND
    // the greeting actually resolved. The second is what the user sees.
    void theHomePaneResolvesItsGreetingWithoutABindingLoop()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        // A record with NO display name on purpose: that is what sends the
        // greeting down the localpart fallback, which is the branch that
        // reads `activeUserId` at all. The record has to be UPSERTED first —
        // `setActiveUser` refuses an id it has no saved record for and
        // clears the selection instead, which is how the data-driven case
        // above ends up with an empty id and cannot see this at all.
        //
        // THE SECRET STORE IS DETACHED FIRST, and that is not incidental:
        // `saveSession` is the public way to create a record, and with a
        // store attached it would write an access token into the
        // maintainer's real keyring. Detached, the token branch is skipped
        // entirely and this writes a RECORD and nothing else.
        const QString uid = QStringLiteral("@alice:mock.local");
        controller.settings()->setSecretStore(nullptr);
        controller.settings()->saveSession(QStringLiteral("https://mock.local"),
                                           uid, QStringLiteral("MOCKDEV"),
                                           QString());
        // AND IT IS REMOVED AGAIN ON EVERY EXIT PATH. This suite's QSettings
        // is a real file that outlives the process, so a record left behind
        // would change what the DATA-DRIVEN case above sees on the next run
        // — it would find a saved account where this run found none. A test
        // that behaves differently on its second run is a flake waiting for
        // a busy machine.
        const auto forgetAccount = qScopeGuard([&controller, &uid] {
            controller.accounts()->removeAccount(uid);
        });
        QCOMPARE(controller.accounts()->activeUserId(), uid);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("HomePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(8000));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        // THE READS COME FIRST, and the scan after them. The defect under
        // test IS lazy first-read evaluation: scanning before anything forces
        // a read would miss a loop that fires on the read itself, which is
        // precisely the failure mode if this pane ever stopped binding
        // `displayName` to a Label that is built at load.
        QCOMPARE(root->property("activeUserId").toString(), uid);
        QCOMPARE(root->property("displayName").toString(),
                 QStringLiteral("alice"));
        QCoreApplication::processEvents();

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
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
