// Thread panel input contracts (swarm C30), as source contracts because every
// one of these looked right in a read of the file and failed live:
//   * Escape: a window Shortcut (TimelinePane) owns Escape while a thread is
//     open and takes it before the focused item, so the composer's own Escape
//     branch (cancel the reply banner first) never ran unless the editor
//     accepts ShortcutOverride for Escape. Both editors, plain and rich.
//   * Tab leaves the composer forward instead of typing a tab character.
//   * The reply list has the middle-click autoscroller the room timeline has.
//   * Opening the Threads list moves the keyboard into it.
// Every sweep carries a found > 0 guard.

#include <QFile>
#include <QRegularExpression>
#include <QtTest/QtTest>

namespace {

QString panelSource()
{
    QFile file(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString out = QString::fromUtf8(file.readAll());
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    return out;
}

/// Brace-matched body starting at the first '{' after `marker` (searched from
/// `from`); empty when absent.
QString bodyAfter(const QString &s, const QString &marker, int from = 0)
{
    const int at = s.indexOf(marker, from);
    if (at < 0)
        return {};
    const int open = s.indexOf(QLatin1Char('{'), at);
    if (open < 0)
        return {};
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('{'))
            ++depth;
        else if (s[i] == QLatin1Char('}') && --depth == 0)
            return s.mid(open, i - open + 1);
    }
    return {};
}

} // namespace

class ThreadPanelKeyboardContractTest : public QObject
{
    Q_OBJECT
private slots:
    void threadEditorsShareHeightAndSendPolicy()
    {
        const QString s = panelSource();
        QVERIFY(s.contains(QStringLiteral("import \"ComposerBehavior.js\" as ComposerBehavior")));
        QVERIFY(s.contains(QStringLiteral("function returnShouldSend(modifiers)")));
        QCOMPARE(s.count(QStringLiteral("panel.returnShouldSend(event.modifiers)")), 2);
        QCOMPARE(s.count(QStringLiteral("panel.composerExpanded, AppTheme.scaled(140)")), 2);
        QVERIFY(s.contains(QStringLiteral("objectName: \"threadComposerHeightButton\"")));
    }

    void bothComposersClaimEscapeFromTheWindowShortcut()
    {
        const QString s = panelSource();
        QVERIFY(!s.isEmpty());
        int found = 0;
        for (const QString &editor : {QStringLiteral("id: threadRichInput"),
                                      QStringLiteral("id: threadComposerInput")}) {
            const int at = s.indexOf(editor);
            QVERIFY2(at >= 0, qPrintable(editor));
            const QString body = bodyAfter(s, QStringLiteral("Keys.onShortcutOverride"), at);
            QVERIFY2(body.contains(QStringLiteral("Qt.Key_Escape")),
                     qPrintable(editor + ": ShortcutOverride must accept Escape"));
            // Narrow claim: only while a reply banner or mention popup can be
            // dismissed, so the window's find/search/thread priority chain
            // still runs for a bare composer.
            QVERIFY2(body.contains(QStringLiteral("app.thread.inReply"))
                         && body.contains(QStringLiteral("threadMentionPopup.visible")),
                     qPrintable(editor + ": Escape claim must be conditional"));
            ++found;
        }
        QCOMPARE(found, 2);
    }

    void tabLeavesBothComposersForward()
    {
        const QString s = panelSource();
        QVERIFY(s.contains(QStringLiteral("function leaveComposerForward")));
        const QRegularExpression call(
            QStringLiteral("Qt\\.Key_Tab\\s*&&\\s*event\\.modifiers\\s*===\\s*Qt\\.NoModifier\\)\\s*\\{\\s*"
                           "panel\\.leaveComposerForward\\(\\s*(\\w+)\\)"));
        QSet<QString> editors;
        auto it = call.globalMatch(s);
        while (it.hasNext())
            editors.insert(it.next().captured(1));
        QVERIFY2(editors.contains(QStringLiteral("threadRichInput")), "rich editor");
        QVERIFY2(editors.contains(QStringLiteral("threadComposerInput")), "plain editor");
    }

    void pendingListFocusIsBounded()
    {
        const QString s = panelSource();
        QVERIFY2(s.contains(QRegularExpression(QStringLiteral(
                     "id:\\s*focusListWindow[\\s\\S]*?running:\\s*panel\\._focusListPending"))),
                 "a timer bounds the pending list-focus request");
        QVERIFY2(s.contains(QRegularExpression(QStringLiteral(
                     "!app\\.thread\\.listLoading\\s*&&\\s*threadListView\\.count\\s*===\\s*0\\)\\s*"
                     "panel\\._focusListPending\\s*=\\s*false"))),
                 "an empty finished load clears the request");
    }

    void replyListHasAnAutoscroller()
    {
        const QString s = panelSource();
        const QString body = bodyAfter(s, QStringLiteral("MiddleClickScroller {"));
        QVERIFY2(!body.isEmpty(), "no MiddleClickScroller in ThreadPanel");
        QVERIFY(body.contains(QRegularExpression(QStringLiteral("view:\\s*replyList"))));
        QVERIFY(body.contains(QStringLiteral("app.threadScroll.cancel()")));
        QVERIFY(body.contains(QStringLiteral("afterWheelSettled()")));
    }

    void openingTheListFocusesIt()
    {
        const QString s = panelSource();
        const QString handler = bodyAfter(s, QStringLiteral("function onListStateChanged"));
        QVERIFY2(!handler.isEmpty(), "no listStateChanged handler");
        QVERIFY(handler.contains(QStringLiteral("_focusListPending")));
        QVERIFY(handler.contains(QStringLiteral("focusFirstThreadRow")));
        // Thread -> list (back arrow) changes `active` through stateChanged.
        const QString back = bodyAfter(s, QStringLiteral("function onStateChanged"),
                                       s.indexOf(QStringLiteral("function onListStateChanged")));
        QVERIFY2(back.contains(QStringLiteral("_focusListPending")),
                 "back to the list must request focus again");
        const QString fn = bodyAfter(s, QStringLiteral("function focusFirstThreadRow"));
        QVERIFY(fn.contains(QStringLiteral("forceActiveFocus")));
        // The request survives an empty list: it is cleared only once a row
        // exists, and the list's count change retries it.
        QVERIFY(fn.contains(QStringLiteral("if (first)")));
        const int view = s.indexOf(QStringLiteral("id: threadListView"));
        QVERIFY(view >= 0);
        const int cc = s.indexOf(QStringLiteral("onCountChanged"), view);
        QVERIFY2(cc >= 0 && cc - view < 600
                     && s.mid(cc, 200).contains(QStringLiteral("focusFirstThreadRow")),
                 "threadListView must retry focus when rows arrive");
    }
};

QTEST_APPLESS_MAIN(ThreadPanelKeyboardContractTest)
#include "ThreadPanelKeyboardContractTest.moc"
