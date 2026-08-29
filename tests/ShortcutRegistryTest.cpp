// ShortcutRegistry — the rules that make rebinding safe rather than merely
// possible.
//
// HONESTY ABOUT WHAT THESE ARE. Nothing here is a regression test in the
// CLAUDE.md §18 sense: before this round there was no rebinding
// infrastructure at all, so there is no "old code" for a case to fail
// against. What each case DOES do is pin one rule against the obvious naive
// implementation of the same feature, and each is annotated with which naive
// implementation it catches. Those are real: every one of them is a shape a
// shortcut editor is normally written in.
//
// WHAT IS NOT COVERED, AND IS NOT CLAIMED. Whether a rebound sequence
// actually reaches a live `Shortcut` in a running client, whether the capture
// control receives a key that is already bound (it relies on Qt delivering a
// ShortcutOverride to the focus item), and whether Qt's ambiguous-overload
// behaviour is what the conflict rule says it is — all three need a running
// GUI and are NOT TESTED here.

#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Every row's resolved sequence, keyed by action id.
QHash<QString, QString> resolvedById(const ShortcutRegistry &registry)
{
    QHash<QString, QString> out;
    for (int row = 0; row < registry.rowCount(); ++row) {
        const QModelIndex idx = registry.index(row);
        out.insert(registry.data(idx, ShortcutRegistry::IdRole).toString(),
                   registry.data(idx, ShortcutRegistry::CurrentSequenceRole)
                       .toString());
    }
    return out;
}

QString roleFor(const ShortcutRegistry &registry, const QString &actionId,
                int role)
{
    for (int row = 0; row < registry.rowCount(); ++row) {
        const QModelIndex idx = registry.index(row);
        if (registry.data(idx, ShortcutRegistry::IdRole).toString() == actionId)
            return registry.data(idx, role).toString();
    }
    return QStringLiteral("<no such action>");
}

} // namespace

