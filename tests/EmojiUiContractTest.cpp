#include <QFile>
#include <QtTest>

class EmojiUiContractTest : public QObject
{
    Q_OBJECT
private:
    QString read(const QString &path) {
        QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(f.readAll());
    }
private Q_SLOTS:
    void pickerContract()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains("GridView"));
        QVERIFY(picker.contains("interval: 150"));
        QVERIFY(picker.contains("Popup.CloseOnEscape | Popup.CloseOnPressOutside"));
        QVERIFY(picker.contains("Accessible.name"));
        QVERIFY(picker.contains("variantsFor"));
        QVERIFY(!picker.contains("http://") && !picker.contains("https://"));
    }
    void integrationContract()
    {
        const QString delegate = read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        QVERIFY(!delegate.contains("reactionPalette"));
        QVERIFY(!delegate.contains("[\"👍\""));
        QCOMPARE(delegate.count("app.composer.reactTo(root.reactionEventId, emoji)"), 1);
        QVERIFY(delegate.contains(
            "root.reactionEventId = root.eventIdForActions()"));
        QVERIFY(delegate.contains("reactionPicker.open()"));
        QVERIFY(composer.contains("input.selectionStart"));
        QVERIFY(composer.contains("input.selectionEnd"));
        QVERIFY(composer.contains("input.remove(start, end)"));
        QVERIFY(composer.contains("input.insert(start, emoji)"));
        QVERIFY(composer.contains("input.cursorPosition = start + emoji.length"));
    }
};
QTEST_MAIN(EmojiUiContractTest)
#include "EmojiUiContractTest.moc"
