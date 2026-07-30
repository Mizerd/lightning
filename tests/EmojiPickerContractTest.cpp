// v0.6.5 (SPEC §1m): source-scan contract for the redesigned EmojiPicker —
// width 324 with zero outer padding, the Material-icon category rail
// (replacing the previous Unicode-glyph ToolButton strip), the
// MenuSectionLabel heading over the single active bucket, the hover/focus
// footer preview state, and NO global skin-tone swatch (the persisted
// preferredTone is never consumed by rendering, so a global control would be
// presentational fiction — the shared per-emoji tone popup is the real
// mechanism). Complements EmojiUiContractTest.cpp, which this suite does not
// duplicate wholesale but re-checks the load-bearing invariants that this
// redesign must not have disturbed.

#include <QFile>
#include <QtTest>

namespace {
QString read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}
}

class EmojiPickerContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void widthAndPaddingMatchSpec()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY(picker.contains(QStringLiteral("width: Math.min(324,")));
        QVERIFY(picker.contains(QStringLiteral("padding: 0")));
    }

    void categoryRailIsIconBasedNotGlyphStrip()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        // The old Unicode-glyph-per-category map is gone.
        QVERIFY(!picker.contains(QStringLiteral("◷"))); // ◷ (old "Recently Used")
        QVERIFY(!picker.contains(QStringLiteral("☆"))); // ☺/☆-family leftovers
        QVERIFY(picker.contains(QStringLiteral("_categoryIcons")));
        // Every category maps to a real Icon.qml glyph (verified against the
        // exact EmojiCatalog::kCategories strings).
        const QList<QPair<QString, QString>> expected = {
            { QStringLiteral("Recently Used"), QStringLiteral("schedule") },
            { QStringLiteral("Smileys & Emotion"), QStringLiteral("mood") },
            { QStringLiteral("People & Body"), QStringLiteral("group") },
            { QStringLiteral("Animals & Nature"), QStringLiteral("pets") },
            { QStringLiteral("Food & Drink"), QStringLiteral("restaurant") },
            { QStringLiteral("Travel & Places"), QStringLiteral("flight") },
            { QStringLiteral("Activities"), QStringLiteral("sports_esports") },
            { QStringLiteral("Objects"), QStringLiteral("lightbulb") },
            { QStringLiteral("Symbols"), QStringLiteral("emoji_symbols") },
            { QStringLiteral("Flags"), QStringLiteral("flag") },
        };
        for (const auto &pair : expected) {
            const QString entry = QStringLiteral("\"%1\": \"%2\"")
                                       .arg(pair.first, pair.second);
            QVERIFY2(picker.contains(entry), qPrintable(entry));
        }
        QVERIFY(picker.contains(QStringLiteral("radius: AppTheme.radiusControl")));
    }

    void noGlobalSkinToneSwatch()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        // The ONE tone mechanism is the shared per-emoji popup
        // (openTonePopupFor / tonePopup), never a standalone header control.
        QVERIFY(picker.contains(QStringLiteral("function openTonePopupFor(")));
        QVERIFY(picker.contains(QStringLiteral("id: tonePopup")));
        // No second interactive control sits beside the search field setting
        // preferredTone outside the shared tone popup's own Repeater.
        const int searchStart = picker.indexOf(QStringLiteral("AppTextField {"));
        const int searchEnd = picker.indexOf(QStringLiteral("ScrollView {"), searchStart);
        QVERIFY(searchStart >= 0 && searchEnd > searchStart);
        const QString row1 = picker.mid(searchStart, searchEnd - searchStart);
        QVERIFY(!row1.contains(QStringLiteral("preferredTone")));
    }

    void sectionHeadingUsesMenuSectionLabel()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("MenuSectionLabel {")));
        QVERIFY(picker.contains(QStringLiteral("text: picker.sectionHeading")));
        QVERIFY(picker.contains(QStringLiteral("sectionHeading")));
    }

    void gridUsesDesignTokenCellsAndFooterPreviewsHoverAndFocus()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        // SPEC 1m fixes the COLUMN COUNT (8), not the cell size: cells
        // divide the body width so exactly 8 columns render at 324px.
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor(width / 8)")));
        QVERIFY(picker.contains(QStringLiteral("cellHeight: AppTheme.emojiCellSize")));
        QVERIFY(picker.contains(QStringLiteral("font.pixelSize: AppTheme.emojiGlyphSize")));
        // Footer previews the hovered/focused cell instead of only ever
        // showing the static hint.
        QVERIFY(picker.contains(QStringLiteral("property string previewEmoji")));
        QVERIFY(picker.contains(QStringLiteral("property string previewName")));
        QVERIFY(picker.contains(QStringLiteral("onHoveredChanged")));
        QVERIFY(picker.contains(QStringLiteral("onActiveFocusChanged")));
        // No fabricated :shortcode: — the catalogue's TSV has no such column.
        QVERIFY(!picker.contains(QStringLiteral(":shortcode:")));
    }

    // Re-pin (defensively, alongside EmojiUiContractTest) every behavior the
    // redesign must not have disturbed.
    void preservedInvariantsSurviveTheRedesign()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("GridView")));
        QVERIFY(picker.contains(QStringLiteral("interval: 150")));
        QVERIFY(picker.contains(
            QStringLiteral("Popup.CloseOnEscape | Popup.CloseOnPressOutside")));
        QVERIFY(picker.contains(QStringLiteral("Accessible.name")));
        QVERIFY(picker.contains(QStringLiteral("variantsFor")));
        QVERIFY(!picker.contains(QStringLiteral("http://"))
                && !picker.contains(QStringLiteral("https://")));
        // recordUse happens BEFORE the emojiChosen signal (never after —
        // never let a listener observe the selection before it is recorded).
        const int chooseStart = picker.indexOf(QStringLiteral("function choose(emoji)"));
        const int recordAt = picker.indexOf(QStringLiteral("recordUse(emoji)"), chooseStart);
        const int emitAt = picker.indexOf(QStringLiteral("emojiChosen(emoji)"), chooseStart);
        QVERIFY(chooseStart >= 0 && recordAt > chooseStart && emitAt > recordAt);
        QVERIFY(picker.contains(QStringLiteral("if (closeAfterSelection) close()")));
        // Per-cell keyboard operability.
        QVERIFY(picker.contains(QStringLiteral("Keys.onReturnPressed: picker.choose(emoji)")));
        QVERIFY(picker.contains(QStringLiteral("Keys.onSpacePressed: picker.choose(emoji)")));
        QVERIFY(picker.contains(QStringLiteral("Keys.onMenuPressed: if (hasSkinTones) openVariants()")));
        // Two distinct empty states.
        QVERIFY(picker.contains(QStringLiteral("No recently used emoji")));
        QVERIFY(picker.contains(QStringLiteral("No emoji found")));
        // anchorPoint + placeInsideWindow clamping.
        QVERIFY(picker.contains(QStringLiteral("function placeInsideWindow()")));
        QVERIFY(picker.contains(QStringLiteral("property point anchorPoint")));
    }
};
QTEST_MAIN(EmojiPickerContractTest)
#include "EmojiPickerContractTest.moc"
