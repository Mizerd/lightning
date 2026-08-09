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
        // v0.6.7: 324 is now the DEFAULT width — AnchoredPopup clamps it to
        // the window and the corner grip can override it — not a hard size.
        QVERIFY(picker.contains(QStringLiteral("defaultWidth: 324")));
        QVERIFY(picker.contains(QStringLiteral("defaultHeight: 480")));
        QVERIFY(picker.contains(QStringLiteral("sizeSettingsKey: \"emoji\"")));
        QVERIFY(!picker.contains(QStringLiteral("width: Math.min(324,")));
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
        // v0.6.7: anchorPoint and the placement clamp moved into the shared
        // AnchoredPopup base, which BOTH overlay pickers now root on, so a
        // window resize re-anchors instead of leaving the popup behind. Each
        // picker must therefore no longer carry its own copy — a leftover
        // local placeInsideWindow()/anchorPoint would shadow the base's
        // bindings and silently restore the place-once behaviour.
        QVERIFY(picker.contains(QStringLiteral("AnchoredPopup {")));
        QVERIFY(!picker.contains(QStringLiteral("function placeInsideWindow()")));
        QVERIFY(!picker.contains(QStringLiteral("property point anchorPoint")));
        QVERIFY(!picker.contains(QStringLiteral("parent: Overlay.overlay")));
    }

    // The shared base is where the placement lives. v0.6.7, final form: it
    // does NOT compute an absolute overlay position at all. The popup is
    // PARENTED to its anchor and expressed in the anchor's coordinates, so
    // Qt's own popup positioner — which already listens to the parent and
    // every ancestor — keeps the two rigid. Three earlier schemes each broke a
    // different case (drift on resize, a frame of lag, then a ~400px error for
    // an anchor moved by an ancestor).
    void anchoredPopupIsParentedToItsAnchorNotPositionedOverIt()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        QVERIFY(!base.isEmpty());
        QVERIFY(base.contains(QStringLiteral("property Item anchorItem")));
        QVERIFY(base.contains(QStringLiteral(
            "parent: anchorItem ? anchorItem : overlayItem")));

        // The whole class of bug is gone because the mechanism is gone: no
        // coordinate mapping, no revision counter, no deferred correction.
        QVERIFY(!base.contains(QStringLiteral("mapToItem")));
        QVERIFY(!base.contains(QStringLiteral("Qt.callLater")));
        QVERIFY(!base.contains(QStringLiteral("placementRevision")));

        // Bottom-right pinned: right edges flush with the anchor, bottom one
        // hairline gap above it.
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real anchoredX: anchorItem "
            "? Math.max(0, anchorItem.width - width) : 0")));
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real anchoredY: -height - anchorGap")));
        QVERIFY(base.contains(QStringLiteral("value: root.anchoredX")));
        QVERIFY(base.contains(QStringLiteral("value: root.anchoredY")));
        QVERIFY(base.contains(QStringLiteral("when: root.anchorItem !== null")));

        // Never wider than the anchor — "on the right go no further than the
        // text box" — and never taller than the room above it.
        QVERIFY(base.contains(QStringLiteral("return anchorItem.width")));
        QVERIFY(base.contains(QStringLiteral("room -= anchorItem.height + anchorGap")));
        QVERIFY(base.contains(QStringLiteral("width: Math.min(maxWidth")));
        QVERIFY(base.contains(QStringLiteral("height: Math.min(maxHeight")));

        // A caller with no anchor item is placed once from its point and only
        // clamped afterwards — never re-placed toward a point that is stale by
        // then.
        const int clampAt = base.indexOf(QStringLiteral("function clampInsideWindow()"));
        QVERIFY(clampAt >= 0);
        const int endAt = base.indexOf(QStringLiteral("Connections {"), clampAt);
        QVERIFY(endAt > clampAt);
        const QString body = base.mid(clampAt, endAt - clampAt);
        QVERIFY(body.contains(QStringLiteral("if (!visible || !overlayItem || anchorItem)")));
        QVERIFY(body.contains(QStringLiteral("Math.min(x, overlayItem.width - width")));
        QVERIFY(base.contains(QStringLiteral("function placeAtPoint()")));
        QVERIFY(base.contains(QStringLiteral("if (!root.anchorItem)\n                root.placeAtPoint()")));
    }

    // v0.6.7: both overlay pickers are user-resizable by a corner grip, and
    // the size survives a restart.
    void pickersAreResizableAndRememberTheirSize()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        const QString grip = read(QStringLiteral(QML_DIR "/PopupResizeGrip.qml"));
        const QString emoji = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        const QString gif = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!base.isEmpty() && !grip.isEmpty());

        // No detach: the bindings pin the bottom-right corner, so a bigger
        // size grows the popup up and to the left by itself. That is what
        // keeps it snapped to the composer even mid-drag.
        QVERIFY(!base.contains(QStringLiteral("detached")));
        QVERIFY(base.contains(QStringLiteral("function resizeTo(w, h)")));
        QVERIFY(base.contains(QStringLiteral("function endResize()")));
        QVERIFY(base.contains(QStringLiteral(
            "userWidth = Math.max(minWidth, Math.min(w, maxWidth))")));
        // Persists the user's INTENT, not the clamped effective size —
        // resizing in a small window otherwise wrote a pair below the store's
        // sanity floor, which is treated as "forget it" and erased a size
        // chosen earlier on a bigger window.
        QVERIFY(base.contains(QStringLiteral(
            "Math.round(userWidth),\n"
            "                                       Math.round(userHeight))")));
        QVERIFY(base.contains(QStringLiteral("app.settings.setPickerSize(sizeSettingsKey")));
        QVERIFY(base.contains(QStringLiteral("app.settings.pickerWidth(root.sizeSettingsKey)")));

        // The grip sits at the TOP-LEFT — the only corner that can move — and
        // its arithmetic is inverted accordingly: dragging away from the
        // anchor grows the popup.
        QVERIFY(grip.contains(QStringLiteral("DragHandler {")));
        QVERIFY(grip.contains(QStringLiteral("target: null")));
        QVERIFY(!grip.contains(QStringLiteral("MouseArea")));
        QVERIFY(grip.contains(QStringLiteral(
            "grip.popup.resizeTo(grip.pressWidth - activeTranslation.x,")));
        QVERIFY(grip.contains(QStringLiteral(
            "grip.pressHeight - activeTranslation.y)")));
        QVERIFY(grip.contains(QStringLiteral("cursorShape: Qt.SizeFDiagCursor")));
        // Visible at rest: the first version faded to 45% and the maintainer
        // could not find it at all.
        QVERIFY(grip.contains(QStringLiteral(
            "opacity: gripHover.hovered || dragHandler.active ? 1 : 0.75")));

        // Both pickers mount one, anchored to their own top-left corner.
        for (const QString &picker : { emoji, gif }) {
            const int at = picker.indexOf(QStringLiteral("PopupResizeGrip {"));
            QVERIFY(at >= 0);
            const QString block = picker.mid(at, 160);
            QVERIFY(block.contains(QStringLiteral("anchors.left: parent.left")));
            QVERIFY(block.contains(QStringLiteral("anchors.top: parent.top")));
            QVERIFY(!block.contains(QStringLiteral("anchors.bottom")));
        }
    }

    // Both overlay pickers are opened from a real button, so both must hand
    // over the ITEM. Handing over a snapshotted point is exactly the bug.
    void callersAnchorToTheItemNotASnapshottedPoint()
    {
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(!composer.isEmpty() && !thread.isEmpty());
        // v0.6.7: the COMPOSER CARD, not the button. The picker sits on top
        // of the card with a hairline gap, right edges flush, and being
        // parented to the card is what makes it rigid with respect to it.
        QVERIFY(composer.contains(QStringLiteral("emojiPicker.anchorItem = composerCard")));
        QVERIFY(composer.contains(QStringLiteral("gifPicker.anchorItem = composerCard")));
        QVERIFY(thread.contains(QStringLiteral(
            "threadGifPicker.anchorItem = threadMiniComposer")));
        // The thread emoji button additionally had a coordinate-space bug: it
        // mapped into `panel` while the anchor is interpreted in OVERLAY
        // coordinates, placing the picker as far left of the button as the
        // 340px thread panel is inset from the window's left edge.
        QVERIFY(thread.contains(QStringLiteral(
            "threadEmojiPicker.anchorItem = threadMiniComposer")));
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
