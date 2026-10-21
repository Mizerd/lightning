// C80 swarm round 1: source contracts for four defects found in the language
// sweep (Arabic RTL, Russian, Hindi). Each case fails on the tree before the
// fix: it asserts text that only the fixed source contains.

#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>

class RtlComposerContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

private slots:
    // Tab inside the message box inserted a tab character and focus never left.
    void tabInTheComposerMovesFocusInsteadOfTyping()
    {
        const QString src = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!src.isEmpty());
        // Both editors (plain and rich) move on with Tab (C37 owns the
        // composer's Tab; Shift+Tab leaves by Qt's own focus chain).
        QCOMPARE(src.count(QStringLiteral("richInput.nextItemInFocusChain(true)")), 1);
        QCOMPARE(src.count(QStringLiteral(" input.nextItemInFocusChain(true)")), 1);
        QCOMPARE(src.count(QStringLiteral("next.forceActiveFocus(Qt.TabFocusReason)")), 2);
        // No Tab handler may fall through to the default (a typed tab).
        int at = 0;
        int handlers = 0;
        while ((at = src.indexOf(QStringLiteral("Keys.onTabPressed"), at)) >= 0) {
            ++handlers;
            const QString body = src.mid(at, 700);
            QVERIFY2(!body.contains(QStringLiteral("event.accepted = false")),
                     "a Tab handler still ends in event.accepted = false, so "
                     "Tab types a tab character again");
            at += 10;
        }
        QCOMPARE(handlers, 2);
    }

    // The active bar and the paddings are physical; they must swap under RTL.
    void theSettingsOutlineBarFollowsTheReadingDirection()
    {
        const QString src = read(QStringLiteral("SettingsNavOutline.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("entryRow.mirrored")),
                 "the outline entry never consults mirrored: the active bar "
                 "stays at the left edge of a right-aligned row in Arabic");
        QVERIFY(src.contains(QStringLiteral("parent.width - width - (AppTheme.spacing12 - 1)")));
    }

    // An implicit left follows the text's direction; an explicit one mirrors.
    void roomListNamesAndPreviewsAlignWithTheLayoutDirection()
    {
        const QString src = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!src.isEmpty());
        const int preview = src.indexOf(QStringLiteral("objectName: \"roomPreviewLabel\""));
        QVERIFY(preview > 0);
        QVERIFY2(src.mid(preview, 1600).contains(
                     QStringLiteral("horizontalAlignment: Text.AlignLeft")),
                 "the preview label has no explicit alignment");
        const int name = src.indexOf(QStringLiteral("text: model.name"));
        QVERIFY(name > 0);
        QVERIFY2(src.mid(name, 2200).contains(
                     QStringLiteral("horizontalAlignment: Text.AlignLeft")),
                 "the room name label has no explicit alignment");
    }

    // "Message #room" with an LTR name in an RTL sentence needs isolates.
    void theRoomPlaceholderIsolatesTheRoomNameInRtl()
    {
        const QString src = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("qsTr(\"Message %1\").arg(root.isolatedRoomName())")));
        QVERIFY(src.contains(QStringLiteral("\"\\u2066\" + n + \"\\u2069\"")));
        QVERIFY(src.contains(QStringLiteral("app.localization.rightToLeft ?")));
    }
};

QTEST_MAIN(RtlComposerContractTest)
#include "RtlComposerContractTest.moc"
