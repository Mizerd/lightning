
#include <QtTest/QtTest>

#include <QFile>

namespace {

QString read(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                          : QString{};
}

QString block(const QString &src, const QString &marker, const QString &closer)
{
    const int a = src.indexOf(marker);
    if (a < 0)
        return {};
    const int b = src.indexOf(closer, a);
    return b < 0 ? QString{} : src.mid(a, b - a);
}

}

class DiscoverJoinDialogContractTest : public QObject
{
    Q_OBJECT

private slots:
    void resetAllClearsTheBrowseState()
    {
        const QString src = read(QStringLiteral("DiscoverJoinDialog.qml"));
        QVERIFY(!src.isEmpty());
        const QString body = block(src, QStringLiteral("function resetAll()"),
                                   QStringLiteral("\n    }"));
        QVERIFY(!body.isEmpty());
        QVERIFY2(body.contains(QStringLiteral("app.discovery.directory.query = \"\"")),
                 "the search text must not survive a close");
        QVERIFY2(body.contains(QStringLiteral("app.discovery.directory.server = \"\"")),
                 "the directory server must not survive a close");
        const int clearAt = body.indexOf(
            QStringLiteral("app.discovery.directory.clear()"));
        QVERIFY2(clearAt > 0, "stale rows must be dropped");
        QVERIFY(clearAt > body.indexOf(QStringLiteral("directory.server = \"\"")));
    }

    void aKnockIsTrackedAndCannotBeRepeated()
    {
        const QString src = read(QStringLiteral("DiscoverJoinDialog.qml"));
        QVERIFY(src.contains(QStringLiteral("function onKnockSent(roomId)")));
        QVERIFY(src.contains(QStringLiteral("root.knockedRoomIds = m")));
        QVERIFY(src.contains(QStringLiteral("root.addressKnockSent = true")));
        QVERIFY2(src.contains(QStringLiteral("enabled: !root.busy && !root.knockInFlight")),
                 "the address button must be disabled while a knock is in flight");
        QVERIFY2(src.contains(QStringLiteral("&& !root.addressKnockSent")),
                 "after a sent knock the address button must go away");
        QVERIFY2(src.contains(QStringLiteral("|| root.addressKnockSent")),
                 "the preview must read Request pending after a sent knock");
        QVERIFY(src.contains(QStringLiteral("|| root.knockedRoomIds[roomId] === true")));
        const QString err = block(src, QStringLiteral("function onErrorMessageChanged()"),
                                  QStringLiteral("root.revealForLink"));
        QVERIFY(err.contains(QStringLiteral("root.knockInFlight = false")));
    }

    void aBrowseRowIsAKeyboardStopThatActs()
    {
        const QString src = read(QStringLiteral("DiscoverJoinDialog.qml"));
        const QString row = block(src, QStringLiteral("id: directoryRow"),
                                  QStringLiteral("RowLayout {\n                        anchors.fill"));
        QVERIFY(!row.isEmpty());
        QVERIFY(row.contains(QStringLiteral("activeFocusOnTab: true")));
        QVERIFY(row.contains(QStringLiteral("Keys.onReturnPressed: activate()")));
        QVERIFY(row.contains(QStringLiteral("Keys.onSpacePressed: activate()")));
        QVERIFY(row.contains(QStringLiteral("directoryRowFocusRing")));
        QVERIFY(row.contains(QStringLiteral("visible: directoryRow.activeFocus")));
    }

    void pickingADmUserMovesFocusToAShownControl()
    {
        const QString src = read(QStringLiteral("NewConversationDialog.qml"));
        const int at = src.indexOf(QStringLiteral("onUserSelected:"));
        QVERIFY(at >= 0);
        const QString h = src.mid(at, 2400);
        QVERIFY(h.contains(QStringLiteral("root.contentItem.forceActiveFocus")));
        const int cb = h.indexOf(QStringLiteral("Qt.callLater"));
        QVERIFY(cb >= 0);
        const QString cl = h.mid(cb, 300);
        QVERIFY2(!cl.contains(QStringLiteral("dmChangeUserButton.forceActiveFocus"))
                 && !cl.contains(QStringLiteral("dmStartButton.forceActiveFocus"))
                 && !cl.contains(QStringLiteral("dmStartAnywayButton.forceActiveFocus")),
                 "focus must not land on a side-effecting control");
        QVERIFY(src.contains(QStringLiteral("id: dmStartButton")));
        QVERIFY(src.contains(QStringLiteral("id: dmStartAnywayButton")));
    }

    void theKnockFlagIsOnlyRaisedWhenAKnockCanStart()
    {
        const QString src = read(QStringLiteral("DiscoverJoinDialog.qml"));
        const QString fn = block(src, QStringLiteral("function startKnock("),
                                 QStringLiteral("\n    }"));
        QVERIFY(!fn.isEmpty());
        QVERIFY(fn.contains(QStringLiteral("!app.discovery.supported")));
        QVERIFY(fn.contains(QStringLiteral("app.discovery.busy")));
        QVERIFY(fn.indexOf(QStringLiteral("return false"))
                < fn.indexOf(QStringLiteral("knockInFlight = true")));
        QVERIFY2(src.contains(QStringLiteral("id: knockGuard")),
                 "a bounded timer must release a flag whose answer never comes");
        QVERIFY(src.count(QStringLiteral("app.discovery.knock(")) == 1);
        QVERIFY(src.count(QStringLiteral("knockInFlight = true")) == 1);
    }

    void theCloseButtonsHaveATooltip()
    {
        for (const QString &name : {QStringLiteral("DiscoverJoinDialog.qml"),
                                    QStringLiteral("NewConversationDialog.qml")}) {
            const QString src = read(name);
            QVERIFY(!src.isEmpty());
            const QString body = block(src, QStringLiteral("iconName: \"close\""),
                                       QStringLiteral("onClicked: root.close()"));
            QVERIFY2(body.contains(QStringLiteral("ToolTip.visible: hovered")),
                     qPrintable(name + ": close X needs a hover tooltip"));
            QVERIFY(body.contains(QStringLiteral("ToolTip.text: Accessible.name")));
        }
    }

    void aFocusedBrowseRowScrollsIntoView()
    {
        const QString src = read(QStringLiteral("DiscoverJoinDialog.qml"));
        QVERIFY(src.contains(QStringLiteral("positionViewAtIndex(index, ListView.Contain)")));
    }
};

QTEST_APPLESS_MAIN(DiscoverJoinDialogContractTest)
#include "DiscoverJoinDialogContractTest.moc"
