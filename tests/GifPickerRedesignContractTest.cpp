// Source-scan contract for GifPicker's layout: a picker that takes a share of
// its anchor with a dynamically sized 3-column grid, its own "GIF" header
// badge, per-tile source tags, and the "return to send" footer hint.
// Navigation is one row: two source tabs and, past a divider, the two
// cross-provider lists; one `tab` state property; one save/unsave vocabulary
// shared with the timeline star. Complements
// QmlBindingContractTest::gifPickerWiredIntoBothComposers without loosening
// it.

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
        // No fixed width: the picker takes a share of the available space. The
        // key is shared with the emoji picker so resizing one resizes both.
        QVERIFY(picker.contains(QStringLiteral("widthFraction:")));
        QVERIFY(picker.contains(QStringLiteral("heightFraction:")));
        QVERIFY(picker.contains(QStringLiteral("sizeSettingsKey: \"picker\"")));
        QVERIFY(!picker.contains(QStringLiteral("width: Math.min(330,")));
        QVERIFY(!picker.contains(QStringLiteral("defaultWidth:")));
        // The minimum still fits three grid columns and the footer.
        QVERIFY(picker.contains(QStringLiteral("minWidth: 300")));
        // No fixed 132px cell.
        QVERIFY(!picker.contains(QStringLiteral("readonly property int cell: 132")));
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor(width / 3)")));
        QVERIFY(picker.contains(QStringLiteral("cellHeight: cellWidth")));
    }

    void headerBadgeIsDistinctFromComposerKeycap()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifPickerHeaderBadge\"")));
        // The header badge keeps its own identity rather than reviving the
        // retired composer keycap's objectName.
        QVERIFY(!picker.contains(QStringLiteral("objectName: \"composerGifKeycap\"")));
        QVERIFY(picker.contains(QStringLiteral("radius: AppTheme.radiusMd")));
    }

    // Sources and lists are peers in one nav row. "Saved" and "Recent" are
    // cross-provider, so rendering them as chips under a provider tab
    // misrepresented them; the section-chip row is gone.
    void navigationIsOneRowOfSourcesAndLists()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifProviderTabs\"")));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifListTabs\"")));
        // The section chips are gone, not hidden: nothing sets a `section`.
        QVERIFY(!picker.contains(QStringLiteral("gifSectionTabs")));
        QVERIFY(!picker.contains(QStringLiteral("picker.section")));
        QVERIFY(!picker.contains(QStringLiteral("property string section")));
        QVERIFY(!picker.contains(QStringLiteral("starredTabActive")));
        QVERIFY(!picker.contains(QStringLiteral("qsTr(\"Favorites\")")));
        // Both lists live in the second control and are always enabled: neither
        // needs a key or issues a request.
        const int start = picker.indexOf(QStringLiteral("objectName: \"gifListTabs\""));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("// ── Category chips"), start);
        QVERIFY(end > start);
        const QString block = picker.mid(start, end - start);
        QVERIFY(block.contains(QStringLiteral("value: \"saved\",")));
        QVERIFY(block.contains(QStringLiteral("value: \"recent\",")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Saved\")")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Recent\")")));
        // The provider strip derives from the real provider list and alone
        // carries the not-configured explanation.
        const int provStart =
            picker.indexOf(QStringLiteral("objectName: \"gifProviderTabs\""));
        QVERIFY(provStart >= 0 && provStart < start);
        const QString provBlock = picker.mid(provStart, start - provStart);
        QVERIFY(provBlock.contains(QStringLiteral("picker.gif.providerIds.map(")));
        QVERIFY(provBlock.contains(QStringLiteral("providerConfigured(id)")));
        QVERIFY(!provBlock.contains(QStringLiteral("value: \"saved\"")));
    }

    // One state property drives the picker, and only a real provider id
    // reaches setActiveProvider(), the one network-triggering entry point;
    // "saved"/"recent" return before it.
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
        // The early return for a local list precedes the provider call.
        QVERIFY(body.indexOf(QStringLiteral("return"), guard) < call);
        // Both strips route through this one function.
        QCOMPARE(picker.count(QStringLiteral("onActivated: (value) => picker.selectTab(value)")),
                 2);
        // Exactly one call site, counted in its qualified call form so prose
        // naming it does not count.
        QCOMPARE(picker.count(QStringLiteral("picker.gif.setActiveProvider(")), 1);
    }

    // One star glyph, one verb, one destination.
    void oneStarWithOneSaveVocabulary()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifTileSaveButton\"")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"Remove from saved GIFs\")")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"Save GIF\")")));
        // The old two-destination vocabulary must not survive anywhere.
        QVERIFY(!picker.contains(QStringLiteral("Add to favorites")));
        QVERIFY(!picker.contains(QStringLiteral("Remove from favorites")));
        QVERIFY(!picker.contains(QStringLiteral("Remove from starred GIFs")));
        // Saved state is asked of the store, never read from FavoriteRole
        // (GifStoredModel answers it with a constant true). A revision counter
        // re-evaluates the binding, since isSaved() is a plain call.
        QVERIFY(picker.contains(QStringLiteral("function isSaved(provider, gifId)")));
        QVERIFY(picker.contains(QStringLiteral(
            "return gif.favorites.isFavorite(provider, gifId)")));
        QVERIFY(picker.contains(QStringLiteral("property int savedRevision: 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "readonly property bool saved: {\n"
            "                    var rev = picker.savedRevision\n"
            "                    return picker.isSaved(tile.provider, tile.gifId)")));
        QVERIFY(!picker.contains(QStringLiteral("|| tile.favorite")));
        // Saved renders as a fill, not only a tint: the bundled Material
        // Symbols subset is FILL=0, with no filled star glyph.
        QVERIFY(picker.contains(QStringLiteral(
            "color: tile.saved ? AppTheme.bolt")));
        QVERIFY(picker.contains(QStringLiteral(
            "color: tile.saved ? AppTheme.boltInk")));
        // Routing uses the snapshot's own provider field, never a row index.
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

    // Every tile in a merged Saved list says where it came from, which also
    // keeps provider credit on the right tiles.
    void everyTileCarriesItsSourceTag()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifTileSourceBadge\"")));
        QVERIFY(picker.contains(QStringLiteral("text: picker.sourceLabel(tile.provider)")));
        // The tag is resolved from the provider registry, never a hardcoded
        // brand string.
        const int start = picker.indexOf(QStringLiteral("function sourceLabel(provider)"));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("widthFraction:"), start);
        QVERIFY(end > start);
        const QString body = picker.mid(start, end - start);
        QVERIFY(body.contains(QStringLiteral("if (provider === \"local\")")));
        QVERIFY(body.contains(QStringLiteral("qsTr(\"Local\")")));
        QVERIFY(body.contains(QStringLiteral("gif.providerDisplayName(provider)")));
        QVERIFY(!picker.contains(QStringLiteral("Tenor")));
        // It replaced the uninformative "GIF" tile badge; the header badge is a
        // separate element.
        QVERIFY(!picker.contains(QStringLiteral("gifTileBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("gifPickerHeaderBadge")));
    }

    void tileBadgesAreLabelsAndOptionalSizeOverlay()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // Badges are Label text, never Icon-name strings.
        QVERIFY(picker.contains(QStringLiteral("sourceBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("gifSizeBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("picker.formatBytes(tile.gifBytes)")));
        // Badge and star scrims reuse the overlayScrim token; no new colour
        // literals.
        QVERIFY(picker.contains(QStringLiteral("AppTheme.overlayScrim")));
        QVERIFY(!picker.contains(QStringLiteral("Qt.rgba(0, 0, 0, 0.35)")));
        // Keyboard-selected thumb border unchanged.
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
        // The footer is the RowLayout after the "── Footer:" comment through the
        // end of the contentItem: exactly one keycap ("↵ send"), no preview
        // hint (there is no preview action).
        const int footerStart = picker.indexOf(QStringLiteral("── Footer:"));
        QVERIFY(footerStart >= 0);
        const QString footer = picker.mid(footerStart);
        // A provider tab shows that provider's credit; a local list can hold
        // rows from either provider, so it credits all of them. Pinned with
        // leading punctuation so a comment cannot satisfy it.
        QVERIFY(footer.contains(QStringLiteral("text: picker.providerTab ? picker.gif.attribution")));
        QVERIFY(footer.contains(QStringLiteral(": picker.allProviderAttribution")));
        QVERIFY(!footer.contains(QStringLiteral("Tenor")));
        QCOMPARE(footer.count(QStringLiteral("MenuKeycap {")), 1);
        QVERIFY(footer.contains(QStringLiteral("iconName: \"keyboard_return\"")));
        QVERIFY(footer.contains(QStringLiteral("qsTr(\"send\")")));
        QVERIFY(!footer.contains(QStringLiteral("qsTr(\"preview\")")));
        QVERIFY(!footer.contains(QStringLiteral("ShiftModifier")));
        // The local-list credit derives from the provider registry.
        QVERIFY(picker.contains(QStringLiteral(
            "return gif.providerIds.map(function(id) {")));
        QVERIFY(picker.contains(QStringLiteral("gif.providerAttribution(id)")));
    }

    // Re-pin of the nine invariants QmlBindingContractTest owns.
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
            // A locally saved tile has no provider URLs; both image branches
            // switch on tile.provider, and the non-local fallback is the
            // validated local copy from app.gif.previews, never the provider
            // URL itself (an SVG answer must not reach Qt's decoders). Bounded
            // to the tile delegate.
            const int tileStart =
                picker.indexOf(QStringLiteral("delegate: Item {"));
            const int tileEnd = picker.indexOf(
                QStringLiteral("Keys.onReturnPressed:"), tileStart);
            QVERIFY(tileStart >= 0 && tileEnd > tileStart);
            const QString tileBlock = picker.mid(tileStart, tileEnd - tileStart);
            QVERIFY(tileBlock.contains(QStringLiteral(
                "source: tile.provider === \"local\"\n"
                "                                ? tile.localSource : tile.stillSource")));
            // A local still (png/jpg/webp) never feeds the movie backend: the
            // local branch narrows to GIF or legacy rows.
            QVERIFY(tileBlock.contains(QStringLiteral(
                "source: tile.provider === \"local\"\n"
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
            // The sendable original is never a live image source. tile.gifUrl
            // appears legitimately in snapshot()'s field capture, so the check
            // is scoped to a `source:` binding.
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

    // The Saved tab binds the merged model as a real Q_PROPERTY. Whether the
    // binding throws at runtime is covered by
    // GifPickerSelectionQmlTest::savedTabBindsTheMergedModelNotResults.
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
        // No network on a local list: pagination, the busy spinner and the
        // search field are gated on providerTab.
        QVERIFY(picker.contains(QStringLiteral(
            "if (!picker.providerTab || contentHeight <= 0)")));
        QVERIFY(picker.contains(QStringLiteral(
            "running: picker.providerTab\n"
            "                     && picker.gif.state === GifSearchController.Loading")));
        QVERIFY(picker.contains(QStringLiteral("visible: picker.providerTab")));
        QVERIFY(picker.contains(QStringLiteral("visible: !picker.providerTab")));
        // The empty-state copy names where a star leads.
        QVERIFY(picker.contains(QStringLiteral(
            "No saved GIFs yet. Press the star on any GIF — ")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"No recent GIFs yet.\")")));
        // The old copy pointing elsewhere is gone.
        QVERIFY(!picker.contains(QStringLiteral("No favorites yet")));
        QVERIFY(!picker.contains(QStringLiteral("save it here")));
    }

    // A local list hides searchField (which hands focus to the grid on
    // provider tabs) and the category chips, so the grid needs its own
    // keyboard entry points there.
    void localListGridIsKeyboardReachable()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "activeFocusOnTab: !picker.providerTab")));
        {
            const int start = picker.indexOf(QStringLiteral("id: grid"));
            QVERIFY(start >= 0);
            const int end = picker.indexOf(QStringLiteral("// A highlighted/"), start);
            QVERIFY(end > start);
            const QString gridHead = picker.mid(start, end - start);
            QVERIFY(gridHead.contains(QStringLiteral(
                "onActiveFocusChanged: {\n"
                "                if (activeFocus && currentIndex < 0 && count > 0)\n"
                "                    currentIndex = 0\n"
                "            }")));
        }
        // The tile's save button is focusable, so it reveals itself on focus
        // and draws a ring, like the chat star.
        QVERIFY(picker.contains(QStringLiteral(
            "opacity: tileHover.hovered || tile.current\n"
            "                                 || saveButton.visualFocus ? 1 : 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "visible: saveButton.visualFocus")));

        // The selected category chip toggles off, the in-session route back
        // from a category.
        QVERIFY(picker.contains(QStringLiteral(
            "picker.gif.mode === GifSearchController.Category")));
        QVERIFY(picker.contains(QStringLiteral(
            "if (categoryChip.selected)\n"
            "                            picker.gif.showTrending()")));

        // Down on the nav row hands off to the grid like searchField's
        // Down/Return do on a provider tab.
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

    // Every provider-facing action goes through the controller: the picker
    // holds no endpoint, key or pagination cursor of its own, so a text scan
    // of what it calls is the right proof.
    void providerTabsSearchAndPaginationRunThroughTheController()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        // Provider tabs and attribution follow the active provider.
        QVERIFY(picker.contains(QStringLiteral("picker.gif.providerIds")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.attribution")));
        // Debounced search, categories and pagination, all via the controller.
        QVERIFY(picker.contains(QStringLiteral("gif.setQueryText(text)")));
        QVERIFY(picker.contains(QStringLiteral("gif.openCategory(modelData)")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.loadMore()")));
        // The grid renders the active tab's model.
        QVERIFY(picker.contains(QStringLiteral("model: picker.activeModel")));
        QVERIFY(picker.contains(QStringLiteral("gif.saved")));
        QVERIFY(picker.contains(QStringLiteral("gif.recent")));
        // Calling a non-invokable C++ method from a binding throws, Qt swallows
        // it, and the tab keeps its previous model. Every model reached from a
        // binding is a Q_PROPERTY read; savedTabBindsTheMergedModelProperty
        // pins `gif.saved()` and `gif.starredStore.model()`, these the rest.
        QVERIFY(!picker.contains(QStringLiteral("gif.recent()")));
        QVERIFY(!picker.contains(QStringLiteral("favoritesAndStarred")));
        // Sending resolves the clicked row against the model on screen: the
        // mouse path hands over the delegate's captured, provider-qualified
        // snapshot, and an unidentifiable row is dropped, never substituted.
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
};
QTEST_MAIN(GifPickerRedesignContractTest)
#include "GifPickerRedesignContractTest.moc"
