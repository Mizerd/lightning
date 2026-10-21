// C27 source contract for the forward and event-source dialogs. Each case
// names the defect it pins and flips on the pre-fix source.
//   F11  a room chosen and then filtered out stays chosen: the dialog must
//        say how many rooms Send will use.
//   F15  the selection dialog listed invites; joined rooms only.
//   F12  no-match copy, and rows that Tab can reach.
//   F13  the focused Send button scrolls its row into view.
//   F14  a shrunken filtered list returns to the top; middle-click scroll.
//   F06/F08  event source viewer: middle-click scroll and keyboard reach.

#include <QFile>
#include <QRegularExpression>
#include <QtTest/QtTest>

namespace {
QString read(const QString &relative)
{
    QFile file(QStringLiteral(QML_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}
QString withoutComments(QString s)
{
    s.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    return s;
}
}

class ForwardDialogsContractTest : public QObject
{
    Q_OBJECT
private slots:
    void selectionDialogListsJoinedRoomsOnly()
    {
        const QString src = withoutComments(read(QStringLiteral("ForwardSelectionDialog.qml")));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("required property string membership")),
                 "the delegate must receive the membership role");
        QVERIFY2(src.contains(QRegularExpression(
                     QStringLiteral("visible:\\s*!isSpace\\s*&&\\s*membership === \"joined\""))),
                 "invites and knocks are offered as forward targets");
    }
    void selectionDialogShowsWhatSendWillUse()
    {
        const QString src = withoutComments(read(QStringLiteral("ForwardSelectionDialog.qml")));
        QVERIFY2(src.contains(QStringLiteral("objectName: \"forwardChosenCount\"")),
                 "a hidden (filtered-out) choice is invisible but still sent");
        QVERIFY(src.contains(QStringLiteral("root.targets.length")));
        QVERIFY2(src.contains(QStringLiteral("objectName: \"forwardClearSelection\""))
                     && src.contains(QStringLiteral("onClicked: root.targets = []")),
                 "a filtered-out choice cannot be dropped without clearing the filter");
    }
    void selectionDialogEmptyStateAndTabStops()
    {
        const QString src = withoutComments(read(QStringLiteral("ForwardSelectionDialog.qml")));
        QVERIFY2(src.contains(QStringLiteral("objectName: \"forwardNoMatchLabel\"")),
                 "no empty-state copy when the search matches nothing");
        QVERIFY2(src.contains(QRegularExpression(QStringLiteral(
                     "forwardNoMatchLabel[^}]*destList\\.count > 0"))),
                 "empty-state flashes while the model is still populating");
        QVERIFY2(src.contains(QStringLiteral("activeFocusOnTab: visible")),
                 "room rows are not keyboard reachable");
    }
    void singleDialogScrollsFocusedRowIntoView()
    {
        const QString src = withoutComments(read(QStringLiteral("ForwardMessageDialog.qml")));
        QVERIFY2(src.contains(QRegularExpression(QStringLiteral(
                     "onActiveFocusChanged:[^}]*positionViewAtIndex\\(\\s*index,\\s*ListView\\.Contain"))),
                 "a Send button focused below the fold is not scrolled into view");
        QVERIFY2(src.contains(QRegularExpression(QStringLiteral(
                     "onSearchTextChanged:\\s*targetList\\.positionViewAtBeginning"))),
                 "a filtered list keeps its stale scroll offset");
        QVERIFY(src.contains(QStringLiteral("view: targetList")));
    }
    void eventSourceScrollsByMouseAndKeyboard()
    {
        const QString src = withoutComments(read(QStringLiteral("EventSourceDialog.qml")));
        QVERIFY2(src.contains(QStringLiteral("MiddleClickScroller")),
                 "the viewer ignores middle-click autoscroll");
        QVERIFY2(src.contains(QStringLiteral("activeFocusOnTab: true")),
                 "the viewer text is not in the Tab order");
        QVERIFY2(src.contains(QStringLiteral("onCursorRectangleChanged")),
                 "the viewer does not follow the caret, so keys scroll nothing");
    }
};

QTEST_APPLESS_MAIN(ForwardDialogsContractTest)
#include "ForwardDialogsContractTest.moc"
