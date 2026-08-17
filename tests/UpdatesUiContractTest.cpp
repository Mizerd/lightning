// Source-contract proof for the Settings -> Updates UI (UPDATE-SPEC.md v1).
// Modeled on ContextMenuContractTest.cpp / QmlBindingContractTest.cpp's
// read()/bounded-block scanning style: this proves the exact SPEC-mandated
// wiring and, more importantly, the exact ABSENCES the spec's non-negotiable
// section (§0) requires — there is no "install anyway" affordance anywhere,
// a package-managed install never gets a self-download/install action, and a
// development/unknown install never gets an install action. It scans source
// text rather than instantiating the QML (this round has no build lock and
// does not link against the real UpdateManager C++ target); it pins exact
// wiring — including the real Q_ENUM comparisons against
// src/update/UpdateManager.h's `State` — so a later change cannot silently
// reintroduce a forbidden affordance or a stringly-typed state comparison
// that would silently never match.

#include <QtTest/QtTest>

#include <QFile>
#include <QStringList>

class UpdatesUiContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // Every forbidden "proceed past a failed verification" phrase, scanned
    // case-insensitively so a differently-capitalized reintroduction still
    // fails this test.
    static QStringList forbiddenBypassPhrases()
    {
        return {
            QStringLiteral("install anyway"),
            QStringLiteral("proceed anyway"),
            QStringLiteral("skip verification"),
            QStringLiteral("ignore verification"),
            QStringLiteral("continue anyway"),
            QStringLiteral("bypass verification"),
        };
    }

