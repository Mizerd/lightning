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

    // EVERY SURFACE THAT DRAWS AN EMOJI MUST NAME THE FACE.
    //
    // Qt's automatic per-character fallback is version-dependent, and the
    // difference is exactly the one that ships: measured with an identical
    // QPainter probe, the same fonts and the same string, Qt 6.8.2 (Debian's,
    // which the Linux AppImage bundles) drew U+1F600 with colouredPx=0 --
    // preferring a MONOCHROME font that claims the codepoint -- while Qt
    // 6.11.1 (the dev shell) drew colouredPx=2580. Naming the family gave
    // colouredPx=4400 on both. Left to fallback, emoji look right in a local
    // build and come out monochrome or tofu in the packaged one, which is how
    // it was reported against the 0.8.1 AppImage.
    //
    // Asserted per FILE rather than once over the tree: a single global
    // substring search stays green when one of the three surfaces loses it.
    void emojiSurfacesNameTheFace()
    {
        struct Surface { const char *file; const char *glyph; };
        const Surface surfaces[] = {
            { QML_DIR "/EmojiPicker.qml",        "cell.emoji" },
            { QML_DIR "/QuickReactionStrip.qml", "cell.emojiValue" },
            { QML_DIR "/MessageDelegate.qml",    "modelData.key" },
        };
        for (const Surface &s : surfaces) {
            const QString src = read(QString::fromLatin1(s.file));
            QVERIFY2(!src.isEmpty(),
                     qPrintable(QStringLiteral("could not read %1").arg(
                         QString::fromLatin1(s.file))));
            // The glyph binding must still be there, or this case is asserting
            // a family on a surface that no longer draws an emoji.
            QVERIFY2(src.contains(QLatin1String(s.glyph)),
                     qPrintable(QStringLiteral("%1 no longer binds %2").arg(
                         QString::fromLatin1(s.file),
                         QString::fromLatin1(s.glyph))));
            QVERIFY2(src.contains(QLatin1String("font.families: AppTheme.emojiFontFamilies")),
                     qPrintable(QStringLiteral("%1 draws an emoji without naming "
                                               "AppTheme.emojiFontFamilies").arg(
                         QString::fromLatin1(s.file))));
        }
        // And the token itself must exist and lead with a COLOUR face. A list
        // whose first entry is monochrome would satisfy every check above and
        // reintroduce the defect.
        const QString theme = read(QStringLiteral(QML_DIR "/AppTheme.qml"));
        QVERIFY(theme.contains(QLatin1String("emojiFontFamilies")));
        QVERIFY(theme.contains(QLatin1String("\"Noto Color Emoji\"")));
        QVERIFY(theme.contains(QLatin1String("\"Segoe UI Emoji\"")));
        QVERIFY(theme.contains(QLatin1String("\"Apple Color Emoji\"")));
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
