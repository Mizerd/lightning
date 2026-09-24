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
    // Big emoji: a body of only 1-3 user-perceived emoji renders large (60px,
    // 48px in compact/thread). The delegate asks the C++ catalogue for the
    // count (grapheme clusters plus catalogue lookup), never a QML regex or
    // code-point count.
    void bigEmojiContract()
    {
        const QString delegate = read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(delegate.contains("app.emojiCatalog.emojiOnlySequenceCount("));
        QVERIFY(delegate.contains("emojiOnlyCount"));
        QVERIFY(delegate.contains("bigEmoji"));
    }

    // Every surface that draws an emoji names the face. Qt's per-character
    // fallback is version-dependent: Qt 6.8 (bundled in the AppImage) picks a
    // monochrome face where 6.11 picks the colour one, while naming the family
    // gives colour on both. Asserted per file, so one surface losing it fails.
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
            // The glyph binding must still be there, or this asserts a family
            // on a surface that no longer draws emoji.
            QVERIFY2(src.contains(QLatin1String(s.glyph)),
                     qPrintable(QStringLiteral("%1 no longer binds %2").arg(
                         QString::fromLatin1(s.file),
                         QString::fromLatin1(s.glyph))));
            QVERIFY2(src.contains(QLatin1String(
                         "font.family: app.emojiFontFamily")),
                     qPrintable(QStringLiteral("%1 draws an emoji without naming "
                                               "the resolved emoji face").arg(
                         QString::fromLatin1(s.file))));
            // `font.families` is a C++ QFont API, not a QML font property;
            // assigning a list is a load-time error that a source scan is the
            // cheap guard against.
            QVERIFY2(!src.contains(QLatin1String("font.families")),
                     qPrintable(QStringLiteral("%1 assigns font.families, which "
                                               "does not exist in QML").arg(
                         QString::fromLatin1(s.file))));
        }
        // The candidate list lives in FontManager::emojiFamily() (which also
        // feeds the application font's fallback) and must lead with colour
        // faces. Path derived from QML_DIR.
        const QString catalog =
            read(QStringLiteral(QML_DIR "/../src/app/FontManager.cpp"));
        QVERIFY(!catalog.isEmpty());
        const int colour = catalog.indexOf(QLatin1String("Noto Color Emoji"));
        const int mono = catalog.indexOf(QLatin1String("\"Noto Emoji\""));
        QVERIFY(colour >= 0);
        QVERIFY(catalog.contains(QLatin1String("Segoe UI Emoji")));
        QVERIFY(catalog.contains(QLatin1String("Apple Color Emoji")));
        QVERIFY2(mono < 0 || colour < mono,
                 "the monochrome fallback must come after the colour faces");
    }

    // Texts that set `font.family` to a single face (room names, headers,
    // sender names, reply quotes, member rows) need the emoji face registered
    // as Qt's fallback at startup, before the QML engine exists.
    void startupRegistersTheEmojiFaceAsQtsFallback()
    {
        const QString mainSource =
            read(QStringLiteral(QML_DIR "/../src/main.cpp"));
        QVERIFY(!mainSource.isEmpty());
        const int install = mainSource.indexOf(QLatin1String(
            "FontManager::installEmojiFallback(FontManager::emojiFamily())"));
        QVERIFY2(install >= 0, "main.cpp never registers the emoji fallback");
        const int engine = mainSource.indexOf(QLatin1String("QQmlApplicationEngine engine"));
        QVERIFY2(engine >= 0 && install < engine,
                 "the emoji fallback must be registered before the QML engine");
    }

    void integrationContract()
    {
        const QString delegate = read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const QString pane = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        const QString thread = read(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(!delegate.contains("reactionPalette"));
        QVERIFY(!delegate.contains("[\"👍\""));
        // One shared reaction picker per view, never per row; the delegate
        // routes through the view with the event id captured at open.
        QVERIFY(!delegate.contains("EmojiPicker {"));
        QVERIFY(delegate.contains(
            "root.openReactionPickerFor(root.eventIdForActions()"));
        QVERIFY(delegate.contains(
            "root.timelineView.openReactionPicker(eventId, p)"));
        // The main timeline's picker uses the room composer: that pane is the
        // room.
        QCOMPARE(pane.count("app.composer.reactTo(targetEventId, emoji)"), 1);
        // The thread panel routes through its own model: app.composer's live
        // room timeline hides threaded events, so reactions there did nothing.
        QCOMPARE(thread.count("app.thread.model.toggleReaction(targetEventId, emoji)"), 1);
        QVERIFY(pane.contains("sharedReactionPicker.targetEventId = eventId"));
        // The composer's picker inserts into whichever editor owns the caret
        // (markdown TextArea or WYSIWYG), never the hidden one.
        QVERIFY(composer.contains("var editor = root.activeEditor()"));
        QVERIFY(composer.contains("return root.richMode ? richInput : input"));
        QVERIFY(composer.contains("editor.selectionStart"));
        QVERIFY(composer.contains("editor.selectionEnd"));
        QVERIFY(composer.contains("editor.remove(start, end)"));
        QVERIFY(composer.contains("editor.insert(start, emoji)"));
        QVERIFY(composer.contains("editor.cursorPosition = start + emoji.length"));
    }
};
QTEST_MAIN(EmojiUiContractTest)
#include "EmojiUiContractTest.moc"
