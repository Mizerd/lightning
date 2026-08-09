// v0.6.6 UX rework: GIF starring moved from the message context menu to a
// Discord-style hover star overlaid on the GIF media itself. This is a
// source-scan contract (modeled on ContextMenuContractTest.cpp's
// read()/bounded-block style) pinning:
//   - the exact same gating the removed "Star GIF"/"Unstar GIF" menu rows
//     used (confirmed image/gif mimetype, a fetchable source, bridge
//     support) — never weaker;
//   - only instantiated (Loader { active: ... }) for a confirmed GIF, never
//     paying for a HoverHandler/Item/Icon on a plain image row;
//   - hover detection via HoverHandler ONLY inside the star's own block,
//     never a MouseArea (which would grab/steal wheel or drag gestures a
//     user needs to scroll the timeline over a GIF);
//   - a single activation function reached from both a TapHandler (mouse,
//     gated on the same "revealed" state as the visible opacity — never
//     hit-testable while invisible) and explicit Keys handlers (keyboard,
//     ignoring key-repeat), plus an Accessible action for assistive
//     technology — never an AbstractButton, whose own built-in Space/Return
//     handling could double-fire alongside an explicit Keys handler for the
//     same key;
//   - keyboard reachability (activeFocusOnTab) and a proper
//     Accessible.role/name that flips with session-starred state;
//   - re-resolution by model.mediaKey (never a captured index), through the
//     exact same app.starChatGif / GifStarredStore.unstarByMediaKey path the
//     removed menu rows used;
//   - live refresh of the displayed session-starred state from every
//     relevant store signal (starFinished/unstarFinished/countChanged — the
//     last one is what Clear All and an account switch actually emit) and
//     from an eligibility flip, never a one-shot binding that can go stale
//     under a row the chat shell never tears down;
//   - bottom-right placement (not top-right), which the v0.6.6 review found
//     colliding with the top-right message action bar in narrow columns;
//   - the deliberate image-row-only scope (never stickerComponent).

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

    // The hover-star Loader, from "id: gifStarLoader" through its closing
    // "}" pair that ends the Loader block — bounded by the very next
    // sibling ("Label {" that renders the image-unavailable/failed-to-load
    // caption), so a negative assertion (no MouseArea) can never
    // false-positive on the unrelated whole-image click MouseArea that
    // precedes this block.
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

    // The imageBox property/function block that owns eligibility and
    // session-starred state — from starEligible's declaration through the
    // Skeleton that follows the two starredStore-facing Connections blocks.
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
        // Never re-added as a dropdown row (ContextMenuContractTest also
        // pins this from the menu side; re-asserted here so this file
        // stands on its own).
        QVERIFY(!delegate.contains(QStringLiteral("starGifMenuItem")));
    }

    void eligibilityGatingMatchesTheRemovedMenuItemForImageRows()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("imageBox.isGif")));
        QVERIFY(block.contains(QStringLiteral("model.mediaSourceAvailable === true")));
        QVERIFY(block.contains(QStringLiteral("app.mediaBridge.supported")));
        // isGif itself is the same confirmed-mimetype check the removed
        // menu row used directly.
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool isGif:\n"
            "                (model.mediaMimetype || \"\").toLowerCase() === \"image/gif\"")));
    }

    // v0.6.6 review (L1): the removed menu row's gate was mimetype-only, so
    // it also matched an animated GIF STICKER — this hover star, unlike
    // that row, is scoped to imageComponent (model.isImage rows) only. Pin
    // the deliberate narrowing so it can never be silently claimed as
    // "matches the removed menu item exactly" again.
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
        // Documented as a deliberate, accepted gap — not silently dropped.
        QVERIFY(delegate.contains(QStringLiteral("DELIBERATE NARROWING")));
        QVERIFY(delegate.contains(QStringLiteral(
            "GifHoverStarContractTest::hoverStarIsScopedToImageRowsNotStickers")));
    }

    void onlyInstantiatedForAConfirmedGifViaALoader()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const int start = delegate.indexOf(QStringLiteral("id: gifStarLoader"));
        QVERIFY(start >= 0);
        // Bounded to the Loader's own declaration line + the next few —
        // `active` must gate creation, not just visibility.
        const QString loaderHead = delegate.mid(start, 200);
        QVERIFY(loaderHead.contains(QStringLiteral("active: imageBox.starEligible")));
    }

    void hoverDetectionIsAHoverHandlerNeverAMouseArea()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("HoverHandler { id: starHover }")));
        // "MouseArea {" (a real type instantiation), not the bare word — the
        // block's own design-rationale comments legitimately say "never a
        // MouseArea" to explain why a HoverHandler was chosen instead.
        QVERIFY(!block.contains(QStringLiteral("MouseArea {")));
        // The media-wide hover handler that gates the star's visibility is
        // also a HoverHandler, not a MouseArea.
        QVERIFY(block.contains(QStringLiteral(
            "HoverHandler {\n                            id: gifStarHover\n"
            "                        }")));
    }

    void oneActivationFunctionReachedByTapAndKeyboardAndAccessibility()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        // Exactly one place decides what to do; every input path calls it.
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
        // Never an AbstractButton instantiation (the block's own comment
        // legitimately names it while explaining why it was NOT chosen —
        // built-in Space/Return handling there could fire a second time on
        // top of the explicit Keys handlers).
        QVERIFY(!block.contains(QStringLiteral("AbstractButton {")));
    }

    // v0.6.6 review (L2): held Space/Return must not toggle star/unstar at
    // OS key-repeat rate (each repeat is a real file write or QFile::remove
    // plus a banner re-trigger).
    void keyboardActivationIgnoresAutoRepeat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QCOMPARE(block.count(QStringLiteral("if (!event.isAutoRepeat) gifStarButton.activate()")),
                 3);
    }

    // v0.6.6 review (L3): the star stays visible: true (so Tab can reach
    // it) at opacity 0 when not hovered/focused — the TapHandler must not
    // be hit-testable in that state, or a touch tap (no synthetic
    // hover-before-tap the way a mouse gets one) on the invisible corner
    // would silently star/unstar a GIF the user never saw a button on.
    // The TapHandler's hit-testability must match whatever is actually
    // VISIBLE. The Item stays hit-testable at opacity 0 so Tab can reach it,
    // and on a TOUCH input (no synthetic hover-before-tap the way a mouse gets
    // one) a tap on the invisible corner would otherwise silently save/unsave
    // a GIF the user never saw a button on.
    //
    // v0.6.7: v0.6.6 had relaxed this to `|| imageBox.starred`, which was only
    // sound while a saved GIF's star was also VISIBLE at rest. The at-rest
    // visibility is gone (see hoverOnlyNeverParkedOnTheMedia below), so the
    // relaxation had to go with it — the two must not drift apart, which is
    // exactly what this case exists to catch.
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

    // v0.6.7 (maintainer request): the star appears on hover/focus ONLY. It
    // is never parked on the media at rest — v0.6.6 had kept a saved GIF's
    // star permanently visible so its state could be read without hovering,
    // which in practice left a yellow badge sitting on every saved GIF.
    void hoverOnlyNeverParkedOnTheMedia()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("opacity: revealed ? 1 : 0")));
        QVERIFY(!block.contains(QStringLiteral(
            "opacity: (revealed || imageBox.starred) ? 1 : 0")));
        // The one star behaves identically in both places it exists.
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
        // v0.6.6 fix: routes through AppController's durable, two-tier
        // check/action (session-exact fast path + content-hash durable
        // fallback — see GifStarredStore's "DURABLE STARRED-STATE DESIGN"
        // class comment) rather than calling the session-only
        // GifStarredStore methods directly.
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
        // v0.6.7: one verb everywhere. This button and the picker's tile star
        // say the same thing and land in the same place (the Saved tab), so
        // the two-destination "Star"/"Favorite" vocabulary is gone.
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Remove from saved GIFs\")")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Save GIF\")")));
        QVERIFY(!block.contains(QStringLiteral("Star GIF")));
        QVERIFY(!block.contains(QStringLiteral("starred GIFs")));
    }

    // v0.6.7: the bundled Material Symbols subset is a static FILL=0 instance
    // — there is no filled star glyph, so colour alone carried the entire
    // saved/not-saved distinction on a busy GIF. Saved is now a bolt FILL with
    // boltInk ink, exactly matching the picker tile (GifPicker.qml's
    // gifTileSaveButton) so the same state reads identically in both places.
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
        // The old amber-tint-only treatment must not survive.
        QVERIFY(!block.contains(QStringLiteral("AppTheme.presenceAway")));
    }

    // v0.6.6 review (M2): bottom-right, never top-right — a top-right star
    // can sit directly under the message action bar's own top-right,
    // higher-z buttons in a narrow column (the 340px thread panel, or any
    // narrow window) on a continuation row whose media fills the row width.
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

    // v0.6.6 review (H1, MUST FIX — data-at-rest): starFinished/
    // unstarFinished are not the only ways the session-starred answer can
    // change under a row the chat shell never tears down. Settings ->
    // Clear All and an account switch (openFor()/close() ->
    // GifStoredModel::reopen()) both emit ONLY countChanged — without a
    // handler for it, a tile Clear All just deleted from disk kept
    // rendering filled/"Remove from starred GIFs", and activating it would
    // have called app.starChatGif() and RE-WRITTEN bytes the user just
    // explicitly deleted.
    //
    // SCOPE OF THIS TEST: it pins the QML HALF only (that the handler
    // exists and refreshes). The fix also required a C++ ordering change —
    // GifStarredStore::clearAll()/unstar() must clear the session map
    // BEFORE the model mutation that emits countChanged, or this handler
    // recomputes the same stale answer one statement early. That half is
    // proven behaviorally by
    // GifStarredStoreTest::sessionMapIsAlreadyClearedWhenCountChangedFires;
    // a source scan here cannot see it, so do not read a green run of this
    // test alone as proof the Clear All scenario is fixed.
    void starredStateRefreshesFromEveryStoreSignalIncludingCountChanged()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        // isStarredThisSession() carries no NOTIFY QML could bind to, so the
        // displayed state is a plain tracked property refreshed explicitly
        // on identity change and on the store's own feedback signals —
        // never a one-shot binding that could go stale after a toggle.
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

    // v0.6.6 review (L4): a row created before the SDK confirmed
    // mediaSourceAvailable/bridge support must re-check the instant
    // eligibility flips true, rather than staying stuck showing an outline
    // star (or no star at all, once L5's Loader gates on the same flag).
    void eligibilityFlipRefreshesStarredState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(
            QStringLiteral("onStarEligibleChanged: refreshStarredState()")));
    }

    // v0.6.6 fix (A3): starring a GIF still on the wire as a local echo
    // hashes/fetches bytes keyed off the echo's temporary id, then can never
    // be matched again once the echo is replaced by the real remote event
    // (whose mediaKey differs) — the star must not be offered at all until
    // the row is a real, stable event.
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
