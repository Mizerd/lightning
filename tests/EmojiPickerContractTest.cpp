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

    // The shared base is where the actual fix lives, so pin what makes it work.
    void anchoredPopupTracksItsAnchorThroughBindings()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        QVERIFY(!base.isEmpty());
        QVERIFY(base.contains(QStringLiteral("property Item anchorItem")));

        // (1) v0.6.7 (reported): placement is a BINDING, never an assignment
        // scheduled through Qt.callLater. The deferred version lagged a full
        // event-loop turn behind the window edge AND fired only once per
        // trigger, so an anchor whose own layout settled afterwards left the
        // popup a few pixels off with nothing left to correct it. A binding
        // re-evaluates on every dependency change, in the same frame.
        // Placement itself is never DEFERRED — x/y are never assigned from a
        // callLater, which is what made the popup lag a frame behind the
        // window edge and then stop correcting.
        QVERIFY(!base.contains(QStringLiteral("Qt.callLater(root.reanchor)")));
        QVERIFY(!base.contains(QStringLiteral("Qt.callLater(root.reflow)")));
        // What IS deferred is a revision bump that makes the binding re-read.
        //
        // v0.6.7 review (H1): this is load-bearing, not belt-and-braces.
        // anchorX names anchorItem.x and parent.width, which misses an anchor
        // whose overlay position moved because an ANCESTOR moved — the thread
        // panel is a fixed-width item at the end of a RowLayout, so on a
        // window resize it slides by the full delta while threadEmojiButton.x
        // never changes. The binding then evaluates once, possibly before the
        // ancestor was repositioned, and nothing re-triggers it (measured 200px
        // off, permanently). The bump re-reads after layouts settle.
        QVERIFY(base.contains(QStringLiteral("Qt.callLater(root.settle)")));
        QVERIFY(base.contains(QStringLiteral("function settle()")));
        // The scene graph is also only valid from aboutToShow — mapToItem()
        // answers 0 before the items share a scene — so the same counter is
        // bumped there, or the binding would evaluate once at component
        // completion and never re-run.
        QVERIFY(base.contains(QStringLiteral("property int placementRevision: 0")));
        QVERIFY(base.contains(QStringLiteral("root.placementRevision++")));
        QVERIFY(base.contains(QStringLiteral("var rev = placementRevision")));
        QVERIFY(base.contains(QStringLiteral("readonly property real placedX")));
        QVERIFY(base.contains(QStringLiteral("readonly property real placedY")));
        QVERIFY(base.contains(QStringLiteral("value: root.placedX")));
        QVERIFY(base.contains(QStringLiteral("value: root.placedY")));

        // (2) mapToItem() registers no dependency on the geometry it walks, so
        // the anchor bindings must read those dependencies by name or they
        // would never re-evaluate — which is exactly the original bug.
        QVERIFY(base.contains(QStringLiteral(
            "var deps = parent.width + anchorItem.x + anchorItem.width")));
        QVERIFY(base.contains(QStringLiteral(
            "var deps = parent.height + anchorItem.y + anchorItem.height")));
        QVERIFY(base.contains(QStringLiteral(
            "anchorItem.mapToItem(parent, anchorItem.width / 2, 0).x")));

        // (3) The placement bindings disengage while detached (a user resize),
        // with RestoreNone so detaching leaves x/y where they are rather than
        // snapping back to an earlier value.
        QVERIFY(base.contains(QStringLiteral("when: !root.detached")));
        QVERIFY(base.contains(QStringLiteral("restoreMode: Binding.RestoreNone")));

        // (4) A popup the bindings are NOT driving — user-resized, or opened at
        // a bare point — still gets clamped back inside a shrinking window,
        // but is never re-placed, which would undo the position the user chose
        // or slide it toward a point that is already stale.
        //
        // v0.6.7 review (L1): the anchorItem test in the Binding `when` above
        // is what keeps the bare-point pickers (the reaction popovers) out of
        // the re-placement path; they take one placement at show instead.
        QVERIFY(base.contains(QStringLiteral(
            "when: !root.detached && root.anchorItem !== null")));
        QVERIFY(base.contains(QStringLiteral("if (!root.anchorItem) {")));
        const int clampAt = base.indexOf(QStringLiteral("function clampInsideWindow()"));
        QVERIFY(clampAt >= 0);
        const int endAt = base.indexOf(QStringLiteral("function settle()"), clampAt);
        QVERIFY(endAt > clampAt);
        const QString body = base.mid(clampAt, endAt - clampAt);
        QVERIFY(body.contains(QStringLiteral("if (anchorItem && !detached)")));
        QVERIFY(body.contains(QStringLiteral("Math.min(x, parent.width - width")));
        QVERIFY(body.contains(QStringLiteral("Math.min(y, parent.height - height")));
        QVERIFY(base.contains(QStringLiteral(
            "target: root.visible ? root.parent : null")));
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

        // A drag pins the top-left (detach) so the dragged corner tracks the
        // pointer, instead of the popup growing symmetrically about its anchor.
        QVERIFY(base.contains(QStringLiteral("function beginResize() { detached = true }")));
        QVERIFY(base.contains(QStringLiteral("function resizeTo(w, h)")));
        QVERIFY(base.contains(QStringLiteral("function endResize()")));
        // Never past the window edge it is growing toward, never below the
        // component's own minimum.
        QVERIFY(base.contains(QStringLiteral(
            "var maxW = parent.width - x - AppTheme.spacingS")));
        QVERIFY(base.contains(QStringLiteral(
            "userWidth = Math.max(minWidth, Math.min(w, maxW))")));
        // Persisted through the whitelisted settings pair, and re-read on each
        // open so a mid-session drag never fights the store.
        // v0.6.7 review (L3): persists the user's INTENT, not the clamped
        // effective size — resizing inside a very narrow window otherwise
        // wrote a pair below the store's sanity floor, which is treated as
        // "forget it" and erased a size chosen earlier on a bigger window.
        QVERIFY(base.contains(QStringLiteral(
            "Math.round(userWidth),\n"
            "                                       Math.round(userHeight))")));
        QVERIFY(base.contains(QStringLiteral("app.settings.setPickerSize(sizeSettingsKey")));
        QVERIFY(base.contains(QStringLiteral("app.settings.pickerWidth(root.sizeSettingsKey)")));
        QVERIFY(base.contains(QStringLiteral("root.detached = false")));

        // The grip is a DragHandler with target:null — never a MouseArea,
        // which could steal the wheel/drag gestures the content needs — and it
        // resolves every frame against the size at PRESS, so a dropped frame
        // cannot accumulate drift.
        QVERIFY(grip.contains(QStringLiteral("DragHandler {")));
        QVERIFY(grip.contains(QStringLiteral("target: null")));
        QVERIFY(!grip.contains(QStringLiteral("MouseArea")));
        QVERIFY(grip.contains(QStringLiteral(
            "grip.popup.resizeTo(grip.pressWidth + activeTranslation.x,")));
        QVERIFY(grip.contains(QStringLiteral("cursorShape: Qt.SizeFDiagCursor")));

        // Both pickers actually mount one.
        QVERIFY(emoji.contains(QStringLiteral("PopupResizeGrip {")));
        QVERIFY(gif.contains(QStringLiteral("PopupResizeGrip {")));
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
