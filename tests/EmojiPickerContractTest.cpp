// Source-scan contract for EmojiPicker: the Material-icon category rail, the
// MenuSectionLabel heading over the active bucket, the hover/focus footer
// preview, and no global skin-tone swatch (preferredTone is never consumed by
// rendering; the per-emoji tone popup is the real mechanism). Complements
// EmojiUiContractTest.cpp and re-checks the invariants it relies on.

#include <QFile>
#include <QRegularExpression>
#include <QtTest>

namespace {
QString read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}
QString pickerSource()
{
    return read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
}
QString between(const QString &src, const QString &from, const QString &to)
{
    const int a = src.indexOf(from);
    if (a < 0)
        return {};
    const int b = src.indexOf(to, a + from.size());
    return b < 0 ? src.mid(a) : src.mid(a, b - a);
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

        // Modality is not the barrier inside the picker. Scoped to the root
        // picker, before the nested tone popup, which keeps its own modality.
        // The composer picker is not modal (it would stop the timeline
        // scrolling behind it); the reaction picker is, because it is placed
        // at a point in a scrolling row and a press outside it must only
        // dismiss it, never also drag the timeline.
        const int toneIdx = src.indexOf(QStringLiteral("id: tonePopup"));
        QVERIFY2(toneIdx > 0, "tonePopup is gone");
        const QString rootOnly = src.left(toneIdx);
        QVERIFY2(rootOnly.contains(QStringLiteral("modal: reactionMode")),
                 "only the reaction picker may be modal");
        QVERIFY(!rootOnly.contains(QStringLiteral("modal: true")));
        QVERIFY(!rootOnly.contains(QStringLiteral("modal: false")));

        // The sink also covers the resize grab band, which is inside the
        // popup but outside the panel.
        QVERIFY(src.contains(QStringLiteral(
            "anchors.leftMargin: -picker.leftInset")));
        QVERIFY(src.contains(QStringLiteral(
            "anchors.bottomMargin: -picker.bottomInset")));
    }

    void customReactionsUseAllPacksAndControlledMedia()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("app.stickers.reactionEmoticons()")));
        QVERIFY(!picker.contains(QStringLiteral("app.stickers.findEmoticons(")));
        QVERIFY(picker.contains(QStringLiteral("app.stickers.revision")));
        QVERIFY(picker.contains(QStringLiteral("app.stickers.refreshIfStale()")));
        QVERIFY(picker.contains(QStringLiteral("bridge.mxcImageSource(")));
        QVERIFY(picker.contains(QStringLiteral("picker.chooseCustom(customCell.modelData.url)")));
    }

    void widthAndPaddingMatchSpec()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(!picker.isEmpty());
        // No fixed width: the composer picker takes a share of the available
        // space; the reaction picker defaults compact, in pixels, and the
        // quick bar is content-sized.
        QVERIFY(picker.contains(QStringLiteral("widthFraction:")));
        QVERIFY(picker.contains(QStringLiteral("heightFraction:")));
        QVERIFY(picker.contains(QStringLiteral(
            "autoWidth: reactionMode ? 360 + gripBandX : 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "autoHeight: reactionMode ? 420 + gripBandY : 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "fixedWidth: compact && quickBar ? quickBar.implicitWidth : 0")));
        QVERIFY(!picker.contains(QStringLiteral("width: Math.min(324,")));
        QVERIFY(!picker.contains(QStringLiteral("defaultWidth:")));
        QVERIFY(picker.contains(QStringLiteral("padding: 0")));
    }

    // The two contexts remember separate sizes: a share of the composer card
    // means something else entirely as a share of the whole window, which is
    // how one shared key made the reaction picker a 1000x740 board.
    void reactionAndComposerContextsHaveSeparateSizeKeys()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "sizeSettingsKey: reactionMode ? \"reaction\" : \"picker\"")));
        QVERIFY(picker.contains(QStringLiteral(
            "readonly property bool reactionMode: mode === \"reaction\"")));
        // The quick bar, and the way out of it.
        QVERIFY(picker.contains(QStringLiteral("objectName: \"reactionQuickBar\"")));
        QVERIFY(picker.contains(QStringLiteral("function expand(initialText)")));
        // The context menu's "more" already offered the quick reactions.
        const QString delegate =
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.openReactionPickerFor(root.menuEventId, bubbleRow,\n"
            "                                               true)")));
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
        // The cell size is fixed, not the column count: as many columns as
        // emojiCellSize cells fit, sharing the width. A fixed 8 columns made
        // 125 px cells in a large picker.
        QVERIFY(picker.contains(QStringLiteral(
            "1, Math.floor(width / AppTheme.emojiCellSize))")));
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor(width / columns)")));
        QVERIFY(!picker.contains(QStringLiteral("cellWidth: Math.floor(width / 8)")));
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

    // B008: the tone popup is modal, so it must also OWN the keys. Without
    // `focus: true` and a focused first tile, Escape and typing went to a dead
    // item behind it and the picker could not be closed from the keyboard.
    void theTonePopupTakesAndReturnsKeyboardFocus()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        const int at = picker.indexOf(QStringLiteral("id: tonePopup"));
        QVERIFY(at > 0);
        const QString tone = picker.mid(at);
        QVERIFY2(tone.contains(QStringLiteral("focus: true")),
                 "a modal tone popup without focus traps the keyboard");
        const int opened = tone.indexOf(QStringLiteral("onOpened:"));
        const int closed = tone.indexOf(QStringLiteral("onClosed:"));
        QVERIFY(opened > 0 && closed > opened);
        QVERIFY2(tone.mid(opened, closed - opened).contains(
                     QStringLiteral("forceActiveFocus")),
                 "opening must focus the first tone tile");
        QVERIFY2(tone.mid(closed).contains(
                     QStringLiteral("search.forceActiveFocus()")),
                 "closing must hand focus back to the search field");
        // Tiles are driven by arrows and Enter, not by Tab alone.
        QVERIFY(tone.contains(QStringLiteral("Keys.onLeftPressed")));
        QVERIFY(tone.contains(QStringLiteral("Keys.onReturnPressed")));
    }

    // B070: Enter in the search field picks the first result, and the
    // "select" hint is advertised only while there is something to select.
    void enterInTheSearchFieldPicksTheFirstResult()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "Keys.onReturnPressed: picker.chooseFirstResult()")));
        QVERIFY(picker.contains(QStringLiteral(
            "Keys.onEnterPressed: picker.chooseFirstResult()")));
        const int fn = picker.indexOf(QStringLiteral("function chooseFirstResult()"));
        QVERIFY(fn > 0);
        const int end = picker.indexOf(QStringLiteral("function choose(emoji"), fn);
        QVERIFY(end > fn);
        // Code only: the function's own comment names the call it avoids.
        QString body;
        const QStringList bodyLines = picker.mid(fn, end - fn).split(QLatin1Char('\n'));
        for (const QString &line : bodyLines) {
            if (!line.trimmed().startsWith(QLatin1String("//")))
                body += line + QLatin1Char('\n');
        }
        // A pending debounced query is applied first, or Enter would choose
        // from the previous result list.
        QVERIFY(body.contains(QStringLiteral("searchTimer.running")));
        // Row 0 comes from the model: a reset leaves no delegate to read.
        QVERIFY2(!body.contains(QStringLiteral("itemAtIndex(")),
                 "a delegate does not exist yet right after the flushed reset");
        QVERIFY(body.contains(QStringLiteral("m.data(m.index(0, 0)")));
        QVERIFY(body.contains(QStringLiteral("Qt.UserRole + 1")));
        QVERIFY(body.contains(QStringLiteral("picker.choose(")));
        QVERIFY2(picker.contains(QStringLiteral("emojiGrid.count > 0")),
                 "the select hint must be gated on a target existing");
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
        QVERIFY(base.contains(QStringLiteral("? autoWidth : maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("? autoHeight : maxHeight * effectiveHeightFraction")));
        // A point-placed popup resizes from the corner away from its point.
        QVERIFY(base.contains(QStringLiteral(
            "anchorItem ? \"topLeft\" : (placedAbove ? \"topRight\" : \"bottomRight\")")));

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

        // The grip sits on the popup's movable corner, and moving that corner
        // away from the popup grows it: the sign follows the corner.
        QVERIFY(grip.contains(QStringLiteral("DragHandler {")));
        QVERIFY(grip.contains(QStringLiteral("target: null")));
        QVERIFY(!grip.contains(QStringLiteral("MouseArea")));
        QVERIFY(grip.contains(QStringLiteral(
            "var dx = grip.onRight ? activeTranslation.x : -activeTranslation.x")));
        QVERIFY(grip.contains(QStringLiteral(
            "var dy = grip.onBottom ? activeTranslation.y : -activeTranslation.y")));
        QVERIFY(grip.contains(QStringLiteral(
            "grip.popup.resizeTo(grip.pressWidth + dx, grip.pressHeight + dy)")));
        QVERIFY(grip.contains(QStringLiteral("grip.popup.beginResize()")));
        QVERIFY(grip.contains(QStringLiteral("Qt.SizeFDiagCursor")));
        QVERIFY(grip.contains(QStringLiteral("Qt.SizeBDiagCursor")));
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
        // layout, so it displaces nothing, and out over a grab band so a
        // press just outside the drawn corner still resizes.
        for (const QString &picker : { emoji, gif }) {
            const int at = picker.indexOf(QStringLiteral("PopupResizeGrip {"));
            QVERIFY(at >= 0);
            const QString block = picker.mid(at, 1100);
            QVERIFY(block.contains(QStringLiteral("-picker.leftPadding")));
            QVERIFY(block.contains(QStringLiteral("-picker.topPadding")));
            QVERIFY(block.contains(QStringLiteral("grabMargin: picker.gripGrab")));
            QVERIFY(block.contains(QStringLiteral("arcCentre:")));
            QVERIFY(block.contains(QStringLiteral("outerRadius:")));
            QVERIFY(!block.contains(QStringLiteral("Layout.alignment")));
        }
        // The emoji picker's grip follows the popup's free corner, placed by
        // x/y: toggling anchors between edges stretched it to full height.
        {
            const int at = emoji.indexOf(QStringLiteral("PopupResizeGrip {"));
            const QString block = emoji.mid(at, 1300);
            QVERIFY(block.contains(QStringLiteral("corner: picker.gripCorner")));
            QVERIFY(block.contains(QStringLiteral(
                "x: picker.gripOnRight ? parent.width - width + picker.rightPadding")));
            QVERIFY(block.contains(QStringLiteral(
                "y: picker.gripOnBottom ? parent.height - height + picker.bottomPadding")));
            QVERIFY(!block.contains(QStringLiteral("anchors.")));
        }
        QVERIFY(!gif.contains(QStringLiteral("Layout.leftMargin: 14")));

        // Every size is a share of the available space, including a dragged
        // one.
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxHeight * effectiveHeightFraction")));
        QVERIFY(!base.contains(QStringLiteral("property real userWidth:")));
        // The composer's two pickers share one remembered value, coherent
        // because both are a share of the composer card. The reaction picker
        // keeps its own (reactionAndComposerContextsHaveSeparateSizeKeys).
        QVERIFY(gif.contains(QStringLiteral("sizeSettingsKey: \"picker\"")));
        QVERIFY(emoji.contains(QStringLiteral(
            "sizeSettingsKey: reactionMode ? \"reaction\" : \"picker\"")));
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

    // ── C39 round 1: keyboard, wheel, autoscroll and reset contracts ──
    // Source scans over EmojiPicker.qml, each pinning one reported defect.

    // C39-F05: the grid is ONE tab stop. A per-cell stop made Tab walk every
    // emoji (160 in Smileys) before leaving.
    void theGridIsOneTabStopNotOnePerCell()
    {
        const QString src = pickerSource();
        const QString cell = between(src, QStringLiteral("id: cell"),
                                     QStringLiteral("Accessible.name: accessibleLabel"));
        QVERIFY2(!cell.isEmpty(), "the emoji cell delegate moved");
        QVERIFY2(!cell.contains(QStringLiteral("activeFocusOnTab: true")),
                 "a cell must not be a Tab stop");
        QVERIFY(cell.contains(QStringLiteral("activeFocusOnTab: false")));
    }

    // C39-F06: the cell's generic key handler accepts by default, which hid
    // Escape from the popup's CloseOnEscape. It must opt out first.
    void theCellKeyHandlerDoesNotSwallowEscape()
    {
        // F06 CAUSE UNVERIFIED: that Keys.onPressed accepts by default is a
        // hypothesis (Qt resets the event first). This only pins the opt-out.
        const QString src = pickerSource();
        const QRegularExpression re(QStringLiteral(
            "Keys\\.onPressed:\\s*\\(event\\)\\s*=>\\s*\\{\\s*event\\.accepted\\s*=\\s*false"));
        QVERIFY2(re.match(src).hasMatch(), "the cell's Keys.onPressed must start "
                                           "with event.accepted = false");
    }

    // C39-F07 and D06: Page/Home/End and Up off the first row.
    void theGridHandlesPageHomeEndAndUpFromRowZero()
    {
        const QString src = pickerSource();
        for (const char *key : {"Qt.Key_PageUp", "Qt.Key_PageDown", "Qt.Key_Home",
                                "Qt.Key_End", "Qt.Key_Up"}) {
            QVERIFY2(src.contains(QLatin1String("case ") + QLatin1String(key) + ':'),
                     key);
        }
        QVERIFY(src.contains(QStringLiteral("keyNavigationWraps: false")));
        QVERIFY(!src.contains(QStringLiteral("keyNavigationWraps: true")));
        // Up from row 0 lands in the search field.
        QVERIFY(between(src, QStringLiteral("case Qt.Key_Up:"),
                        QStringLiteral("case Qt.Key_Home:"))
                    .contains(QStringLiteral("search.forceActiveFocus()")));
    }

    // C39-F04: a right press on a cell with no skin tones is consumed and
    // does nothing; it must not fall through to choose().
    void rightClickOnAPlainEmojiDoesNotPick()
    {
        const QString src = pickerSource();
        QVERIFY2(QRegularExpression(QStringLiteral(
                     "mouse\\.button\\s*===\\s*Qt\\.RightButton\\)\\s*\\{\\s*if\\s*\\(cell\\.hasSkinTones"))
                     .match(src).hasMatch(),
                 "right-click must open tones or do nothing");
    }

    // C39-F01: middle-click autoscroll over the grid, like the timeline.
    void theGridHasMiddleClickAutoscroll()
    {
        const QString src = pickerSource();
        const QString block = between(src, QStringLiteral("MiddleClickScroller {"),
                                      QStringLiteral("}"));
        QVERIFY2(!block.isEmpty(), "no MiddleClickScroller in the picker");
        QVERIFY(block.contains(QStringLiteral("view: emojiGrid")));
    }

    // C39-F08: wheel over the heading and the footer scrolls the grid, and the
    // category rail scrolls horizontally for a narrow picker (D04).
    void wheelOutsideTheGridStillScrollsIt()
    {
        const QString src = pickerSource();
        QVERIFY2(src.count(QStringLiteral("scrollTarget: emojiGrid")) >= 2,
                 "heading and footer need a wheel handler targeting the grid");
        QVERIFY(src.contains(QStringLiteral("scrollTarget: railScroll.contentItem")));
    }

    // C39-D01: a new category or query starts at its first emoji.
    void aNewBucketStartsAtItsFirstEmoji()
    {
        const QString src = pickerSource();
        QVERIFY(src.contains(QStringLiteral("function resetToStart()")));
        QVERIFY(src.contains(QStringLiteral("positionViewAtBeginning()")));
        QVERIFY(src.contains(QStringLiteral("function onCategoryChanged()")));
        QVERIFY(src.contains(QStringLiteral("function onSearchTextChanged()")));
    }

    // C39-D05: no category is highlighted while a query is active.
    void theRailIsDeselectedWhileSearching()
    {
        const QString src = pickerSource();
        QVERIFY(QRegularExpression(QStringLiteral(
                    "search\\.text\\.length\\s*===\\s*0\\s*&&\\s*app\\.emojiCatalog\\.category\\s*===\\s*modelData"))
                    .match(src).hasMatch());
    }

    // C39-D07 and D09: the tone strip returns focus to its cell and names the
    // tone in the footer instead of a tooltip.
    void theToneStripReturnsFocusAndUsesTheFooter()
    {
        const QString src = pickerSource();
        const QString tone = src.mid(src.indexOf(QStringLiteral("id: tonePopup")));
        QVERIFY(tone.contains(QStringLiteral("origin.forceActiveFocus()")));
        QVERIFY(!tone.contains(QStringLiteral("ToolTip.text: modelData.name")));
        QVERIFY(tone.contains(QStringLiteral("picker.previewName = modelData.name")));
    }

    // C39-D08: a reaction picker opens on recents, not the last category.
    void aReactionPickerOpensOnRecents()
    {
        const QString src = pickerSource();
        const QString open = between(src, QStringLiteral("onAboutToShow: {"),
                                     QStringLiteral("background: Item {"));
        QVERIFY(open.contains(QStringLiteral("if (reactionMode)")));
        QVERIFY(open.contains(QStringLiteral("\"Recently Used\"")));
        // The MRU may hold only entries rebuild() drops: test the grid, not
        // recentEmoji.length, and fall back when it is empty.
        QVERIFY2(!open.contains(QStringLiteral("recentEmoji")),
                 "must not decide on the raw MRU length");
        QVERIFY(QRegularExpression(QStringLiteral(
                    "emojiCatalog\\.count\\s*===\\s*0")).match(open).hasMatch());
    }

    // C39 r2 (review D09 regression): closing the tone strip clears the
    // footer preview its tone tiles wrote.
    void closingTheToneStripClearsTheFooterPreview()
    {
        const QString src = pickerSource();
        const QString closed = between(src, QStringLiteral("onClosed: {"),
                                       QStringLiteral("background: Item {"));
        QVERIFY(closed.contains(QStringLiteral("picker.previewEmoji = \"\"")));
        QVERIFY(closed.contains(QStringLiteral("picker.previewName = \"\"")));
        QVERIFY(closed.contains(QStringLiteral("picker.previewCell = null")));
    }
};
QTEST_MAIN(EmojiPickerContractTest)
#include "EmojiPickerContractTest.moc"
