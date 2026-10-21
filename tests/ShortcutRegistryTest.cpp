// ShortcutRegistry: the rules that make rebinding safe. Each case pins one rule
// against the obvious naive implementation, noted per case.
//
// Not covered (needs a running GUI): whether a rebound sequence reaches a live
// `Shortcut`, whether the capture control receives an already-bound key (it
// relies on Qt's ShortcutOverride), and Qt's ambiguous-overload behaviour.

#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"

#include <QKeySequence>
#include <QList>
#include <QPair>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>
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
    void messageSelectionDefaultsAndLookupAreSeparateFromComposerPaging()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFor(QStringLiteral("timeline.selectLatestMessage")),
                 QStringLiteral("Ctrl+Shift+Space"));
        QCOMPARE(registry.sequenceFor(QStringLiteral("timeline.resumeMessageSelection")),
                 QStringLiteral("Alt+L"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_K, Qt::ControlModifier),
                 QStringLiteral("timeline.olderMessage"));
        QVERIFY(registry.timelineActionForKey(Qt::Key_K, Qt::ControlModifier).isEmpty());
        QCOMPARE(registry.timelineActionForKey(Qt::Key_PageUp, Qt::NoModifier),
                 QStringLiteral("timeline.pageUp"));
        QVERIFY(registry.messageSelectionActionForKey(Qt::Key_PageUp, Qt::NoModifier).isEmpty());
        QCOMPARE(registry.conflictCount(), 0);
    }

    void plainMessageKeysAreSafeOnlyInTheSelectionContext()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("K")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.newerMessage"),
                                     QStringLiteral("Shift+J")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.reply"),
                                     QStringLiteral("R")), QString());
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_R, Qt::NoModifier),
                 QStringLiteral("timeline.reply"));
        QVERIFY(registry.timelineActionForKey(Qt::Key_R, Qt::NoModifier).isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_R, Qt::NoModifier).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                           QStringLiteral("K")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("nav.quickSwitcher"),
                                           QStringLiteral("K")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.reply"),
                                           QStringLiteral("Esc")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.reply"),
                                           QStringLiteral("Space")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.reply"),
                                           QStringLiteral("Ctrl+X, Ctrl+Y")).isEmpty());
        registry.reload();
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_K, Qt::NoModifier),
                 QStringLiteral("timeline.olderMessage"));
    }

    void selectionKeysShadowGlobalActionsButConflictWithSharedTimelineKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QVERIFY(roleFor(registry, QStringLiteral("timeline.olderMessage"),
                        ShortcutRegistry::ShadowNoteRole).contains(QStringLiteral("timeline")));
        QVERIFY(roleFor(registry, QStringLiteral("nav.quickSwitcher"),
                        ShortcutRegistry::ShadowNoteRole).contains(QStringLiteral("timeline")));
        QVERIFY(!registry.validationError(QStringLiteral("timeline.olderMessage"),
                                           QStringLiteral("PgUp")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                           QStringLiteral("Ctrl+K")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.newerMessage"),
                                           QStringLiteral("Ctrl+K")).isEmpty());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("Ctrl+N")), QString());
        QVERIFY(registry.messageSelectionActionForKey(Qt::Key_K, Qt::ControlModifier).isEmpty());
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_N, Qt::ControlModifier),
                 QStringLiteral("timeline.olderMessage"));
    }

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

    // The seed list.
    //
    // No two actions ship on the same default in one context: a duplicate
    // default makes both actions dead on first launch.
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
            // The one designed share: find-previous and the GIF picker hold
            // Ctrl+Shift+G on mutually exclusive QML gates (m_exclusive).
            const bool designedShare =
                seen.contains(seq)
                && QSet<QString>({ seen.value(seq), id })
                       == QSet<QString>({ QStringLiteral("room.findPrevious"),
                                          QStringLiteral("composer.gifPicker") });
            QVERIFY2(!seen.contains(seq) || designedShare,
                     qPrintable(QStringLiteral(
                                    "%1 and %2 both default to %3 in context "
                                    "%4 — Qt would fire neither")
                                    .arg(seen.value(seq), id, seq, ctx)));
            seen.insert(seq, id);
        }
        QCOMPARE(registry.conflictCount(), 0);
    }

    // Seeded defaults avoid the keys hard-coded in QML (Escape, the message
    // menu's single-letter accelerators), which the registry must know about.
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
        // The reserved list is not empty, or the loop above defends nothing.
        QVERIFY(registry.isReserved(QStringLiteral("Esc")));
        QVERIFY(registry.isReserved(QStringLiteral("Escape")));
        QVERIFY(registry.isReserved(QStringLiteral("Alt+V")));
        QVERIFY(registry.isReserved(QStringLiteral("Ctrl+C")));
        QVERIFY(registry.isReserved(QStringLiteral("Space")));
    }

    // Every seeded default survives the QKeySequence::PortableText round trip:
    // normalize() returns "" for anything Qt cannot parse, and an empty
    // sequence makes an inert `Shortcut`.
    void everySeededDefaultSurvivesThePortableTextRoundTrip()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            const QString seq =
                registry.data(idx, ShortcutRegistry::DefaultSequenceRole)
                    .toString();
            QVERIFY2(!seq.isEmpty(),
                     qPrintable(QStringLiteral("%1 has an unparseable default")
                                    .arg(id)));
            QCOMPARE(ShortcutRegistry::normalize(seq), seq);
            // ...and the resolved sequence QML binds agrees with it on a clean
            // profile.
            QCOMPARE(registry.data(idx, ShortcutRegistry::CurrentSequenceRole)
                         .toString(),
                     seq);
        }
    }

    // Every seeded default carries a real modifier, in both contexts: the
    // modifier rule is otherwise only enforced on user rebinds, and a bare-key
    // default would take that key from every text field.
    void everySeededDefaultCarriesARealModifier()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        int checkedRows = 0;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            const QString seq =
                registry.data(idx, ShortcutRegistry::DefaultSequenceRole)
                    .toString();
            // A Timeline action is a Keys handler, not a Shortcut, so the
            // non-text keys (PgUp, PgDn) are legal bare.
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                    == ShortcutRegistry::TimelineContext
                && (seq == QStringLiteral("PgUp")
                    || seq == QStringLiteral("PgDown"))) {
                ++checkedRows;
                continue;
            }
            ++checkedRows;
            const QKeySequence parsed =
                QKeySequence::fromString(seq, QKeySequence::PortableText);
            QVERIFY2(parsed.count() == 1,
                     qPrintable(QStringLiteral("unparseable: ") + seq));
            const Qt::KeyboardModifiers mods = parsed[0].keyboardModifiers();
            QVERIFY2(mods.testFlag(Qt::ControlModifier)
                         || mods.testFlag(Qt::AltModifier)
                         || mods.testFlag(Qt::MetaModifier),
                     qPrintable(QStringLiteral(
                                    "%1 ships %2, which carries no Ctrl/Alt/"
                                    "Meta — it would be eaten before any text "
                                    "field saw the key")
                                    .arg(id, seq)));
        }
        QVERIFY2(checkedRows > 0, "no rows at all — the sweep is inert");
    }

    // The named additions ship on exactly these ids and keys: the rule sweeps
    // iterate whatever rows exist, so a missing or mistyped row would pass them
    // all.
    void theDiscordStyleAdditionsShipOnExactlyTheseKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const auto resolved = resolvedById(registry);

        const QVector<QPair<QString, QString>> expected = {
            { QStringLiteral("app.shortcutsHelp"), QStringLiteral("Ctrl+/") },
            { QStringLiteral("app.openSettings"), QStringLiteral("Ctrl+,") },
            { QStringLiteral("room.togglePeople"), QStringLiteral("Ctrl+U") },
            { QStringLiteral("room.togglePinned"),
              QStringLiteral("Ctrl+Shift+P") },
            { QStringLiteral("composer.emojiPicker"),
              QStringLiteral("Ctrl+Shift+E") },
            { QStringLiteral("composer.gifPicker"),
              QStringLiteral("Ctrl+Shift+G") },
            { QStringLiteral("composer.attach"),
              QStringLiteral("Ctrl+Shift+O") },
            { QStringLiteral("call.toggleCamera"),
              QStringLiteral("Ctrl+Shift+V") },
            { QStringLiteral("call.returnToCall"),
              QStringLiteral("Ctrl+Alt+A") },
        };
        for (const auto &row : expected) {
            QVERIFY2(resolved.contains(row.first),
                     qPrintable(QStringLiteral("%1 is not a row at all — every "
                                               "sweep in this file would skip "
                                               "it silently")
                                    .arg(row.first)));
            QCOMPARE(resolved.value(row.first), row.second);
        }
        QCOMPARE(registry.conflictCount(), 0);

        // The widened row replaced the old narrow one; both defaulting to
        // Ctrl+, would conflict.
        QVERIFY(!resolved.contains(QStringLiteral("app.settingsSearch")));
    }

    // Room history: the conventional browser keys, free of every other
    // binding and reserved sequence (so neither is left unbound).
    void roomHistoryShipsOnAltLeftAndAltRight()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const auto resolved = resolvedById(registry);
        QVERIFY(resolved.contains(QStringLiteral("nav.historyBack")));
        QVERIFY(resolved.contains(QStringLiteral("nav.historyForward")));
        QCOMPARE(resolved.value(QStringLiteral("nav.historyBack")),
                 QStringLiteral("Alt+Left"));
        QCOMPARE(resolved.value(QStringLiteral("nav.historyForward")),
                 QStringLiteral("Alt+Right"));
        QCOMPARE(registry.conflictCount(), 0);
        // Nothing else resolves to either key.
        int left = 0, right = 0;
        for (auto it = resolved.cbegin(); it != resolved.cend(); ++it) {
            left += it.value() == QStringLiteral("Alt+Left");
            right += it.value() == QStringLiteral("Alt+Right");
        }
        QCOMPARE(left, 1);
        QCOMPARE(right, 1);
    }

    // More rows pinned by id and key, since sweeps pass vacuously on a missing
    // row. Each is wired in qml/MainScreen.qml, which
    // theShortcutRowsWiredInQmlCoverEveryRegistryId keeps true.
    void theDiscordAuditAdditionsShipOnExactlyTheseKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const auto resolved = resolvedById(registry);

        const QVector<QPair<QString, QString>> expected = {
            { QStringLiteral("nav.newDirectMessage"),
              QStringLiteral("Ctrl+Shift+T") },
            { QStringLiteral("nav.activityCenter"),
              QStringLiteral("Ctrl+Shift+I") },
            { QStringLiteral("room.markAllRead"),
              QStringLiteral("Ctrl+Shift+A") },
            { QStringLiteral("room.markUnread"),
              QStringLiteral("Ctrl+Shift+R") },
            { QStringLiteral("call.startCall"),
              QStringLiteral("Ctrl+Shift+C") },
            { QStringLiteral("call.leave"), QStringLiteral("Ctrl+Shift+Y") },
            { QStringLiteral("call.toggleScreenShare"),
              QStringLiteral("Ctrl+Shift+S") },
            // The voice/audio player (design-media U5); never Ctrl+Alt.
            { QStringLiteral("media.focusPlayer"),
              QStringLiteral("Ctrl+Shift+J") },
        };
        for (const auto &row : expected) {
            QVERIFY2(resolved.contains(row.first),
                     qPrintable(QStringLiteral("%1 is not a row at all — every "
                                               "sweep in this file would skip "
                                               "it silently")
                                    .arg(row.first)));
            QCOMPARE(resolved.value(row.first), row.second);
        }
        QCOMPARE(registry.conflictCount(), 0);

        // Ctrl+B is Bold in the message box and the room-list toggle
        // elsewhere; pinned here too, since the cross-context case would pass
        // if both moved to another key together.
        QCOMPARE(resolved.value(QStringLiteral("composer.bold")),
                 QStringLiteral("Ctrl+B"));
        QCOMPARE(resolved.value(QStringLiteral("shell.toggleRoomList")),
                 QStringLiteral("Ctrl+B"));
    }

    // "Is every row wired in QML?" needs QML_DIR, which this target lacks; it
    // lives in CallUiContractTest::everyGlobalShortcutRowIsActuallyBoundInQml.

    // The picker/attach actions are Global, not EditorContext: both composers
    // pass an EditorContext hit to applyFormat() by stripping "composer.", so
    // "emojiPicker" would arrive as an unknown format.
    void theMessageBoxSurfaceActionsAreGlobalRatherThanEditorFormats()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        for (const QString &id : { QStringLiteral("composer.emojiPicker"),
                                   QStringLiteral("composer.gifPicker"),
                                   QStringLiteral("composer.attach") }) {
            QCOMPARE(roleFor(registry, id, ShortcutRegistry::ContextRole),
                     QString::number(int(ShortcutRegistry::GlobalContext)));
        }
        // ...and the composers' lookup refuses them, which protects
        // applyFormat().
        const int shiftCtrl = int(Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(registry.editorActionForKey(Qt::Key_E, shiftCtrl).isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_G, shiftCtrl).isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_O, shiftCtrl).isEmpty());
        // The neighbouring editor key is unaffected: Ctrl+E is still code.
        QCOMPARE(registry.editorActionForKey(Qt::Key_E, Qt::ControlModifier),
                 QStringLiteral("composer.code"));
    }

    // Ctrl+U is deliberately bound twice like Ctrl+B: Underline in the message
    // box, the people panel elsewhere. Cross-context pairs are not conflicts.
    void ctrlUIsTheSecondDeliberateDualBindingAndBothRowsSaySo()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const auto resolved = resolvedById(registry);

        QCOMPARE(resolved.value(QStringLiteral("composer.underline")),
                 QStringLiteral("Ctrl+U"));
        QCOMPARE(resolved.value(QStringLiteral("room.togglePeople")),
                 QStringLiteral("Ctrl+U"));

        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("room.togglePeople"),
                        ShortcutRegistry::ConflictsWithRole)
                    .isEmpty());
        QVERIFY(!roleFor(registry, QStringLiteral("room.togglePeople"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
        QVERIFY(!roleFor(registry, QStringLiteral("composer.underline"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
        // The editor half still resolves, so the composer claims the
        // ShortcutOverride and the global action does not run while typing.
        QCOMPARE(registry.editorActionForKey(Qt::Key_U, Qt::ControlModifier),
                 QStringLiteral("composer.underline"));
    }

    // Ctrl+B is Bold only with a selection; with none the composer must not
    // claim ShortcutOverride, so the app-level panel toggle fires.
    void ctrlBReachesThePanelToggleFromAnEmptyComposerSelection()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const int ctrl = int(Qt::ControlModifier);

        QVERIFY(registry.editorOverrideFor(Qt::Key_B, ctrl, false).isEmpty());
        QCOMPARE(registry.editorOverrideFor(Qt::Key_B, ctrl, true),
                 QStringLiteral("composer.bold"));
        // Keys with no Global twin are claimed regardless of selection.
        QCOMPARE(registry.editorOverrideFor(Qt::Key_E, ctrl, false),
                 QStringLiteral("composer.code"));
        // Unrelated keys are never claimed.
        QVERIFY(registry.editorOverrideFor(Qt::Key_K, ctrl, true).isEmpty());
    }

    // The composer yields a key a Global action also holds when nothing is
    // selected (editorOverrideFor), so the hint on both rows must say the
    // override applies to a selection only, not to "focus" (C75-F02).
    void shadowNotesSayTheEditorWinsOnlyOverASelection()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        for (const QString &id : { QStringLiteral("composer.bold"),
                                   QStringLiteral("composer.underline"),
                                   QStringLiteral("room.togglePeople"),
                                   QStringLiteral("shell.toggleRoomList") }) {
            const QString note = roleFor(registry, id,
                                         ShortcutRegistry::ShadowNoteRole);
            QVERIFY2(note.contains(QStringLiteral("selected")),
                     qPrintable(id + QStringLiteral(": ") + note));
        }
    }

    // Pin intentional shadows, including selection's timeline-only Ctrl+K
    // and Ctrl+U. conflictCount() cannot identify an accidental legal share.
    void theOnlyCrossContextSharesAreTheDesignedOnes()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QHash<QString, QStringList> idsBySequence;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            idsBySequence[registry
                              .data(idx,
                                    ShortcutRegistry::CurrentSequenceRole)
                              .toString()]
                .append(registry.data(idx, ShortcutRegistry::IdRole)
                            .toString());
        }
        QStringList shared;
        for (auto it = idsBySequence.cbegin(); it != idsBySequence.cend();
             ++it) {
            if (it.value().size() > 1)
                shared.append(it.key());
        }
        shared.sort();
        QCOMPARE(shared,
                 QStringList({ QStringLiteral("Ctrl+B"),
                               QStringLiteral("Ctrl+K"),
                               QStringLiteral("Ctrl+Shift+G"),
                               QStringLiteral("Ctrl+U") }));
        QCOMPARE(idsBySequence.value(QStringLiteral("Ctrl+U")).size(), 3);
        QCOMPARE(idsBySequence.value(QStringLiteral("Ctrl+K")).size(), 2);
    }

    // GitHub #38: PgUp/PgDn, Ctrl+Up/Down and Ctrl+G / Ctrl+Shift+G are
    // rebindable rows that ship on exactly these keys.
    void theTimelineAndFindKeysShipOnExactlyTheseKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QHash<QString, QString> resolved = resolvedById(registry);
        QCOMPARE(resolved.value(QStringLiteral("timeline.pageUp")),
                 QStringLiteral("PgUp"));
        QCOMPARE(resolved.value(QStringLiteral("timeline.pageDown")),
                 QStringLiteral("PgDown"));
        QCOMPARE(resolved.value(QStringLiteral("timeline.jumpToStart")),
                 QStringLiteral("Ctrl+Up"));
        QCOMPARE(resolved.value(QStringLiteral("timeline.jumpToEnd")),
                 QStringLiteral("Ctrl+Down"));
        QCOMPARE(resolved.value(QStringLiteral("room.findNext")),
                 QStringLiteral("Ctrl+G"));
        QCOMPARE(resolved.value(QStringLiteral("room.findPrevious")),
                 QStringLiteral("Ctrl+Shift+G"));
        // The designed share with the GIF picker is not reported as a conflict.
        QCOMPARE(resolved.value(QStringLiteral("composer.gifPicker")),
                 QStringLiteral("Ctrl+Shift+G"));
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("room.findPrevious"),
                        ShortcutRegistry::ConflictsWithRole).isEmpty());
        // And the two plain-Up/Down composer keys are untouched: nothing is
        // bound to a bare arrow, which the message box uses for history/edit.
        for (auto it = resolved.cbegin(); it != resolved.cend(); ++it) {
            QVERIFY2(it.value() != QStringLiteral("Up")
                         && it.value() != QStringLiteral("Down"),
                     qPrintable(it.key()));
        }
    }

    // A Timeline action may be bound bare only to a key that types nothing.
    void aTimelineActionAcceptsBarePageKeysAndFKeysOnly()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString id = QStringLiteral("timeline.pageUp");
        QVERIFY(registry.validationError(id, QStringLiteral("PgDown")).isEmpty()
                == false); // taken by timeline.pageDown
        QVERIFY(registry.validationError(id, QStringLiteral("F5")).isEmpty());
        QVERIFY(registry.validationError(id, QStringLiteral("Ctrl+Left"))
                    .isEmpty());
        // A letter or an arrow bare is what the message box types or moves by.
        QVERIFY(!registry.validationError(id, QStringLiteral("X")).isEmpty());
        QVERIFY(!registry.validationError(id, QStringLiteral("Up")).isEmpty());
        // A Global action still may not use PgUp bare.
        QVERIFY(!registry.validationError(QStringLiteral("room.find"),
                                          QStringLiteral("PgUp")).isEmpty());
    }

    // A Timeline sequence collides with Global and Editor ones too: a Global
    // Shortcut would take the key first, an Editor format would claim it in
    // the same box.
    void aTimelineSequenceConflictsAcrossContexts()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        // Ctrl+B is Global (panel) AND Editor (bold).
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                          QStringLiteral("Ctrl+B")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("room.find"),
                                          QStringLiteral("Ctrl+Up")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("composer.bold"),
                                          QStringLiteral("Ctrl+Down")).isEmpty());
        // Shift+PgUp (first unread) is reserved.
        QVERIFY(registry.isReserved(QStringLiteral("Shift+PgUp")));
        QVERIFY(!registry.isReserved(QStringLiteral("PgUp")));
    }

    void timelineActionForKeyAnswersTimelineBindingsAndFollowsARebind()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.timelineActionForKey(Qt::Key_PageUp, Qt::NoModifier),
                 QStringLiteral("timeline.pageUp"));
        QCOMPARE(registry.timelineActionForKey(Qt::Key_Down, Qt::ControlModifier),
                 QStringLiteral("timeline.jumpToEnd"));
        // Plain arrows, Shift+PgUp, Global and Editor keys are not Timeline.
        QVERIFY(registry.timelineActionForKey(Qt::Key_Up, Qt::NoModifier)
                    .isEmpty());
        QVERIFY(registry.timelineActionForKey(Qt::Key_PageUp, Qt::ShiftModifier)
                    .isEmpty());
        QVERIFY(registry.timelineActionForKey(Qt::Key_B, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.timelineActionForKey(Qt::Key_G, Qt::ControlModifier)
                    .isEmpty());
        // And the Editor lookup does not claim a Timeline key on
        // ShortcutOverride.
        QVERIFY(registry.editorActionForKey(Qt::Key_Up, Qt::ControlModifier)
                    .isEmpty());

        QVERIFY(registry.setBinding(QStringLiteral("timeline.pageUp"),
                                    QStringLiteral("F6")).isEmpty());
        QCOMPARE(registry.timelineActionForKey(Qt::Key_F6, Qt::NoModifier),
                 QStringLiteral("timeline.pageUp"));
        QVERIFY(registry.timelineActionForKey(Qt::Key_PageUp, Qt::NoModifier)
                    .isEmpty());
    }

    // The exemption is for that one pair only: binding find-next onto the GIF
    // picker's key is still refused.
    void theGifFindShareIsNotAGeneralLoophole()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QVERIFY(!registry.validationError(QStringLiteral("room.findNext"),
                                          QStringLiteral("Ctrl+Shift+G"))
                     .isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("room.findPrevious"),
                                          QStringLiteral("Ctrl+Shift+O"))
                     .isEmpty());
    }

    // Rule (a): a Global action needs a real modifier. Qt dispatches
    // QEvent::Shortcut before the focused item sees the key, so a global
    // Shortcut on a bare letter takes it from every text field, including the
    // capture field.
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
        // A refusal is a no-op, never a half-applied rebind.
        QCOMPARE(registry.sequenceFor(QStringLiteral("nav.quickSwitcher")),
                 QStringLiteral("Ctrl+K"));
        QVERIFY(!registry.anyCustomised());
    }

    // Shift is not a qualifying modifier (Shift+B types a capital B).
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

    // Rule (b): a conflicting binding is refused, not stored with a warning:
    // two enabled Shortcuts on one sequence make Qt fire neither.
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
        // Ctrl+C, not Escape: validationError refuses modifier-less sequences
        // before consulting the reserved table, so only a reserved sequence
        // with a modifier reaches this branch.
        const QString error = registry.setBinding(QStringLiteral("app.quit"),
                                                  QStringLiteral("Ctrl+C"));
        QVERIFY(!error.isEmpty());
        QVERIFY(error.contains(QStringLiteral("Ctrl+C")));
        QVERIFY(error.contains(QStringLiteral("Copy")));
        QCOMPARE(registry.sequenceFor(QStringLiteral("app.quit")),
                 QStringLiteral("Ctrl+Q"));
    }

    // Global vs Editor on one sequence is a shadow, not a conflict: this is
    // what lets Ctrl+B be Bold in the message box and the room-list toggle
    // elsewhere.
    void aGlobalAndAnEditorActionMayShareASequenceAndSaySo()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        // Shipped that way.
        const auto resolved = resolvedById(registry);
        QCOMPARE(resolved.value(QStringLiteral("composer.bold")),
                 QStringLiteral("Ctrl+B"));
        QCOMPARE(resolved.value(QStringLiteral("shell.toggleRoomList")),
                 QStringLiteral("Ctrl+B"));

        // Not a conflict...
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("composer.bold"),
                        ShortcutRegistry::ConflictsWithRole)
                    .isEmpty());
        // ...but both rows describe the shadow, in both directions.
        QVERIFY(!roleFor(registry, QStringLiteral("composer.bold"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
        QVERIFY(!roleFor(registry, QStringLiteral("shell.toggleRoomList"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
    }

    // Storage.

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

    // Binding back to the default clears the override, so the account keeps
    // following later default changes and Reset is meaningful.
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

    // An unparseable stored value degrades to the default rather than reaching
    // QML as an inert `Shortcut`.
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

    // An unsafe action id (e.g. containing a slash, like
    // "../session/accessToken") reads and writes nothing rather than
    // addressing another settings group.
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

    // Every change bumps a revision QML can bind to: `Shortcut.sequences` binds
    // through sequenceFor(), a function call with no tracked dependency, so
    // dataChanged alone would not re-evaluate it.
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

    void resetRefusesADefaultThatWouldDuplicateAnotherBinding()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString back = QStringLiteral("nav.historyBack");
        const QString fwd = QStringLiteral("nav.historyForward");
        const QString def = registry.sequenceFor(back);
        QCOMPARE(registry.setBinding(back, QStringLiteral("Ctrl+Alt+B")), QString());
        QCOMPARE(registry.setBinding(fwd, def), QString());
        QVERIFY(!registry.resetToDefault(back).isEmpty());
        QCOMPARE(registry.sequenceFor(back), QStringLiteral("Ctrl+Alt+B"));
        QCOMPARE(registry.conflictCount(), 0);
    }

    // The capture control's input path.
    //
    // A bare modifier press is not a captured sequence: the user is still
    // reaching for the letter.
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

    // KeypadModifier is stripped from a captured sequence, or it would only
    // ever match the numeric keypad.
    void theKeypadModifierIsStrippedFromACapturedSequence()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFromKeyEvent(
                     Qt::Key_0, Qt::ControlModifier | Qt::KeypadModifier),
                 QStringLiteral("Ctrl+0"));
    }

    // Presentation.

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

    // Role names are prefixed: the delegate assigns them onto ShortcutRow's
    // like-named properties, and QML forbids redeclaring an existing property.
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

    // The editor-context lookup both composers share, driven by the registry's
    // EditorContext flag rather than per-composer lists (a composer without a
    // list let Ctrl+B fall through to the window).
    void editorActionForKeyAnswersEveryEditorBindingAndNothingElse()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QCOMPARE(registry.editorActionForKey(Qt::Key_B, Qt::ControlModifier),
                 QStringLiteral("composer.bold"));
        QCOMPARE(registry.editorActionForKey(Qt::Key_I, Qt::ControlModifier),
                 QStringLiteral("composer.italic"));

        // Every EditorContext row is reachable, so a new one cannot go
        // unhandled.
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

        // A Global binding does not resolve here, or the composer would claim
        // the ShortcutOverride for Ctrl+K and swallow the quick switcher.
        QVERIFY(registry.editorActionForKey(Qt::Key_K, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_Q, Qt::ControlModifier)
                    .isEmpty());
        // A bare letter is typing, never a format.
        QVERIFY(registry.editorActionForKey(Qt::Key_B, Qt::NoModifier)
                    .isEmpty());
    }

    // The lookup follows a rebind.
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
