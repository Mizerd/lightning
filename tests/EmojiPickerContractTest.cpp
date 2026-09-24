// Source-scan contract for EmojiPicker: the Material-icon category rail, the
// MenuSectionLabel heading over the active bucket, the hover/focus footer
// preview, and no global skin-tone swatch (preferredTone is never consumed by
// rendering; the per-emoji tone popup is the real mechanism). Complements
// EmojiUiContractTest.cpp and re-checks the invariants it relies on.

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
    // Every press inside the picker must be consumed by it, or it also reaches
    // TapHandlers beneath (e.g. the message context menu). Modality does not
    // do this: QQuickPopup only blocks presses outside its own item. A
    // background sink would eat the picker's own clicks (a TapHandler grabs
    // without accepting), so each grid cell consumes its press with a
    // MouseArea and the background sink catches only the chrome.
    void everyPressInsideThePickerIsConsumedByThePicker()
    {
        const QString src = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("dim: false")));

        // The cell consumes its own press; a TapHandler here lets it through.
        const int cellIdx = src.indexOf(QStringLiteral("picker.choose(cell.emoji)"));
        QVERIFY2(cellIdx > 0, "the emoji cell's choose path is gone");
        const QString cellBlock = src.mid(qMax(0, cellIdx - 900), 1200);
        QVERIFY2(cellBlock.contains(QStringLiteral("MouseArea")),
                 "the emoji cell must consume its own press with a MouseArea "
                 "— a TapHandler grabs without accepting, so the press "
                 "reaches the message row underneath");
        QVERIFY2(cellBlock.contains(QStringLiteral("Qt.LeftButton | Qt.RightButton")),
                 "the cell must take BOTH buttons: right-click opens skin "
                 "tones, and an unhandled right press is the original leak");

        // The chrome keeps its all-buttons sink, below the content.
        QVERIFY2(src.contains(QStringLiteral("acceptedButtons: Qt.AllButtons")),
                 "the picker background must still sink presses on its chrome");

        // Modality is not the mechanism. Scoped to the root picker, before the
        // nested tone popup, which may keep its own modality.
        const int toneIdx = src.indexOf(QStringLiteral("id: tonePopup"));
        QVERIFY2(toneIdx > 0, "tonePopup is gone");
        const QString rootOnly = src.left(toneIdx);
        QVERIFY2(rootOnly.contains(QStringLiteral("modal: false")),
                 "the picker must not be modal — modality does not buy the "
                 "input barrier and it blocks scrolling the timeline behind "
                 "the open picker");
        QVERIFY(!rootOnly.contains(QStringLiteral("modal: true")));
    }

    void widthAndPaddingMatchSpec()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(!picker.isEmpty());
        // No fixed width: the picker takes a share of the available space.
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
        // The old Unicode-glyph category map is gone.
        QVERIFY(!picker.contains(QStringLiteral("◷"))); // ◷ (old "Recently Used")
        QVERIFY(!picker.contains(QStringLiteral("☆"))); // ☺/☆-family leftovers
        QVERIFY(picker.contains(QStringLiteral("_categoryIcons")));
        // Every category maps to a real Icon.qml glyph (checked against
        // EmojiCatalog::kCategories).
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
        // The only tone mechanism is the shared per-emoji popup
        // (openTonePopupFor / tonePopup), never a header control.
        QVERIFY(picker.contains(QStringLiteral("function openTonePopupFor(")));
        QVERIFY(picker.contains(QStringLiteral("id: tonePopup")));
        // No other control beside the search field sets preferredTone outside
        // the tone popup's Repeater.
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
        // The column count (8) is fixed, not the cell size: cells divide the
        // body width.
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor(width / 8)")));
        QVERIFY(picker.contains(QStringLiteral("cellHeight: AppTheme.emojiCellSize")));
        QVERIFY(picker.contains(QStringLiteral("font.pixelSize: AppTheme.emojiGlyphSize")));
        // The footer previews the hovered/focused cell.
        QVERIFY(picker.contains(QStringLiteral("property string previewEmoji")));
        QVERIFY(picker.contains(QStringLiteral("property string previewName")));
        QVERIFY(picker.contains(QStringLiteral("onHoveredChanged")));
        QVERIFY(picker.contains(QStringLiteral("onActiveFocusChanged")));
        // No fabricated :shortcode:; the catalogue TSV has no such column.
        QVERIFY(!picker.contains(QStringLiteral(":shortcode:")));
    }

    // Behaviours the layout changes must not disturb (also pinned in
    // EmojiUiContractTest).
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
        // recordUse happens before emojiChosen, so no listener sees an
        // unrecorded selection.
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
        // anchorPoint and the placement clamp live in the shared AnchoredPopup
        // base; a local placeInsideWindow()/anchorPoint would shadow its
        // bindings and restore place-once behaviour.
        QVERIFY(picker.contains(QStringLiteral("AnchoredPopup {")));
        QVERIFY(!picker.contains(QStringLiteral("function placeInsideWindow()")));
        QVERIFY(!picker.contains(QStringLiteral("property point anchorPoint")));
        QVERIFY(!picker.contains(QStringLiteral("parent: Overlay.overlay")));
    }

    // AnchoredPopup computes no absolute overlay position: the popup is
    // parented to its anchor and expressed in its coordinates, so Qt's popup
    // positioner (which follows the parent and all ancestors) keeps them rigid.
    void anchoredPopupIsParentedToItsAnchorNotPositionedOverIt()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        QVERIFY(!base.isEmpty());
        QVERIFY(base.contains(QStringLiteral("property Item anchorItem")));
        QVERIFY(base.contains(QStringLiteral(
            "parent: anchorItem ? anchorItem : overlayItem")));

        // No coordinate mapping, revision counter or deferred correction.
        QVERIFY(!base.contains(QStringLiteral("mapToItem")));
        QVERIFY(!base.contains(QStringLiteral("Qt.callLater")));
        QVERIFY(!base.contains(QStringLiteral("placementRevision")));

        // Bottom-right pinned: right edges flush, bottom one hairline gap above
        // the anchor.
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real anchoredX: anchorItem "
            "? Math.max(0, anchorItem.width - width) : 0")));
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real anchoredY: -height - anchorGap")));
        QVERIFY(base.contains(QStringLiteral("value: root.anchoredX")));
        QVERIFY(base.contains(QStringLiteral("value: root.anchoredY")));
        QVERIFY(base.contains(QStringLiteral("when: root.anchorItem !== null")));

        // Never wider than the anchor, nor taller than the room above it.
        QVERIFY(base.contains(QStringLiteral("return anchorItem.width")));
        QVERIFY(base.contains(QStringLiteral("room -= anchorItem.height + anchorGap")));
        QVERIFY(base.contains(QStringLiteral("var want = maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("var want = maxHeight * effectiveHeightFraction")));

        // A popup with no anchor item is placed once from its point and only
        // clamped afterwards.
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

    // Both overlay pickers resize by a corner grip, and the size survives a
    // restart.
    void pickersAreResizableAndRememberTheirSize()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        const QString grip = read(QStringLiteral(QML_DIR "/PopupResizeGrip.qml"));
        const QString emoji = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        const QString gif = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!base.isEmpty() && !grip.isEmpty());

        // No detach: with the bottom-right corner pinned, a bigger size grows
        // up and left and stays snapped to the composer.
        QVERIFY(!base.contains(QStringLiteral("detached")));
        QVERIFY(base.contains(QStringLiteral("function resizeTo(w, h)")));
        QVERIFY(base.contains(QStringLiteral("function endResize()")));
        QVERIFY(base.contains(QStringLiteral(
            "userWidthFraction = Math.max(0.08, Math.min(1, w / maxWidth))")));
        // Persists a share of the available space, so the picker tracks the
        // window and both pickers share one value.
        QVERIFY(base.contains(QStringLiteral("app.settings.setPickerShare(sizeSettingsKey")));
        QVERIFY(base.contains(QStringLiteral(
            "app.settings.pickerWidthShare(root.sizeSettingsKey) / 1000")));
        QVERIFY(base.contains(QStringLiteral("Math.round(userWidthFraction * 1000)")));

        // The grip is at the top-left, the only movable corner, so its
        // arithmetic is inverted: dragging away from the anchor grows the popup.
        QVERIFY(grip.contains(QStringLiteral("DragHandler {")));
        QVERIFY(grip.contains(QStringLiteral("target: null")));
        QVERIFY(!grip.contains(QStringLiteral("MouseArea")));
        QVERIFY(grip.contains(QStringLiteral(
            "grip.popup.resizeTo(grip.pressWidth - activeTranslation.x,")));
        QVERIFY(grip.contains(QStringLiteral(
            "grip.pressHeight - activeTranslation.y)")));
        QVERIFY(grip.contains(QStringLiteral("cursorShape: Qt.SizeFDiagCursor")));
        // A recognisable handle, legible beside the focused search field (whose
        // focus ring is drawn in that corner): its own inset fill and border,
        // lifting to the accent when engaged.
        QVERIFY(grip.contains(QStringLiteral(
            "readonly property bool engaged: gripHover.hovered || dragHandler.active")));
        // Nested quarter-arcs concentric with the panel's corner radius,
        // parallel to the border and inside the padding band, clear of the
        // search field's focus ring; no bordered button in the header.
        QVERIFY(grip.contains(QStringLiteral("property real arcCentre")));
        QVERIFY(grip.contains(QStringLiteral("property real outerRadius")));
        // Tangential segments, never a clipped ring: QtQuick.Shapes is not
        // linked and Canvas paints nothing offscreen (see StormNode.qml /
        // TrustCard.qml), and a clip has square edges.
        QVERIFY(!grip.contains(QStringLiteral("clip: true")));
        QVERIFY(grip.contains(QStringLiteral("Math.cos(angle)")));
        QVERIFY(grip.contains(QStringLiteral("Math.sin(angle)")));
        QVERIFY(grip.contains(QStringLiteral("rotation: angle * 180 / Math.PI + 90")));
        // Bolt colour at rest, not only on hover.
        QVERIFY(grip.contains(QStringLiteral("color: AppTheme.bolt")));
        QVERIFY(grip.contains(QStringLiteral("property real strokeWidth: 2.5")));

        // Both pickers mount it as a corner ornament, outside the header
        // layout, so it displaces nothing.
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

        // Every size is a share of the available space, including a dragged
        // one.
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxHeight * effectiveHeightFraction")));
        QVERIFY(!base.contains(QStringLiteral("property real userWidth:")));
        // Both pickers share one remembered value, coherent because it is a
        // share.
        const QString gifKey = QStringLiteral("sizeSettingsKey: \"picker\"");
        QVERIFY(gif.contains(gifKey));
        QVERIFY(emoji.contains(gifKey));
        {
        }
    }

    // Button-opened pickers anchor to an item, never a snapshotted point.
    void callersAnchorToTheItemNotASnapshottedPoint()
    {
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(!composer.isEmpty() && !thread.isEmpty());
        // Anchored to the composer card, not the button: parenting to the card
        // keeps it rigid on top of the card with a hairline gap.
        QVERIFY(composer.contains(QStringLiteral("emojiPicker.anchorItem = composerCard")));
        QVERIFY(composer.contains(QStringLiteral("gifPicker.anchorItem = composerCard")));
        QVERIFY(thread.contains(QStringLiteral(
            "threadGifPicker.anchorItem = threadMiniComposer")));
        // The thread button maps into overlay coordinates, not `panel`, which
        // would offset the picker by the thread panel's inset.
        QVERIFY(thread.contains(QStringLiteral(
            "threadEmojiPicker.anchorItem = threadMiniComposer")));
        QVERIFY(!thread.contains(QStringLiteral("threadEmojiPicker.anchorPoint")));
        // None of the four button-opened pickers snapshots a point. Reaction
        // pickers still do: they open at a point inside a scrolling row and are
        // only re-clamped on resize.
        QVERIFY(!composer.contains(QStringLiteral("gifPicker.anchorPoint")));
        QVERIFY(!composer.contains(QStringLiteral("emojiPicker.anchorPoint")));
        QVERIFY(!thread.contains(QStringLiteral("threadGifPicker.anchorPoint")));
    }
};
QTEST_MAIN(EmojiPickerContractTest)
#include "EmojiPickerContractTest.moc"
