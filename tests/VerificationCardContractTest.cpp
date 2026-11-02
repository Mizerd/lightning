#include <QFile>
#include <QRegularExpression>
#include <QtTest>

class VerificationCardContractTest : public QObject
{
    Q_OBJECT
private:
    static QString read(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    static QString normalized(const QString &s)
    {
        QString out = s;
        out.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        return out.trimmed();
    }

private Q_SLOTS:
    void verificationCardVisibilityPredicatesArePinned()
    {
        const QString src =
            read(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
        QVERIFY2(!src.isEmpty(), "could not read SettingsScreen.qml");
        const QString norm = normalized(src);

        const QString startRow = normalized(QStringLiteral(
            "visible: !app.verificationActive "
            "&& app.verificationState === \"\" "
            "&& !(app.cryptoHealth "
            "&& app.cryptoHealth.cryptoSupported)"));
        QVERIFY2(norm.contains(startRow),
                 "legacy start-row visibility predicate not found — expected "
                 "the flow-card complement ANDed with the trust-card's "
                 "crypto-backend gate");

        const QString dialogSrc =
            read(QStringLiteral(QML_DIR "/VerificationDialog.qml"));
        QVERIFY2(!dialogSrc.isEmpty(), "could not read VerificationDialog.qml");
        const QString dialogNorm = normalized(dialogSrc);

        const QString flowPresent = normalized(QStringLiteral(
            "readonly property bool flowPresent: "
            "app.verificationActive || app.verificationState !== \"\""));
        QVERIFY2(dialogNorm.contains(flowPresent),
                 "flow-presence predicate not found in VerificationDialog — "
                 "this must stay the exact complement of the start row's "
                 "condition");

        QVERIFY2(dialogNorm.contains(normalized(QStringLiteral(
                     "onFlowPresentChanged: _syncOpenState()"))),
                 "VerificationDialog must track flowPresent, not just its "
                 "rising edge");
        QVERIFY2(dialogNorm.contains(normalized(QStringLiteral(
                     "} else if (opened) { close() }"))),
                 "_syncOpenState must CLOSE when the flow state clears, or "
                 "Dismiss leaves the dialog open on an empty panel");

        QVERIFY2(!dialogNorm.contains(normalized(QStringLiteral(
                     "readonly property bool flowIsLive: "
                     "app.verificationActive"))),
                 "flowIsLive must not be derived from verificationActive — "
                 "the flow id survives every terminal state");

        const QString panelSrc =
            read(QStringLiteral(QML_DIR "/VerificationPanel.qml"));
        QVERIFY2(!panelSrc.isEmpty(), "could not read VerificationPanel.qml");
        const QString panelNorm = normalized(panelSrc);

        const QString cancelVisible = normalized(QStringLiteral(
            "visible: app.verificationState !== \"\" "
            "&& app.verificationState !== \"done\" "
            "&& app.verificationState !== \"cancelled\" "
            "&& !app.verificationState.startsWith(\"failed\")"));
        QVERIFY2(panelNorm.contains(cancelVisible),
                 "Cancel's negative-state visibility predicate not found — "
                 "it must stay defined as \"present unless terminal\", not "
                 "as a positive list of in-progress states");
    }

    void verificationWarningIsDismissibleAndBadgeScoped()
    {
        const QString rail = read(QStringLiteral(QML_DIR "/SpacesRail.qml"));
        QVERIFY2(!rail.isEmpty(), "could not read SpacesRail.qml");
        QVERIFY2(normalized(rail).contains(normalized(QStringLiteral(
                     "visible: app.sessionVerificationWarning"))),
                 "the rail cog badge must be gated on "
                 "sessionVerificationWarning (the dismissible form)");

        const QString settings =
            read(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
        const QString settingsNorm = normalized(settings);
        QVERIFY2(settingsNorm.contains(normalized(QStringLiteral(
                     "alert: app.sessionVerificationWarning"))),
                 "the Sessions nav row alert must use the dismissible gate");
        QVERIFY2(settingsNorm.contains(normalized(QStringLiteral(
                     "objectName: \"verificationRestingStatus\""))),
                 "the Sessions page must keep a resting verification status");
        QVERIFY2(settingsNorm.contains(QStringLiteral(
                     "app.sessionVerificationNeeded")),
                 "the resting status must read the undismissible fact");
    }
};

QTEST_MAIN(VerificationCardContractTest)
#include "VerificationCardContractTest.moc"