private Q_SLOTS:
    // ---- Files exist and are non-empty ----

    void filesExist()
    {
        QVERIFY(!read(QStringLiteral("UpdatesSettingsSection.qml")).isEmpty());
        QVERIFY(!read(QStringLiteral("UpdateAvailableDialog.qml")).isEmpty());
        QVERIFY(!read(QStringLiteral("SettingsScreen.qml")).isEmpty());
    }

    // A component that exists but is never created is dead code, and the rest
    // of this file cannot tell the difference: every other case here scans
    // UpdateAvailableDialog.qml's own text, which passes whether or not
    // anything instantiates it. An independent review caught exactly that --
    // the dialog was complete, self-opening, listed in the QML module, and
    // created by nobody, so the only way to hear about an update was to open
    // Settings and look. Pin the instantiation itself.
    void updateAvailableDialogIsActuallyInstantiated()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY2(!main.isEmpty(), "Main.qml must be readable");
        QVERIFY2(main.contains(QStringLiteral("UpdateAvailableDialog {")),
                 "Main.qml must create the update-available dialog; the dialog "
                 "opens itself off UpdateManager's state, so if nothing "
                 "instantiates it the update-available prompt never appears");
    }

    // Same failure shape one layer down: the Settings switch writes
    // update/automaticChecks, and for a while nothing in the application ever
    // read it back by calling maybeCheckAutomatically(). The preference was
    // stored, documented in the privacy policy, and inert. Pin the call site.
    // The handoff state is entered as soon as the helper process starts. The
    // helper then waits for Lightning to exit, so at that moment nothing has
    // been installed and the install can still fail. Claiming otherwise made
    // a failed update look like a success, because the real outcome only
    // becomes visible on the next start.
    void handoffStateNeverClaimsTheUpdateIsInstalled()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int idx = section.indexOf(QStringLiteral("updateRestartRequiredBlock"));
        QVERIFY(idx >= 0);
        // Wide enough to cover both labels and their explanatory comments.
        const QString block = section.mid(idx, 2000);
        QVERIFY2(!block.contains(QStringLiteral("is installed.")),
                 "the handoff state must not assert a completed installation");
        QVERIFY(block.contains(QStringLiteral("ready to install")));
        QVERIFY2(block.contains(QStringLiteral("handoffSummary")),
                 "the detail line must come from UpdateManager, which words it "
                 "differently for the quit-now and apply-on-next-quit paths");
    }

    // The helper runs after Lightning exits, so its result can only be shown
    // on the next start. Without this surface the typed failures it records
    // were written to disk and discarded.
    void previousUpdateOutcomeIsSurfacedAndDismissible()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int idx = section.indexOf(QStringLiteral("updateLastResultBlock"));
        QVERIFY2(idx >= 0, "the previous update's outcome must be shown");
        const QString block = section.mid(idx, 1600);
        QVERIFY(block.contains(QStringLiteral("lastUpdateResult")));
        QVERIFY(block.contains(QStringLiteral("UpdateManager.NoResult")));
        QVERIFY(block.contains(QStringLiteral("UpdateManager.InstallFailed")));
        QVERIFY(block.contains(QStringLiteral("lastUpdateError")));
        QVERIFY2(block.contains(QStringLiteral("clearLastUpdateResult()")),
                 "a stale success/failure must be dismissible for good");
    }

    void automaticCheckIsActuallyTriggered()
    {
        QFile file(QStringLiteral(QML_DIR "/../src/app/AppController.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), "AppController.cpp must be readable");
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY2(source.contains(QStringLiteral("maybeCheckAutomatically()")),
                 "AppController must trigger the automatic update check, or the "
                 "Settings preference and the privacy policy describe behaviour "
                 "that never happens");
    }

    // ---- Reachable from SettingsScreen navigation ----

    void updatesSectionIsReachableFromSettingsNavigation()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!settings.isEmpty());
        QVERIFY(settings.contains(QStringLiteral("sectionKey: \"updates\"")));
        QVERIFY(settings.contains(QStringLiteral("navLabel: qsTr(\"Updates\")")));
        // The header bar's icon/title mapping also knows the section (so
        // "Settings — Updates" renders, matching every other section).
        QVERIFY(settings.contains(QStringLiteral(
            "if (key === \"updates\") return qsTr(\"Updates\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "if (key === \"updates\") return \"download\"")));
        // The real content pane is instantiated (visibility-toggled, the
        // file's own established convention — never Loader, so in-flight
        // state in other panes is never destroyed by switching category).
        QVERIFY(settings.contains(QStringLiteral("UpdatesSettingsSection {")));
        QVERIFY(settings.contains(QStringLiteral(
            "visible: root.section === \"updates\"")));
        QVERIFY(!settings.contains(QStringLiteral(
            "Loader { source: \"UpdatesSettingsSection.qml\"")));
    }

    // ---- The reused icon codepoint is a real, already-verified glyph ----

    void updatesIconReusesAnExistingVerifiedCodepoint()
    {
        const QString icon = read(QStringLiteral("Icon.qml"));
        QVERIFY(!icon.isEmpty());
        // "download" must already be a mapped codepoint in the bundled
        // Material Symbols subset (Icon.qml is not owned by this round —
        // this proves the reused name pre-existed rather than assuming a
        // new, unverified glyph would render).
        QVERIFY(icon.contains(QStringLiteral("\"download\": \"\\uf090\"")));
    }

    // ---- No affordance to proceed after a verification failure ----

    void noBypassAffordanceAfterVerificationFailure()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(!dialog.isEmpty());
        const QString sectionLower = section.toLower();
        const QString dialogLower = dialog.toLower();
        for (const QString &phrase : forbiddenBypassPhrases()) {
            QVERIFY2(!sectionLower.contains(phrase),
                     qPrintable(QStringLiteral("UpdatesSettingsSection.qml contains: ")
                                + phrase));
            QVERIFY2(!dialogLower.contains(phrase),
                     qPrintable(QStringLiteral("UpdateAvailableDialog.qml contains: ")
                                + phrase));
        }
    }

    void failedStateOffersOnlyRetryAndDismiss()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int start = section.indexOf(QStringLiteral("updateFailedBlock"));
        QVERIFY(start >= 0);
        const int end = section.indexOf(QStringLiteral("updateStatusCard"), start);
        // updateStatusCard only appears once (the Pane's own objectName,
        // before the failed block) — fall back to end-of-file bounding if
        // that search fails, so the block is never silently empty.
        const QString block = end > start ? section.mid(start, end - start)
                                           : section.mid(start);
        QVERIFY(!block.isEmpty());
        // state is UpdateManager's real Q_ENUM (see UpdateManager.h) —
        // compared against the enum value, never a string literal.
        QVERIFY(block.contains(QStringLiteral("state === UpdateManager.Failed")));
        QVERIFY(block.contains(QStringLiteral("root.um.errorMessage")));
        QVERIFY(block.contains(QStringLiteral("updateRetryButton")));
        QVERIFY(block.contains(QStringLiteral("root.um.checkForUpdates()")));
        QVERIFY(block.contains(QStringLiteral("updateDismissFailureButton")));
        // Exactly two buttons in the failed block.
        QCOMPARE(block.count(QStringLiteral("AppButton {")), 2);
        // The dismiss button never calls into UpdateManager at all — it is
        // a purely local banner collapse, never a state override.
        const int dismissIdx = block.indexOf(QStringLiteral("updateDismissFailureButton"));
        QVERIFY(dismissIdx >= 0);
        const QString dismissTail = block.mid(dismissIdx, 200);
        QVERIFY(dismissTail.contains(QStringLiteral("root.failureDismissed = true")));
        QVERIFY(!dismissTail.contains(QStringLiteral("root.um.")));
    }

    // ---- Flatpak/Snap never expose an install/download action ----

    void packageManagedInstallsNeverExposeDownloadOrInstall()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(!dialog.isEmpty());

        for (const QString &source : { section, dialog }) {
            // The disclosure sentences exist verbatim.
            QVERIFY(source.contains(QStringLiteral(
                "Updates for this installation are managed by Flatpak.")));
            QVERIFY(source.contains(QStringLiteral(
                "Updates for this installation are managed by Snap.")));
            QVERIFY(source.contains(QStringLiteral("openManagedUpdateHelp()")));
        }

        // Settings pane: the self-update action row is gated OFF whenever
        // packageManaged is true — never merely hidden by a positive
        // condition that could independently be satisfied.
        const int actionsIdx = section.indexOf(QStringLiteral("updateActionsRow"));
        QVERIFY(actionsIdx >= 0);
        const QString actionsTail = section.mid(actionsIdx, 400);
        QVERIFY(actionsTail.contains(QStringLiteral(
            "visible: !root.packageManaged && root.canInstallAutomatically")));

        // Dialog: same gate on its "Update now" button.
        const int dialogNowIdx = dialog.indexOf(QStringLiteral("updateDialogUpdateNowButton"));
        QVERIFY(dialogNowIdx >= 0);
        const QString dialogNowBlock = dialog.mid(dialogNowIdx, 300);
        QVERIFY(dialogNowBlock.contains(QStringLiteral(
            "visible: !root.packageManaged && root.canInstallAutomatically")));

        // The managed block itself never contains a download/install call.
        const int managedIdx = section.indexOf(QStringLiteral("updateManagedBlock"));
        QVERIFY(managedIdx >= 0);
        const int managedEnd = section.indexOf(QStringLiteral("updateActionsRow"), managedIdx);
        QVERIFY(managedEnd > managedIdx);
        const QString managedBlock = section.mid(managedIdx, managedEnd - managedIdx);
        QVERIFY(!managedBlock.contains(QStringLiteral("downloadUpdate()")));
        QVERIFY(!managedBlock.contains(QStringLiteral("installUpdate()")));
        QVERIFY(!managedBlock.contains(QStringLiteral("installAndRestart()")));
    }

    // ---- Automatic-checks control exists and is honestly worded ----

    void automaticChecksControlExistsAndDisclosesNoAccountData()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral("updateAutomaticChecksSwitch")));
        QVERIFY(section.contains(QStringLiteral("AppSwitch {")));
        QVERIFY(section.contains(QStringLiteral(
            "checked: root.um ? root.um.automaticChecksEnabled : false")));
        QVERIFY(section.contains(QStringLiteral(
            "root.um.automaticChecksEnabled =")));

        const int captionIdx = section.indexOf(QStringLiteral("updateAutomaticChecksCaption"));
        QVERIFY(captionIdx >= 0);
        const QString captionBlock = section.mid(captionIdx, 900);
        // Must state the REAL default and offer the way out of it — the
        // copy and UpdateManager's default are changed together or the
        // application lies about its own privacy posture. It must still
        // never claim data is sent.
        QVERIFY(captionBlock.contains(QStringLiteral("On by default")));
        QVERIFY(captionBlock.contains(QStringLiteral("turn it off")));
        QVERIFY(captionBlock.contains(QStringLiteral("never includes any account")));
        QVERIFY(captionBlock.contains(QStringLiteral("device, or Matrix")));
        QVERIFY(!captionBlock.toLower().contains(QStringLiteral("room id")));
        QVERIFY(!captionBlock.toLower().contains(QStringLiteral("user id")));

        // "Check for updates" is disabled exactly while Checking/Downloading.
        const int checkIdx = section.indexOf(QStringLiteral("updateCheckNowButton"));
        QVERIFY(checkIdx >= 0);
        const QString checkBlock = section.mid(checkIdx, 450);
        QVERIFY(checkBlock.contains(QStringLiteral(
            "root.um.state !== UpdateManager.Checking")));
        QVERIFY(checkBlock.contains(QStringLiteral(
            "root.um.state !== UpdateManager.Downloading")));
        QVERIFY(checkBlock.contains(QStringLiteral("root.um.checkForUpdates()")));
    }

    // ---- lastCheckTime is handled as a QDateTime, not a string ----

    void lastCheckTimeIsHandledAsADateTimeNotAString()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int idx = section.indexOf(QStringLiteral("updateLastCheckedLabel"));
        QVERIFY(idx >= 0);
        const QString block = section.mid(idx, 900);
        // Validity is checked via getTime()/isNaN (the QDateTime-as-JS-Date
        // idiom already used for Sessions "last seen" in this same file),
        // never a .length check (which a marshaled Date object does not
        // have).
        QVERIFY(block.contains(QStringLiteral(
            "!isNaN(root.um.lastCheckTime.getTime())")));
        QVERIFY(!block.contains(QStringLiteral("lastCheckTime.length")));
        QVERIFY(block.contains(QStringLiteral("Qt.formatDateTime(")));
    }

    // ---- statusDetail (non-error diagnostic) is surfaced ----

    void statusDetailIsSurfaced()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral("updateStatusDetailLabel")));
        QVERIFY(section.contains(QStringLiteral("root.um.statusDetail")));
    }

    // ---- installRefused is surfaced, never silently discarded ----

    void installRefusedIsSurfacedNotDiscarded()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral("function onInstallRefused(reason)")));
        QVERIFY(section.contains(QStringLiteral("root.installRefusedReason = reason")));
        QVERIFY(section.contains(QStringLiteral("updateInstallRefusedBlock")));
    }

    // ---- managedUpdateHelpRequested is handled, not fired-and-forgotten ---

    void managedUpdateHelpRequestedIsHandledInBothSurfaces()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(!dialog.isEmpty());
        for (const QString &source : { section, dialog }) {
            QVERIFY(source.contains(QStringLiteral(
                "function onManagedUpdateHelpRequested(command, explanation)")));
            // The command/explanation come ONLY from the signal payload —
            // nothing here builds a "flatpak update ..." string itself.
            QVERIFY(source.contains(QStringLiteral("managedHelpCommand = command")));
            QVERIFY(source.contains(QStringLiteral("managedHelpExplanation = explanation")));
            QVERIFY(!source.contains(QStringLiteral("\"flatpak update")));
            QVERIFY(!source.contains(QStringLiteral("\"snap refresh")));
        }
        // The dialog's help button no longer closes the dialog immediately
        // (there would be nothing left to read/copy) — pin the absence of
        // root.close() directly on that handler.
        const int dialogHelpIdx = dialog.indexOf(QStringLiteral("updateDialogManagedHelpButton"));
        QVERIFY(dialogHelpIdx >= 0);
        const QString dialogHelpBlock = dialog.mid(dialogHelpIdx, 450);
        QVERIFY(dialogHelpBlock.contains(QStringLiteral("openManagedUpdateHelp()")));
        QVERIFY(!dialogHelpBlock.contains(QStringLiteral("root.close()")));
    }

    // ---- Up-to-date wording is exact ----

    void upToDateWordingIsExact()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral(
            "text: qsTr(\"Lightning is up to date.\")")));
    }

    // ---- Development/unknown installs never offer an install action ----

    void developmentInstallDoesNotOfferInstallation()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral(
            "readonly property bool isDevOrUnknown:")));
        QVERIFY(section.contains(QStringLiteral(
            "root.installType === \"development\" || root.installType === \"unknown\"")));
        QVERIFY(section.contains(QStringLiteral("updateDevBuildNotice")));
        QVERIFY(section.contains(QStringLiteral(
            "Automatic installation is disabled for this")));

        // The two install-triggering buttons (Update now, Install and
        // restart / Install without restarting) are gated on
        // canInstallAutomatically, which the spec (§5) sets false for
        // development/unknown — so neither button is reachable for those
        // install types. Pin the exact gate on both.
        const int nowIdx = section.indexOf(QStringLiteral("updateNowButton"));
        QVERIFY(nowIdx >= 0);
        const int rowStart = section.lastIndexOf(QStringLiteral("RowLayout {"), nowIdx);
        QVERIFY(rowStart >= 0 && rowStart < nowIdx);
        const QString nowRow = section.mid(rowStart, nowIdx - rowStart + 200);
        QVERIFY(nowRow.contains(QStringLiteral("root.canInstallAutomatically")));

        // installAndRestart()/installUpdate() are gated together by the
        // wrapping row's own visible: — both share ONE gate, not two that
        // could drift apart.
        const int actionsRowIdx = section.indexOf(
            QStringLiteral("updateReadyToInstallActionsRow"));
        QVERIFY(actionsRowIdx >= 0);
        const QString actionsRowBlock = section.mid(actionsRowIdx, 700);
        QVERIFY(actionsRowBlock.contains(QStringLiteral(
            "visible: root.canInstallAutomatically")));
        QVERIFY(actionsRowBlock.contains(QStringLiteral("updateInstallAndRestartButton")));
        QVERIFY(actionsRowBlock.contains(QStringLiteral("updateInstallButton")));
    }

    // ---- Restart is always an explicit, user-initiated action ----
    //
    // UpdateManager::startInstall (the shared body behind both
    // installAndRestart() and installUpdate()) guards on
    // `m_state == ReadyToInstall`; both calls leave RestartRequired behind
    // them. So installAndRestart()/installUpdate() must be called ONLY from
    // the ReadyToInstall block — a button in the RestartRequired block
    // calling either would silently no-op (the guard would just return),
    // which is worse than no button at all. This test pins BOTH halves of
    // that: the real calls live only at ReadyToInstall, and
    // RestartRequired offers no button that reaches back into
    // UpdateManager for either.

    void installAndRestartIsOnlyEverCalledFromReadyToInstall()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());

        const int readyIdx = section.indexOf(QStringLiteral("updateReadyToInstallBlock"));
        QVERIFY(readyIdx >= 0);
        const int readyEnd = section.indexOf(QStringLiteral("updateInstallingBlock"), readyIdx);
        QVERIFY(readyEnd > readyIdx);
        const QString readyBlock = section.mid(readyIdx, readyEnd - readyIdx);
        QVERIFY(readyBlock.contains(QStringLiteral("root.um.installAndRestart()")));
        QVERIFY(readyBlock.contains(QStringLiteral("root.um.installUpdate()")));

        const int restartIdx = section.indexOf(QStringLiteral("updateRestartRequiredBlock"));
        QVERIFY(restartIdx >= 0);
        // End at whichever block comes next, so this window stays exactly the
        // handoff block. It used to run to updateFailedBlock, which silently
        // swallowed anything added in between — and the last-update-result
        // block, which legitimately has its own Dismiss button, now sits
        // there.
        int restartEnd = section.indexOf(QStringLiteral("updateLastResultBlock"), restartIdx);
        const int failedIdx = section.indexOf(QStringLiteral("updateFailedBlock"), restartIdx);
        if (restartEnd < 0 || (failedIdx >= 0 && failedIdx < restartEnd))
            restartEnd = failedIdx;
        QVERIFY(restartEnd > restartIdx);
        const QString restartBlock = section.mid(restartIdx, restartEnd - restartIdx);
        QVERIFY(!restartBlock.contains(QStringLiteral("installAndRestart()")));
        QVERIFY(!restartBlock.contains(QStringLiteral("installUpdate()")));
        // No AppButton at all in this block — it is purely informational.
        QCOMPARE(restartBlock.count(QStringLiteral("AppButton {")), 0);
        // The detail line is UpdateManager's, which words the quit-now and
        // apply-on-next-quit paths differently instead of asserting either.
        QVERIFY(restartBlock.contains(QStringLiteral("handoffSummary")));
    }

    // ---- Release notes are never rendered as RichText/StyledText ----

    void releaseNotesAreNeverRichOrStyledText()
    {
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(!dialog.contains(QStringLiteral("Text.RichText")));
        QVERIFY(!dialog.contains(QStringLiteral("Text.StyledText")));
        QVERIFY(!dialog.contains(QStringLiteral("TextEdit.RichText")));
        QVERIFY(!dialog.contains(QStringLiteral("TextEdit.MarkdownText")));
        QVERIFY(!dialog.contains(QStringLiteral("Text.MarkdownText")));
        QVERIFY(!dialog.contains(QStringLiteral("onLinkActivated")));
        QVERIFY(dialog.contains(QStringLiteral("textFormat: TextEdit.PlainText")));
        QVERIFY(dialog.contains(QStringLiteral("updateReleaseNotesText")));
        QVERIFY(dialog.contains(QStringLiteral("readOnly: true")));
    }

    // ---- releaseNotesUrl is shown, but only as inert plain text ----

    void releaseNotesUrlIsPlainTextNeverALiveLink()
    {
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        const int idx = dialog.indexOf(QStringLiteral("updateReleaseNotesUrlLabel"));
        QVERIFY(idx >= 0);
        const QString block = dialog.mid(idx, 500);
        QVERIFY(block.contains(QStringLiteral("root.um.releaseNotesUrl")));
        QVERIFY(block.contains(QStringLiteral("textFormat: Text.PlainText")));
        QVERIFY(!block.contains(QStringLiteral("MouseArea")));
        QVERIFY(!block.contains(QStringLiteral("onLinkActivated")));
        QVERIFY(!block.contains(QStringLiteral("Qt.openUrlExternally")));
    }

    // ---- The dialog opens/closes off UpdateManager state, not a caller ----

    void dialogFollowsUpdateManagerStateExclusively()
    {
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral(
            "root.um.state === UpdateManager.UpdateAvailable")));
        QVERIFY(dialog.contains(QStringLiteral(
            "root.um.latestVersion !== root.um.dismissedVersion")));
        QVERIFY(dialog.contains(QStringLiteral("onShouldBeOpenChanged:")));
    }
};

QTEST_GUILESS_MAIN(UpdatesUiContractTest)
#include "UpdatesUiContractTest.moc"
