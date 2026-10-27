// Source contracts for the app shell (swarm C01). These read the QML as text:
// each is a property no running case could cheaply observe, and each assertion
// flips on the pre-fix source (stated per case).

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
} // namespace

class AppShellContractTest : public QObject
{
    Q_OBJECT
private slots:
    // Old source: "z: 900" put the whole host, ring included, above the
    // modal scrim; the first fix buried the ring too. The passives are at -1,
    // the ring has its own host above popups, and VoiceMiniPlayer (which looks
    // for the name "cornerPromptHost") must still find a host spanning both.
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
    // The avoid-rect host VoiceMiniPlayer finds is a proxy whose size is the
    // sum of BOTH real hosts; without that the ring is not avoided.
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
    // The probe must not walk the whole window every tick, must not retry a
    // miss until the room changes, and must drop a dead cached item.
    void theComposerProbeIsBounded()
    {
        const QString s = readQml("Main.qml");
        QVERIFY(s.contains(QStringLiteral("findByObjectName(pageLoader.item,")));
        QVERIFY(!s.contains(QStringLiteral("findByObjectName(window.contentItem")));
        QVERIFY(s.contains(QStringLiteral("composerMissKey !== key")));
        QVERIFY(s.contains(QStringLiteral("c.visible !== undefined")));
        QVERIFY(s.contains(QStringLiteral("c.parent !== null")));
    }
    // Old source: only anchors.margins (then a fixed 88 px guess). The
    // clearance must come from the composer item's real top edge.
    void cornerPromptsClearTheMeasuredComposer()
    {
        const QString s = readQml("Main.qml");
        QVERIFY(s.contains(QStringLiteral("\"composerCard\"")));
        QVERIFY(s.contains(QStringLiteral("c.mapToItem(window.contentItem, 0, 0).y")));
        QVERIFY(s.contains(QStringLiteral(
            "anchors.bottomMargin: AppTheme.spacing16 + window.composerClearance")));
        QVERIFY(!s.contains(QStringLiteral("AppTheme.scaled(88)")));
    }
    // Old source: 200 (clipped Reject by up to 8 px).
    void roomListMinimumFitsInviteButtons()
    {
        const QString s = readQml("MainScreen.qml");
        const int at = s.indexOf(QStringLiteral("objectName: \"roomsPanel\""));
        QVERIFY(at > 0);
        // The split's floor is the compact column; the EXPANDED floor, which
        // every non-compact width is clamped to, is what must fit an invite.
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
    // Old source: custom date/time had no onAccepted, default was new Date(),
    // one message for every invalid input.
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
