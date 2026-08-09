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
        // v0.6.7: anchorPoint + the placeInsideWindow clamp moved into the
        // shared AnchoredPopup base, which BOTH overlay pickers now root on,
        // so a window resize re-anchors instead of leaving the popup behind.
        // Each picker must therefore no longer carry its own copy — a
        // leftover local placeInsideWindow()/anchorPoint would shadow the
        // base's and silently restore the place-once behaviour.
        QVERIFY(picker.contains(QStringLiteral("AnchoredPopup {")));
        QVERIFY(!picker.contains(QStringLiteral("function placeInsideWindow()")));
        QVERIFY(!picker.contains(QStringLiteral("property point anchorPoint")));
        QVERIFY(!picker.contains(QStringLiteral("parent: Overlay.overlay")));
    }

    // The shared base is where the actual fix lives, so pin the two things
    // that make it work at all.
    void anchoredPopupReanchorsOnEveryReflow()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        QVERIFY(!base.isEmpty());
        QVERIFY(base.contains(QStringLiteral("property Item anchorItem")));
        QVERIFY(base.contains(QStringLiteral("function reanchor()")));
        QVERIFY(base.contains(QStringLiteral(
            "anchorPoint = anchorItem.mapToItem(parent, anchorItem.width / 2, 0)")));

        // (1) Every trigger is a Connections object, so the base's placement
        // runs AFTER a subclass's own about-to-show handler (GifPicker.qml
        // sets `tab` there, and placement must read the resulting geometry)
        // and cannot be displaced by a subclass assigning the same property.
        //
        // v0.6.7 review (L1): an earlier version of this case justified the
        // rule with "a derived handler OVERRIDES the base's, so an inline
        // handler would never run". That premise is false on Qt 6.11.1 — both
        // run, base-inline then derived-inline then base-Connections — so
        // this is an ORDERING pin, not a correctness one, and it no longer
        // forbids the inline form on a claim that does not hold.
        QVERIFY(base.contains(QStringLiteral("function onAboutToShow() { root.reanchor() }")));

        // (2) Reflow triggers are deferred/coalesced: a resize moves the
        // overlay before the layouts underneath have repositioned the anchor,
        // so an inline recompute would read the anchor's PREVIOUS position.
        QVERIFY(base.contains(QStringLiteral("Qt.callLater(root.reflow)")));
        QVERIFY(base.contains(QStringLiteral(
            "target: root.visible ? root.parent : null")));
        QVERIFY(base.contains(QStringLiteral(
            "target: root.visible ? root.anchorItem : null")));

        // (3) v0.6.7 review (L4): a deferred call can land after the popup
        // closed, so visibility is re-checked at CALL time, not only when the
        // call was scheduled.
        QVERIFY(base.contains(QStringLiteral(
            "function reanchor() {\n"
            "        if (!visible || !parent)")));
        QVERIFY(base.contains(QStringLiteral(
            "function reflow() {\n"
            "        if (!visible || !parent)")));

        // (4) v0.6.7 review (L6): a popup with NO anchor item is only
        // re-clamped into bounds on a reflow, never fully re-placed — a
        // reaction popover pinned to a message row must not slide back toward
        // a stale point when the window grows.
        const int reflowAt = base.indexOf(QStringLiteral("function reflow()"));
        QVERIFY(reflowAt >= 0);
        const int scheduleAt =
            base.indexOf(QStringLiteral("function scheduleReflow()"), reflowAt);
        QVERIFY(scheduleAt > reflowAt);
        const QString body = base.mid(reflowAt, scheduleAt - reflowAt);
        QVERIFY(body.contains(QStringLiteral("if (anchorItem) {")));
        QVERIFY(body.contains(QStringLiteral("Math.min(x, parent.width - width")));
        QVERIFY(body.contains(QStringLiteral("Math.min(y, parent.height - height")));
    }

    // Both overlay pickers are opened from a real button, so both must hand
    // over the ITEM. Handing over a snapshotted point is exactly the bug.
    void callersAnchorToTheItemNotASnapshottedPoint()
    {
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(!composer.isEmpty() && !thread.isEmpty());
        QVERIFY(composer.contains(QStringLiteral("emojiPicker.anchorItem = emojiButton")));
        QVERIFY(composer.contains(QStringLiteral("gifPicker.anchorItem = gifButton")));
        QVERIFY(thread.contains(QStringLiteral(
            "threadGifPicker.anchorItem = threadGifButton")));
        // The thread emoji button additionally had a coordinate-space bug: it
        // mapped into `panel` while the anchor is interpreted in OVERLAY
        // coordinates, placing the picker as far left of the button as the
        // 340px thread panel is inset from the window's left edge.
        QVERIFY(thread.contains(QStringLiteral(
            "threadEmojiPicker.anchorItem = threadEmojiButton")));
        QVERIFY(!thread.contains(QStringLiteral("threadEmojiPicker.anchorPoint")));
        // None of the four button-opened pickers may snapshot a point any
        // more. (The reaction pickers legitimately still do: they open at a
        // point inside a scrolling message row, with no stable item to hold
        // — they get the clamp re-applied on resize, nothing more.)
        QVERIFY(!composer.contains(QStringLiteral("gifPicker.anchorPoint")));
        QVERIFY(!composer.contains(QStringLiteral("emojiPicker.anchorPoint")));
        QVERIFY(!thread.contains(QStringLiteral("threadGifPicker.anchorPoint")));
    }
};
QTEST_MAIN(EmojiPickerContractTest)
#include "EmojiPickerContractTest.moc"
