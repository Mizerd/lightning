// Keyboard reachability and small-window behaviour of the verification dialog
// and the corner prompt (swarm findings C55-F01/D01, F02, F03/D03, F05/D02,
// D04). Source contracts: each predicate is absent from the pre-fix QML, so
// each case fails on the old code.
#include <QFile>
#include <QRegularExpression>
#include <QtTest>

class VerificationDialogContractTest : public QObject
{
    Q_OBJECT
    static QString norm(const QString &name)
    {
        QFile f(QStringLiteral(QML_DIR "/") + name);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        QString s = QString::fromUtf8(f.readAll());
        s.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        return s;
    }

private Q_SLOTS:
    void theDialogTakesKeyboardFocusAndHandsItToTheStepsPrimaryAction()
    {
        const QString d = norm(QStringLiteral("VerificationDialog.qml"));
        QVERIFY(!d.isEmpty());
        QVERIFY2(d.contains(QStringLiteral("focus: true")),
                 "Popup must hold active focus or Tab walks the page behind");
        QVERIFY2(d.contains(QStringLiteral("onOpened: verificationPanel.focusPrimary()")),
                 "opening must focus the step's primary action");
        const QString p = norm(QStringLiteral("VerificationPanel.qml"));
        QVERIFY(p.contains(QStringLiteral("function focusPrimary()")));
        QVERIFY(p.contains(QStringLiteral("forceActiveFocus(")));
        // Safe action only: auto-focusing a trust-confirming button lets a
        // stray Space/Enter confirm a SAS unseen (CLAUDE.md section 6).
        QVERIFY2(p.contains(QStringLiteral("var order = [dismissButton, cancelButton]")),
                 "initial focus must be Dismiss/Cancel only");
        const int fn = p.indexOf(QStringLiteral("function focusPrimary()"));
        const int fnEnd = p.indexOf(QStringLiteral("function _holdsFocus()"));
        QVERIFY(fn >= 0 && fnEnd > fn);
        const QString body = p.mid(fn, fnEnd - fn);
        QVERIFY2(!body.contains(QStringLiteral("matchButton"))
                     && !body.contains(QStringLiteral("acceptButton"))
                     && !body.contains(QStringLiteral("qrConfirmButton")),
                 "focusPrimary must never target a confirming button");
        QVERIFY2(p.contains(QStringLiteral("onVerificationStateChanged")),
                 "focus must follow the step, or a button that disappears "
                 "drops focus back to the page");
    }
    void theDialogNeverOutgrowsTheWindowAndScrolls()
    {
        const QString d = norm(QStringLiteral("VerificationDialog.qml"));
        QVERIFY2(d.contains(QStringLiteral("height: Math.min(implicitHeight,")),
                 "dialog height must be clamped to the window");
        QVERIFY2(d.contains(QStringLiteral("contentItem: Flickable")),
                 "clamped content must scroll so Cancel stays reachable");
    }
    void anIncomingRequestNamesTheAskingSession()
    {
        const QString p = norm(QStringLiteral("VerificationPanel.qml"));
        QVERIFY2(p.contains(QStringLiteral("app.verificationOtherDevice")),
                 "request text must name the requesting device");
    }
    void theCornerPromptIsReachableByKeyboardEarly()
    {
        // F6 reaches it as a region of the shell's F6 cycle while it shows
        // (one F6 owner, MASTER-3 M3), not through a Shortcut of its own.
        const QString v = norm(QStringLiteral("VerifySessionPrompt.qml"));
        QVERIFY(!v.contains(QStringLiteral("sequence: \"F6\"")));
        QVERIFY(v.contains(QStringLiteral("Keys.onEscapePressed: app.dismissVerificationWarning()")));
        // The card lives on the window overlay, outside MainScreen: Main.qml
        // hands it in, and F6 inside it is forwarded back to the shell's
        // cycle. KeyboardNavigationQmlTest drives both directions.
        const QString m = norm(QStringLiteral("MainScreen.qml"));
        QVERIFY(m.contains(QStringLiteral("property Item promptRegion: null")));
        QVERIFY(m.contains(QStringLiteral("shellRoot._promptRegion()")));
        QVERIFY(m.contains(QStringLiteral("function onRegionCycleRequested(backward)")));
        QVERIFY(norm(QStringLiteral("Main.qml"))
                    .contains(QStringLiteral("MainScreen { promptRegion: verifySessionPromptCard }")));
        QVERIFY(v.contains(QStringLiteral("signal regionCycleRequested(bool backward)")));
    }
    void aFailedFlowDropsThePreVerificationIntro()
    {
        const QString d = norm(QStringLiteral("VerificationDialog.qml"));
        QVERIFY(d.contains(QStringLiteral(
            "visible: app.verificationState !== \"cancelled\" && "
            "!app.verificationState.startsWith(\"failed\")")));
    }
};

QTEST_MAIN(VerificationDialogContractTest)
#include "VerificationDialogContractTest.moc"
