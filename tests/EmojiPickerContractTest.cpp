
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
    void packImagesStayBoundedToVisibleCells()
    {
        const QString sticker = read(QStringLiteral(QML_DIR "/StickerPicker.qml"));
        const QString palette = pickerSource();
        QVERIFY(!sticker.isEmpty());
        QVERIFY(sticker.contains(QStringLiteral("id: grid")));
        QVERIFY(sticker.contains(QStringLiteral("GridView {")));
        QVERIFY(sticker.contains(QStringLiteral("model: picker.stickers.images")));
        QVERIFY(!sticker.contains(QStringLiteral("Repeater {")));
        const QString still = between(sticker, QStringLiteral("id: tileImage"),
                                      QStringLiteral("AnimatedImage {"));
        QVERIFY(still.contains(QStringLiteral("cache: false")));
        QVERIFY(still.contains(QStringLiteral("sourceSize.width:")));
        QVERIFY(still.contains(QStringLiteral("sourceSize.height:")));
        const QString animated = between(sticker, QStringLiteral("id: tileAnim"),
                                         QStringLiteral("Connections {"));
        QVERIFY(animated.contains(QStringLiteral("cache: false")));
        QVERIFY(animated.contains(QStringLiteral("sourceSize.width:")));
        const QString strip = between(palette, QStringLiteral("id: customEmojiStrip"),
                                      QStringLiteral("MenuSectionLabel {"));
        QVERIFY(palette.contains(QStringLiteral("picker.visible && !picker.compact")));
        QVERIFY(strip.contains(QStringLiteral("ListView.Horizontal")));
        QVERIFY(strip.contains(QStringLiteral("CustomEmojiImage {")));
        QVERIFY(strip.contains(QStringLiteral("edge: 28")));
        QVERIFY(!strip.contains(QStringLiteral("stillCache:")));
        const QString cell = read(QStringLiteral(QML_DIR "/CustomEmojiImage.qml"));
        QVERIFY(cell.contains(QStringLiteral("property bool stillCache: false")));
        QVERIFY(cell.contains(QStringLiteral("cache: emoji.stillCache")));
        QVERIFY(cell.contains(QStringLiteral("sourceSize.width: emoji.edge")));
    }

    void packTraceOmitsRemoteIdentifiers()
    {
        const QString manager = read(QStringLiteral(QML_DIR "/../src/stickers/StickerPackManager.cpp"));
        const QString provider = read(QStringLiteral(QML_DIR "/../src/media/MediaImageProvider.cpp"));
        QVERIFY(manager.contains(QStringLiteral("LIGHTNING_PACKS_TRACE")));
        QVERIFY(manager.contains(QStringLiteral("phase=snapshot")));
        QVERIFY(manager.contains(QStringLiteral("phase=reaction_rows")));
        QVERIFY(manager.contains(QStringLiteral("phase=completion_search")));
        QVERIFY(provider.contains(QStringLiteral("phase=image_decode")));
    }

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

        QVERIFY2(src.contains(QStringLiteral("acceptedButtons: Qt.AllButtons")),
                 "the picker background must still sink presses on its chrome");

        const int toneIdx = src.indexOf(QStringLiteral("id: tonePopup"));
        QVERIFY2(toneIdx > 0, "tonePopup is gone");
        const QString rootOnly = src.left(toneIdx);
        QVERIFY2(rootOnly.contains(QStringLiteral("modal: reactionMode")),
                 "only the reaction picker may be modal");
        QVERIFY(!rootOnly.contains(QStringLiteral("modal: true")));
        QVERIFY(!rootOnly.contains(QStringLiteral("modal: false")));

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
        QVERIFY(picker.contains(QStringLiteral("contentItem: CustomEmojiImage {")));
        QVERIFY(read(QStringLiteral(QML_DIR "/CustomEmojiImage.qml"))
                    .contains(QStringLiteral("bridge.mxcImageSource(")));
        QVERIFY(picker.contains(QStringLiteral("picker.chooseCustom(customCell.modelData.url,")));
        QVERIFY(picker.contains(QStringLiteral("readonly property bool customEmojiOffered: allowCustomEmoji && app.stickers.available")));
        QVERIFY(picker.contains(QStringLiteral("property bool allowCustomEmoji: true")));
        QVERIFY(picker.contains(QStringLiteral("customChosen(row)")));
    }

    void widthAndPaddingMatchSpec()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(!picker.isEmpty());
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

    void reactionAndComposerContextsHaveSeparateSizeKeys()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "sizeSettingsKey: reactionMode ? \"reaction\" : \"picker\"")));
        QVERIFY(picker.contains(QStringLiteral(
            "readonly property bool reactionMode: mode === \"reaction\"")));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"reactionQuickBar\"")));
        QVERIFY(picker.contains(QStringLiteral("function expand(initialText)")));
        const QString delegate =
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.openReactionPickerFor(root.menuEventId, bubbleRow,\n"
            "                                               true)")));
    }

    void categoryRailIsIconBasedNotGlyphStrip()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        QVERIFY(!picker.contains(QStringLiteral("◷")));
        QVERIFY(!picker.contains(QStringLiteral("☆")));
        QVERIFY(picker.contains(QStringLiteral("_categoryIcons")));
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
        QVERIFY(picker.contains(QStringLiteral("function openTonePopupFor(")));
        QVERIFY(picker.contains(QStringLiteral("id: tonePopup")));
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
        QVERIFY(picker.contains(QStringLiteral(
            "1, Math.floor(width / AppTheme.emojiCellSize))")));
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor(width / columns)")));
        QVERIFY(!picker.contains(QStringLiteral("cellWidth: Math.floor(width / 8)")));
        QVERIFY(picker.contains(QStringLiteral("cellHeight: AppTheme.emojiCellSize")));
        QVERIFY(picker.contains(QStringLiteral("font.pixelSize: AppTheme.emojiGlyphSize")));
        QVERIFY(picker.contains(QStringLiteral("property string previewEmoji")));
        QVERIFY(picker.contains(QStringLiteral("property string previewName")));
        QVERIFY(picker.contains(QStringLiteral("onHoveredChanged")));
        QVERIFY(picker.contains(QStringLiteral("onActiveFocusChanged")));
        QVERIFY(!picker.contains(QStringLiteral(":shortcode:")));
    }

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
        QVERIFY(tone.contains(QStringLiteral("Keys.onLeftPressed")));
        QVERIFY(tone.contains(QStringLiteral("Keys.onReturnPressed")));
    }

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
        QString body;
        const QStringList bodyLines = picker.mid(fn, end - fn).split(QLatin1Char('\n'));
        for (const QString &line : bodyLines) {
            if (!line.trimmed().startsWith(QLatin1String("//")))
                body += line + QLatin1Char('\n');
        }
        QVERIFY(body.contains(QStringLiteral("searchTimer.running")));
        QVERIFY2(!body.contains(QStringLiteral("itemAtIndex(")),
                 "a delegate does not exist yet right after the flushed reset");
        QVERIFY(body.contains(QStringLiteral("m.data(m.index(0, 0)")));
        QVERIFY(body.contains(QStringLiteral("Qt.UserRole + 1")));
        QVERIFY(body.contains(QStringLiteral("picker.choose(")));
        QVERIFY2(picker.contains(QStringLiteral("emojiGrid.count > 0")),
                 "the select hint must be gated on a target existing");
    }

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
        const int chooseStart = picker.indexOf(QStringLiteral("function choose(emoji)"));
        const int recordAt = picker.indexOf(QStringLiteral("recordUse(emoji)"), chooseStart);
        const int emitAt = picker.indexOf(QStringLiteral("emojiChosen(emoji)"), chooseStart);
        QVERIFY(chooseStart >= 0 && recordAt > chooseStart && emitAt > recordAt);
        QVERIFY(picker.contains(QStringLiteral("if (closeAfterSelection) close()")));
        QVERIFY(picker.contains(QStringLiteral("Keys.onReturnPressed: picker.choose(emoji)")));
        QVERIFY(picker.contains(QStringLiteral("Keys.onSpacePressed: picker.choose(emoji)")));
        QVERIFY(picker.contains(QStringLiteral("Keys.onMenuPressed: if (hasSkinTones) openVariants()")));
        QVERIFY(picker.contains(QStringLiteral("No recently used emoji")));
        QVERIFY(picker.contains(QStringLiteral("No emoji found")));
        QVERIFY(picker.contains(QStringLiteral("AnchoredPopup {")));
        QVERIFY(!picker.contains(QStringLiteral("function placeInsideWindow()")));
        QVERIFY(!picker.contains(QStringLiteral("property point anchorPoint")));
        QVERIFY(!picker.contains(QStringLiteral("parent: Overlay.overlay")));
    }

    void anchoredPopupIsParentedToItsAnchorNotPositionedOverIt()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        QVERIFY(!base.isEmpty());
        QVERIFY(base.contains(QStringLiteral("property Item anchorItem")));
        QVERIFY(base.contains(QStringLiteral(
            "parent: anchorItem ? anchorItem : overlayItem")));

        QVERIFY(!base.contains(QStringLiteral("mapToItem")));
        QVERIFY(!base.contains(QStringLiteral("Qt.callLater")));
        QVERIFY(!base.contains(QStringLiteral("placementRevision")));

        QVERIFY(base.contains(QStringLiteral(
            "readonly property real anchoredX: anchorItem "
            "? Math.max(0, anchorItem.width - width) : 0")));
        QVERIFY(base.contains(QStringLiteral(
            "readonly property real anchoredY: -height - anchorGap")));
        QVERIFY(base.contains(QStringLiteral("value: root.anchoredX")));
        QVERIFY(base.contains(QStringLiteral("value: root.anchoredY")));
        QVERIFY(base.contains(QStringLiteral("when: root.anchorItem !== null")));

        QVERIFY(base.contains(QStringLiteral("return anchorItem.width")));
        QVERIFY(base.contains(QStringLiteral("room -= anchorItem.height + anchorGap")));
        QVERIFY(base.contains(QStringLiteral("? autoWidth : maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("? autoHeight : maxHeight * effectiveHeightFraction")));
        QVERIFY(base.contains(QStringLiteral(
            "anchorItem ? \"topLeft\" : (placedAbove ? \"topRight\" : \"bottomRight\")")));

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

    void pickersAreResizableAndRememberTheirSize()
    {
        const QString base = read(QStringLiteral(QML_DIR "/AnchoredPopup.qml"));
        const QString grip = read(QStringLiteral(QML_DIR "/PopupResizeGrip.qml"));
        const QString emoji = read(QStringLiteral(QML_DIR "/EmojiPicker.qml"));
        const QString gif = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!base.isEmpty() && !grip.isEmpty());

        QVERIFY(!base.contains(QStringLiteral("detached")));
        QVERIFY(base.contains(QStringLiteral("function resizeTo(w, h)")));
        QVERIFY(base.contains(QStringLiteral("function endResize()")));
        QVERIFY(base.contains(QStringLiteral(
            "userWidthFraction = Math.max(0.08, Math.min(1, w / maxWidth))")));
        QVERIFY(base.contains(QStringLiteral("app.settings.setPickerShare(sizeSettingsKey")));
        QVERIFY(base.contains(QStringLiteral(
            "app.settings.pickerWidthShare(root.sizeSettingsKey) / 1000")));
        QVERIFY(base.contains(QStringLiteral("Math.round(userWidthFraction * 1000)")));

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
        QVERIFY(grip.contains(QStringLiteral(
            "readonly property bool engaged: gripHover.hovered || dragHandler.active")));
        QVERIFY(grip.contains(QStringLiteral("property real arcCentre")));
        QVERIFY(grip.contains(QStringLiteral("property real outerRadius")));
        QVERIFY(!grip.contains(QStringLiteral("clip: true")));
        QVERIFY(grip.contains(QStringLiteral("Math.cos(angle)")));
        QVERIFY(grip.contains(QStringLiteral("Math.sin(angle)")));
        QVERIFY(grip.contains(QStringLiteral("rotation: angle * 180 / Math.PI + 90")));
        QVERIFY(grip.contains(QStringLiteral("color: AppTheme.bolt")));
        QVERIFY(grip.contains(QStringLiteral("property real strokeWidth: 2.5")));

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

        QVERIFY(base.contains(QStringLiteral(
            "readonly property real effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxWidth * effectiveWidthFraction")));
        QVERIFY(base.contains(QStringLiteral("maxHeight * effectiveHeightFraction")));
        QVERIFY(!base.contains(QStringLiteral("property real userWidth:")));
        QVERIFY(gif.contains(QStringLiteral("sizeSettingsKey: \"picker\"")));
        QVERIFY(emoji.contains(QStringLiteral(
            "sizeSettingsKey: reactionMode ? \"reaction\" : \"picker\"")));
    }

    void callersAnchorToTheItemNotASnapshottedPoint()
    {
        const QString composer = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(!composer.isEmpty() && !thread.isEmpty());
        QVERIFY(composer.contains(QStringLiteral("emojiPicker.anchorItem = composerCard")));
        QVERIFY(composer.contains(QStringLiteral("gifPicker.anchorItem = composerCard")));
        QVERIFY(thread.contains(QStringLiteral(
            "threadGifPicker.anchorItem = threadMiniComposer")));
        QVERIFY(thread.contains(QStringLiteral(
            "threadEmojiPicker.anchorItem = threadMiniComposer")));
        QVERIFY(!thread.contains(QStringLiteral("threadEmojiPicker.anchorPoint")));
        QVERIFY(!composer.contains(QStringLiteral("gifPicker.anchorPoint")));
        QVERIFY(!composer.contains(QStringLiteral("emojiPicker.anchorPoint")));
        QVERIFY(!thread.contains(QStringLiteral("threadGifPicker.anchorPoint")));
    }

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

    void theCellKeyHandlerDoesNotSwallowEscape()
    {
        const QString src = pickerSource();
        const QRegularExpression re(QStringLiteral(
            "Keys\\.onPressed:\\s*\\(event\\)\\s*=>\\s*\\{\\s*event\\.accepted\\s*=\\s*false"));
        QVERIFY2(re.match(src).hasMatch(), "the cell's Keys.onPressed must start "
                                           "with event.accepted = false");
    }

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
        QVERIFY(between(src, QStringLiteral("case Qt.Key_Up:"),
                        QStringLiteral("case Qt.Key_Home:"))
                    .contains(QStringLiteral("search.forceActiveFocus()")));
    }

    void rightClickOnAPlainEmojiDoesNotPick()
    {
        const QString src = pickerSource();
        QVERIFY2(QRegularExpression(QStringLiteral(
                     "mouse\\.button\\s*===\\s*Qt\\.RightButton\\)\\s*\\{\\s*if\\s*\\(cell\\.hasSkinTones"))
                     .match(src).hasMatch(),
                 "right-click must open tones or do nothing");
    }

    void theGridHasMiddleClickAutoscroll()
    {
        const QString src = pickerSource();
        const QString block = between(src, QStringLiteral("MiddleClickScroller {"),
                                      QStringLiteral("}"));
        QVERIFY2(!block.isEmpty(), "no MiddleClickScroller in the picker");
        QVERIFY(block.contains(QStringLiteral("view: emojiGrid")));
    }

    void wheelOutsideTheGridStillScrollsIt()
    {
        const QString src = pickerSource();
        QVERIFY2(src.count(QStringLiteral("scrollTarget: emojiGrid")) >= 2,
                 "heading and footer need a wheel handler targeting the grid");
        QVERIFY(src.contains(QStringLiteral("scrollTarget: railScroll.contentItem")));
    }

    void aNewBucketStartsAtItsFirstEmoji()
    {
        const QString src = pickerSource();
        QVERIFY(src.contains(QStringLiteral("function resetToStart()")));
        QVERIFY(src.contains(QStringLiteral("positionViewAtBeginning()")));
        QVERIFY(src.contains(QStringLiteral("function onCategoryChanged()")));
        QVERIFY(src.contains(QStringLiteral("function onSearchTextChanged()")));
    }

    void theRailIsDeselectedWhileSearching()
    {
        const QString src = pickerSource();
        QVERIFY(QRegularExpression(QStringLiteral(
                    "search\\.text\\.length\\s*===\\s*0\\s*&&\\s*app\\.emojiCatalog\\.category\\s*===\\s*modelData"))
                    .match(src).hasMatch());
    }

    void theToneStripReturnsFocusAndUsesTheFooter()
    {
        const QString src = pickerSource();
        const QString tone = src.mid(src.indexOf(QStringLiteral("id: tonePopup")));
        QVERIFY(tone.contains(QStringLiteral("origin.forceActiveFocus()")));
        QVERIFY(!tone.contains(QStringLiteral("ToolTip.text: modelData.name")));
        QVERIFY(tone.contains(QStringLiteral("picker.previewName = modelData.name")));
    }

    void aReactionPickerOpensOnRecents()
    {
        const QString src = pickerSource();
        const QString open = between(src, QStringLiteral("onAboutToShow: {"),
                                     QStringLiteral("background: Item {"));
        QVERIFY(open.contains(QStringLiteral("if (reactionMode)")));
        QVERIFY(open.contains(QStringLiteral("\"Recently Used\"")));
        QVERIFY2(!open.contains(QStringLiteral("recentEmoji")),
                 "must not decide on the raw MRU length");
        QVERIFY(QRegularExpression(QStringLiteral(
                    "emojiCatalog\\.count\\s*===\\s*0")).match(open).hasMatch());
    }

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
