// Source-scan contract for the hover star on image media (bounded-block style
// of ContextMenuContractTest.cpp). Pins:
//   - gating at least as strict as the old menu rows (confirmed raster
//     mimetype, a fetchable source, bridge support);
//   - instantiation only for eligible rows (Loader { active: ... });
//   - hover via HoverHandler only, never a MouseArea (which would steal
//     wheel or drag gestures over the media);
//   - one activation function reached from a TapHandler (gated on the same
//     "revealed" state as opacity), explicit Keys handlers (ignoring
//     key-repeat) and an Accessible action; never an AbstractButton, whose
//     built-in Space/Return handling could double-fire;
//   - keyboard reachability and an Accessible role/name that follows state;
//   - re-resolution by model.mediaKey through app.starChatGif /
//     GifStarredStore.unstarByMediaKey, never a captured index;
//   - live refresh from starFinished/unstarFinished/countChanged (Clear All
//     and account switches emit only countChanged) and from eligibility flips;
//   - bottom-right placement, clear of the top-right action bar;
//   - image rows only, never stickerComponent.

#include <QtTest/QtTest>

#include <QFile>

class GifHoverStarContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // The hover-star Loader from "id: gifStarLoader" to the next sibling
    // ("Label {", the load-failure caption), so the no-MouseArea check cannot
    // match the whole-image click MouseArea before it.
    static QString starBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: gifStarLoader"));
        if (start < 0) return {};
        const int end = delegate.indexOf(QStringLiteral(
            "Label {\n                anchors.centerIn: parent\n"
            "                width: parent.width - 12"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    // The imageBox block owning eligibility and starred state: from
    // starEligible's declaration to the Skeleton after the two
    // starredStore-facing Connections blocks.
    static QString stateBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(
            QStringLiteral("readonly property bool starEligible:"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("objectName: \"imageSkeleton\""), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

private Q_SLOTS:
    void hoverStarReplacedTheMenuItemEntirely()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"gifHoverStarButton\"")));
        // Never re-added as a dropdown row (also pinned from the menu side).
        QVERIFY(!delegate.contains(QStringLiteral("starGifMenuItem")));
    }

    // The star is offered for any raster format GifStarredStore can store
    // (GIF/PNG/JPEG/WebP, see gif::validateRasterBytes), via
    // imageBox.isRasterImage. isGif still exists and drives GIF-only
    // animation, and is asserted too.
    void eligibilityGatingMatchesTheRemovedMenuItemForImageRows()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("imageBox.isRasterImage")));
        QVERIFY(block.contains(QStringLiteral("model.mediaSourceAvailable === true")));
        QVERIFY(block.contains(QStringLiteral("app.mediaBridge.supported")));
        // isGif is the confirmed-mimetype check and still drives animation.
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool isGif:\n"
            "                (model.mediaMimetype || \"\").toLowerCase() === \"image/gif\"")));
        // isRasterImage accepts exactly the four validated formats, never
        // every image/* (which would admit image/svg+xml).
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool isRasterImage: {\n"
            "                var m = (model.mediaMimetype || \"\").toLowerCase()\n"
            "                return m === \"image/gif\" || m === \"image/png\"\n"
            "                    || m === \"image/jpeg\" || m === \"image/webp\"")));
    }

    // The star is scoped to imageComponent (model.isImage rows); unlike the
    // old mimetype-only menu gate it does not match animated GIF stickers.
    void hoverStarIsScopedToImageRowsNotStickers()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int imageStart = delegate.indexOf(QStringLiteral("id: imageComponent"));
        const int stickerStart = delegate.indexOf(QStringLiteral("id: stickerComponent"));
        QVERIFY(imageStart >= 0 && stickerStart > imageStart);
        const QString imageBlock = delegate.mid(imageStart, stickerStart - imageStart);
        const int stickerEnd = delegate.indexOf(QStringLiteral("// ---- video ----"),
                                                stickerStart);
        QVERIFY(stickerEnd > stickerStart);
        const QString stickerBlock =
            delegate.mid(stickerStart, stickerEnd - stickerStart);
        QVERIFY(imageBlock.contains(QStringLiteral("gifHoverStarButton")));
        QVERIFY(!stickerBlock.contains(QStringLiteral("gifHoverStarButton")));
        QVERIFY(!stickerBlock.contains(QStringLiteral("starEligible")));
        // The narrowing is documented in the source, not silently dropped.
        QVERIFY(delegate.contains(QStringLiteral("DELIBERATE NARROWING")));
        QVERIFY(delegate.contains(QStringLiteral(
            "GifHoverStarContractTest::hoverStarIsScopedToImageRowsNotStickers")));
    }

    void onlyInstantiatedForAConfirmedGifViaALoader()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const int start = delegate.indexOf(QStringLiteral("id: gifStarLoader"));
        QVERIFY(start >= 0);
        // The Loader's declaration head: `active` must gate creation, not only
        // visibility.
        const QString loaderHead = delegate.mid(start, 200);
        QVERIFY(loaderHead.contains(QStringLiteral("active: imageBox.starEligible")));
    }

    void hoverDetectionIsAHoverHandlerNeverAMouseArea()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("HoverHandler { id: starHover }")));
        // "MouseArea {" (an instantiation), not the word, which the block's
        // rationale comments use.
        QVERIFY(!block.contains(QStringLiteral("MouseArea {")));
        // The media-wide hover handler gating visibility is a HoverHandler too.
        QVERIFY(block.contains(QStringLiteral(
            "HoverHandler {\n                            id: gifStarHover\n"
            "                        }")));
    }

    void oneActivationFunctionReachedByTapAndKeyboardAndAccessibility()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        // One place decides what to do; every input path calls it.
        QCOMPARE(block.count(QStringLiteral("function activate()")), 1);
        QVERIFY(block.contains(QStringLiteral("onTapped: gifStarButton.activate()")));
        QVERIFY(block.contains(QStringLiteral(
            "Keys.onReturnPressed: (event) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "Keys.onEnterPressed: (event) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "Keys.onSpacePressed: (event) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "Accessible.onPressAction: gifStarButton.activate()")));
        // Never an AbstractButton instantiation (the block's comment names it
        // to explain why not).
        QVERIFY(!block.contains(QStringLiteral("AbstractButton {")));
    }

    // Held Space/Return must not toggle at key-repeat rate: each repeat is a
    // file write or removal plus a banner.
    void keyboardActivationIgnoresAutoRepeat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QCOMPARE(block.count(QStringLiteral("if (!event.isAutoRepeat) gifStarButton.activate()")),
                 3);
    }

    // The TapHandler's hit-testability matches what is visible. The item stays
    // visible at opacity 0 so Tab can reach it, and a touch tap (no synthetic
    // hover first) on the invisible corner must not save or unsave a GIF. The
    // two conditions must not drift apart.
    void tapHandlerIsGatedOnTheSameVisibilityStateAsOpacity()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "readonly property bool revealed:\n"
            "                                gifStarHover.hovered || starHover.hovered\n"
            "                                || gifStarButton.activeFocus")));
        QVERIFY(block.contains(QStringLiteral(
            "TapHandler {\n"
            "                                enabled: gifStarButton.revealed\n")));
        QVERIFY(!block.contains(QStringLiteral(
            "enabled: gifStarButton.revealed || imageBox.starred")));
    }

    // The star appears on hover or focus only; it is never parked on the
    // media at rest.
    void hoverOnlyNeverParkedOnTheMedia()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("opacity: revealed ? 1 : 0")));
        QVERIFY(!block.contains(QStringLiteral(
            "opacity: (revealed || imageBox.starred) ? 1 : 0")));
        // The one star behaves identically in both places.
        const QString picker = read(QStringLiteral("GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY(picker.contains(QStringLiteral(
            "opacity: tileHover.hovered || tile.current\n"
            "                                 || saveButton.visualFocus ? 1 : 0")));
        QVERIFY(!picker.contains(QStringLiteral(
            "opacity: tile.saved || tileHover.hovered ? 1 : 0")));
    }

    void activationResolvesByMediaKeyNeverAnIndex()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("var key = model.mediaKey || \"\"")));
        // Routes through AppController's two-tier check/action (session-exact
        // fast path plus content-hash fallback; see GifStarredStore's
        // "DURABLE STARRED-STATE DESIGN" comment), not the session-only store
        // methods.
        QVERIFY(block.contains(QStringLiteral("app.isChatGifStarred(key)")));
        QVERIFY(block.contains(QStringLiteral("app.unstarChatGif(key)")));
        QVERIFY(block.contains(QStringLiteral("app.starChatGif(key)")));
        QVERIFY(!block.contains(QStringLiteral(
            "app.gif.starredStore.isStarredThisSession(key)")));
        QVERIFY(!block.contains(QStringLiteral(
            "app.gif.starredStore.unstarByMediaKey(key)")));
        QVERIFY(!block.contains(QStringLiteral("currentIndex")));
    }

    void keyboardReachableWithStateDependentAccessibleName()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("activeFocusOnTab: true")));
        QVERIFY(block.contains(QStringLiteral("Accessible.role: Accessible.Button")));
        QVERIFY(block.contains(QStringLiteral(
            "Accessible.name: imageBox.starred")));
        // One format-neutral verb everywhere: this button and the picker star
        // both lead to the Saved tab.
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Remove from saved\")")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Save image\")")));
        QVERIFY(!block.contains(QStringLiteral("Star GIF")));
        QVERIFY(!block.contains(QStringLiteral("starred GIFs")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Save GIF\")")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Remove from saved GIFs\")")));
    }

    // Saved is shown as a fill, not only a tint (the bundled Material Symbols
    // subset has no filled star), matching the picker's gifTileSaveButton.
    void savedStateIsAFillNotOnlyATint()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "color: imageBox.starred ? AppTheme.bolt")));
        QVERIFY(block.contains(QStringLiteral(
            "color: imageBox.starred\n"
            "                                       ? AppTheme.boltInk : AppTheme.scrimInk")));
        // The old tint-only treatment must not survive.
        QVERIFY(!block.contains(QStringLiteral("AppTheme.presenceAway")));
    }

    // Bottom-right, never top-right: a top-right star can sit under the
    // action bar's higher-z buttons in a narrow column.
    void starIsAnchoredBottomRightNeverTopRight()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        const int buttonStart = block.indexOf(QStringLiteral("id: gifStarButton"));
        QVERIFY(buttonStart >= 0);
        const QString buttonHead = block.mid(buttonStart, 200);
        QVERIFY(buttonHead.contains(QStringLiteral("anchors.bottom: parent.bottom")));
        QVERIFY(buttonHead.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(!buttonHead.contains(QStringLiteral("anchors.top: parent.top")));
    }

    // The starred state refreshes on countChanged too: Clear All and account
    // switches emit only that, and a stale "saved" tile would re-write bytes
    // the user just deleted. This pins the QML half only; the required C++
    // ordering (clear the session map before the model emits) is proven by
    // GifStarredStoreTest::sessionMapIsAlreadyClearedWhenCountChangedFires.
    void starredStateRefreshesFromEveryStoreSignalIncludingCountChanged()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        // isStarredThisSession() has no NOTIFY, so the state is a tracked
        // property refreshed on identity change and on store signals.
        QVERIFY(delegate.contains(QStringLiteral("function refreshStarredState()")));
        const int connStart = delegate.indexOf(
            QStringLiteral("Connections {\n                target: app.gif.starredStore"));
        QVERIFY(connStart >= 0);
        const int connEnd = delegate.indexOf(QStringLiteral("\n            }\n\n            "
                                                             "readonly property string resolvedSource:"),
                                             connStart);
        QVERIFY(connEnd > connStart);
        const QString connBlock = delegate.mid(connStart, connEnd - connStart);
        QVERIFY(connBlock.contains(QStringLiteral("function onStarFinished(")));
        QVERIFY(connBlock.contains(QStringLiteral("function onUnstarFinished(")));
        QVERIFY(connBlock.contains(QStringLiteral("function onCountChanged() {\n"
                                                  "                    imageBox.refreshStarredState()")));
        QVERIFY(connBlock.contains(QStringLiteral("imageBox.refreshStarredState()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onMediaIdentityChanged: {\n"
            "                animatedSource = \"\"\n"
            "                bridgeSource = \"\"\n"
            "                bridgeFailed = false\n"
            "                refreshBridgeSource()\n"
            "                refreshStarredState()\n"
            "            }")));
    }

    // A row created before eligibility was confirmed re-checks when it flips
    // true.
    void eligibilityFlipRefreshesStarredState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(
            QStringLiteral("onStarEligibleChanged: refreshStarredState()")));
    }

    // No star on a pending local echo: bytes keyed by the echo's temporary id
    // could never be matched once the real event (different mediaKey)
    // replaces it.
    void starEligibilityExcludesPendingLocalEchoRows()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("!imageBox.pendingMedia")));
    }
};

QTEST_GUILESS_MAIN(GifHoverStarContractTest)
#include "GifHoverStarContractTest.moc"
