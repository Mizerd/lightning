
#include <QFile>
#include <QJSValue>
#include <QRegularExpression>
#include <QtTest>

namespace {
QString readQml(const char *name)
{
    QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/") + QLatin1String(name));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                       : QString();
}
}

class AppShellContractTest : public QObject
{
    Q_OBJECT
private slots:
    void cornerPromptsSitBelowEveryPopupButTheRing()
    {
        const QString s = readQml("Main.qml");
        const int pa = s.indexOf(QStringLiteral("objectName: \"cornerPassiveHost\""));
        QVERIFY(pa > 0);
        const int next = s.indexOf(QStringLiteral("// Picture-in-picture"), pa);
        QVERIFY(next > pa);
        const QString host = s.mid(pa, next - pa);
        QVERIFY(!host.contains(QStringLiteral("z: 900")));
        QVERIFY(host.contains(QRegularExpression(QStringLiteral("\\bz: -1\\b"))));
        QVERIFY(!host.contains(QStringLiteral("IncomingCallPrompt {")));
        const int ring = s.indexOf(QStringLiteral("objectName: \"incomingCallHost\""));
        QVERIFY(ring > 0 && ring < pa);
        const QString ringHost = s.mid(ring, pa - ring);
        QVERIFY(ringHost.contains(QStringLiteral("z: 900")));
        QVERIFY(ringHost.contains(QStringLiteral("IncomingCallPrompt {")));
    }
    void theAvoidHostSpansTheRingToo()
    {
        const QString s = readQml("Main.qml");
        QCOMPARE(s.count(QStringLiteral("objectName: \"cornerPromptHost\"")), 1);
        const int at = s.indexOf(QStringLiteral("objectName: \"cornerPromptHost\""));
        const QString proxy = s.mid(at, 900);
        QVERIFY(proxy.contains(QStringLiteral("incomingCallHost.height")));
        QVERIFY(proxy.contains(QStringLiteral("cornerPassiveHost.height")));
        QVERIFY(proxy.contains(QStringLiteral("enabled: false")));
        QVERIFY(readQml("VoiceMiniPlayer.qml")
                    .contains(QStringLiteral("\"cornerPromptHost\"")));
    }
    void theComposerProbeIsBounded()
    {
        const QString s = readQml("Main.qml");
        QVERIFY(s.contains(QStringLiteral("findByObjectName(pageLoader.item,")));
        QVERIFY(!s.contains(QStringLiteral("findByObjectName(window.contentItem")));
        QVERIFY(s.contains(QStringLiteral("composerMissKey !== key")));
        QVERIFY(s.contains(QStringLiteral("c.visible !== undefined")));
        QVERIFY(s.contains(QStringLiteral("c.parent !== null")));
    }
    void cornerPromptsClearTheMeasuredComposer()
    {
        const QString s = readQml("Main.qml");
        QVERIFY(s.contains(QStringLiteral("\"composerCard\"")));
        QVERIFY(s.contains(QStringLiteral("c.mapToItem(window.contentItem, 0, 0).y")));
        QVERIFY(s.contains(QStringLiteral(
            "anchors.bottomMargin: AppTheme.spacing16 + window.composerClearance")));
        QVERIFY(!s.contains(QStringLiteral("AppTheme.scaled(88)")));
    }
    void roomListMinimumFitsInviteButtons()
    {
        const QString s = readQml("MainScreen.qml");
        const int at = s.indexOf(QStringLiteral("objectName: \"roomsPanel\""));
        QVERIFY(at > 0);
        QVERIFY(s.mid(at, 800).contains(
            QStringLiteral("SplitView.minimumWidth:   app.settings.roomListMinWidth")));
        QFile header(QStringLiteral(LIGHTNING_SOURCE_DIR "/src/app/SettingsManager.h"));
        QVERIFY(header.open(QIODevice::ReadOnly));
        QRegularExpression re(QStringLiteral(
            "kRoomListExpandedMinWidth\\s*=\\s*(\\d+)"));
        const auto m = re.match(QString::fromUtf8(header.readAll()));
        QVERIFY(m.hasMatch());
        QVERIFY2(m.captured(1).toInt() >= 208, qPrintable(m.captured(1)));
    }
    void statusDialogBehaviours()
    {
        const QString s = readQml("StatusDialog.qml");
        QVERIFY(!s.contains(QStringLiteral("customDate.text = Qt.formatDate(new Date()")));
        QCOMPARE(s.count(QStringLiteral("onAccepted: if (setButton.enabled)")), 3);
        QVERIFY(s.contains(QStringLiteral("customMalformed()")));
    }
};

QTEST_GUILESS_MAIN(AppShellContractTest)
#include "AppShellContractTest.moc"
