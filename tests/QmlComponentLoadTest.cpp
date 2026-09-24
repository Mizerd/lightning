// Every component in this list must actually load.
//
// A load-time QML error (such as assigning `font.families`) is invisible to
// qmlformat, qmlcachegen and source-text contract scans, and makes the
// component and every parent unavailable.
//
// # Adding a component
//
// Put its name in kComponents. If it cannot load standalone (it needs a
// required property, a parent, or is a delegate), list it in kNotLoadable
// with the reason rather than leaving it out silently.
//
// # This proves loading, not correctness
//
// A failure inside a `Loader`'s `sourceComponent` still leaves the root
// loading fine.

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
    "StickerPackEditor",     // pack CRUD dialog
    "QrLoginDialog",         // MSC4108 sign-in-another-device
    "PolicyListDialog",      // Mjolnir-style moderation lists
    "AddWidgetDialog",       // widget kind picker
    "MemberProfilePopover",  // carries the policy-list notice and its Connections
    "CallPipWindow",         // the floating call window
    // The in-call device menu (a Slider, Labels and a Layout inside a Menu)
    // and its Settings sibling.
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
    "HomePane",
    // Collapsed-embed summary row; reads only AppTheme and Icon.
    "CollapsedEmbedRow",
    // Space Home lobby; every input has an empty default and it reads no `app`.
    "SpaceLobby",
    // Space kick/ban confirmation; renders app.spaceModeration.
    "SpaceMemberActionDialog",
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
        // A binding loop is a load-time fact: the component loads, but Qt
        // abandons the binding and the property keeps whatever the aborted
        // evaluation left behind.
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

    // HomePane must resolve its greeting without a binding loop. Unlike the
    // data-driven case above, this needs a real active account id at creation:
    // `displayName` falls back to the localpart and so reads `activeUserId`.
    // Asserts both that no loop was reported and that the greeting resolved.
    void theHomePaneResolvesItsGreetingWithoutABindingLoop()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        // A record with no display name, so the greeting takes the localpart
        // fallback. It must be upserted first: `setActiveUser` refuses an id
        // with no saved record. The secret store is detached first so
        // `saveSession` cannot write a token to the real keyring.
        const QString uid = QStringLiteral("@alice:mock.local");
        controller.settings()->setSecretStore(nullptr);
        controller.settings()->saveSession(QStringLiteral("https://mock.local"),
                                           uid, QStringLiteral("MOCKDEV"),
                                           QString());
        // Removed again on every exit path: this suite's QSettings file
        // outlives the process and would change the data-driven case's next
        // run.
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

        // Read first, then scan: the loop fires on lazy first-read
        // evaluation.
        QCOMPARE(root->property("activeUserId").toString(), uid);
        QCOMPARE(root->property("displayName").toString(),
                 QStringLiteral("alice"));
        QCoreApplication::processEvents();

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
    }

    // `enabled` propagates to children, so `enabled: false` on a delegate
    // root also disables every control inside it. A text scan, because these
    // delegates are only built when a model supplies rows; the `found` guard
    // stops it from sweeping nothing after a rename.
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
            // Comments are stripped first, so an explanation of the rule does
            // not trip the scan.
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
            // Every delegate block, from `delegate:` to end of file: a coarse
            // bound, enough to see whether `enabled: false` and an interactive
            // control share one delegate.
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