class ShortcutRegistryTest : public QObject
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
            QStringLiteral("shortcut-registry-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ── The seed list itself ────────────────────────────────────────────

    // NAIVE IMPLEMENTATION THIS CATCHES: a seed list written by hand with a
    // duplicate default — which is exactly what happens when someone adds
    // "Bold = Ctrl+B" to a list that already contains "toggle room list =
    // Ctrl+B" without noticing. Shipping that would make BOTH actions dead
    // on first launch, before anyone had rebound anything.
    void noTwoActionsShipOnTheSameDefaultInOneContext()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QHash<QString, QHash<QString, QString>> seenPerContext;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString ctx =
                registry.data(idx, ShortcutRegistry::ContextRole).toString();
            const QString seq =
                registry.data(idx, ShortcutRegistry::DefaultSequenceRole)
                    .toString();
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            auto &seen = seenPerContext[ctx];
            QVERIFY2(!seen.contains(seq),
                     qPrintable(QStringLiteral(
                                    "%1 and %2 both default to %3 in context "
                                    "%4 — Qt would fire neither")
                                    .arg(seen.value(seq), id, seq, ctx)));
            seen.insert(seq, id);
        }
        QCOMPARE(registry.conflictCount(), 0);
    }

    // NAIVE IMPLEMENTATION THIS CATCHES: a seed list that only knows about
    // its own rows. Escape and the message menu's single-letter accelerators
    // stay hard-coded in QML, so a registry that does not carry them would
    // cheerfully hand Escape to "Quit" and break every dialog in the client
    // at the same time — with the settings page reporting no conflict.
    void everySeededDefaultAvoidsTheHardCodedKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString seq =
                registry.data(idx, ShortcutRegistry::DefaultSequenceRole)
                    .toString();
            QVERIFY2(!registry.isReserved(seq),
                     qPrintable(QStringLiteral("default %1 is reserved for %2")
                                    .arg(seq, registry.reservedOwner(seq))));
        }
        // And the reserved list is not vacuously empty — a list that matched
        // nothing would pass the loop above while defending nothing at all.
        QVERIFY(registry.isReserved(QStringLiteral("Esc")));
        QVERIFY(registry.isReserved(QStringLiteral("Escape")));
        QVERIFY(registry.isReserved(QStringLiteral("Alt+V")));
        QVERIFY(registry.isReserved(QStringLiteral("Ctrl+C")));
        QVERIFY(registry.isReserved(QStringLiteral("Space")));
    }

    // ── Rule (a): a Global action needs a real modifier ─────────────────

    // NAIVE IMPLEMENTATION THIS CATCHES: accepting whatever the capture
    // control reports. Qt dispatches QEvent::Shortcut BEFORE the focused
    // item sees the key, so a global Shortcut on a bare letter takes that
    // letter away from every text field in the application — INCLUDING the
    // capture field that would undo it. Commit 4c2317f is this defect
    // happening once already, with Space.
    void aGlobalActionRefusesAModifierLessSequence()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        for (const QString &bad : { QStringLiteral("B"), QStringLiteral("F5"),
                                    QStringLiteral("Shift+B"),
                                    QStringLiteral("Space") }) {
            const QString error = registry.setBinding(
                QStringLiteral("nav.quickSwitcher"), bad);
            QVERIFY2(!error.isEmpty(),
                     qPrintable(QStringLiteral("%1 was accepted").arg(bad)));
        }
        // Refusal must also be a NO-OP: a half-applied rebind that stores the
        // value and returns an error is the worst of both.
        QCOMPARE(registry.sequenceFor(QStringLiteral("nav.quickSwitcher")),
                 QStringLiteral("Ctrl+K"));
        QVERIFY(!registry.anyCustomised());
    }

    // Shift is deliberately NOT a qualifying modifier: Shift+B is how a
    // capital B is typed. A naive `modifiers != NoModifier` check passes it.
    void shiftAloneIsNotAModifier()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QVERIFY(!registry
                     .validationError(QStringLiteral("nav.quickSwitcher"),
                                      QStringLiteral("Shift+F7"))
                     .isEmpty());
        QVERIFY(registry
                    .validationError(QStringLiteral("nav.quickSwitcher"),
                                     QStringLiteral("Ctrl+Shift+F7"))
                    .isEmpty());
    }

    // ── Rule (b): a duplicate binding kills BOTH actions ────────────────

    // NAIVE IMPLEMENTATION THIS CATCHES: storing the conflicting binding and
    // drawing a warning triangle next to it. Two enabled Shortcuts on one
    // sequence make Qt report an ambiguous overload and fire NEITHER, so a
    // stored conflict is not "a setting with a caveat", it is two actions
    // silently switched off — and the symptom points nowhere near the page
    // that caused it.
    void aConflictingBindingIsRefusedRatherThanStored()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        const QString error = registry.setBinding(
            QStringLiteral("nav.messageSearch"), QStringLiteral("Ctrl+K"));
        QVERIFY(!error.isEmpty());
        QVERIFY2(error.contains(registry.descriptionFor(
                     QStringLiteral("nav.quickSwitcher"))),
                 "the refusal must name what already owns the key");
        QCOMPARE(registry.sequenceFor(QStringLiteral("nav.messageSearch")),
                 QStringLiteral("Ctrl+Shift+F"));
        QCOMPARE(registry.conflictCount(), 0);
    }

    void aReservedSequenceIsRefusedAndSaysWhatOwnsIt()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        // Ctrl+C, not Escape. validationError refuses a MODIFIER-LESS
        // sequence before it ever consults the reserved table, so binding a
        // global action to bare Escape returns the modifier error and this
        // case passed its !isEmpty() check while testing nothing about
        // reservation. Esc is reserved AND modifier-less, so it is refused
        // twice over and cannot reach this branch at all; a reserved
        // sequence that carries a modifier is the only way in.
        const QString error = registry.setBinding(QStringLiteral("app.quit"),
                                                  QStringLiteral("Ctrl+C"));
        QVERIFY(!error.isEmpty());
        QVERIFY(error.contains(QStringLiteral("Ctrl+C")));
        QVERIFY(error.contains(QStringLiteral("Copy")));
        QCOMPARE(registry.sequenceFor(QStringLiteral("app.quit")),
                 QStringLiteral("Ctrl+Q"));
    }

    // ── Global vs Editor is a shadow, not a conflict ────────────────────

    // NAIVE IMPLEMENTATION THIS CATCHES: treating every same-sequence pair as
    // a conflict. That would forbid the one arrangement that resolves the
    // Ctrl+B collision — Bold while the message box has focus, toggle the
    // room list everywhere else — and force one of the two to be silently
    // rebound instead, which is the outcome this whole feature exists to
    // avoid.
    void aGlobalAndAnEditorActionMayShareASequenceAndSaySo()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        // Shipped that way: Bold and "toggle the conversation list" are both
        // Ctrl+B out of the box.
        const auto resolved = resolvedById(registry);
        QCOMPARE(resolved.value(QStringLiteral("composer.bold")),
                 QStringLiteral("Ctrl+B"));
        QCOMPARE(resolved.value(QStringLiteral("shell.toggleRoomList")),
                 QStringLiteral("Ctrl+B"));

        // Not a conflict…
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("composer.bold"),
                        ShortcutRegistry::ConflictsWithRole)
                    .isEmpty());
        // …but both rows say what happens, in both directions. A key that
        // does two things has to be described rather than discovered.
        QVERIFY(!roleFor(registry, QStringLiteral("composer.bold"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
        QVERIFY(!roleFor(registry, QStringLiteral("shell.toggleRoomList"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
    }

    // ── Storage ─────────────────────────────────────────────────────────

    void aBindingSurvivesANewRegistryOverTheSameSettings()
    {
        SettingsManager settings;
        {
            ShortcutRegistry registry(&settings);
            QCOMPARE(registry.setBinding(QStringLiteral("nav.messageSearch"),
                                         QStringLiteral("Ctrl+Alt+F")),
                     QString());
            QVERIFY(registry.anyCustomised());
        }
        ShortcutRegistry reopened(&settings);
        QCOMPARE(reopened.sequenceFor(QStringLiteral("nav.messageSearch")),
                 QStringLiteral("Ctrl+Alt+F"));
        QVERIFY(reopened.anyCustomised());
    }

    // NAIVE IMPLEMENTATION THIS CATCHES: storing a binding that equals the
    // default. The account then carries a pinned copy of TODAY's default and
    // stops following a later change to it, and "Reset" appears to do
    // nothing because the value it resets to is the value already stored.
    void bindingBackToTheDefaultClearsTheOverride()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QCOMPARE(registry.setBinding(QStringLiteral("view.zoomReset"),
                                     QStringLiteral("Ctrl+Alt+0")),
                 QString());
        QVERIFY(registry.anyCustomised());

        QCOMPARE(registry.setBinding(QStringLiteral("view.zoomReset"),
                                     QStringLiteral("Ctrl+0")),
                 QString());
        QVERIFY(!registry.anyCustomised());
        QSettings raw;
        QVERIFY2(!raw.contains(QStringLiteral("shortcuts/view.zoomReset")),
                 "an override equal to the default must not be stored");
    }

    // NAIVE IMPLEMENTATION THIS CATCHES: an unparseable stored value handed
    // straight to QML. A `Shortcut` bound to a string Qt cannot parse is
    // INERT and looks exactly like a shortcut that simply does not work,
    // with nothing anywhere saying why. Degrading to the default is a state
    // the user can see and act on.
    void anUnparseableStoredValueDegradesToTheDefault()
    {
        {
            QSettings raw;
            raw.setValue(QStringLiteral("shortcuts/nav.quickSwitcher"),
                         QStringLiteral("!!! not a key !!!"));
            raw.sync();
        }
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFor(QStringLiteral("nav.quickSwitcher")),
                 QStringLiteral("Ctrl+K"));
    }

    // NAIVE IMPLEMENTATION THIS CATCHES: building the QSettings key by
    // concatenation with no check on the id. An id carrying a slash would
    // address a key in a DIFFERENT group — "../session/accessToken" is the
    // shape of that mistake — so an unsafe id reads and writes nothing at
    // all rather than being quietly rewritten into a safe one.
    void anUnsafeActionIdReachesNoOtherSettingsGroup()
    {
        SettingsManager settings;
        settings.setShortcutSequence(QStringLiteral("../session/accessToken"),
                                     QStringLiteral("Ctrl+Alt+Z"));
        QSettings raw;
        QVERIFY(!raw.contains(QStringLiteral("session/accessToken")));
        QCOMPARE(settings.shortcutSequence(
                     QStringLiteral("../session/accessToken")),
                 QString());
    }

    void resetAllReturnsEveryActionToItsDefault()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.setBinding(QStringLiteral("nav.commandMode"),
                                     QStringLiteral("Ctrl+Alt+P")),
                 QString());
        QCOMPARE(registry.setBinding(QStringLiteral("composer.italic"),
                                     QStringLiteral("Ctrl+Alt+I")),
                 QString());
        QVERIFY(registry.anyCustomised());

        registry.resetAll();
        QVERIFY(!registry.anyCustomised());
        QCOMPARE(registry.sequenceFor(QStringLiteral("nav.commandMode")),
                 QStringLiteral("Ctrl+Shift+K"));
        QCOMPARE(registry.sequenceFor(QStringLiteral("composer.italic")),
                 QStringLiteral("Ctrl+I"));
        QCOMPARE(registry.conflictCount(), 0);
    }

    // NAIVE IMPLEMENTATION THIS CATCHES: announcing a rebind with
    // dataChanged alone. QML binds `Shortcut.sequences` through
    // sequenceFor(), which is a function call and creates NO dependency Qt
    // can track — so without a property the binding reads, a rebind would
    // not take effect until the component was next created. This is the
    // media-cache "imperative assignment destroys the binding" lesson in a
    // different costume, and the counter is the same remedy.
    void everyChangeBumpsARevisionQmlCanBindTo()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QSignalSpy spy(&registry, &ShortcutRegistry::bindingsChanged);
        const int before = registry.bindingRevision();

        QCOMPARE(registry.setBinding(QStringLiteral("room.markRead"),
                                     QStringLiteral("Ctrl+Alt+R")),
                 QString());
        QCOMPARE(spy.count(), 1);
        QVERIFY(registry.bindingRevision() > before);

        registry.resetToDefault(QStringLiteral("room.markRead"));
        QCOMPARE(spy.count(), 2);
    }

    // ── The capture control's input path ────────────────────────────────

    // NAIVE IMPLEMENTATION THIS CATCHES: closing the capture on the first
    // key event. A capture control receives a key event for Ctrl on its own
    // while the user is still reaching for the letter — treating that as the
    // answer would bind "Ctrl" and end the capture before the real key ever
    // arrived.
    void aBareModifierPressIsNotACapturedSequence()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFromKeyEvent(Qt::Key_Control,
                                               Qt::ControlModifier),
                 QString());
        QCOMPARE(registry.sequenceFromKeyEvent(Qt::Key_Shift,
                                               Qt::ShiftModifier),
                 QString());
        QCOMPARE(registry.sequenceFromKeyEvent(Qt::Key_B,
                                               Qt::ControlModifier),
                 QStringLiteral("Ctrl+B"));
    }

    // NAIVE IMPLEMENTATION THIS CATCHES: passing event.modifiers straight
    // through. KeypadModifier says WHICH physical key produced the
    // character, so leaving it in stores a sequence that only ever matches
    // the numeric keypad — the shortcut then "does not work" on the number
    // row the user was looking at when they set it.
    void theKeypadModifierIsStrippedFromACapturedSequence()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFromKeyEvent(
                     Qt::Key_0, Qt::ControlModifier | Qt::KeypadModifier),
                 QStringLiteral("Ctrl+0"));
    }

    // ── Presentation ────────────────────────────────────────────────────

    void everyRowCarriesACategoryAndACategoryListCoversThemAll()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QStringList categories = registry.categories();
        QVERIFY(!categories.isEmpty());
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QString category =
                registry.data(registry.index(row),
                              ShortcutRegistry::CategoryRole)
                    .toString();
            QVERIFY(!category.isEmpty());
            QVERIFY2(categories.contains(category),
                     "a row in a category the page never draws is invisible");
        }
    }

    // The QML delegate assigns the model's roles onto ShortcutRow's own
    // like-named properties. Unprefixed role names would collide with them
    // (a QML component may not redeclare a property its type already has),
    // so the prefix is load-bearing rather than cosmetic.
    void roleNamesAreNamespacedAwayFromTheDelegatesOwnProperties()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QHash<int, QByteArray> roles = registry.roleNames();
        for (const QByteArray &name : roles)
            QVERIFY(name.startsWith("shortcut"));
        QVERIFY(roles.values().contains(QByteArray("shortcutId")));
        QVERIFY(roles.values().contains(QByteArray("shortcutCurrent")));
    }

    // ── The editor-context lookup both composers share ──────────────────

    // NAIVE IMPLEMENTATION THIS CATCHES: each composer carrying its own
    // hand-written list of editor action ids. That is what the QML did, and
    // the THREAD composer had no list at all -- so Ctrl+B inside a thread
    // reply fell through to the window and toggled the conversation list
    // while the user was typing. Driving it from the registry's own
    // EditorContext flag is what makes one list impossible to forget.
    void editorActionForKeyAnswersEveryEditorBindingAndNothingElse()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QCOMPARE(registry.editorActionForKey(Qt::Key_B, Qt::ControlModifier),
                 QStringLiteral("composer.bold"));
        QCOMPARE(registry.editorActionForKey(Qt::Key_I, Qt::ControlModifier),
                 QStringLiteral("composer.italic"));

        // Every EditorContext row in the table must be reachable, so adding a
        // seventh one cannot silently go unhandled in either composer.
        int editorRows = 0;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row, 0);
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                != int(ShortcutRegistry::EditorContext))
                continue;
            ++editorRows;
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            const QString seq =
                registry.data(idx, ShortcutRegistry::CurrentSequenceRole)
                    .toString();
            const QKeySequence parsed =
                QKeySequence::fromString(seq, QKeySequence::PortableText);
            QVERIFY2(parsed.count() == 1,
                     qPrintable(QStringLiteral("unparseable: ") + seq));
            const int combo = parsed[0].toCombined();
            const int key = combo & ~int(Qt::KeyboardModifierMask);
            const int mods = combo & int(Qt::KeyboardModifierMask);
            QCOMPARE(registry.editorActionForKey(key, mods), id);
        }
        QVERIFY2(editorRows > 0, "no EditorContext rows -- the sweep is inert");

        // A GLOBAL binding must NOT resolve here, or the composer would claim
        // the ShortcutOverride for Ctrl+K and swallow the quick switcher
        // while the message box has focus.
        QVERIFY(registry.editorActionForKey(Qt::Key_K, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_Q, Qt::ControlModifier)
                    .isEmpty());
        // A bare letter is typing, never a format.
        QVERIFY(registry.editorActionForKey(Qt::Key_B, Qt::NoModifier)
                    .isEmpty());
    }

    // The lookup must follow a REBIND. Reading the default table instead
    // would leave the old sequence working and the new one dead.
    void editorActionForKeyFollowsARebind()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString error =
            registry.setBinding(QStringLiteral("composer.bold"),
                                QStringLiteral("Ctrl+Alt+B"));
        QCOMPARE(error, QString());
        QCOMPARE(registry.editorActionForKey(
                     Qt::Key_B, Qt::ControlModifier | Qt::AltModifier),
                 QStringLiteral("composer.bold"));
        QVERIFY(registry.editorActionForKey(Qt::Key_B, Qt::ControlModifier)
                    .isEmpty());
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(ShortcutRegistryTest)
#include "ShortcutRegistryTest.moc"
