
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

QString between(const QString &s, const QString &start, const QString &end)
{
    const int a = s.indexOf(start);
    if (a < 0)
        return {};
    const int b = s.indexOf(end, a + start.size());
    return b < 0 ? s.mid(a) : s.mid(a, b - a);
}

}

class SpaceSettingsKeyboardContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sourceIsReadable()
    {
        QVERIFY2(dialogSource().size() > 1000, "SpaceSettingsDialog.qml unreadable");
    }

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

    void focusIsScrolledIntoView()
    {
        const QString s = dialogSource();
        QVERIFY(s.contains(QStringLiteral("function ensureVisibleInBody")));
        QVERIFY(s.contains(QStringLiteral("function onActiveFocusItemChanged")));
        const QString fn = between(s, QStringLiteral("function ensureVisibleInBody"),
                                   QStringLiteral("onSectionChanged"));
        QVERIFY(!fn.isEmpty());
        QVERIFY(fn.contains(QStringLiteral("f.contentX =")));
        QVERIFY(fn.contains(QStringLiteral("f.contentWidth - f.width")));
    }

    void enterCommitsFields()
    {
        const QString s = dialogSource();
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

    void everyTabOpensAtItsTop()
    {
        const QString s = dialogSource();
        QVERIFY(s.contains(QRegularExpression(
            QStringLiteral(R"(onSectionChanged:\s*bodyFlick\.contentY\s*=\s*0)"))));
        QVERIFY(between(s, QStringLiteral("function openFor"), QStringLiteral("open()\n    }"))
                    .contains(QStringLiteral("bodyFlick.contentY = 0")));
    }

    void middleClickAutoscrollIsWired()
    {
        const QString s = dialogSource();
        QVERIFY(s.contains(QStringLiteral("MiddleClickScroller {")));
        QVERIFY(s.contains(QStringLiteral("view: bodyFlick")));
    }

    void wideSectionsHaveAMinimumContentWidth()
    {
        const QString s = dialogSource();
        QVERIFY(!between(s, QStringLiteral("readonly property real minContentWidth"),
                         QStringLiteral("contentWidth:")).isEmpty());
        QVERIFY(s.contains(QStringLiteral("minContentWidth")));
        QVERIFY(s.contains(QStringLiteral("contentWidth: Math.max(availableWidth")));
    }

    void memberButtonOpensItsMenuFromTheMenuKey()
    {
        const QString b = between(dialogSource(), QStringLiteral("id: memberMenuButton"),
                                  QStringLiteral("// Role chip"));
        QVERIFY(!b.isEmpty());
        const QString blk = b;
        QVERIFY(blk.contains(QStringLiteral("Keys.onMenuPressed")));
        QVERIFY(blk.contains(QStringLiteral("Qt.Key_F10")));
    }

    void emptyMemberResultSaysSo()
    {
        QVERIFY(dialogSource().contains(QStringLiteral("spaceSettingsMembersEmpty")));
    }

    void nonJoinedMembersGetNoRoleChanges()
    {
        const QString s = dialogSource();
        QVERIFY(between(s, QStringLiteral("function openMemberMenu"), QStringLiteral("AppMenu {"))
                    .contains(QStringLiteral("notJoined")));
        QVERIFY(between(s, QStringLiteral("text: qsTr(\"Member roles\")"),
                        QStringLiteral("delegate: RowLayout"))
                    .contains(QStringLiteral("m.membership !== \"banned\"")));
    }

    void copyConfirms()
    {
        QVERIFY(dialogSource().contains(QStringLiteral("qsTr(\"Copied\")")));
    }
};

QTEST_APPLESS_MAIN(SpaceSettingsKeyboardContractTest)
#include "SpaceSettingsKeyboardContractTest.moc"
