// v0.6.5 (SPEC §1n): source-scan contract for the redesigned GifPicker —
// width 330 with a dynamically-sized 3-column grid (replacing the fixed
// 132px cell that assumed the previous 460px width), the picker's own "GIF"
// header badge (distinct from the composer's pinned composerGifKeycap), the
// pill-styled Trending/Favorites/Recent section chips (with the GIPHY/KLIPY
// provider tabs kept as their own, separate, unchanged row), the retinted
// favorite star + "GIF" tile badge, the stretch byte-size overlay, and the
// "return to send" footer hint. This is a belt-and-suspenders re-pin
// alongside tests/QmlBindingContractTest.cpp's gifPickerWiredIntoBothComposers
// (which owns the authoritative nine-invariant literals); this suite adds the
// redesign-specific structure without loosening any of those.

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
        QVERIFY(picker.contains(QStringLiteral("width: Math.min(330,")));
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

    void sectionChipsAreDistinctFromProviderTabs()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // Provider tabs (GIPHY/KLIPY) keep their own row and existing
        // wiring, untouched.
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifProviderTabs\"")));
        QVERIFY(picker.contains(QStringLiteral("setActiveProvider(value)")));
        // The section nav (Trending/Favorites/Recent) is now bespoke pill
        // chips, not the shared SegmentedControl (whose idle state carries
        // no border, which the spec requires).
        QVERIFY(picker.contains(QStringLiteral("objectName: \"gifSectionTabs\"")));
        {
            const int start = picker.indexOf(QStringLiteral("objectName: \"gifSectionTabs\""));
            const int end = picker.indexOf(QStringLiteral("// ── Category chips"), start);
            QVERIFY(start >= 0 && end > start);
            const QString block = picker.mid(start, end - start);
            QVERIFY(!block.contains(QStringLiteral("SegmentedControl")));
            QVERIFY(block.contains(QStringLiteral("picker.section = modelData.value")));
            QVERIFY(block.contains(QStringLiteral("picker.gif.showTrending()")));
            // Storm skin (§3.7): the selected section chip is the bolt fill
            // with panel ink; resting chips carry the strong storm outline.
            QVERIFY(block.contains(QStringLiteral("AppTheme.bolt")));
            QVERIFY(block.contains(QStringLiteral("AppTheme.stormBorderStrong")));
            // Favorites gets the star glyph via Icon — never the ★ character
            // (IconChromeTest already bans it repo-wide in this file).
            QVERIFY(block.contains(QStringLiteral("icon: \"star\"")));
        }
    }

    void tileBadgesAreLabelsRetintedStarAndOptionalSizeOverlay()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // The "GIF" tile badge and the stretch size overlay are both Label
        // text, never an Icon-name string (IconChromeTest bans the ★
        // character but a badge must still never masquerade as an icon).
        QVERIFY(picker.contains(QStringLiteral("gifTileBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("text: qsTr(\"GIF\")")));
        QVERIFY(picker.contains(QStringLiteral("gifSizeBadgeLabel")));
        QVERIFY(picker.contains(QStringLiteral("picker.formatBytes(tile.gifBytes)")));
        // Favorite star retints to presenceAway (was warning); Accessible
        // name stays state-dependent.
        QVERIFY(picker.contains(QStringLiteral("AppTheme.presenceAway")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"Remove from favorites\")")));
        QVERIFY(picker.contains(QStringLiteral("qsTr(\"Add to favorites\")")));
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
        // v0.6.6 UX rework: the footer text is now conditional (Starred tab
        // shows a "no provider" note instead — see
        // starredTabHasNoAttributionAndNoProviderTraffic below), so this
        // pins the real provider attribution as the ternary's FALSE branch
        // (the leading ": " is load-bearing — a bare substring match would
        // also pass if "picker.gif.attribution" only ever appeared in a
        // comment), bounded to the footer block so it cannot false-positive
        // on anything outside it.
        QVERIFY(footer.contains(QStringLiteral(": picker.gif.attribution")));
        QVERIFY(!footer.contains(QStringLiteral("Tenor")));
        QCOMPARE(footer.count(QStringLiteral("MenuKeycap {")), 1);
        QVERIFY(footer.contains(QStringLiteral("iconName: \"keyboard_return\"")));
        QVERIFY(footer.contains(QStringLiteral("qsTr(\"send\")")));
        QVERIFY(!footer.contains(QStringLiteral("qsTr(\"preview\")")));
        QVERIFY(!footer.contains(QStringLiteral("ShiftModifier")));
    }

    // Belt-and-suspenders re-pin of the nine invariants QmlBindingContractTest
    // already owns — the redesign must not have moved any of these literals.
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
            // v0.6.6: a local-starred tile (rendered on the picker's own
            // Starred tab — see GifStarredStore) has no provider previewUrl/
            // stillUrl — both the static Image and the AnimatedImage branch on
            // tile.provider, but the non-local fallback is still exactly
            // tile.stillUrl / tile.previewUrl. Bounded to the tile delegate
            // block so the negative check below cannot false-positive on
            // anything outside it.
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

    // v0.6.6 UX rework (maintainer feedback): "make a separate tab from
    // klipy and giffy for stared gifs" — Starred is a third peer tab in the
    // SAME row as GIPHY/KLIPY, not folded into Favorites. Pins that the
    // Starred entry never routes through setActiveProvider (no provider
    // network traffic) and that the grid binds GifStarredModel directly.
    void starredIsAThirdPeerTabNextToGiphyAndKlipy()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        const int start = picker.indexOf(QStringLiteral("objectName: \"gifProviderTabs\""));
        QVERIFY(start >= 0);
        const int end = picker.indexOf(QStringLiteral("// ── Row 2:"), start);
        QVERIFY(end > start);
        const QString block = picker.mid(start, end - start);

        // Still the real provider list, plus exactly one appended "starred"
        // entry — never a hand-maintained duplicate of providerIds.
        QVERIFY(block.contains(QStringLiteral("picker.gif.providerIds.map(")));
        QVERIFY(block.contains(QStringLiteral("items.push({")));
        QVERIFY(block.contains(QStringLiteral("value: \"starred\",")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Starred\")")));

        // Selecting "starred" is a distinct branch that NEVER calls
        // setActiveProvider — the only network-triggering entry point on
        // this control — so the Starred tab makes no provider request.
        const int activatedStart = block.indexOf(QStringLiteral("onActivated: (value) => {"));
        QVERIFY(activatedStart >= 0);
        const QString activatedBlock = block.mid(activatedStart);
        const int starredBranch = activatedBlock.indexOf(QStringLiteral("value === \"starred\""));
        QVERIFY(starredBranch >= 0);
        const int elseBranch = activatedBlock.indexOf(QStringLiteral("} else {"), starredBranch);
        QVERIFY(elseBranch > starredBranch);
        const QString ifBranch = activatedBlock.mid(starredBranch, elseBranch - starredBranch);
        // The if-branch's own comment may mention setActiveProvider() by
        // name to explain why it is deliberately NOT called here — the real
        // assertion is that the actual call form never appears in this
        // branch, only in the else branch (checked below).
        QVERIFY(!ifBranch.contains(QStringLiteral("picker.gif.setActiveProvider(")));
        QVERIFY(ifBranch.contains(QStringLiteral("picker.starredTabActive = true")));
        QVERIFY(activatedBlock.mid(elseBranch).contains(
            QStringLiteral("picker.gif.setActiveProvider(value)")));
    }

    void starredTabBindsTheStarredModelDirectlyNotAMergedModel()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // v0.6.6 live-bug fix: `.model()` called a plain, non-invokable C++
        // method from a QML binding — it THROWS, and Qt silently leaves
        // activeModel at its previous value (see QmlBindingContractTest for
        // the full mechanism). `.model` is now a real Q_PROPERTY on
        // GifStarredStore; a text scan alone cannot prove the binding does
        // not throw at runtime — see
        // GifPickerSelectionQmlTest::starredTabBindsTheStarredModelNotResults
        // for the real-engine assertion.
        QVERIFY(picker.contains(QStringLiteral(
            "starredTabActive ? gif.starredStore.model")));
        QVERIFY(!picker.contains(QStringLiteral(
            "starredTabActive ? gif.starredStore.model()")));
        // Favorites reverted to provider favorites ONLY — no merged model
        // class exists any more (GifFavoritesMergedModel was deleted).
        QVERIFY(picker.contains(QStringLiteral(
            "section === \"favorites\" ? gif.favorites")));
        QVERIFY(!picker.contains(QStringLiteral("favoritesAndStarred")));
        QVERIFY(!picker.contains(QStringLiteral("GifFavoritesMergedModel")));
    }

    void starredTabHasNoAttributionAndNoProviderTraffic()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        // No network on the Starred tab: pagination, the busy spinner, and
        // the search field are all explicitly gated off it.
        QVERIFY(picker.contains(QStringLiteral(
            "if (picker.starredTabActive\n"
            "                    || picker.section !== \"browse\" || contentHeight <= 0)")));
        QVERIFY(picker.contains(QStringLiteral(
            "running: !picker.starredTabActive && picker.section === \"browse\"")));
        QVERIFY(picker.contains(QStringLiteral("visible: !picker.starredTabActive")));
        // Own empty-state copy, and no provider attribution string is
        // claimed for the user's own locally-saved Matrix media.
        QVERIFY(picker.contains(QStringLiteral(
            "Hover a GIF in chat and press the star to save it here.")));
        QVERIFY(picker.contains(QStringLiteral(
            "text: picker.starredTabActive")));
        QVERIFY(picker.contains(QStringLiteral("no provider involved")));
    }

    // v0.6.6 review (M1, MUST FIX — a11y regression): the Starred tab hides
    // searchField (the only path that used to hand focus to the grid) along
    // with the section/category chips, so before this fix the grid was
    // entirely keyboard-unreachable there — a real regression versus the
    // old merged Favorites tab, which WAS reachable. Two independent entry
    // points: the grid becomes a direct Tab stop only on this tab, and Down
    // on the provider tab strip hands off to it exactly like searchField's
    // own Down/Return handlers do for every other tab.
    void starredTabGridIsKeyboardReachable()
    {
        const QString picker = read(QStringLiteral(QML_DIR "/GifPicker.qml"));
        QVERIFY(picker.contains(QStringLiteral(
            "activeFocusOnTab: picker.starredTabActive")));
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
        {
            const int start = picker.indexOf(QStringLiteral("objectName: \"gifProviderTabs\""));
            QVERIFY(start >= 0);
            const int end = picker.indexOf(QStringLiteral("// ── Row 2:"), start);
            QVERIFY(end > start);
            const QString providerBlock = picker.mid(start, end - start);
            QVERIFY(providerBlock.contains(QStringLiteral("Keys.onDownPressed: {")));
            QVERIFY(providerBlock.contains(QStringLiteral("grid.forceActiveFocus()")));
            QVERIFY(providerBlock.contains(QStringLiteral(
                "if (picker.starredTabActive) {")));
        }
    }
};
QTEST_MAIN(GifPickerRedesignContractTest)
#include "GifPickerRedesignContractTest.moc"
