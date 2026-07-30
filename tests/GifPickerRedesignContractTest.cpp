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
            QVERIFY(block.contains(QStringLiteral("AppTheme.accentSoft")));
            QVERIFY(block.contains(QStringLiteral("AppTheme.accentBorder")));
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
        QVERIFY(picker.contains(QStringLiteral("text: picker.gif.attribution")));
        QVERIFY(!picker.contains(QStringLiteral("Tenor")));
        // The footer is the RowLayout after the "── Footer:" comment through
        // the end of the popup's contentItem: exactly one keycap ("↵ send"),
        // never a second "⇧↵ preview" hint — no preview action exists.
        const int footerStart = picker.indexOf(QStringLiteral("── Footer:"));
        QVERIFY(footerStart >= 0);
        const QString footer = picker.mid(footerStart);
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
        QVERIFY(picker.contains(QStringLiteral("source: tile.previewUrl")));
        QVERIFY(!picker.contains(QStringLiteral("source: tile.gifUrl")));
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
};
QTEST_MAIN(GifPickerRedesignContractTest)
#include "GifPickerRedesignContractTest.moc"
