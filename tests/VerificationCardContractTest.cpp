// Gap test G2 (0.6.5 design round, Wave 2): pins the SettingsScreen own-
// session verification-card visibility complement that the c259b60 fix
// established but never had a dedicated test for. Before that fix, a
// verification failure raised before a flow id existed (no cross-signing
// identity, a failed request send, not signed in) landed in the hidden
// "active flow" card while the "start" row's condition also hid it — an
// invisible failure with no visible feedback at all. The fix made the two
// conditions exact complements of `verificationActive`/`verificationState`,
// and gave Cancel a negatively-defined visibility so a newly added
// non-terminal state can never silently lose the only escape hatch again.
//
// v0.6.5 (Wave 2 integration): the new sessionsTrustCard (SPEC 1r) embeds its
// own Verify start affordance (TrustCard.showVerify), gated on crypto-
// backend support. That made the legacy "Verify this session" row's start
// condition ADD a third clause — `&& !(app.cryptoHealth &&
// app.cryptoHealth.cryptoSupported)` — so it only covers backends without
// the card. The complement invariant this test pins is therefore no longer
// "the start row and the flow card are exact complements of each other" in
// isolation; it is "exactly one start affordance is visible whenever no
// flow is running — the trust card's on crypto-supporting backends, the
// legacy row otherwise, never both". The flow-card and Cancel predicates
// are unchanged.
//
// This scans against the PREDICATES with whitespace/formatting collapsed to
// single spaces — never exact indentation — so it survives incidental
// reflow.
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

    // Collapse all whitespace (spaces, tabs, newlines, indentation) to single
    // spaces so a predicate wrapped, re-indented, or reflowed by the ongoing
    // restyle still matches — only the boolean predicate text itself is
    // pinned, never its formatting.
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

        // The legacy "Verify this session" row is hidden whenever a flow is
        // active or has ever produced a non-empty state (never shown at the
        // same time as the flow card below), AND ALSO whenever the new
        // sessionsTrustCard already offers its own Verify affordance for
        // this backend — the two start controls must never coexist.
        const QString startRow = normalized(QStringLiteral(
            "visible: !app.verificationActive "
            "&& app.verificationState === \"\" "
            "&& !(app.cryptoHealth "
            "&& app.cryptoHealth.cryptoSupported)"));
        QVERIFY2(norm.contains(startRow),
                 "legacy start-row visibility predicate not found — expected "
                 "the flow-card complement ANDed with the trust-card's "
                 "crypto-backend gate");

        // The active-flow card is the EXACT complement of the start row: any
        // non-empty state OR an active flow shows it. This is the c259b60
        // fix itself — a failure raised before a flow id existed used to
        // satisfy neither condition and vanish silently.
        const QString flowCard = normalized(QStringLiteral(
            "visible: app.verificationActive "
            "|| app.verificationState !== \"\""));
        QVERIFY2(norm.contains(flowCard),
                 "flow-card visibility predicate not found — this must stay "
                 "the exact complement of the start row's condition");

        // Cancel is defined NEGATIVELY (present unless terminal/absent) so a
        // newly added non-terminal state can never drop it by omission.
        const QString cancelVisible = normalized(QStringLiteral(
            "visible: app.verificationState !== \"\" "
            "&& app.verificationState !== \"done\" "
            "&& app.verificationState !== \"cancelled\" "
            "&& !app.verificationState.startsWith(\"failed\")"));
        QVERIFY2(norm.contains(cancelVisible),
                 "Cancel's negative-state visibility predicate not found — "
                 "it must stay defined as \"present unless terminal\", not "
                 "as a positive list of in-progress states");
    }
};

QTEST_MAIN(VerificationCardContractTest)
#include "VerificationCardContractTest.moc"
