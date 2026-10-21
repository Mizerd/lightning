// Source-contract test for the About / log-export / Updates fixes of swarm
// chunk C71. Each case fails on the code before the fix (see the notes).

#include <QtTest/QtTest>

#include <QFile>

class AboutLogUiContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll());
    }

private Q_SLOTS:
    // C71-F05: the card refreshes while it stays open, and drops a stale status.
    void logCardFollowsTheLogWhileOpen()
    {
        const QString q = read(QStringLiteral("LogExportCard.qml"));
        QVERIFY(!q.isEmpty());
        QVERIFY(q.contains(QStringLiteral("logExportRefreshTimer")));
        QVERIFY(q.contains(QStringLiteral("repeat: true")));
        // prepare() rebuilds the whole zip: poll slowly, never stack runs.
        QVERIFY(q.contains(QStringLiteral("interval: 30000")));
        QVERIFY(q.contains(QStringLiteral("preview.preparing")));
        QVERIFY(q.contains(QStringLiteral("root.statusText = \"\"\n            refresh()")));
    }

    // C71-F07: deleting the saved logs asks first; the button must not delete.
    void deletingLogsNeedsConfirmation()
    {
        const QString q = read(QStringLiteral("LogExportCard.qml"));
        QVERIFY(q.contains(QStringLiteral("logExportDeleteConfirm")));
        QVERIFY(q.contains(QStringLiteral("onClicked: logDeleteConfirm.open()")));
        const int btn = q.indexOf(QStringLiteral("objectName: \"logExportDelete\""));
        const int dlg = q.indexOf(QStringLiteral("id: logDeleteConfirm"));
        QVERIFY(btn >= 0 && dlg > btn);
        QVERIFY(!q.mid(btn, dlg - btn).contains(QStringLiteral("deleteSavedLogs")));
    }

    // C71-F07: a dismissed failure leaves a sentence, not an empty card.
    void dismissedFailureIsExplained()
    {
        const QString q = read(QStringLiteral("UpdatesSettingsSection.qml"));
        QVERIFY(q.contains(QStringLiteral("updateFailureDismissedNote")));
    }

    // C71-F09: the confirm dialog hands keyboard focus in and back out.
    void resetDialogManagesFocus()
    {
        const QString q = read(QStringLiteral("AboutToolsSection.qml"));
        QVERIFY(q.contains(QStringLiteral("resetCancelButton.forceActiveFocus(Qt.TabFocusReason)")));
        QVERIFY(q.contains(QStringLiteral("resetOpener.forceActiveFocus(Qt.TabFocusReason)")));
    }

    // C71-F10: values in the version summary are bidi-isolated for display.
    void versionValuesAreBidiIsolated()
    {
        const QString q = read(QStringLiteral("AboutToolsSection.qml"));
        QVERIFY(q.contains(QStringLiteral("function isolateValues")));
        QVERIFY(q.contains(QStringLiteral("root.isolateValues(root.about.versionSummary)")));
        QVERIFY(q.contains(QStringLiteral("\\u2066")));
        QVERIFY(q.contains(QStringLiteral("\\u2069")));
    }
};

QTEST_APPLESS_MAIN(AboutLogUiContractTest)
#include "AboutLogUiContractTest.moc"
