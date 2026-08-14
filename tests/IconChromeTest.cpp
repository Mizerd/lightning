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
#include <QtEndian>
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

// Minimal TTF cmap coverage reader (formats 4 and 12), big-endian per the
// OpenType spec. Returns every Unicode codepoint the font maps — enough to
// prove a mapped Icon.qml codepoint actually has a glyph, with no font
// database or Gui dependency.
QSet<uint> fontCoveredCodepoints(const QString &path)
{
    QSet<uint> covered;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return covered;
    const QByteArray data = file.readAll();
    const auto u16 = [&data](qsizetype off) -> quint16 {
        return qFromBigEndian<quint16>(
            reinterpret_cast<const uchar *>(data.constData()) + off);
    };
    const auto u32 = [&data](qsizetype off) -> quint32 {
        return qFromBigEndian<quint32>(
            reinterpret_cast<const uchar *>(data.constData()) + off);
    };
    if (data.size() < 12)
        return covered;
    const quint16 numTables = u16(4);
    qsizetype cmapOff = -1;
    for (quint16 i = 0; i < numTables; ++i) {
        const qsizetype rec = 12 + i * 16;
        if (rec + 16 > data.size())
            return covered;
        if (data.mid(rec, 4) == QByteArrayLiteral("cmap"))
            cmapOff = u32(rec + 8);
    }
    if (cmapOff < 0 || cmapOff + 4 > data.size())
        return covered;
    const quint16 numSubtables = u16(cmapOff + 2);
    for (quint16 i = 0; i < numSubtables; ++i) {
        const qsizetype rec = cmapOff + 4 + i * 8;
        if (rec + 8 > data.size())
            return covered;
        const qsizetype sub = cmapOff + u32(rec + 4);
        if (sub + 2 > data.size())
            continue;
        const quint16 format = u16(sub);
        if (format == 4) {
            const quint16 segCount = u16(sub + 6) / 2;
            const qsizetype endCodes = sub + 14;
            const qsizetype startCodes = endCodes + segCount * 2 + 2;
            if (startCodes + segCount * 2 > data.size())
                continue;
            for (quint16 s = 0; s < segCount; ++s) {
                const quint16 end = u16(endCodes + s * 2);
                const quint16 start = u16(startCodes + s * 2);
                for (uint cp = start; cp <= end && cp != 0xFFFF; ++cp)
                    covered.insert(cp);
            }
        } else if (format == 12) {
            if (sub + 16 > data.size())
                continue;
            const quint32 numGroups = u32(sub + 12);
            for (quint32 g = 0; g < numGroups; ++g) {
                const qsizetype group = sub + 16 + qsizetype(g) * 12;
                if (group + 12 > data.size())
                    break;
                const quint32 start = u32(group);
                const quint32 end = u32(group + 4);
                for (quint32 cp = start; cp <= end; ++cp)
                    covered.insert(cp);
            }
        }
    }
    return covered;
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

    void everyMappedIconIsInTheSubsetScript()
    {
        // v0.6.5: a name mapped in Icon.qml but absent from the subset
        // script's ICONS list renders blank — the subset font never carried
        // its glyph (this is exactly how account_circle shipped broken).
        // The script asserts its names against the upstream codepoints, so
        // map ⊆ script-list keeps map and font in step.
        const QString iconQml = readAll(QStringLiteral(QML_DIR "/Icon.qml"));
        QVERIFY(!iconQml.isEmpty());
        const QString script = readAll(QStringLiteral(
            SOURCE_DIR "/scripts/generate-icon-font.sh"));
        QVERIFY(!script.isEmpty());
        const QRegularExpression icons(
            QStringLiteral("ICONS=\"([^\"]+)\""));
        const auto match = icons.match(script);
        QVERIFY(match.hasMatch());
        const QStringList names = match.captured(1)
                .replace(QLatin1Char('\\'), QLatin1Char(' '))
                .split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        const QSet<QString> listed(names.begin(), names.end());
        QRegularExpression mapEntry(QStringLiteral("\"([a-z0-9_]+)\": \"\\\\u"));
        auto it = mapEntry.globalMatch(iconQml);
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            QVERIFY2(listed.contains(name),
                     qPrintable(QStringLiteral(
                         "Icon.qml maps '%1' but scripts/generate-icon-font.sh"
                         " does not subset it — the glyph would render blank")
                                    .arg(name)));
        }
    }

    void everyMappedCodepointHasAGlyphInTheSubsetFont()
    {
        // 2026-08-14 (review H1): the subset-script check above keeps NAMES
        // in step, but a wrong codepoint VALUE in Icon.qml still rendered
        // tofu — "block" shipped mapped to the legacy Material Icons
        // U+E14B while the Material Symbols subset carries U+F08C. Parse
        // the font's own cmap and require every mapped codepoint to
        // resolve.
        const QSet<uint> covered = fontCoveredCodepoints(QStringLiteral(
            SOURCE_DIR "/data/fonts/MaterialSymbolsRounded-subset.ttf"));
        QVERIFY(covered.size() >= 40);

        const QString iconQml = readAll(QStringLiteral(QML_DIR "/Icon.qml"));
        QVERIFY(!iconQml.isEmpty());
        const QRegularExpression mapEntry(QStringLiteral(
            "\"([a-z0-9_]+)\": \"\\\\u([0-9A-Fa-f]{4})\""));
        auto it = mapEntry.globalMatch(iconQml);
        int checked = 0;
        while (it.hasNext()) {
            const auto match = it.next();
            const uint cp = match.captured(2).toUInt(nullptr, 16);
            QVERIFY2(covered.contains(cp),
                     qPrintable(QStringLiteral(
                         "Icon.qml maps '%1' to U+%2 but the subset font has "
                         "no glyph there — it would render blank")
                         .arg(match.captured(1),
                              match.captured(2).toUpper())));
            ++checked;
        }
        QVERIFY(checked >= 40);
    }
};

QTEST_GUILESS_MAIN(IconChromeTest)
#include "IconChromeTest.moc"
