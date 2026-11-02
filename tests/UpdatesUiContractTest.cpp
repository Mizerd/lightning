
#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QStringList>

class UpdatesUiContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QString src = QString::fromUtf8(file.readAll());
        src.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                      QRegularExpression::DotMatchesEverythingOption));
        src.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        return src;
    }

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

    void filesExist()
    {
        QVERIFY(!read(QStringLiteral("UpdatesSettingsSection.qml")).isEmpty());
        QVERIFY(!read(QStringLiteral("UpdateAvailableDialog.qml")).isEmpty());
        QVERIFY(!read(QStringLiteral("SettingsScreen.qml")).isEmpty());
    }

    void updateAvailableDialogIsActuallyInstantiated()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY2(!main.isEmpty(), "Main.qml must be readable");
        QVERIFY2(main.contains(QStringLiteral("UpdateAvailableDialog {")),
                 "Main.qml must create the update-available dialog; the dialog "
                 "opens itself off UpdateManager's state, so if nothing "
                 "instantiates it the update-available prompt never appears");
    }

    void handoffStateNeverClaimsTheUpdateIsInstalled()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int idx = section.indexOf(QStringLiteral("updateRestartRequiredBlock"));
        QVERIFY(idx >= 0);
        const QString block = section.mid(idx, 2000);
        QVERIFY2(!block.contains(QStringLiteral("is installed.")),
                 "the handoff state must not assert a completed installation");
        QVERIFY(block.contains(QStringLiteral("ready to install")));
        QVERIFY2(block.contains(QStringLiteral("handoffSummary")),
                 "the detail line must come from UpdateManager, which words it "
                 "differently for the quit-now and apply-on-next-quit paths");
    }

    void previousUpdateOutcomeIsSurfacedAndDismissible()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int idx = section.indexOf(QStringLiteral("updateLastResultBlock"));
        QVERIFY2(idx >= 0, "the previous update's outcome must be shown");
        const QString block = section.mid(idx, 3200);
        QVERIFY(block.contains(QStringLiteral("lastUpdateResult")));
        QVERIFY(block.contains(QStringLiteral("UpdateManager.NoResult")));
        QVERIFY(block.contains(QStringLiteral("UpdateManager.InstallFailed")));
        QVERIFY(block.contains(QStringLiteral("lastUpdateError")));
        QVERIFY2(block.contains(QStringLiteral("clearLastUpdateResult()")),
                 "a stale success/failure must be dismissible for good");
        QVERIFY2(block.contains(QStringLiteral("explainInstallError")),
                 "the failure must be explained, not shown as its enum token");
        QVERIFY2(block.contains(QStringLiteral("updateLastResultCode")),
                 "the token must still be shown separately, for reporting");
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

    void updatesSectionIsReachableFromSettingsNavigation()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!settings.isEmpty());
        QVERIFY(settings.contains(QStringLiteral("sectionKey: \"updates\"")));
        QVERIFY(settings.contains(QStringLiteral("navLabel: qsTr(\"Updates\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "if (key === \"updates\") return qsTr(\"Updates\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "if (key === \"updates\") return \"download\"")));
        QVERIFY(settings.contains(QStringLiteral("UpdatesSettingsSection {")));
        QVERIFY(settings.contains(QStringLiteral(
            "visible: root.section === \"updates\"")));
        QVERIFY(!settings.contains(QStringLiteral(
            "Loader { source: \"UpdatesSettingsSection.qml\"")));
    }

    void updatesIconReusesAnExistingVerifiedCodepoint()
    {
        const QString icon = read(QStringLiteral("IconGlyphs.js"));
        QVERIFY(!icon.isEmpty());
        QVERIFY(icon.contains(QStringLiteral("\"download\": \"\\uf090\"")));
    }

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
        const QString block = end > start ? section.mid(start, end - start)
                                           : section.mid(start);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("state === UpdateManager.Failed")));
        QVERIFY(block.contains(QStringLiteral("root.um.errorMessage")));
        QVERIFY(block.contains(QStringLiteral("updateRetryButton")));
        QVERIFY(block.contains(QStringLiteral("root.um.checkForUpdates()")));
        QVERIFY(block.contains(QStringLiteral("updateDismissFailureButton")));
        QCOMPARE(block.count(QStringLiteral("AppButton {")), 2);
        const int dismissIdx = block.indexOf(QStringLiteral("updateDismissFailureButton"));
        QVERIFY(dismissIdx >= 0);
        const QString dismissTail = block.mid(dismissIdx, 200);
        QVERIFY(dismissTail.contains(QStringLiteral("root.failureDismissed = true")));
        QVERIFY(!dismissTail.contains(QStringLiteral("root.um.")));
    }

    void theDialogsUpdateNowLandsOnTheUpdatesSection()
    {
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        const int idx = dialog.indexOf(QStringLiteral("updateDialogUpdateNowButton"));
        QVERIFY2(idx > 0, "the dialog's Update now button is gone");
        const int handlerEnd = dialog.indexOf(QStringLiteral("\n            }"), idx);
        QVERIFY2(handlerEnd > idx, "could not find the end of the button's handler");
        const QString handler = dialog.mid(idx, handlerEnd - idx);
        QVERIFY2(handler.contains(QStringLiteral("downloadUpdate()")),
                 qPrintable(QStringLiteral("Update now no longer starts the "
                                           "download: %1").arg(handler)));
        QVERIFY2(handler.contains(QStringLiteral("showSettingsSection")),
                 qPrintable(QStringLiteral(
                     "Update now starts a download and then shows the user "
                     "nothing — the progress and the Install button are only "
                     "in the settings section: %1").arg(handler)));
        QVERIFY2(handler.contains(QStringLiteral("\"updates\"")),
                 qPrintable(QStringLiteral("Update now navigates somewhere "
                                           "other than Updates: %1").arg(handler)));
    }

    void packageManagedInstallsNeverExposeDownloadOrInstall()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(!dialog.isEmpty());

        for (const QString &source : { section, dialog }) {
            QVERIFY(source.contains(QStringLiteral(
                "Updates for this installation are managed by Flatpak.")));
            QVERIFY(source.contains(QStringLiteral(
                "Updates for this installation are managed by Snap.")));
            QVERIFY(source.contains(QStringLiteral(
                "root.installType === \"linux-rpm-repo\"\n")));
            QVERIFY(source.contains(QStringLiteral(
                "? qsTr(\"Updates for this installation are managed by dnf.\")")));
            QVERIFY(source.contains(QStringLiteral(
                "root.um.packageManagerName === \"zypper\"")));
            QVERIFY(source.contains(QStringLiteral(
                "? qsTr(\"Updates for this installation are managed by zypper.\")")));
            QVERIFY(source.contains(QStringLiteral("openManagedUpdateHelp()")));
        }

        const int actionsIdx = section.indexOf(QStringLiteral("updateActionsRow"));
        QVERIFY(actionsIdx >= 0);
        const QString actionsTail = section.mid(actionsIdx, 400);
        QVERIFY(actionsTail.contains(QStringLiteral(
            "visible: !root.packageManaged && root.canInstallAutomatically")));

        const int dialogNowIdx = dialog.indexOf(QStringLiteral("updateDialogUpdateNowButton"));
        QVERIFY(dialogNowIdx >= 0);
        const QString dialogNowBlock = dialog.mid(dialogNowIdx, 300);
        QVERIFY(dialogNowBlock.contains(QStringLiteral(
            "visible: !root.packageManaged && root.canInstallAutomatically")));

        const int managedIdx = section.indexOf(QStringLiteral("updateManagedBlock"));
        QVERIFY(managedIdx >= 0);
        const int managedEnd = section.indexOf(QStringLiteral("updateActionsRow"), managedIdx);
        QVERIFY(managedEnd > managedIdx);
        const QString managedBlock = section.mid(managedIdx, managedEnd - managedIdx);
        QVERIFY(!managedBlock.contains(QStringLiteral("downloadUpdate()")));
        QVERIFY(!managedBlock.contains(QStringLiteral("installUpdate()")));
        QVERIFY(!managedBlock.contains(QStringLiteral("installAndRestart()")));
    }

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
        QVERIFY(captionBlock.contains(QStringLiteral("On by default")));
        QVERIFY(captionBlock.contains(QStringLiteral("turn it off")));
        QVERIFY(captionBlock.contains(QStringLiteral("never includes any account")));
        QVERIFY(captionBlock.contains(QStringLiteral("device, or Matrix")));
        QVERIFY(!captionBlock.toLower().contains(QStringLiteral("room id")));
        QVERIFY(!captionBlock.toLower().contains(QStringLiteral("user id")));

        const int checkIdx = section.indexOf(QStringLiteral("updateCheckNowButton"));
        QVERIFY(checkIdx >= 0);
        const QString checkBlock = section.mid(checkIdx, 450);
        QVERIFY(checkBlock.contains(QStringLiteral(
            "root.um.state !== UpdateManager.Checking")));
        QVERIFY(checkBlock.contains(QStringLiteral(
            "root.um.state !== UpdateManager.Downloading")));
        QVERIFY(checkBlock.contains(QStringLiteral("root.um.checkForUpdates()")));
    }

    void lastCheckTimeIsHandledAsADateTimeNotAString()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int propIdx = section.indexOf(QStringLiteral(
            "readonly property bool everChecked:"));
        QVERIFY2(propIdx >= 0, "everChecked is gone from UpdatesSettingsSection");
        const int propEnd = section.indexOf(
            QRegularExpression(QStringLiteral("\n    [A-Za-z/]")), propIdx + 1);
        QVERIFY2(propEnd > propIdx,
                 "could not find the end of the everChecked declaration");
        const QString propBlock = section.mid(propIdx, propEnd - propIdx);
        QVERIFY(propBlock.contains(QStringLiteral(
            "!isNaN(root.um.lastCheckTime.getTime())")));

        const int idx = section.indexOf(QStringLiteral("updateLastCheckedLabel"));
        QVERIFY(idx >= 0);
        const QString block = section.mid(idx, 900);
        QVERIFY(block.contains(QStringLiteral("root.everChecked")));
        QVERIFY(!section.contains(QStringLiteral("lastCheckTime.length")));
        QVERIFY(block.contains(QStringLiteral("Qt.locale(AppLocale.name).toString(")));
    }

    void theIdleStatusDoesNotContradictTheLastCheckedRow()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        const int idx = section.indexOf(QStringLiteral("updateIdleLabel"));
        QVERIFY2(idx >= 0, "updateIdleLabel is gone from UpdatesSettingsSection");
        const int nextObj = section.indexOf(QStringLiteral("objectName:"),
                                            idx + 1);
        QVERIFY2(nextObj > idx, "could not bound the updateIdleLabel block");
        const QString block = section.mid(idx, nextObj - idx);
        QVERIFY2(block.contains(QStringLiteral("root.everChecked")),
                 qPrintable(QStringLiteral(
                     "the Idle status text does not consult everChecked, so it "
                     "claims no check has ever run while the row above it "
                     "prints the date of the last one:\n") + block));
        QVERIFY(block.contains(QStringLiteral("Updates haven't been checked yet.")));
    }

    void statusDetailIsSurfaced()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral("updateStatusDetailLabel")));
        QVERIFY(section.contains(QStringLiteral("root.um.statusDetail")));
    }

    void installRefusedIsSurfacedNotDiscarded()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral("function onInstallRefused(reason)")));
        QVERIFY(section.contains(QStringLiteral("root.installRefusedReason = reason")));
        QVERIFY(section.contains(QStringLiteral("updateInstallRefusedBlock")));
    }

    void managedUpdateHelpRequestedIsHandledInBothSurfaces()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        const QString dialog = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(!dialog.isEmpty());
        for (const QString &source : { section, dialog }) {
            QVERIFY(source.contains(QStringLiteral(
                "function onManagedUpdateHelpRequested(command, explanation)")));
            QVERIFY(source.contains(QStringLiteral("managedHelpCommand = command")));
            QVERIFY(source.contains(QStringLiteral("managedHelpExplanation = explanation")));
            QVERIFY(!source.contains(QStringLiteral("\"flatpak update")));
            QVERIFY(!source.contains(QStringLiteral("\"snap refresh")));
        }
        const int dialogHelpIdx = dialog.indexOf(QStringLiteral("updateDialogManagedHelpButton"));
        QVERIFY(dialogHelpIdx >= 0);
        const QString dialogHelpBlock = dialog.mid(dialogHelpIdx, 450);
        QVERIFY(dialogHelpBlock.contains(QStringLiteral("openManagedUpdateHelp()")));
        QVERIFY(!dialogHelpBlock.contains(QStringLiteral("root.close()")));
    }

    void upToDateWordingIsExact()
    {
        const QString section = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(!section.isEmpty());
        QVERIFY(section.contains(QStringLiteral(
            "text: qsTr(\"Lightning is up to date.\")")));
    }

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

        const int nowIdx = section.indexOf(QStringLiteral("updateNowButton"));
        QVERIFY(nowIdx >= 0);
        const int rowStart = section.lastIndexOf(QStringLiteral("RowLayout {"), nowIdx);
        QVERIFY(rowStart >= 0 && rowStart < nowIdx);
        const QString nowRow = section.mid(rowStart, nowIdx - rowStart + 200);
        QVERIFY(nowRow.contains(QStringLiteral("root.canInstallAutomatically")));

        const int actionsRowIdx = section.indexOf(
            QStringLiteral("updateReadyToInstallActionsRow"));
        QVERIFY(actionsRowIdx >= 0);
        const QString actionsRowBlock = section.mid(actionsRowIdx, 700);
        QVERIFY(actionsRowBlock.contains(QStringLiteral(
            "visible: root.canInstallAutomatically")));
        QVERIFY(actionsRowBlock.contains(QStringLiteral("updateInstallAndRestartButton")));
        QVERIFY(actionsRowBlock.contains(QStringLiteral("updateInstallButton")));
    }

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
        int restartEnd = section.indexOf(QStringLiteral("updateLastResultBlock"), restartIdx);
        const int failedIdx = section.indexOf(QStringLiteral("updateFailedBlock"), restartIdx);
        if (restartEnd < 0 || (failedIdx >= 0 && failedIdx < restartEnd))
            restartEnd = failedIdx;
        QVERIFY(restartEnd > restartIdx);
        const QString restartBlock = section.mid(restartIdx, restartEnd - restartIdx);
        QVERIFY(!restartBlock.contains(QStringLiteral("installAndRestart()")));
        QVERIFY(!restartBlock.contains(QStringLiteral("installUpdate()")));
        QCOMPARE(restartBlock.count(QStringLiteral("AppButton {")), 0);
        QVERIFY(restartBlock.contains(QStringLiteral("handoffSummary")));
    }

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
