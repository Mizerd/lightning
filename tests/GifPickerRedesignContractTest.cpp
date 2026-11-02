
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

class GifPickerRedesignContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void widthShrankAndGridIsDynamic()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY(picker.contains(QStringLiteral("widthFraction:")));
        QVERIFY(picker.contains(QStringLiteral("heightFraction:")));
        QVERIFY(picker.contains(QStringLiteral("sizeSettingsKey: \"picker\"")));
        QVERIFY(!picker.contains(QStringLiteral("width: Math.min(330,")));
        QVERIFY(!picker.contains(QStringLiteral("defaultWidth:")));
        QVERIFY(picker.contains(QStringLiteral("minWidth: 300")));
        QVERIFY(!picker.contains(QStringLiteral("readonly property int cell: 132")));
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor((width - AppTheme.scrollbarWidth) / 3)")));
        QVERIFY(picker.contains(QStringLiteral("cellHeight: cellWidth")));
    }

    void headerBadgeIsDistinctFromComposerKeycap()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifPickerHeaderBadge\"")));
        QVERIFY(!picker.contains(QStringLiteral("objectName: \"composerGifKeycap\"")));
        QVERIFY(picker.contains(QStringLiteral("radius: AppTheme.radiusMd")));
    }

    void navigationIsOneRowOfSourcesAndLists()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifProviderTabs\"")));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifListTabs\"")));
        QVERIFY(!picker.contains(QStringLiteral("gifSectionTabs")));
        QVERIFY(!picker.contains(QStringLiteral("picker.section")));
        QVERIFY(!picker.contains(QStringLiteral("property string section")));
        QVERIFY(!picker.contains(QStringLiteral("starredTabActive")));
        QVERIFY(!picker.contains(QStringLiteral("qsTr(\"Favorites\")")));
        const int start = picker.indexOf(QStringLiteral("objectName: \"gifListTabs\""));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("objectName: \"gifCategoryChips\""), start);
        QVERIFY(end > start);
        const QString block = picker.mid(start, end - start);
        QVERIFY(block.contains(QStringLiteral("value: \"saved\",")));
        QVERIFY(block.contains(QStringLiteral("value: \"recent\",")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Saved\")")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Recent\")")));
        const int provStart =
            picker.indexOf(QStringLiteral("objectName: \"gifProviderTabs\""));
        QVERIFY(provStart >= 0 && provStart < start);
        const QString provBlock = picker.mid(provStart, start - provStart);
        QVERIFY(provBlock.contains(QStringLiteral("picker.gif.providerIds.map(")));
        QVERIFY(provBlock.contains(QStringLiteral("providerConfigured(id)")));
        QVERIFY(!provBlock.contains(QStringLiteral("value: \"saved\"")));
    }

    void oneTabPropertyAndNoRequestFromALocalList()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("property string tab: \"giphy\"")));
        QVERIFY(picker.contains(QStringLiteral(
            "readonly property bool providerTab: "
            "tab !== \"saved\" && tab !== \"recent\"")));
        QVERIFY(picker.contains(QStringLiteral(
            "onTabChanged: grid.currentIndex = -1")));

        const int start = picker.indexOf(QStringLiteral("function selectTab(value)"));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("function focusGridFromTabs"), start);
        QVERIFY(end > start);
        const QString body = picker.mid(start, end - start);
        const int guard = body.indexOf(
            QStringLiteral("if (value === \"saved\" || value === \"recent\")"));
        QVERIFY(guard >= 0);
        const int call = body.indexOf(QStringLiteral("setActiveProvider(value)"));
        QVERIFY(call >= 0);
        QVERIFY(body.indexOf(QStringLiteral("return"), guard) < call);
        QCOMPARE(picker.count(QStringLiteral("onActivated: (value) => picker.selectTab(value)")),
                 2);
        QCOMPARE(picker.count(QStringLiteral("picker.gif.setActiveProvider(")), 1);
    }

    void oneStarWithOneSaveVocabulary()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifTileSaveButton\"")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"Remove from saved GIFs\")")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"Save GIF\")")));
        QVERIFY(!picker.contains(QStringLiteral("Add to favorites")));
        QVERIFY(!picker.contains(QStringLiteral("Remove from favorites")));
        QVERIFY(!picker.contains(QStringLiteral("Remove from starred GIFs")));
        QVERIFY(picker.contains(QStringLiteral("function isSaved(provider, gifId)")));
        QVERIFY(picker.contains(QStringLiteral(
            "return gif.favorites.isFavorite(provider, gifId)")));
        QVERIFY(picker.contains(QStringLiteral("property int savedRevision: 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "readonly property bool saved: {\n"
            "                    var rev = picker.savedRevision\n"
            "                    return picker.isSaved(tile.provider, tile.gifId)")));
        QVERIFY(!picker.contains(QStringLiteral("|| tile.favorite")));
        QVERIFY(picker.contains(QStringLiteral(
            "color: tile.saved ? AppTheme.bolt")));
        QVERIFY(picker.contains(QStringLiteral(
            "color: tile.saved ? AppTheme.boltInk")));
        const int start = picker.indexOf(QStringLiteral("function toggleSaved(result)"));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("background: Rectangle {"), start);
        QVERIFY(end > start);
        const QString body = picker.mid(start, end - start);
        QVERIFY(body.contains(QStringLiteral("if (result.provider === \"local\")")));
        QVERIFY(body.contains(QStringLiteral("gif.starredStore.unstar(result.gifId)")));
        QVERIFY(body.contains(QStringLiteral("gif.toggleFavorite(result)")));
        QVERIFY(picker.contains(QStringLiteral("picker.toggleSaved(tile.snapshot())")));
        QVERIFY(!picker.contains(QStringLiteral("function toggleFavorite(result)")));
    }

    void everyTileCarriesItsSourceTag()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifTileSourceBadge\"")));
        QVERIFY(picker.contains(QStringLiteral("text: picker.sourceLabel(tile.provider)")));
        const int start = picker.indexOf(QStringLiteral("function sourceLabel(provider)"));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("widthFraction:"), start);
        QVERIFY(end > start);
        const QString body = picker.mid(start, end - start);
        QVERIFY(body.contains(QStringLiteral("if (provider === \"local\")")));
        QVERIFY(body.contains(QStringLiteral("qsTr(\"Local\")")));
        QVERIFY(body.contains(QStringLiteral("gif.providerDisplayName(provider)")));
        QVERIFY(!picker.contains(QStringLiteral("Tenor")));
        QVERIFY(!picker.contains(QStringLiteral("gifTileBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("gifPickerHeaderBadge")));
    }

    void tileBadgesAreLabelsAndOptionalSizeOverlay()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("sourceBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("gifSizeBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("picker.formatBytes(tile.gifBytes)")));
        QVERIFY(picker.contains(QStringLiteral("AppTheme.overlayScrim")));
        QVERIFY(!picker.contains(QStringLiteral("Qt.rgba(0, 0, 0, 0.35)")));
        QVERIFY(picker.contains(QStringLiteral("border.width: tile.current ? 2 : 0")));
        QVERIFY(picker.contains(QStringLiteral("radius: AppTheme.radiusThumb")));
    }

    void gifBytesRoleIsAdditiveAndRequiredWithoutADefaultInitializer()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // `required property real gifBytes` with no initializer (required
        // properties cannot have one); both models always supply the role.
        QVERIFY(picker.contains(QStringLiteral("required property real gifBytes")));
        QVERIFY(!picker.contains(QStringLiteral("required property real gifBytes: 0")));
        QVERIFY(picker.contains(QStringLiteral("gifBytes: tile.gifBytes")));
    }

    void footerKeepsRealAttributionAndAddsSendHintOnly()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        const int footerStart = picker.indexOf(QStringLiteral("id: footerRow"));
        QVERIFY(footerStart >= 0);
        const QString footer = picker.mid(footerStart);
        QVERIFY(footer.contains(QStringLiteral("text: picker.providerTab ? picker.gif.attribution")));
        QVERIFY(footer.contains(QStringLiteral(": picker.allProviderAttribution")));
        QVERIFY(!footer.contains(QStringLiteral("Tenor")));
        QCOMPARE(footer.count(QStringLiteral("MenuKeycap {")), 1);
        QVERIFY(footer.contains(QStringLiteral("iconName: \"keyboard_return\"")));
        QVERIFY(footer.contains(QStringLiteral("qsTr(\"send\")")));
        QVERIFY(!footer.contains(QStringLiteral("qsTr(\"preview\")")));
        QVERIFY(!footer.contains(QStringLiteral("ShiftModifier")));
        QVERIFY(picker.contains(QStringLiteral(
            "return gif.providerIds.map(function(id) {")));
        QVERIFY(picker.contains(QStringLiteral("gif.providerAttribution(id)")));
    }

    void nineInvariantsSurviveTheRedesign()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        {
            const int chooseStart =
                picker.indexOf(QStringLiteral("function choose(resultOrRow)"));
            const int chooseEnd = picker.indexOf(
                QStringLiteral("property int cfgRevision: 0"), chooseStart);
            QVERIFY(chooseStart >= 0 && chooseEnd > chooseStart);
            const QString block = picker.mid(chooseStart, chooseEnd - chooseStart);
            QVERIFY(block.contains(QStringLiteral("activeModel.get(resultOrRow)")));
            QVERIFY(!block.contains(QStringLiteral("gif.results.get(")));
            QVERIFY(block.contains(QStringLiteral(
                "if (!result || !result.provider || !result.gifId)")));
            QVERIFY(block.contains(QStringLiteral("picker.gifChosen(result)")));
        }
        QVERIFY(picker.contains(QStringLiteral("picker.choose(tile.snapshot())")));
        QVERIFY(picker.contains(QStringLiteral("function snapshot()")));
        QVERIFY(picker.contains(
            QStringLiteral("function onModelReset() { grid.currentIndex = -1 }")));
        QVERIFY(picker.contains(QStringLiteral("gif.toggleFavorite(")));
        QVERIFY(picker.contains(QStringLiteral("playing: picker.visible")));
        {
            const int tileStart =
                picker.indexOf(QStringLiteral("delegate: Item {"));
            const int tileEnd = picker.indexOf(
                QStringLiteral("Keys.onReturnPressed:"), tileStart);
            QVERIFY(tileStart >= 0 && tileEnd > tileStart);
            const QString tileBlock = picker.mid(tileStart, tileEnd - tileStart);
            QVERIFY(tileBlock.contains(QStringLiteral(
                "source: tile.provider === \"local\"\n"
                "                                ? tile.localSource\n"
                "                                : (tile.stillSource.length > 0\n"
                "                                   || MotionPolicy.allowed\n"
                "                                   ? tile.stillSource : tile.previewSource)")));
            QVERIFY(tileBlock.contains(QStringLiteral(
                "source: !MotionPolicy.allowed ? \"\"\n"
                "                                : tile.provider === \"local\"\n"
                "                                ? (tile.localExt.length === 0\n"
                "                                   || tile.localExt === \"gif\"\n"
                "                                   ? tile.localSource : \"\")\n"
                "                                : tile.previewSource")));
            QVERIFY(tileBlock.contains(QStringLiteral(
                "picker.gif.previews.source(tile.stillUrl, true)")));
            QVERIFY(tileBlock.contains(QStringLiteral(
                "picker.gif.previews.source(tile.previewUrl, false)")));
            QVERIFY(tileBlock.contains(QStringLiteral(
                "picker.gif.previews.hold(tile, heldUrls)")));
            QVERIFY(!tileBlock.contains(QStringLiteral(": tile.previewUrl\n")));
            QVERIFY(!tileBlock.contains(QStringLiteral(": tile.stillUrl\n")));
            QVERIFY(!tileBlock.contains(QStringLiteral("source: tile.previewUrl")));
            QVERIFY(!tileBlock.contains(QStringLiteral("source: tile.stillUrl")));
            QVERIFY(!tileBlock.contains(QStringLiteral("source: tile.gifUrl")));
        }
        QVERIFY(picker.contains(QStringLiteral("GifSearchController.MissingKey")));
        QVERIFY(picker.contains(QStringLiteral("GifSearchController.RateLimited")));
        QVERIFY(picker.contains(
            QStringLiteral("Popup.CloseOnEscape | Popup.CloseOnPressOutside")));
        {
            const int returnStart =
                picker.indexOf(QStringLiteral("Keys.onReturnPressed: {"));
            const int returnEnd =
                picker.indexOf(QStringLiteral("IconButton {"), returnStart);
            QVERIFY(returnStart >= 0 && returnEnd > returnStart);
            const QString block = picker.mid(returnStart, returnEnd - returnStart);
            QVERIFY(!block.contains(QStringLiteral("picker.choose(")));
            QVERIFY(block.contains(QStringLiteral("picker.gif.searchNow(searchField.text)")));
            QVERIFY(block.contains(QStringLiteral("grid.forceActiveFocus()")));
        }
        QVERIFY(!picker.contains(QStringLiteral("sendToRoom")));
        QVERIFY(!picker.contains(QStringLiteral("sendTextMessage")));
    }

    void savedTabBindsTheMergedModelProperty()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("tab === \"saved\" ? gif.saved")));
        QVERIFY(picker.contains(QStringLiteral(": tab === \"recent\" ? gif.recent")));
        QVERIFY(!picker.contains(QStringLiteral("gif.saved()")));
        QVERIFY(!picker.contains(QStringLiteral("gif.starredStore.model()")));
    }

    void localListsMakeNoProviderTrafficAndOwnTheirEmptyState()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "if (!picker.providerTab || contentHeight <= 0)")));
        QVERIFY(picker.contains(QStringLiteral(
            "running: picker.providerTab\n"
            "                     && picker.gif.state === GifSearchController.Loading")));
        QVERIFY(picker.contains(QStringLiteral("visible: picker.providerTab")));
        QVERIFY(picker.contains(QStringLiteral("visible: !picker.providerTab")));
        QVERIFY(picker.contains(QStringLiteral(
            "No saved GIFs yet. Press the star on any GIF — ")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"No recent GIFs yet.\")")));
        QVERIFY(!picker.contains(QStringLiteral("No favorites yet")));
        QVERIFY(!picker.contains(QStringLiteral("save it here")));
    }

    void localListGridIsKeyboardReachable()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "activeFocusOnTab: !picker.providerTab")));
        {
            const int start = picker.indexOf(QStringLiteral("id: grid"));
            QVERIFY(start >= 0);
            const int end = picker.indexOf(QStringLiteral("target: picker.activeModel"), start);
            QVERIFY(end > start);
            const QString gridHead = picker.mid(start, end - start);
            QVERIFY(gridHead.contains(QStringLiteral(
                "onActiveFocusChanged: {\n"
                "                if (activeFocus && currentIndex < 0 && count > 0)\n"
                "                    currentIndex = 0\n"
                "            }")));
        }
        QVERIFY(picker.contains(QStringLiteral(
            "opacity: tileHover.hovered || tile.current\n"
            "                                 || saveButton.visualFocus ? 1 : 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "visible: saveButton.visualFocus")));

        QVERIFY(picker.contains(QStringLiteral(
            "picker.gif.mode === GifSearchController.Category")));
        QVERIFY(picker.contains(QStringLiteral(
            "if (categoryChip.selected)\n"
            "                            picker.gif.showTrending()")));

        QVERIFY(picker.contains(QStringLiteral(
            "Keys.onDownPressed: picker.focusGridFromTabs()")));
        const int start = picker.indexOf(QStringLiteral("function focusGridFromTabs"));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("function toggleSaved"), start);
        QVERIFY(end > start);
        const QString body = picker.mid(start, end - start);
        QVERIFY(body.contains(QStringLiteral("if (picker.providerTab)")));
        QVERIFY(body.contains(QStringLiteral("grid.forceActiveFocus()")));
    }

    void providerTabsSearchAndPaginationRunThroughTheController()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY(picker.contains(QStringLiteral("picker.gif.providerIds")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.attribution")));
        QVERIFY(picker.contains(QStringLiteral("gif.setQueryText(text)")));
        QVERIFY(picker.contains(QStringLiteral("gif.openCategory(modelData)")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.loadMore()")));
        QVERIFY(picker.contains(QStringLiteral("model: picker.activeModel")));
        QVERIFY(picker.contains(QStringLiteral("gif.saved")));
        QVERIFY(picker.contains(QStringLiteral("gif.recent")));
        // Calling a non-invokable C++ method from a binding throws, Qt swallows
        // it, and the tab keeps its previous model. Every model reached from a
        // binding is a Q_PROPERTY read; savedTabBindsTheMergedModelProperty
        // pins `gif.saved()` and `gif.starredStore.model()`, these the rest.
        QVERIFY(!picker.contains(QStringLiteral("gif.recent()")));
        QVERIFY(!picker.contains(QStringLiteral("favoritesAndStarred")));
        {
            const int snapStart =
                picker.indexOf(QStringLiteral("function snapshot()"));
            const int snapEnd =
                picker.indexOf(QStringLiteral("Rectangle {"), snapStart);
            QVERIFY(snapStart >= 0 && snapEnd > snapStart);
            const QString snapshotBlock =
                picker.mid(snapStart, snapEnd - snapStart);
            QVERIFY(snapshotBlock.contains(
                QStringLiteral("provider: tile.provider")));
            QVERIFY(snapshotBlock.contains(QStringLiteral("gifId: tile.gifId")));
        }
        QVERIFY(!picker.contains(
            QStringLiteral("picker.gifChosen(gif.results.get(row))")));
    }

    void scrollbarHasItsOwnGutter()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        const int g = picker.indexOf(QStringLiteral("objectName: \"gifResultGrid\""));
        QVERIFY(g >= 0);
        const QString head = picker.mid(g, 1400);
        QVERIFY(head.contains(QStringLiteral(
            "cellWidth: Math.floor((width - AppTheme.scrollbarWidth) / 3)")));
        QVERIFY(!head.contains(QStringLiteral("Layout.rightMargin")));
    }

    void closeClearIsGuardedFromTheController()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "onTextChanged: if (!picker.clearingField) picker.gif.setQueryText(text)")));
        const int c = picker.indexOf(QStringLiteral("onClosed: {"));
        QVERIFY(c >= 0);
        const QString body = picker.mid(c, 700);
        const int on = body.indexOf(QStringLiteral("clearingField = true"));
        const int clr = body.indexOf(QStringLiteral("searchField.clear()"));
        const int off = body.indexOf(QStringLiteral("clearingField = false"));
        QVERIFY(on >= 0 && on < clr && clr < off);
    }

    void savedTooltipUnchanged()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!picker.contains(QStringLiteral("qsTr(\"Starred GIFs\")")));
    }

    void pressBarrierLeavesMiddleAndHistoryButtonsAlone()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!picker.contains(QStringLiteral("acceptedButtons: Qt.AllButtons")));
        QVERIFY(picker.contains(QStringLiteral(
            "acceptedButtons: Qt.LeftButton | Qt.RightButton")));
        const int m = picker.indexOf(QStringLiteral("MiddleClickScroller {"));
        QVERIFY(m >= 0);
        QVERIFY(picker.mid(m, 400).contains(QStringLiteral("view: grid")));
    }

    void gridHandlesHomeEndAndPagesAndDrawsACurrentRing()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        for (const char *k : { "Qt.Key_Home", "Qt.Key_End", "Qt.Key_PageDown",
                               "Qt.Key_PageUp" })
            QVERIFY2(picker.contains(QString::fromLatin1(k)), k);
        QVERIFY(picker.contains(QStringLiteral("positionViewAtIndex(i, GridView.Contain)")));
        const int r = picker.indexOf(QStringLiteral("objectName: \"gifTileCurrentRing\""));
        QVERIFY(r >= 0);
        QVERIFY(picker.mid(r, 400).contains(QStringLiteral("visible: tile.current && grid.activeFocus")));
        QVERIFY(picker.contains(QStringLiteral("event.modifiers & (Qt.ShiftModifier")));
    }

    void closingClearsTheSearchFieldWithTheController()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        const int c = picker.indexOf(QStringLiteral("onClosed: {"));
        QVERIFY(c >= 0);
        const QString body = picker.mid(c, 700);
        QVERIFY(body.contains(QStringLiteral("gif.reset()")));
        QVERIFY(body.indexOf(QStringLiteral("searchField.clear()"))
                > body.indexOf(QStringLiteral("gif.reset()")));
    }
};
QTEST_MAIN(GifPickerRedesignContractTest)
#include "GifPickerRedesignContractTest.moc"
