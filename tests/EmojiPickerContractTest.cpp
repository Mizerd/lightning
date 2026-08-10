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
        // v0.6.7: there is no fixed width any more. The picker takes a SHARE
        // of the space available to it, so it scales with the window instead
        // of sitting at 324 until the window got too small to hold it.
        QVERIFY(picker.contains(QStringLiteral("widthFraction:")));
        QVERIFY(picker.contains(QStringLiteral("heightFraction:")));
        QVERIFY(picker.contains(QStringLiteral("sizeSettingsKey: \"picker\"")));
        QVERIFY(!picker.contains(QStringLiteral("width: Math.min(324,")));
        QVERIFY(!picker.contains(QStringLiteral("defaultWidth:")));
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
        QVERIFY(base.contains(QStringLiteral("var want = maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("var want = maxHeight * effectiveHeightFraction")));

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
            "userWidthFraction = Math.max(0.08, Math.min(1, w / maxWidth))")));
        // Persists a SHARE of the available space, not a pixel size. That is
        // what makes the picker track the window, keeps a dragged size sensible
        // on another display, and lets both pickers remember ONE value.
        QVERIFY(base.contains(QStringLiteral("app.settings.setPickerShare(sizeSettingsKey")));
        QVERIFY(base.contains(QStringLiteral(
            "app.settings.pickerWidthShare(root.sizeSettingsKey) / 1000")));
        QVERIFY(base.contains(QStringLiteral("Math.round(userWidthFraction * 1000)")));

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
        // Unmistakably a HANDLE, and legible next to a focused search field.
        // Three attempts failed first: 45% opacity was invisible, a bracket
        // plus dots read as a placeholder arrow, and diagonal strokes seated in
        // the corner were swallowed by the search field's bolt focus ring —
        // which is drawn there because the field takes focus as the picker
        // opens. It now carries its own inset fill and border and lifts to the
        // accent when engaged.
        QVERIFY(grip.contains(QStringLiteral(
            "readonly property bool engaged: gripHover.hovered || dragHandler.active")));
        // Nested QUARTER-ARCS concentric with the panel's own corner radius,
        // so they run parallel to the border rather than cutting across it —
        // and, being inside the padding band, out of reach of the search
        // field's focus ring. No chip, no border box: attempt 4 was a bordered
        // button in the header row, which was legible but clunky and stole
        // layout space the header had none of.
        QVERIFY(grip.contains(QStringLiteral("property real arcCentre")));
        QVERIFY(grip.contains(QStringLiteral("property real outerRadius")));
        // Tangential SEGMENTS, never a clipped ring. QtQuick.Shapes is not
        // linked in this application and Canvas paints nothing offscreen (see
        // StormNode.qml / TrustCard.qml), so a ring could only be drawn by
        // clipping a rounded Rectangle — and a clip has square edges, which is
        // what sliced the previous arcs off flat.
        QVERIFY(!grip.contains(QStringLiteral("clip: true")));
        QVERIFY(grip.contains(QStringLiteral("Math.cos(angle)")));
        QVERIFY(grip.contains(QStringLiteral("Math.sin(angle)")));
        QVERIFY(grip.contains(QStringLiteral("rotation: angle * 180 / Math.PI + 90")));
        // Bolt, at rest — not only on hover.
        QVERIFY(grip.contains(QStringLiteral("color: AppTheme.bolt")));
        QVERIFY(grip.contains(QStringLiteral("property real strokeWidth: 2.5")));

        // Both pickers mount one as a CORNER ORNAMENT, never a member of the
        // header layout: it must displace nothing, which is what made the
        // header misalign in two earlier attempts.
        for (const QString &picker : { emoji, gif }) {
            const int at = picker.indexOf(QStringLiteral("PopupResizeGrip {"));
            QVERIFY(at >= 0);
            const QString block = picker.mid(at, 520);
            QVERIFY(block.contains(QStringLiteral("anchors.left: parent.left")));
            QVERIFY(block.contains(QStringLiteral("anchors.top: parent.top")));
            QVERIFY(block.contains(QStringLiteral("arcCentre:")));
            QVERIFY(block.contains(QStringLiteral("outerRadius:")));
            QVERIFY(!block.contains(QStringLiteral("Layout.alignment")));
        }
        QVERIFY(!gif.contains(QStringLiteral("Layout.leftMargin: 14")));

        // v0.6.7: EVERY size is a share of the available space, so the picker
        // scales continuously with the window — a dragged size included. It
        // used to sit at one fixed size until the window got too small for it.
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxHeight * effectiveHeightFraction")));
        QVERIFY(!base.contains(QStringLiteral("property real userWidth:")));
        // Both pickers share ONE remembered value, so resizing either resizes
        // the other — only coherent because the value is a share.
        const QString gifKey = QStringLiteral("sizeSettingsKey: \"picker\"");
        QVERIFY(gif.contains(gifKey));
        QVERIFY(emoji.contains(gifKey));
        {
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
