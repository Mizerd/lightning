
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"

#include <QKeyEvent>
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

bool paletteOnly(const QString &id)
{
    static const QStringList ids = {
        QStringLiteral("nav.threadsActivity"),
        QStringLiteral("view.toggleLayoutPreset"),
        QStringLiteral("thread.togglePlacement"),
        QStringLiteral("thread.openRootInRoom"),
        QStringLiteral("composer.toggleHeight"),
    };
    return ids.contains(id);
}
}

class ShortcutRegistryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void streamInfoCanBeRebound()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString id = QStringLiteral("call.toggleStreamInfo");
        QCOMPARE(registry.defaultSequenceFor(id), QStringLiteral("Ctrl+F9"));
        QVERIFY(!roleFor(registry, id, ShortcutRegistry::DescriptionRole).isEmpty());
        QCOMPARE(registry.validationError(id, QStringLiteral("Ctrl+F9")), QString());
        QCOMPARE(registry.setBinding(id, QStringLiteral("Ctrl+Shift+F9")), QString());
        QCOMPARE(registry.sequenceFor(id), QStringLiteral("Ctrl+Shift+F9"));
        QVERIFY(!registry.validationError(id, QStringLiteral("Ctrl+Shift+I")).isEmpty());
        registry.resetToDefault(id);
    }

    void newNavigationActionsRefuseConflictsAndReservedKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QStringList ids = {
            QStringLiteral("nav.threadsActivity"),
            QStringLiteral("view.toggleLayoutPreset"),
            QStringLiteral("thread.togglePlacement"),
            QStringLiteral("thread.openRootInRoom"),
            QStringLiteral("composer.toggleHeight")
        };
        for (const QString &id : ids) {
            QVERIFY2(roleFor(registry, id, ShortcutRegistry::DescriptionRole)
                         != QStringLiteral("<no such action>"), qPrintable(id));
            QVERIFY(!registry.validationError(id, QStringLiteral("Esc")).isEmpty());
        }
        QCOMPARE(registry.setBinding(QStringLiteral("nav.threadsActivity"),
                                     QStringLiteral("Ctrl+Shift+F8")), QString());
        QVERIFY(!registry.validationError(QStringLiteral("view.toggleLayoutPreset"),
                                           QStringLiteral("Ctrl+Shift+F8")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("composer.toggleHeight"),
                                           QStringLiteral("Ctrl+Shift+D")).isEmpty());
        QCOMPARE(registry.sequenceFor(QStringLiteral("view.toggleLayoutPreset")),
                 QString());
    }

    void dialogBindingsAreScopedAndHaveNoFalseCopyConflict()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.dialogActionForKey(Qt::Key_K, Qt::ControlModifier),
                 QStringLiteral("dialog.previousItem"));
        QCOMPARE(registry.dialogActionForKey(Qt::Key_J, Qt::ControlModifier),
                 QStringLiteral("dialog.nextItem"));
        QCOMPARE(registry.dialogActionForKey(Qt::Key_C, Qt::ControlModifier),
                 QStringLiteral("dialog.close"));
        QVERIFY(registry.dialogActionForKey(Qt::Key_C, Qt::ControlModifier, true).isEmpty());
        QVERIFY(roleFor(registry, QStringLiteral("dialog.close"),
                        ShortcutRegistry::ConflictsWithRole).isEmpty());
        QCOMPARE(registry.conflictCount(), 0);
        QCOMPARE(registry.validationError(QStringLiteral("dialog.close"),
                                          QStringLiteral("Ctrl+C")), QString());
        QVERIFY(!registry.validationError(QStringLiteral("app.quit"),
                                           QStringLiteral("Ctrl+C")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("dialog.nextItem"),
                                           QStringLiteral("Ctrl+C")).isEmpty());
        QCOMPARE(registry.setBinding(QStringLiteral("dialog.nextItem"),
                                     QStringLiteral("Ctrl+U")), QString());
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(!registry.validationError(QStringLiteral("dialog.previousItem"),
                                           QStringLiteral("Ctrl+U")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("dialog.nextItem"),
                                           QStringLiteral("J")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("dialog.nextItem"),
                                           QStringLiteral("Ctrl+X, Ctrl+Y")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("dialog.close"),
                                           QStringLiteral("Esc")).isEmpty());
    }

    void dialogRebindingsPersistAndLeaveOtherContextsAlone()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.setBinding(QStringLiteral("dialog.nextItem"),
                                     QStringLiteral("Ctrl+N")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("dialog.close"),
                                     QStringLiteral("Alt+L")), QString());
        ShortcutRegistry restored(&settings);
        QVERIFY(restored.dialogActionForKey(Qt::Key_J, Qt::ControlModifier).isEmpty());
        QCOMPARE(restored.dialogActionForKey(Qt::Key_N, Qt::ControlModifier),
                 QStringLiteral("dialog.nextItem"));
        QCOMPARE(restored.dialogActionForKey(Qt::Key_L, Qt::AltModifier, false),
                 QStringLiteral("dialog.close"));
        QVERIFY(restored.editorActionForKey(Qt::Key_N, Qt::ControlModifier).isEmpty());
        QCOMPARE(restored.messageSelectionActionForKey(Qt::Key_K, Qt::NoModifier),
                 QStringLiteral("timeline.olderMessage"));
        QCOMPARE(restored.sequenceFor(QStringLiteral("nav.quickSwitcher")),
                 QStringLiteral("Ctrl+K"));
    }

    void messageSelectionDefaultsAndLookupAreSeparateFromComposerPaging()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFor(QStringLiteral("timeline.selectLatestMessage")),
                 QStringLiteral("Ctrl+Shift+Space"));
        QCOMPARE(registry.sequenceFor(QStringLiteral("timeline.resumeMessageSelection")),
                 QStringLiteral("Ctrl+Shift+W"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_K, Qt::NoModifier),
                 QStringLiteral("timeline.olderMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_J, Qt::NoModifier),
                 QStringLiteral("timeline.newerMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_U, Qt::NoModifier),
                 QStringLiteral("timeline.halfPageUp"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_D, Qt::NoModifier),
                 QStringLiteral("timeline.halfPageDown"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_R, Qt::NoModifier),
                 QStringLiteral("timeline.reply"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_T, Qt::NoModifier),
                 QStringLiteral("timeline.replyInThread"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_E, Qt::NoModifier),
                 QStringLiteral("timeline.editMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_A, Qt::NoModifier),
                 QStringLiteral("timeline.reactToMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_P, Qt::NoModifier),
                 QStringLiteral("timeline.pinMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_F, Qt::NoModifier),
                 QStringLiteral("timeline.forwardMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_Delete, Qt::NoModifier),
                 QStringLiteral("timeline.deleteMessage"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_C, Qt::NoModifier),
                 QStringLiteral("timeline.copyMessageLink"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_C, Qt::ControlModifier),
                 QStringLiteral("timeline.copyMessageText"));
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_V, Qt::NoModifier),
                 QStringLiteral("timeline.viewMessageSource"));
        QVERIFY(registry.timelineActionForKey(Qt::Key_K, Qt::NoModifier).isEmpty());
        QCOMPARE(registry.timelineActionForKey(Qt::Key_PageUp, Qt::NoModifier),
                 QStringLiteral("timeline.pageUp"));
        QVERIFY(registry.messageSelectionActionForKey(Qt::Key_PageUp, Qt::NoModifier).isEmpty());
        QCOMPARE(registry.conflictCount(), 0);
    }

    void noSelectionDefaultTakesAKeyAnotherContextHolds()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QHash<QString, QStringList> bySequence;
        QStringList selection;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString seq =
                registry.data(idx, ShortcutRegistry::CurrentSequenceRole).toString();
            const QString id = registry.data(idx, ShortcutRegistry::IdRole).toString();
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                == ShortcutRegistry::DialogContext)
                continue;
            bySequence[seq].append(id);
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                == ShortcutRegistry::MessageSelectionContext)
                selection.append(id);
        }
        QCOMPARE(selection.size(), 15);
        for (const QString &id : selection) {
            const QString seq = registry.sequenceFor(id);
            QCOMPARE(bySequence.value(seq).size(), 1);
            if (id != QStringLiteral("timeline.copyMessageText"))
                QVERIFY2(!seq.contains(QLatin1Char('+')), qPrintable(id + QStringLiteral(" ") + seq));
            const QString shadow = roleFor(registry, id, ShortcutRegistry::ShadowNoteRole);
            if (id == QStringLiteral("timeline.copyMessageText"))
                QVERIFY(!shadow.isEmpty());
            else
                QVERIFY(shadow.isEmpty());
        }
        const QString resume =
            registry.sequenceFor(QStringLiteral("timeline.resumeMessageSelection"));
        QVERIFY(!resume.contains(QStringLiteral("Alt")));
        QCOMPARE(bySequence.value(resume).size(), 1);
        QVERIFY(registry.sequenceFor(QStringLiteral("timeline.olderMessage"))
                != QStringLiteral("Ctrl+K"));
        QVERIFY(registry.sequenceFor(QStringLiteral("timeline.halfPageUp"))
                != QStringLiteral("Ctrl+U"));
    }

    void plainMessageKeysAreSafeOnlyInTheSelectionContext()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("H")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.newerMessage"),
                                     QStringLiteral("Shift+J")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.reply"),
                                     QStringLiteral("Y")), QString());
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_Y, Qt::NoModifier),
                 QStringLiteral("timeline.reply"));
        QVERIFY(registry.timelineActionForKey(Qt::Key_Y, Qt::NoModifier).isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_Y, Qt::NoModifier).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                           QStringLiteral("K")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("composer.bold"),
                                           QStringLiteral("K")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.reply"),
                                           QStringLiteral("Esc")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.reply"),
                                           QStringLiteral("Space")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.reply"),
                                           QStringLiteral("Ctrl+X, Ctrl+Y")).isEmpty());
        registry.reload();
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_H, Qt::NoModifier),
                 QStringLiteral("timeline.olderMessage"));
    }

    void selectionRefusesFocusAndNavigationKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        for (const char *key : { "Tab", "Backtab", "Shift+Tab", "F6", "Shift+F6",
                                 "Return", "Enter", "Up", "Down", "Left", "Right",
                                 "Shift+Up", "Backspace", "Esc", "Space",
                                 "Shift+Space" }) {
            const QString seq = QString::fromLatin1(key);
            const QString error = registry.validationError(
                QStringLiteral("timeline.olderMessage"), seq);
            QVERIFY2(!error.isEmpty(), key);
            QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"), seq),
                     error);
            QCOMPARE(registry.sequenceFor(QStringLiteral("timeline.olderMessage")),
                     QStringLiteral("K"));
        }
        QVERIFY(registry.validationError(QStringLiteral("timeline.olderMessage"),
                                         QStringLiteral("Ctrl+Tab")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("room.find"),
                                          QStringLiteral("Tab")).isEmpty());
    }

    void selectionOnMenuLetterIsNotReportedAsAConflict()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.replyInThread"),
                                     QStringLiteral("Y")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.editMessage"),
                                     QStringLiteral("G")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("T")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.newerMessage"),
                                     QStringLiteral("E")), QString());
        QCOMPARE(registry.sequenceFor(QStringLiteral("timeline.reply")),
                 QStringLiteral("R"));
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("timeline.reply"),
                        ShortcutRegistry::ConflictsWithRole).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("room.find"),
                                          QStringLiteral("R")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                          QStringLiteral("R")).isEmpty());
    }

    void selectionKeysConflictWithSharedTimelineKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QVERIFY(!registry.validationError(QStringLiteral("timeline.olderMessage"),
                                           QStringLiteral("PgUp")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                           QStringLiteral("K")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                           QStringLiteral("Ctrl+Up")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("timeline.newerMessage"),
                                           QStringLiteral("Ctrl+Up")).isEmpty());
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("Ctrl+K")), QString());
        QVERIFY(roleFor(registry, QStringLiteral("nav.quickSwitcher"),
                        ShortcutRegistry::ShadowNoteRole).contains(QStringLiteral("timeline")));
        QCOMPARE(registry.conflictCount(), 0);
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("Ctrl+N")), QString());
        QVERIFY(registry.messageSelectionActionForKey(Qt::Key_K, Qt::ControlModifier).isEmpty());
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_N, Qt::ControlModifier),
                 QStringLiteral("timeline.olderMessage"));
    }

    void settingsOverridesAllowPageTabsAndKeepDialogKeysSeparate()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QVERIFY(registry.sequenceFor(QStringLiteral("settings.nextPage")).isEmpty());
        QCOMPARE(registry.settingsActionForKey(Qt::Key_J, Qt::ControlModifier),
                 QStringLiteral("dialog.nextItem"));
        QCOMPARE(registry.setBinding(QStringLiteral("settings.nextPage"), QStringLiteral("Tab")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("settings.previousPage"), QStringLiteral("Shift+Tab")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("settings.nextSubsection"), QStringLiteral("Ctrl+J")), QString());
        QCOMPARE(registry.setBinding(QStringLiteral("settings.previousSubsection"), QStringLiteral("Ctrl+K")), QString());
        QCOMPARE(registry.settingsActionForKey(Qt::Key_Tab, Qt::NoModifier),
                 QStringLiteral("settings.nextPage"));
        QCOMPARE(registry.settingsActionForKey(Qt::Key_Backtab, Qt::ShiftModifier),
                 QStringLiteral("settings.previousPage"));
        QCOMPARE(registry.settingsActionForKey(Qt::Key_J, Qt::ControlModifier),
                 QStringLiteral("settings.nextSubsection"));
        QCOMPARE(registry.dialogActionForKey(Qt::Key_J, Qt::ControlModifier),
                 QStringLiteral("dialog.nextItem"));
        QVERIFY(!registry.validationError(QStringLiteral("settings.nextSubsection"), QStringLiteral("Tab")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("settings.nextPage"), QStringLiteral("J")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("settings.nextPage"), QStringLiteral("Ctrl+J")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("dialog.close"), QStringLiteral("Ctrl+J")).isEmpty());
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("settings.nextPage"),
                        ShortcutRegistry::ShadowNoteRole).contains(QStringLiteral("Tab")));
        ShortcutRegistry restored(&settings);
        QCOMPARE(restored.sequenceFor(QStringLiteral("settings.nextPage")), QStringLiteral("Tab"));
        QCOMPARE(restored.resetToDefault(QStringLiteral("settings.nextPage")), QString());
        QVERIFY(restored.sequenceFor(QStringLiteral("settings.nextPage")).isEmpty());
        QCOMPARE(ShortcutRegistry::normalize(QStringLiteral("Shift+Backtab")), QStringLiteral("Shift+Tab"));
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
            if (seq.isEmpty()) {
                if (!paletteOnly(id))
                    QCOMPARE(ctx, QString::number(int(ShortcutRegistry::SettingsContext)));
                continue;
            }
            auto &seen = seenPerContext[ctx];
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

    void everySeededDefaultAvoidsTheHardCodedKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString seq =
                registry.data(idx, ShortcutRegistry::DefaultSequenceRole)
                    .toString();
            const QString id = registry.data(idx, ShortcutRegistry::IdRole).toString();
            if (id == QStringLiteral("dialog.close"))
                continue;
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                    == ShortcutRegistry::MessageSelectionContext
                && (seq == QStringLiteral("R") || seq == QStringLiteral("T")
                    || seq == QStringLiteral("E")
                    || seq == QStringLiteral("Ctrl+C")))
                continue;
            QVERIFY2(!registry.isReserved(seq),
                     qPrintable(QStringLiteral("default %1 is reserved for %2")
                                    .arg(seq, registry.reservedOwner(seq))));
        }
        QVERIFY(registry.isReserved(QStringLiteral("Esc")));
        QVERIFY(registry.isReserved(QStringLiteral("Escape")));
        QVERIFY(registry.isReserved(QStringLiteral("Alt+V")));
        QVERIFY(registry.isReserved(QStringLiteral("Ctrl+C")));
        QVERIFY(registry.isReserved(QStringLiteral("Space")));
    }

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
            if (seq.isEmpty()
                && (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                        == ShortcutRegistry::SettingsContext
                    || paletteOnly(id)))
                continue;
            QVERIFY2(!seq.isEmpty(),
                     qPrintable(QStringLiteral("%1 has an unparseable default")
                                    .arg(id)));
            QCOMPARE(ShortcutRegistry::normalize(seq), seq);
            QCOMPARE(registry.data(idx, ShortcutRegistry::CurrentSequenceRole)
                         .toString(),
                     seq);
        }
    }

    void everySeededDefaultCarriesARealModifier()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        int checkedRows = 0;
        int selectionRows = 0;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            const QString seq =
                registry.data(idx, ShortcutRegistry::DefaultSequenceRole)
                    .toString();
            if (seq.isEmpty()
                && (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                        == ShortcutRegistry::SettingsContext
                    || paletteOnly(id)))
                continue;
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                    == ShortcutRegistry::MessageSelectionContext) {
                ++checkedRows;
                ++selectionRows;
                QVERIFY2(registry.validationError(id, seq).isEmpty(),
                         qPrintable(id + QStringLiteral(" ships ") + seq));
                continue;
            }
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
        QCOMPARE(selectionRows, 15);
    }

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

        QVERIFY(!resolved.contains(QStringLiteral("app.settingsSearch")));
    }

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
        int left = 0, right = 0;
        for (auto it = resolved.cbegin(); it != resolved.cend(); ++it) {
            left += it.value() == QStringLiteral("Alt+Left");
            right += it.value() == QStringLiteral("Alt+Right");
        }
        QCOMPARE(left, 1);
        QCOMPARE(right, 1);
    }

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

        QCOMPARE(resolved.value(QStringLiteral("composer.bold")),
                 QStringLiteral("Ctrl+B"));
        QCOMPARE(resolved.value(QStringLiteral("shell.toggleRoomList")),
                 QStringLiteral("Ctrl+B"));
    }

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
        const int shiftCtrl = int(Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(registry.editorActionForKey(Qt::Key_E, shiftCtrl).isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_G, shiftCtrl).isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_O, shiftCtrl).isEmpty());
        QCOMPARE(registry.editorActionForKey(Qt::Key_E, Qt::ControlModifier),
                 QStringLiteral("composer.code"));
    }

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
        QCOMPARE(registry.editorActionForKey(Qt::Key_U, Qt::ControlModifier),
                 QStringLiteral("composer.underline"));
    }

    void ctrlBReachesThePanelToggleFromAnEmptyComposerSelection()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const int ctrl = int(Qt::ControlModifier);

        QVERIFY(registry.editorOverrideFor(Qt::Key_B, ctrl, false).isEmpty());
        QCOMPARE(registry.editorOverrideFor(Qt::Key_B, ctrl, true),
                 QStringLiteral("composer.bold"));
        QCOMPARE(registry.editorOverrideFor(Qt::Key_E, ctrl, false),
                 QStringLiteral("composer.code"));
        QVERIFY(registry.editorOverrideFor(Qt::Key_K, ctrl, true).isEmpty());
    }

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

    void theOnlyCrossContextSharesAreTheDesignedOnes()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QHash<QString, QStringList> idsBySequence;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            if (registry.data(idx, ShortcutRegistry::CurrentSequenceRole).toString().isEmpty())
                continue;
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
                               QStringLiteral("Ctrl+C"),
                               QStringLiteral("Ctrl+K"),
                               QStringLiteral("Ctrl+Shift+G"),
                               QStringLiteral("Ctrl+U") }));
        QCOMPARE(idsBySequence.value(QStringLiteral("Ctrl+U")).size(), 2);
        QCOMPARE(idsBySequence.value(QStringLiteral("Ctrl+C")).size(), 2);
        QCOMPARE(idsBySequence.value(QStringLiteral("Ctrl+K")).size(), 2);
    }

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
        QCOMPARE(resolved.value(QStringLiteral("composer.gifPicker")),
                 QStringLiteral("Ctrl+Shift+G"));
        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("room.findPrevious"),
                        ShortcutRegistry::ConflictsWithRole).isEmpty());
        for (auto it = resolved.cbegin(); it != resolved.cend(); ++it) {
            QVERIFY2(it.value() != QStringLiteral("Up")
                         && it.value() != QStringLiteral("Down"),
                     qPrintable(it.key()));
        }
    }

    void aTimelineActionAcceptsBarePageKeysAndFKeysOnly()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString id = QStringLiteral("timeline.pageUp");
        QVERIFY(registry.validationError(id, QStringLiteral("PgDown")).isEmpty()
                == false);
        QVERIFY(registry.validationError(id, QStringLiteral("F5")).isEmpty());
        QVERIFY(registry.validationError(id, QStringLiteral("Ctrl+Left"))
                    .isEmpty());
        QVERIFY(!registry.validationError(id, QStringLiteral("X")).isEmpty());
        QVERIFY(!registry.validationError(id, QStringLiteral("Up")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("room.find"),
                                          QStringLiteral("PgUp")).isEmpty());
    }

    void aTimelineSequenceConflictsAcrossContexts()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QVERIFY(!registry.validationError(QStringLiteral("timeline.pageUp"),
                                          QStringLiteral("Ctrl+B")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("room.find"),
                                          QStringLiteral("Ctrl+Up")).isEmpty());
        QVERIFY(!registry.validationError(QStringLiteral("composer.bold"),
                                          QStringLiteral("Ctrl+Down")).isEmpty());
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
        QVERIFY(registry.timelineActionForKey(Qt::Key_Up, Qt::NoModifier)
                    .isEmpty());
        QVERIFY(registry.timelineActionForKey(Qt::Key_PageUp, Qt::ShiftModifier)
                    .isEmpty());
        QVERIFY(registry.timelineActionForKey(Qt::Key_B, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.timelineActionForKey(Qt::Key_G, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_Up, Qt::ControlModifier)
                    .isEmpty());

        QVERIFY(registry.setBinding(QStringLiteral("timeline.pageUp"),
                                    QStringLiteral("F6")).isEmpty());
        QCOMPARE(registry.timelineActionForKey(Qt::Key_F6, Qt::NoModifier),
                 QStringLiteral("timeline.pageUp"));
        QVERIFY(registry.timelineActionForKey(Qt::Key_PageUp, Qt::NoModifier)
                    .isEmpty());
    }

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

    void aGlobalActionAcceptsASingleKeyOnItsOwn()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString mute = QStringLiteral("call.toggleMute");
        for (const QString &key : { QStringLiteral("B"), QStringLiteral("."),
                                    QStringLiteral("7"), QStringLiteral("F5"),
                                    QStringLiteral("Num+."), QStringLiteral("Num+5"),
                                    QStringLiteral("Pause") }) {
            QCOMPARE(registry.setBinding(mute, key), QString());
            QCOMPARE(registry.sequenceFor(mute), key);
        }
        QCOMPARE(registry.conflictCount(), 0);
    }

    void aGlobalSingleKeyRefusesShiftAloneAndNavigationKeys()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString mute = QStringLiteral("call.toggleMute");
        const QString shiftError = registry.validationError(mute, QStringLiteral("Shift+B"));
        QVERIFY2(shiftError.contains(QStringLiteral("Shift alone changes the character")),
                 qPrintable(shiftError));
        const QString chordError = registry.validationError(mute, QStringLiteral("A, B"));
        QVERIFY2(chordError.contains(QStringLiteral("Shift alone changes the character")),
                 qPrintable(chordError));
        for (const char *key : { "Space", "Tab", "Return", "Enter",
                                 "Num+Enter", "Up", "Num+Left", "Home", "End",
                                 "PgUp", "PgDown", "Backspace", "Del", "Num+Del",
                                 "Ins", "Esc", "F6", "Menu" }) {
            const QString error = registry.setBinding(mute, QString::fromLatin1(key));
            QVERIFY2(error.contains(QStringLiteral("moves focus, navigates or edits text")),
                     qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(key), error)));
        }
        QCOMPARE(registry.sequenceFor(mute), QStringLiteral("Ctrl+Shift+U"));
        QVERIFY(!registry.anyCustomised());
    }

    void aGlobalSingleKeyStillMeetsTheReservedTableAndConflicts()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString mute = QStringLiteral("call.toggleMute");
        const QString deafen = QStringLiteral("call.toggleDeafen");

        const QString reserved = registry.setBinding(mute, QStringLiteral("R"));
        QVERIFY2(reserved.contains(QStringLiteral("Reply accelerator"))
                     && reserved.contains(QStringLiteral("Two shortcuts on one key")),
                 qPrintable(reserved));

        QCOMPARE(registry.setBinding(mute, QStringLiteral("M")), QString());
        const QString clash = registry.setBinding(deafen, QStringLiteral("M"));
        QVERIFY2(clash.contains(registry.descriptionFor(mute)), qPrintable(clash));
        QCOMPARE(registry.sequenceFor(deafen), QStringLiteral("Ctrl+Shift+H"));

        QCOMPARE(registry.setBinding(QStringLiteral("timeline.pageUp"),
                                     QStringLiteral("F5")), QString());
        const QString timelineClash = registry.setBinding(deafen, QStringLiteral("F5"));
        QVERIFY2(timelineClash.contains(registry.descriptionFor(
                     QStringLiteral("timeline.pageUp"))),
                 qPrintable(timelineClash));
        QCOMPARE(registry.conflictCount(), 0);
    }

    void aNumpadKeyIsDistinctFromItsMainKeyboardTwin()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const QString mute = QStringLiteral("call.toggleMute");
        const QString deafen = QStringLiteral("call.toggleDeafen");
        QCOMPARE(registry.setBinding(mute, QStringLiteral("Num+.")), QString());
        QCOMPARE(registry.setBinding(deafen, QStringLiteral(".")), QString());
        QCOMPARE(registry.sequenceFor(mute), QStringLiteral("Num+."));
        QCOMPARE(registry.sequenceFor(deafen), QStringLiteral("."));
        QCOMPARE(registry.conflictCount(), 0);
        const QString clash = registry.setBinding(QStringLiteral("call.leave"),
                                                  QStringLiteral("Num+."));
        QVERIFY2(clash.startsWith(QStringLiteral("Num . is already used for"))
                     && clash.contains(registry.descriptionFor(mute)),
                 qPrintable(clash));
    }

    void aLoneCharacterKeyYieldsToTypingAndOtherKeysDoNot()
    {
        for (const char *key : { "M", ".", "7", "Num+.", "Num+5", "/" })
            QVERIFY2(ShortcutRegistry::yieldsToTyping(QString::fromLatin1(key)), key);
        for (const char *key : { "F5", "Pause", "Ctrl+M", "Shift+M", "Space", "" })
            QVERIFY2(!ShortcutRegistry::yieldsToTyping(QString::fromLatin1(key)), key);
    }

    void aNumpadSequenceIsShownWithASpace()
    {
        QCOMPARE(ShortcutRegistry::displayText(QStringLiteral("Num+.")),
                 QStringLiteral("Num ."));
        QCOMPARE(ShortcutRegistry::displayText(QStringLiteral("Num++")),
                 QStringLiteral("Num +"));
        QCOMPARE(ShortcutRegistry::displayText(QStringLiteral("Ctrl+Shift+U")),
                 QStringLiteral("Ctrl+Shift+U"));
    }

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
        const QString error = registry.setBinding(QStringLiteral("app.quit"),
                                                  QStringLiteral("Ctrl+C"));
        QVERIFY(!error.isEmpty());
        QVERIFY(error.contains(QStringLiteral("Ctrl+C")));
        QVERIFY(error.contains(QStringLiteral("Copy")));
        QCOMPARE(registry.sequenceFor(QStringLiteral("app.quit")),
                 QStringLiteral("Ctrl+Q"));
    }

    void aGlobalAndAnEditorActionMayShareASequenceAndSaySo()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        const auto resolved = resolvedById(registry);
        QCOMPARE(resolved.value(QStringLiteral("composer.bold")),
                 QStringLiteral("Ctrl+B"));
        QCOMPARE(resolved.value(QStringLiteral("shell.toggleRoomList")),
                 QStringLiteral("Ctrl+B"));

        QCOMPARE(registry.conflictCount(), 0);
        QVERIFY(roleFor(registry, QStringLiteral("composer.bold"),
                        ShortcutRegistry::ConflictsWithRole)
                    .isEmpty());
        QVERIFY(!roleFor(registry, QStringLiteral("composer.bold"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
        QVERIFY(!roleFor(registry, QStringLiteral("shell.toggleRoomList"),
                         ShortcutRegistry::ShadowNoteRole)
                     .isEmpty());
    }

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

    void theKeypadModifierIsStrippedFromACapturedChord()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFromKeyEvent(
                     Qt::Key_0, Qt::ControlModifier | Qt::KeypadModifier),
                 QStringLiteral("Ctrl+0"));
    }

    void aLoneNumpadKeyIsCapturedAsANumpadKey()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.sequenceFromKeyEvent(Qt::Key_Period, Qt::KeypadModifier),
                 QStringLiteral("Num+."));
        QCOMPARE(registry.sequenceFromKeyEvent(Qt::Key_Period, Qt::NoModifier),
                 QStringLiteral("."));
    }

    void aNumpadPressStillReachesAMainKeyboardBinding()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_Delete, Qt::KeypadModifier),
                 QStringLiteral("timeline.deleteMessage"));
        QCOMPARE(registry.timelineActionForKey(Qt::Key_PageUp, Qt::KeypadModifier),
                 QStringLiteral("timeline.pageUp"));
        QCOMPARE(registry.setBinding(QStringLiteral("timeline.olderMessage"),
                                     QStringLiteral("Num+8")), QString());
        QCOMPARE(registry.messageSelectionActionForKey(Qt::Key_8, Qt::KeypadModifier),
                 QStringLiteral("timeline.olderMessage"));
        QVERIFY(registry.messageSelectionActionForKey(Qt::Key_8, Qt::NoModifier).isEmpty());
    }

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

    void shiftedSymbolsAreNotTranslatedToDigits()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        const auto ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;

        QVERIFY(registry.editorActionForKey(Qt::Key_ParenLeft, ctrlShift)
                != QStringLiteral("composer.quote"));
        QVERIFY(registry.editorActionForKey(Qt::Key_ParenLeft, ctrlShift)
                    .isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_Asterisk, ctrlShift)
                    .isEmpty());
        QCOMPARE(registry.editorActionForKey(Qt::Key_8, ctrlShift),
                 QStringLiteral("composer.list"));
    }

    void physicalDigitKeyMapsTheMainRowOnEveryPlatform()
    {
        using P = ShortcutRegistry::KeyboardPlatform;
        const quint32 macRow[10] = {0x1D, 0x12, 0x13, 0x14, 0x15,
                                    0x17, 0x16, 0x1A, 0x1C, 0x19};
        for (int d = 0; d <= 9; ++d) {
            const int want = Qt::Key_0 + d;
            const quint32 xkb = d == 0 ? 19 : 9 + d;
            const quint32 win = d == 0 ? 0x0B : 0x01 + d;
            QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Xkb, xkb, 0), want);
            QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Windows, win, 0), want);
            QCOMPARE(ShortcutRegistry::physicalDigitKey(P::MacOS, 0, macRow[d]), want);
            QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Unknown, xkb, macRow[d]), 0);
        }
        // Keypad 8, Minus and the extended (0x100) variant of a row code.
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Xkb, 80, 0), 0);
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Xkb, 20, 0), 0);
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Windows, 0x48, 0), 0);
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Windows, 0x109, 0), 0);
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::MacOS, 0x5B, 0), 0);
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::MacOS, 0x1C, 0x1C), Qt::Key_8);
        QCOMPARE(ShortcutRegistry::physicalDigitKey(P::Windows, 0, 0x38), 0);
    }

    void aShiftedDigitMatchesItsBindingByPhysicalKey()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QObject target;
        const auto ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
        using P = ShortcutRegistry::KeyboardPlatform;
        struct Press {
            P platform;
            int key;
            quint32 scan;
            quint32 vk;
            const char *action;
        };
        // US "*" and German "(" on the 8 key, US "(" on the 9 key, French
        // delivering the digit itself, on each platform's own key numbering.
        const Press presses[] = {
            {P::Xkb, Qt::Key_Asterisk, 17, 0x2a, "composer.list"},
            {P::Xkb, Qt::Key_ParenLeft, 17, 0x28, "composer.list"},
            {P::Xkb, Qt::Key_ParenLeft, 18, 0x28, "composer.quote"},
            {P::Xkb, Qt::Key_8, 17, 0x38, "composer.list"},
            {P::Windows, Qt::Key_ParenLeft, 0x09, 0x38, "composer.list"},
            {P::Windows, Qt::Key_ParenLeft, 0x0A, 0x39, "composer.quote"},
            {P::MacOS, Qt::Key_ParenLeft, 0, 0x1C, "composer.list"},
            {P::MacOS, Qt::Key_ParenRight, 0, 0x19, "composer.quote"},
        };
        for (const Press &p : presses) {
            registry.setKeyboardPlatformForTesting(p.platform);
            QKeyEvent override(QEvent::ShortcutOverride, p.key, ctrlShift,
                               p.scan, p.vk, 0);
            registry.eventFilter(&target, &override);
            QCOMPARE(registry.editorOverrideFor(p.key, ctrlShift, false),
                     QString::fromLatin1(p.action));
            QKeyEvent press(QEvent::KeyPress, p.key, ctrlShift, p.scan, p.vk, 0);
            registry.eventFilter(&target, &press);
            QCOMPARE(registry.editorActionForKey(p.key, ctrlShift),
                     QString::fromLatin1(p.action));
            // Another key or other modifiers than the recorded press do not
            // borrow its physical digit.
            QVERIFY(registry.editorActionForKey(Qt::Key_BraceLeft, ctrlShift).isEmpty());
            if (p.key != Qt::Key_8)
                QVERIFY(registry.editorActionForKey(p.key, Qt::ControlModifier)
                            .isEmpty());
            QKeyEvent release(QEvent::KeyRelease, p.key, ctrlShift, p.scan, p.vk, 0);
            registry.eventFilter(&target, &release);
            if (p.key != Qt::Key_8)
                QVERIFY(registry.editorActionForKey(p.key, ctrlShift).isEmpty());
        }
    }

    void aLiteralSymbolBindingWinsOverThePhysicalDigit()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QObject target;
        const auto ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
        QCOMPARE(registry.setBinding(QStringLiteral("composer.code"),
                                     QStringLiteral("Ctrl+Shift+(")),
                 QString());
        registry.setKeyboardPlatformForTesting(ShortcutRegistry::KeyboardPlatform::Xkb);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_ParenLeft, ctrlShift, 17, 0x28, 0);
        registry.eventFilter(&target, &press);
        QCOMPARE(registry.editorActionForKey(Qt::Key_ParenLeft, ctrlShift),
                 QStringLiteral("composer.code"));
    }

    void anUnknownPlatformNeverTranslatesAPress()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        QObject target;
        const auto ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
        registry.setKeyboardPlatformForTesting(ShortcutRegistry::KeyboardPlatform::Unknown);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Asterisk, ctrlShift, 17, 0x1C, 0);
        registry.eventFilter(&target, &press);
        QVERIFY(registry.editorActionForKey(Qt::Key_Asterisk, ctrlShift).isEmpty());
    }

    void editorActionForKeyAnswersEveryEditorBindingAndNothingElse()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QCOMPARE(registry.editorActionForKey(Qt::Key_B, Qt::ControlModifier),
                 QStringLiteral("composer.bold"));
        QCOMPARE(registry.editorActionForKey(Qt::Key_I, Qt::ControlModifier),
                 QStringLiteral("composer.italic"));

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
            if (seq.isEmpty() && paletteOnly(id))
                continue;
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

        QVERIFY(registry.editorActionForKey(Qt::Key_K, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_Q, Qt::ControlModifier)
                    .isEmpty());
        QVERIFY(registry.editorActionForKey(Qt::Key_B, Qt::NoModifier)
                    .isEmpty());
    }

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
