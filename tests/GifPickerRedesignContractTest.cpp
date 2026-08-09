// v0.6.5 (SPEC §1n): source-scan contract for the redesigned GifPicker —
// width 330 with a dynamically-sized 3-column grid (replacing the fixed
// 132px cell that assumed the previous 460px width), the picker's own "GIF"
// header badge (distinct from the composer's pinned composerGifKeycap), the
// tile badges, and the "return to send" footer hint. This is a
// belt-and-suspenders re-pin alongside tests/QmlBindingContractTest.cpp's
// gifPickerWiredIntoBothComposers (which owns the authoritative nine-invariant
// literals); this suite adds the redesign-specific structure without loosening
// any of those.
//
// v0.6.7 UX rework — ONE star, ONE saved list. The suite now pins the
// collapsed navigation (two SOURCE tabs and, past a divider, the two LISTS
// that were always cross-provider), the single `tab` state property that
// replaced the contradictory `section` + `starredTabActive` pair, the one
// save/unsave vocabulary shared with the chat-timeline star, and the per-tile
// source tag that keeps provider credit attached to the exact tiles it belongs
// to inside a merged Saved list.

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
        // v0.6.7: 330 is now the DEFAULT width — AnchoredPopup clamps it to
        // the window and the corner grip can override it — not a hard size.
        QVERIFY(picker.contains(QStringLiteral("defaultWidth: 330")));
        QVERIFY(picker.contains(QStringLiteral("defaultHeight: 520")));
        QVERIFY(picker.contains(QStringLiteral("sizeSettingsKey: \"gif\"")));
        QVERIFY(!picker.contains(QStringLiteral("width: Math.min(330,")));
        // A minimum that still fits three grid columns and the footer.
        QVERIFY(picker.contains(QStringLiteral("minWidth: 300")));
        // No more fixed 132px cell tied to the old 460px width.
        QVERIFY(!picker.contains(QStringLiteral("readonly property int cell: 132")));
        QVERIFY(picker.contains(QStringLiteral("cellWidth: Math.floor(width / 3)")));
        QVERIFY(picker.contains(QStringLiteral("cellHeight: cellWidth")));
    }

    void headerBadgeIsDistinctFromComposerKeycap()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifPickerHeaderBadge\"")));
        // Never reuses the composer's pinned objectName/geometry (1.5px
        // border / radius 5 — see tests/ComposerQmlTest.cpp's
        // gifKeycapIsBorderedMonoChip).
        QVERIFY(!picker.contains(QStringLiteral("objectName: \"composerGifKeycap\"")));
        QVERIFY(picker.contains(QStringLiteral("radius: AppTheme.radiusMd")));
    }

    // v0.6.7: the two nav strips became one row of peers. The maintainer's
    // report was that a star meant two different things in two places; a large
    // part of why is that the navigation itself lied — "Favorites" and
    // "Recent" were CROSS-provider lists rendered as chips *underneath* a
    // provider tab, so selecting KLIPY and then Favorites showed GIPHY
    // favorites. Both lists are now peers of the providers, and the old
    // section-chip row is gone entirely.
    void navigationIsOneRowOfSourcesAndLists()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifProviderTabs\"")));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifListTabs\"")));
        // The section chips are GONE — not merely hidden. Nothing may still
        // set a `section`, and no Favorites chip may survive anywhere.
        QVERIFY(!picker.contains(QStringLiteral("gifSectionTabs")));
        QVERIFY(!picker.contains(QStringLiteral("picker.section")));
        QVERIFY(!picker.contains(QStringLiteral("property string section")));
        QVERIFY(!picker.contains(QStringLiteral("starredTabActive")));
        QVERIFY(!picker.contains(QStringLiteral("qsTr(\"Favorites\")")));
        // Both lists live in the second control, and both are always enabled:
        // neither needs a key and neither ever issues a request, so they work
        // even with no provider configured at all.
        const int start = picker.indexOf(QStringLiteral("objectName: \"gifListTabs\""));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("// ── Category chips"), start);
        QVERIFY(end > start);
        const QString block = picker.mid(start, end - start);
        QVERIFY(block.contains(QStringLiteral("value: \"saved\",")));
        QVERIFY(block.contains(QStringLiteral("value: \"recent\",")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Saved\")")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Recent\")")));
        // The provider strip is still derived from the real provider list,
        // never a hand-maintained duplicate, and it is the only strip that
        // carries the not-configured explanation.
        const int provStart =
            picker.indexOf(QStringLiteral("objectName: \"gifProviderTabs\""));
        QVERIFY(provStart >= 0 && provStart < start);
        const QString provBlock = picker.mid(provStart, start - provStart);
        QVERIFY(provBlock.contains(QStringLiteral("picker.gif.providerIds.map(")));
        QVERIFY(provBlock.contains(QStringLiteral("providerConfigured(id)")));
        QVERIFY(!provBlock.contains(QStringLiteral("value: \"saved\"")));
    }

    // Exactly one state property drives the whole picker, and only a real
    // provider id may reach setActiveProvider() — the one network-triggering
    // entry point. "saved"/"recent" return before it, so neither list can make
    // a provider request.
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
        // Exactly ONE call site for the whole file. Counted in its qualified
        // call form so prose mentioning setActiveProvider() by name does not
        // inflate it — the point is that no second code path can reach it.
        QCOMPARE(picker.count(QStringLiteral("picker.gif.setActiveProvider(")), 1);
    }

    // The whole point of the rework: one glyph, one verb, one destination.
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
        // v0.6.7 review (H1): saved state is asked of the STORE, never read
        // from the row's FavoriteRole. GifStoredModel answers that role with
        // a constant `true`, so reading it made every Recent tile claim to be
        // saved and made its star insert while announcing "Remove". The
        // revision counter is what re-evaluates the binding, since isSaved()
        // is a plain call that establishes no dependency.
        QVERIFY(picker.contains(QStringLiteral("function isSaved(provider, gifId)")));
        QVERIFY(picker.contains(QStringLiteral(
            "return gif.favorites.isFavorite(provider, gifId)")));
        QVERIFY(picker.contains(QStringLiteral("property int savedRevision: 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "readonly property bool saved: {\n"
            "                    var rev = picker.savedRevision\n"
            "                    return picker.isSaved(tile.provider, tile.gifId)")));
        QVERIFY(!picker.contains(QStringLiteral("|| tile.favorite")));
        // Saved is rendered as a FILL, not only a tint — the bundled Material
        // Symbols subset is a static FILL=0 instance, so there is no filled
        // star glyph to switch to.
        QVERIFY(picker.contains(QStringLiteral(
            "color: tile.saved ? AppTheme.bolt")));
        QVERIFY(picker.contains(QStringLiteral(
            "color: tile.saved ? AppTheme.boltInk")));
        // Routing is by the snapshot's own provider field, never a row index.
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

    // A merged Saved list must still say where each GIF came from — the
    // maintainer's follow-up ("add a tag on the gif like where it says size of
    // the gif, write klpy giphy or local"). It also keeps provider credit
    // attached to the exact tiles it belongs to (ruling R15).
    void everyTileCarriesItsSourceTag()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifTileSourceBadge\"")));
        QVERIFY(picker.contains(QStringLiteral("text: picker.sourceLabel(tile.provider)")));
        // The tag is real text, resolved from the provider registry — never a
        // hardcoded brand string, which is how a wrong brand gets shown.
        const int start = picker.indexOf(QStringLiteral("function sourceLabel(provider)"));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("defaultWidth: 330"), start);
        QVERIFY(end > start);
        const QString body = picker.mid(start, end - start);
        QVERIFY(body.contains(QStringLiteral("if (provider === \"local\")")));
        QVERIFY(body.contains(QStringLiteral("qsTr(\"Local\")")));
        QVERIFY(body.contains(QStringLiteral("gif.providerDisplayName(provider)")));
        QVERIFY(!picker.contains(QStringLiteral("Tenor")));
        // It replaced the old "GIF" tile badge, which said nothing new inside
        // a GIF picker; the header badge is a different, surviving element.
        QVERIFY(!picker.contains(QStringLiteral("gifTileBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("gifPickerHeaderBadge")));
    }

    void tileBadgesAreLabelsAndOptionalSizeOverlay()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // Both badges are Label text, never an Icon-name string
        // (IconChromeTest bans the ★ character but a badge must still never
        // masquerade as an icon).
        QVERIFY(picker.contains(QStringLiteral("sourceBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("gifSizeBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("picker.formatBytes(tile.gifBytes)")));
        // Badge/star scrims reuse the semantic overlayScrim token — no new
        // hex or Qt.rgba literal was introduced for them.
        QVERIFY(picker.contains(QStringLiteral("AppTheme.overlayScrim")));
        QVERIFY(!picker.contains(QStringLiteral("Qt.rgba(0, 0, 0, 0.35)")));
        // Keyboard-selected thumb border unchanged.
        QVERIFY(picker.contains(QStringLiteral("border.width: tile.current ? 2 : 0")));
        QVERIFY(picker.contains(QStringLiteral("radius: AppTheme.radiusThumb")));
    }

    void gifBytesRoleIsAdditiveAndRequiredWithoutADefaultInitializer()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // `required property real gifBytes` — NOT `... : 0` (a required
        // property cannot take an initializer; the role is always present
        // on both GifResultModel and GifStoredModel so this is safe).
        QVERIFY(picker.contains(QStringLiteral("required property real gifBytes")));
        QVERIFY(!picker.contains(QStringLiteral("required property real gifBytes: 0")));
        QVERIFY(picker.contains(QStringLiteral("gifBytes: tile.gifBytes")));
    }

    void footerKeepsRealAttributionAndAddsSendHintOnly()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // The footer is the RowLayout after the "── Footer:" comment through
        // the end of the popup's contentItem: exactly one keycap ("↵ send"),
        // never a second "⇧↵ preview" hint — no preview action exists.
        const int footerStart = picker.indexOf(QStringLiteral("── Footer:"));
        QVERIFY(footerStart >= 0);
        const QString footer = picker.mid(footerStart);
        // A provider tab shows that provider's required credit; a local list
        // can hold rows from EITHER provider, so it credits every provider
        // rather than whichever one happens to be active. Both branches are
        // pinned with their leading punctuation so a bare substring match in a
        // comment cannot satisfy them.
        QVERIFY(footer.contains(QStringLiteral("text: picker.providerTab ? picker.gif.attribution")));
        QVERIFY(footer.contains(QStringLiteral(": picker.allProviderAttribution")));
        QVERIFY(!footer.contains(QStringLiteral("Tenor")));
        QCOMPARE(footer.count(QStringLiteral("MenuKeycap {")), 1);
        QVERIFY(footer.contains(QStringLiteral("iconName: \"keyboard_return\"")));
        QVERIFY(footer.contains(QStringLiteral("qsTr(\"send\")")));
        QVERIFY(!footer.contains(QStringLiteral("qsTr(\"preview\")")));
        QVERIFY(!footer.contains(QStringLiteral("ShiftModifier")));
        // The credit for a local list is derived from the provider registry,
        // never a literal brand list that could drift out of date.
        QVERIFY(picker.contains(QStringLiteral(
            "return gif.providerIds.map(function(id) {")));
        QVERIFY(picker.contains(QStringLiteral("gif.providerAttribution(id)")));
    }

    // Belt-and-suspenders re-pin of the nine invariants QmlBindingContractTest
    // already owns — neither redesign moved any of these literals.
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
            // A locally-saved tile has no provider previewUrl/stillUrl — both
            // the static Image and the AnimatedImage branch on tile.provider,
            // but the non-local fallback is still exactly tile.stillUrl /
            // tile.previewUrl. Bounded to the tile delegate block so the
            // negative check below cannot false-positive on anything outside.
            const int tileStart =
                picker.indexOf(QStringLiteral("delegate: Item {"));
            const int tileEnd = picker.indexOf(
                QStringLiteral("Keys.onReturnPressed:"), tileStart);
            QVERIFY(tileStart >= 0 && tileEnd > tileStart);
            const QString tileBlock = picker.mid(tileStart, tileEnd - tileStart);
            QVERIFY(tileBlock.contains(QStringLiteral(
                "source: tile.provider === \"local\"\n"
                "                                ? tile.localSource : tile.stillUrl")));
            QVERIFY(tileBlock.contains(QStringLiteral(
                "source: tile.provider === \"local\"\n"
                "                                ? tile.localSource : tile.previewUrl")));
            // The sendable original is NEVER rendered as a live image
            // source anywhere in the tile. tile.gifUrl DOES legitimately
            // appear once, in snapshot()'s "gifUrl: tile.gifUrl," field
            // capture (send-time identity, not a rendered source) — the
            // check is deliberately scoped to an actual `source:` binding.
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

    // The Saved tab binds the merged model as a real Q_PROPERTY. A text scan
    // alone cannot prove the binding does not throw at runtime — see
    // GifPickerSelectionQmlTest::savedTabBindsTheMergedModelNotResults for the
    // real-engine assertion. (The v0.6.6 live bug was exactly this: a
    // non-invokable `.model()` call threw and Qt silently left activeModel at
    // its previous value.)
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
        // search field are all gated on providerTab.
        QVERIFY(picker.contains(QStringLiteral(
            "if (!picker.providerTab || contentHeight <= 0)")));
        QVERIFY(picker.contains(QStringLiteral(
            "running: picker.providerTab\n"
            "                     && picker.gif.state === GifSearchController.Loading")));
        QVERIFY(picker.contains(QStringLiteral("visible: picker.providerTab")));
        QVERIFY(picker.contains(QStringLiteral("visible: !picker.providerTab")));
        // Its own empty-state copy names the one place a star ever leads.
        QVERIFY(picker.contains(QStringLiteral(
            "No saved GIFs yet. Press the star on any GIF — ")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"No recent GIFs yet.\")")));
        // The old copy pointed at a separate destination and must be gone.
        QVERIFY(!picker.contains(QStringLiteral("No favorites yet")));
        QVERIFY(!picker.contains(QStringLiteral("save it here")));
    }

    // A local list hides searchField — the component that hands focus to the
    // grid on every provider tab — along with the category chips, so without
    // these two entry points the grid would be entirely keyboard-unreachable
    // there. (This was a real, reviewed a11y regression when the Starred tab
    // first landed; the same hazard applies to both lists now.)
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
        // v0.6.7 review (M1): the tile's save button is a focusable control,
        // so Tab lands on it — it must reveal itself on focus and carry a
        // ring, exactly as the chat star does. Without this, Tab focused a
        // fully transparent button.
        QVERIFY(picker.contains(QStringLiteral(
            "opacity: tileHover.hovered || tile.current\n"
            "                                 || saveButton.visualFocus ? 1 : 0")));
        QVERIFY(picker.contains(QStringLiteral(
            "visible: saveButton.visualFocus")));

        // v0.6.7 review (L2): deleting the section-chip row removed the
        // "Trending" chip and with it the only in-session route back from a
        // category. The selected category chip toggles off instead.
        QVERIFY(picker.contains(QStringLiteral(
            "picker.gif.mode === GifSearchController.Category")));
        QVERIFY(picker.contains(QStringLiteral(
            "if (categoryChip.selected)\n"
            "                            picker.gif.showTrending()")));

        // Down on the nav row hands off exactly like searchField's own
        // Down/Return handlers do for a provider tab, and is a no-op there.
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
};
QTEST_MAIN(GifPickerRedesignContractTest)
#include "GifPickerRedesignContractTest.moc"
