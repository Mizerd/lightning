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
    // Big-emoji contract: a message whose body is only 1-3 user-perceived
    // emoji sequences renders large (one uniform size — 60px, 48px in
    // compact/thread — per maintainer preference). The delegate must ask
    // the C++ catalogue for the count (grapheme-cluster + catalogue lookup
    // — never a QML regex or code-point count).
    void bigEmojiContract()
    {
        const QString delegate = read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(delegate.contains("app.emojiCatalog.emojiOnlySequenceCount("));
        QVERIFY(delegate.contains("emojiOnlyCount"));
        QVERIFY(delegate.contains("bigEmoji"));
    }

    void integrationContract()
    {
        const QString delegate = read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const QString pane = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        const QString thread = read(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(!delegate.contains("reactionPalette"));
        QVERIFY(!delegate.contains("[\"👍\""));
        // v0.7: ONE shared reaction picker per view (never a per-row popup);
        // the delegate routes through the view with the event id captured
        // at open, and the shared picker applies the reaction to that
        // snapshotted target.
        QVERIFY(!delegate.contains("EmojiPicker {"));
        QVERIFY(delegate.contains(
            "root.openReactionPickerFor(root.eventIdForActions()"));
        QVERIFY(delegate.contains(
            "root.timelineView.openReactionPicker(eventId, p)"));
        QCOMPARE(pane.count("app.composer.reactTo(targetEventId, emoji)"), 1);
        QCOMPARE(thread.count("app.composer.reactTo(targetEventId, emoji)"), 1);
        QVERIFY(pane.contains("sharedReactionPicker.targetEventId = eventId"));
        QVERIFY(composer.contains("input.selectionStart"));
        QVERIFY(composer.contains("input.selectionEnd"));
        QVERIFY(composer.contains("input.remove(start, end)"));
        QVERIFY(composer.contains("input.insert(start, emoji)"));
        QVERIFY(composer.contains("input.cursorPosition = start + emoji.length"));
    }
};
QTEST_MAIN(EmojiUiContractTest)
#include "EmojiUiContractTest.moc"
