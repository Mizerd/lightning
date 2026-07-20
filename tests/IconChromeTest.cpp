// v0.7 design shell: interface-chrome icon hygiene. Chrome glyphs come from
// the bundled Material Symbols Rounded subset through qml/Icon.qml; emoji
// belong only in user content and reactions. This test (a) rejects emoji
// codepoints used as chrome in the shell QML files, (b) checks every icon
// name referenced via `name: "..."` on Icon elements exists in Icon.qml's
// codepoint map, and (c) confirms the subset font ships in the repo.

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QtTest/QtTest>

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

bool containsEmoji(const QString &text, QString *found)
{
    // Chrome-relevant emoji blocks: Misc Symbols & Pictographs, Emoticons,
    // Transport, Supplemental Symbols, plus legacy misc symbols that render
    // as colour emoji (⚙☺★, etc. are allowed in comments only — strip
    // comments first at the call site).
    for (int i = 0; i < text.size(); ) {
        const uint cp = text.at(i).isHighSurrogate() && i + 1 < text.size()
            ? QChar::surrogateToUcs4(text.at(i), text.at(i + 1))
            : text.at(i).unicode();
        i += QChar::requiresSurrogates(cp) ? 2 : 1;
        if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF)
            || cp == 0x2B50 || cp == 0x2B55) {
            if (found)
                *found = QString::fromUcs4(&cp, 1);
            return true;
        }
    }
    return false;
}

QString stripCommentsAndTr(QString qml)
{
    // Remove comments and qsTr() literals: user-facing copy may legally
    // carry arbitrary text; chrome glyph properties may not.
    qml.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
    qml.remove(QRegularExpression(QStringLiteral("qsTr\\(\"[^\"]*\"\\)")));
    return qml;
}

} // namespace

class IconChromeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void chromeFilesCarryNoEmojiGlyphs()
    {
        // Shell surfaces whose glyphs must all be Icon-based. EmojiPicker
        // and the emoji catalogue legitimately contain emoji and are
        // excluded; message BODIES are model data, not QML literals.
        const QStringList files = {
            QStringLiteral("Main.qml"), QStringLiteral("MainScreen.qml"),
            QStringLiteral("SpacesRail.qml"), QStringLiteral("RoomsPanel.qml"),
            QStringLiteral("RoomDelegate.qml"),
            QStringLiteral("TimelinePane.qml"),
            QStringLiteral("MessageDelegate.qml"),
            QStringLiteral("MessageComposerBar.qml"),
            QStringLiteral("ThreadPanel.qml"),
            QStringLiteral("ThreadSummaryCard.qml"),
            QStringLiteral("AccountMenu.qml"), QStringLiteral("GifPicker.qml"),
            QStringLiteral("RoomInfoPanel.qml"),
            QStringLiteral("SettingsScreen.qml"),
            QStringLiteral("QuickSwitcher.qml"),
            QStringLiteral("LoginScreen.qml"),
        };
        for (const QString &name : files) {
            const QString content = stripCommentsAndTr(
                readAll(QStringLiteral(QML_DIR "/") + name));
            QVERIFY2(!content.isEmpty(), qPrintable(name));
            QString glyph;
            QVERIFY2(!containsEmoji(content, &glyph),
                     qPrintable(QStringLiteral("emoji '%1' used as chrome in %2")
                                    .arg(glyph, name)));
        }
    }

    void everyReferencedIconNameIsMapped()
    {
        const QString iconQml = readAll(QStringLiteral(QML_DIR "/Icon.qml"));
        QVERIFY(!iconQml.isEmpty());
        QSet<QString> mapped;
        QRegularExpression mapEntry(QStringLiteral("\"([a-z0-9_]+)\": \"\\\\u"));
        auto it = mapEntry.globalMatch(iconQml);
        while (it.hasNext())
            mapped.insert(it.next().captured(1));
        QVERIFY(mapped.size() >= 40);

        QDir qmlDir(QStringLiteral(QML_DIR));
        // Icon { name: "x" } and IconButton { iconName: "x" } must both
        // resolve in the codepoint map.
        const QRegularExpression use(
            QStringLiteral("(?:icon[nN]ame|name): \"([a-z0-9_]+)\""));
        const auto entries =
            qmlDir.entryList({QStringLiteral("*.qml")}, QDir::Files);
        for (const QString &name : entries) {
            if (name == QLatin1String("Icon.qml"))
                continue;
            const QString content =
                readAll(qmlDir.filePath(name));
            auto uses = use.globalMatch(content);
            while (uses.hasNext()) {
                const QString icon = uses.next().captured(1);
                QVERIFY2(mapped.contains(icon),
                         qPrintable(QStringLiteral("%1 references unmapped "
                                                   "icon '%2'")
                                        .arg(name, icon)));
            }
        }
    }

    void iconFontShipsInRepo()
    {
        QVERIFY(QFile::exists(QStringLiteral(
            SOURCE_DIR "/data/fonts/MaterialSymbolsRounded-subset.ttf")));
        QVERIFY(QFile::exists(QStringLiteral(
            SOURCE_DIR "/scripts/generate-icon-font.sh")));
    }
};

QTEST_GUILESS_MAIN(IconChromeTest)
#include "IconChromeTest.moc"
