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
    void bigEmojiContract()
    {
        const QString delegate = read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(delegate.contains("app.emojiCatalog.emojiOnlySequenceCount("));
        QVERIFY(delegate.contains("emojiOnlyCount"));
        QVERIFY(delegate.contains("bigEmoji"));
    }

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
            QVERIFY2(src.contains(QLatin1String(s.glyph)),
                     qPrintable(QStringLiteral("%1 no longer binds %2").arg(
                         QString::fromLatin1(s.file),
                         QString::fromLatin1(s.glyph))));
            QVERIFY2(src.contains(QLatin1String(
                         "font.family: app.emojiFontFamily")),
                     qPrintable(QStringLiteral("%1 draws an emoji without naming "
                                               "the resolved emoji face").arg(
                         QString::fromLatin1(s.file))));
            QVERIFY2(!src.contains(QLatin1String("font.families")),
                     qPrintable(QStringLiteral("%1 assigns font.families, which "
                                               "does not exist in QML").arg(
                         QString::fromLatin1(s.file))));
        }
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
        QVERIFY(!delegate.contains("EmojiPicker {"));
        QVERIFY(delegate.contains(
            "root.openReactionPickerFor(root.eventIdForActions()"));
        QVERIFY(delegate.contains(
            "root.timelineView.openReactionPicker(eventId, p, expanded === true)"));
        QCOMPARE(pane.count("app.composer.reactTo(targetEventId, emoji)"), 1);
        QCOMPARE(thread.count("app.thread.model.toggleReaction(targetEventId, emoji)"), 1);
        QVERIFY(pane.contains("sharedReactionPicker.targetEventId = eventId"));
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
