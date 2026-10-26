// Space settings dialog: keyboard, scroll and member-menu contracts from the
// swarm round 1 findings of chunk C11 (source contracts over
// qml/SpaceSettingsDialog.qml). Each case names the finding it pins and
// asserts something the previous source did not contain. The source directory
// is QML_DIR, or LIGHTNING_QML_DIR when set (a worktree override).
//
// A source scan cannot prove behaviour: the live keyboard paths are NOT
// TESTED here, only that the code that provides them exists.

#include <QFile>
#include <QRegularExpression>
#include <QtTest/QtTest>

namespace {

QString dialogSource()
{
    QString dir = qEnvironmentVariable("LIGHTNING_QML_DIR");
    if (dir.isEmpty())
        dir = QStringLiteral(QML_DIR);
    QFile f(dir + QStringLiteral("/SpaceSettingsDialog.qml"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// The text from `start` up to (not including) the next `end` after it.
QString between(const QString &s, const QString &start, const QString &end)
{
    const int a = s.indexOf(start);
    if (a < 0)
        return {};
    const int b = s.indexOf(end, a + start.size());
    return b < 0 ? s.mid(a) : s.mid(a, b - a);
}

} // namespace

class SpaceSettingsKeyboardContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sourceIsReadable()
    {
        QVERIFY2(dialogSource().size() > 1000, "SpaceSettingsDialog.qml unreadable");
    }

    // C11-F01: a visible:false item stays in the Menu's keyboard chain, so it
    // must be disabled as well.
    void hiddenModerationItemsAreDisabledToo()
    {
        const QString s = dialogSource();
        int checked = 0;
        for (const char *n : {"Kick", "Ban", "Unban"}) {
            const QString block =
                between(s, QStringLiteral("objectName: \"spaceMember%1\"").arg(n),
                        QStringLiteral("onTriggered"));
            QVERIFY2(block.contains(QStringLiteral("visible: memberMenu.can%1").arg(n)), n);
            QVERIFY2(block.contains(QStringLiteral("enabled: memberMenu.can%1").arg(n)),
                     qPrintable(QStringLiteral("%1 is hidden but still enabled").arg(n)));
            ++checked;
        }
        QCOMPARE(checked, 3);
    }

    // C11-F02
    void navRowsTakeFocusAndKeys()
    {
        const QString nav = between(dialogSource(), QStringLiteral("id: navRow"),
                                    QStringLiteral("RowLayout {"));
        QVERIFY(nav.contains(QStringLiteral("activeFocusOnTab: true")));
        QVERIFY(nav.contains(QStringLiteral("Keys.onReturnPressed")));
        QVERIFY(nav.contains(QStringLiteral("Keys.onSpacePressed")));
        QVERIFY(nav.contains(QStringLiteral("Keys.onDownPressed")));
        QVERIFY(nav.contains(QStringLiteral("Keys.onUpPressed")));
    }

    void pageKeysScrollTheBody()
    {
        const QString body = between(dialogSource(), QStringLiteral("id: bodyScroll"),
                                     QStringLiteral("ColumnLayout {"));
        for (const char *k : {"Key_PageDown", "Key_PageUp", "Key_Home", "Key_End"})
            QVERIFY2(body.contains(QLatin1String(k)), k);
    }

    // C11-F03
    void focusIsScrolledIntoView()
    {
        const QString s = dialogSource();
        QVERIFY(s.contains(QStringLiteral("function ensureVisibleInBody")));
        QVERIFY(s.contains(QStringLiteral("function onActiveFocusItemChanged")));
        // C11 r2: the horizontal axis is scrolled as well.
        const QString fn = between(s, QStringLiteral("function ensureVisibleInBody"),
                                   QStringLiteral("onSectionChanged"));
        QVERIFY(!fn.isEmpty());
        QVERIFY(fn.contains(QStringLiteral("f.contentX =")));
        QVERIFY(fn.contains(QStringLiteral("f.contentWidth - f.width")));
    }

    // C11-F04 and F05
    void enterCommitsFields()
    {
        const QString s = dialogSource();
        // Name and topic are SpaceTextEditRows since 2026-10-25: Return in
        // the field takes the row's own guarded submit (proved behaviourally
        // by space-text-edit-row-qml), shared with the inline Space Home card.
        for (const char *id : { "id: nameField", "id: topicField" }) {
            const int at = s.indexOf(QLatin1String(id));
            QVERIFY2(at > 0, id);
            QVERIFY2(s.lastIndexOf(QStringLiteral("SpaceTextEditRow {"), at)
                         > s.lastIndexOf(QStringLiteral("AppTextField {"), at),
                     id);
        }
        QString dir = qEnvironmentVariable("LIGHTNING_QML_DIR");
        if (dir.isEmpty())
            dir = QStringLiteral(QML_DIR);
        QFile row(dir + QStringLiteral("/SpaceTextEditRow.qml"));
        QVERIFY(row.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(row.readAll())
                    .contains(QStringLiteral("onAccepted: root.submit()")));
        QVERIFY(between(s, QStringLiteral("id: aliasField"), QStringLiteral("AppButton {"))
                    .contains(QStringLiteral("onAccepted")));
        QVERIFY(between(s, QStringLiteral("id: customLevel"), QStringLiteral("IconButton {"))
                    .contains(QStringLiteral("onAccepted")));
    }

    // C11-F06
    void everyTabOpensAtItsTop()
    {
        const QString s = dialogSource();
        QVERIFY(s.contains(QRegularExpression(
            QStringLiteral(R"(onSectionChanged:\s*bodyFlick\.contentY\s*=\s*0)"))));
        QVERIFY(between(s, QStringLiteral("function openFor"), QStringLiteral("open()\n    }"))
                    .contains(QStringLiteral("bodyFlick.contentY = 0")));
    }

    // C11-F07
    void middleClickAutoscrollIsWired()
    {
        const QString s = dialogSource();
        QVERIFY(s.contains(QStringLiteral("MiddleClickScroller {")));
        QVERIFY(s.contains(QStringLiteral("view: bodyFlick")));
    }

    // C11-F09 and F10: wide sections scroll sideways rather than clip.
    void wideSectionsHaveAMinimumContentWidth()
    {
        const QString s = dialogSource();
        QVERIFY(!between(s, QStringLiteral("readonly property real minContentWidth"),
                         QStringLiteral("contentWidth:")).isEmpty());
        QVERIFY(s.contains(QStringLiteral("minContentWidth")));
        QVERIFY(s.contains(QStringLiteral("contentWidth: Math.max(availableWidth")));
    }

    // C11-F12
    void memberButtonOpensItsMenuFromTheMenuKey()
    {
        const QString b = between(dialogSource(), QStringLiteral("id: memberMenuButton"),
                                  QStringLiteral("// Role chip"));
        QVERIFY(!b.isEmpty());
        const QString blk = b;
        QVERIFY(blk.contains(QStringLiteral("Keys.onMenuPressed")));
        QVERIFY(blk.contains(QStringLiteral("Qt.Key_F10")));
    }

    // C11-F13
    void emptyMemberResultSaysSo()
    {
        QVERIFY(dialogSource().contains(QStringLiteral("spaceSettingsMembersEmpty")));
    }

    // C11-F15
    void nonJoinedMembersGetNoRoleChanges()
    {
        const QString s = dialogSource();
        QVERIFY(between(s, QStringLiteral("function openMemberMenu"), QStringLiteral("AppMenu {"))
                    .contains(QStringLiteral("notJoined")));
        QVERIFY(between(s, QStringLiteral("text: qsTr(\"Member roles\")"),
                        QStringLiteral("delegate: RowLayout"))
                    .contains(QStringLiteral("m.membership !== \"banned\"")));
    }

    // C11-F11
    void copyConfirms()
    {
        QVERIFY(dialogSource().contains(QStringLiteral("qsTr(\"Copied\")")));
    }
};

QTEST_APPLESS_MAIN(SpaceSettingsKeyboardContractTest)
#include "SpaceSettingsKeyboardContractTest.moc"
