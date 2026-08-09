#include <QtTest/QtTest>

#include <QFile>

class QmlBindingContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // The `stateActivity` Item through the sibling `layout` ColumnLayout
    // that follows it in MessageDelegate.qml.
    static QString stateActivityBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: stateActivity"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("\n    ColumnLayout {\n        id: layout"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

private Q_SLOTS:
    void dialogsHaveIndependentBoundedWidths()
    {
        const QString roomInfo = read(QStringLiteral("RoomInfoPanel.qml"));
        const QString account = read(QStringLiteral("AccountMenu.qml"));
        QVERIFY(!roomInfo.isEmpty());
        QVERIFY(!account.isEmpty());
        QVERIFY(roomInfo.contains(QStringLiteral(
            "width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))")));
        QVERIFY(account.contains(QStringLiteral(
            "width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))")));
        QVERIFY(!roomInfo.contains(QStringLiteral("Layout.maximumWidth: 360")));
        QVERIFY(!account.contains(QStringLiteral("Layout.maximumWidth: 380")));
    }

    void roomPreviewIsHardClampedToOneLine()
    {
        // The summary layer normalizes previews, but persisted
        // pre-normalization strings (and future producer bugs) must still
        // never expand a room row: explicit '\n's break lines even with
        // elide set, so the label needs the hard clamp. Plain text keeps a
        // message body from rich-formatting the room list.
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int label =
            delegate.indexOf(QStringLiteral("objectName: \"roomPreviewLabel\""));
        QVERIFY(label >= 0);
        const QString block = delegate.mid(label, 1400);
        QVERIFY(block.contains(QStringLiteral("maximumLineCount: 1")));
        QVERIFY(block.contains(QStringLiteral("wrapMode: Text.NoWrap")));
        QVERIFY(block.contains(QStringLiteral("textFormat: Text.PlainText")));
    }

    void paginationVisibilityDoesNotDependOnGeometry()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("app.pagination.presentationState")));
        QVERIFY(pane.contains(QStringLiteral("PaginationController.Hidden ? 0 : 32")));
        // v0.6.4: the loading / failure indicator is a TOP OVERLAY, not
        // ListView content. As a ListView header its 0<->32 height toggle
        // changed contentHeight and shoved the reader's viewport (and flipped
        // atYBeginning into extra near-top requests) every time pagination
        // started or stopped. The overlay keeps the semantic-state height but
        // never perturbs timeline geometry.
        QVERIFY(!pane.contains(QStringLiteral("header: Item {")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"paginationHeader\"")));
        QVERIFY(pane.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("restoreScrollAnchor(app.currentRoomId)")));
        QVERIFY(pane.contains(QStringLiteral("saveScrollAnchor(")));
        QVERIFY(pane.contains(QStringLiteral("eventIdAtViewRow(row)")));

        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("jumpToEvent(model.replyToEventId")));
        QVERIFY(delegate.contains(QStringLiteral("highlightedEventId")));
        QVERIFY(pane.contains(QStringLiteral("viewportFillCheckScheduled")));
        QVERIFY(pane.contains(QStringLiteral("Qt.callLater(function()")));
        QVERIFY(pane.contains(QStringLiteral("app.pagination.requestViewportFill()")));
        // v0.6.4: near-top pagination is EDGE-triggered with hysteresis, so a
        // reader sitting near the top cannot re-send userInitiated requests
        // every frame and spin the zero-progress loop. The user-scroll trigger
        // sites go through checkNearTopEdge (latched between an enter and a
        // wider exit band); the passive atYBeginning fill trigger stays.
        QVERIFY(pane.contains(QStringLiteral("function checkNearTopEdge(")));
        QVERIFY(pane.contains(QStringLiteral("nearTopArmed")));

        // v0.7.3: every near-top proximity comparison is measured against
        // distanceFromTop(), never against raw contentY. contentY is an offset
        // from originY, and originY is arbitrary and MOVES as history loads.
        // MEASURED: it sat at ~+2484 in the offscreen fixture with the reader at
        // the very top, so `contentY <= height/2` was permanently FALSE there.
        // INFERRED from the live trace: it sat far enough the other way that raw
        // contentY stayed inside the band through all of loaded history, making
        // the enter band permanently true and the exit band unreachable, so the
        // gesture-settle re-arm fired after EVERY gesture in either direction and
        // each one bought four more pagination batches — the reported "it keeps
        // loading old messages each time I scroll up ... and down".
        //
        // A geometry test cannot police this on its own: whether the two
        // measures disagree depends on where originY happens to sit in the
        // fixture, so a fixture with a small originY would pass either way. This
        // scan is the mechanism-level guard — the comparison sites themselves —
        // and no choice of fixture geometry can make it vacuous.
        QVERIFY(pane.contains(QStringLiteral("function distanceFromTop()")));
        // The rotation moved the top of history to the HIGH bound, so the
        // distance is measured from wheelMaxY(). The property under test is
        // unchanged: a distance against a bound, never raw contentY.
        QVERIFY(pane.contains(QStringLiteral(
            "return wheelMaxY() - contentY")));
        // The bands are DISTANCES now, and are named so. The old ...Y names
        // invited exactly the frame confusion above; forbid their return.
        QVERIFY(pane.contains(QStringLiteral("nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("nearTopExitDistance")));
        QVERIFY(!pane.contains(QStringLiteral("nearTopEnterY")));
        QVERIFY(!pane.contains(QStringLiteral("nearTopExitY")));
        for (const QString &raw :
                 { QStringLiteral("contentY <= nearTopEnterDistance"),
                   QStringLiteral("contentY >= nearTopExitDistance"),
                   QStringLiteral("contentY < nearTopEnterDistance"),
                   QStringLiteral("contentY > nearTopExitDistance") }) {
            QVERIFY2(!pane.contains(raw),
                     qPrintable(QStringLiteral(
                         "near-top proximity compared against raw contentY "
                         "(\"%1\"); contentY is an offset from a moving originY, "
                         "so this is not a proximity test — use "
                         "distanceFromTop()").arg(raw)));
        }
        // Both bands, and the gesture-settle re-arm, read the corrected measure.
        QVERIFY(pane.contains(QStringLiteral("fromTop <= nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop >= nearTopExitDistance")));
        QVERIFY(pane.contains(QStringLiteral(
            "<= timeline.nearTopEnterDistance")));
        // The progress gate lives at the DISPATCH site, not on the settle
        // re-arm. Guarding only the re-arm was wrong twice over: an upward
        // gesture re-armed the latch and the next DOWNWARD gesture consumed it
        // and fetched, and a reader parked at the exact top could never re-arm
        // because contentY is at its minimum there and cannot decrease. Both
        // conditions live in the distanceFromTop() frame.
        QVERIFY(pane.contains(QStringLiteral("nearTopRequestDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop <= 1")));
        QVERIFY(pane.contains(QStringLiteral(
            "fromTop < nearTopRequestDistance - 1")));
        // The baseline must RATCHET to the closest approach, on every in-band
        // sample — not merely record the distance at the last dispatch. Without
        // this, everything between the top and the dispatch point stays unpaid
        // and a later downward sample fetches, which is the reported defect
        // surviving its own fix.
        QVERIFY(pane.contains(QStringLiteral(
            "if (fromTop < nearTopRequestDistance)")));
        QVERIFY2(!pane.contains(QStringLiteral("nearTopRequestY")),
                 "the progress baseline must not live in the raw contentY frame");
        QVERIFY(!pane.contains(QStringLiteral(
            "readonly property int paginationState")));
        QVERIFY(!pane.contains(QStringLiteral("showPaginationStatus")));
        QVERIFY(!pane.contains(QStringLiteral(
            "height: paginationHeader.visible ? paginationHeader.implicitHeight")));
    }

    // v0.7: the loading presentation is the shared Skeleton, and it keys off
    // BOTH renderers' actual status (static Image and AnimatedImage), so a
    // ready GIF frame can never sit behind a lingering placeholder. Delegate
    // reuse still resets media identity.
    void animatedGifSkeletonUsesActiveRendererState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"imageSkeleton\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: img.status !== Image.Ready")));
        QVERIFY(delegate.contains(QStringLiteral(
            "animatedImg.status !== AnimatedImage.Ready")));
        QVERIFY(delegate.contains(QStringLiteral(
            "playing: imageBox.animateGif && root.rowOnScreen")));
        QVERIFY(delegate.contains(QStringLiteral("onMediaIdentityChanged")));
    }

    // v0.6.1: the GIF picker is wired to app.gif in both composers, presents
    // provider tabs / search / categories / a result grid / state overlays /
    // attribution, animates previews only while visible, and never renders the
    // sendable original GIF as a grid tile (previews use the small variant).
    void gifPickerWiredIntoBothComposers()
    {
        const QString room = read(QStringLiteral("MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(room.contains(QStringLiteral("GifPicker {")));
        QVERIFY(room.contains(QStringLiteral("target: \"room\"")));
        QVERIFY(room.contains(QStringLiteral("root.openGifPicker()")));
        QVERIFY(room.contains(QStringLiteral("app.gif.available")));
        QVERIFY(thread.contains(QStringLiteral("GifPicker {")));
        QVERIFY(thread.contains(QStringLiteral("target: \"thread\"")));
        QVERIFY(thread.contains(QStringLiteral("threadGifButton")));

        const QString picker = read(QStringLiteral("GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        // Provider tabs + attribution follow the ACTIVE provider.
        QVERIFY(picker.contains(QStringLiteral("picker.gif.providerIds")));
        QVERIFY(picker.contains(QStringLiteral("setActiveProvider(value)")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.attribution")));
        // Debounced search + categories + pagination through the controller.
        QVERIFY(picker.contains(QStringLiteral("gif.setQueryText(text)")));
        QVERIFY(picker.contains(QStringLiteral("gif.openCategory(modelData)")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.loadMore()")));
        // Grid binds to the active section model (results / favorites / recent).
        QVERIFY(picker.contains(QStringLiteral("model: picker.activeModel")));
        QVERIFY(picker.contains(QStringLiteral("gif.favorites")));
        QVERIFY(picker.contains(QStringLiteral("gif.recent")));
        // v0.6.6 UX rework: Starred is a third tab next to GIPHY/KLIPY
        // (never merged into Favorites — see GifPickerRedesignContractTest
        // for the full tab-wiring pin), bound directly to GifStarredStore's
        // own model.
        // v0.6.6 live-bug fix: `gif.starredStore.model()` — calling a plain
        // C++ method (not Q_INVOKABLE/a property) from a QML binding —
        // THROWS. Qt catches the exception in QQmlBinding::update and
        // leaves activeModel at its previous value, so the Starred tab
        // silently kept rendering whatever activeModel was bound to before
        // (GIPHY trending), with every other tab element (header, footer,
        // hidden search) correctly switched. GifStarredStore::model is now a
        // real Q_PROPERTY (see GifStarredStore.h), read as a property, never
        // called as a function. A pure text scan cannot prove the binding
        // does not throw at runtime — see
        // GifPickerSelectionQmlTest::starredTabBindsTheStarredModelNotResults
        // for the real-engine assertion that actually exercises this path.
        QVERIFY(picker.contains(QStringLiteral("gif.starredStore.model")));
        QVERIFY(!picker.contains(QStringLiteral("gif.starredStore.model()")));
        QVERIFY(!picker.contains(QStringLiteral("favoritesAndStarred")));
        // v0.7 regression (live bug): SENDING must resolve the clicked row
        // against the model the user is looking at. Reading gif.results in
        // choose() sent the first Trending item when a favorite was
        // clicked. The chosen record must carry its own provider-qualified
        // identity; an unidentifiable row is dropped, never substituted.
        //
        // v0.7 follow-up (live bug, thread composer): a stale currentIndex
        // or a debounce-racing Enter could still resolve a DIFFERENT model
        // row than the one the user acted on, even though every row was
        // individually identified correctly. choose() now accepts either an
        // already-captured result map (the mouse path hands over the exact
        // delegate's own snapshot(), so it can never drift) or a bare row
        // number (the keyboard path, resolved against activeModel
        // immediately in the same call — never stored for later). Pin the
        // STRONGER contract at each of its three points instead of one
        // brittle whole-line literal.
        {
            // (1) The keyboard path still resolves a row against
            // activeModel — never gif.results directly, so a numeric
            // activation can never cross into the wrong section's model.
            const int chooseStart =
                picker.indexOf(QStringLiteral("function choose(resultOrRow)"));
            const int chooseEnd = picker.indexOf(
                QStringLiteral("property int cfgRevision: 0"), chooseStart);
            QVERIFY(chooseStart >= 0 && chooseEnd > chooseStart);
            const QString chooseBlock =
                picker.mid(chooseStart, chooseEnd - chooseStart);
            QVERIFY(chooseBlock.contains(
                QStringLiteral("activeModel.get(resultOrRow)")));
            QVERIFY(!chooseBlock.contains(QStringLiteral("gif.results.get(")));
            QVERIFY(chooseBlock.contains(QStringLiteral(
                "if (!result || !result.provider || !result.gifId)")));
            QVERIFY(chooseBlock.contains(
                QStringLiteral("picker.gifChosen(result)")));
        }
        // (2) The mouse path hands choose() a captured snapshot, never a
        // bare index the picker would have to re-resolve later.
        QVERIFY(picker.contains(
            QStringLiteral("picker.choose(tile.snapshot())")));
        QVERIFY(picker.contains(QStringLiteral("function snapshot()")));
        {
            // (3) The snapshot itself carries provider-qualified identity —
            // an unidentifiable row is still dropped by the choose() guard
            // above, never substituted.
            const int snapStart =
                picker.indexOf(QStringLiteral("function snapshot()"));
            const int snapEnd =
                picker.indexOf(QStringLiteral("Rectangle {"), snapStart);
            QVERIFY(snapStart >= 0 && snapEnd > snapStart);
            const QString snapshotBlock =
                picker.mid(snapStart, snapEnd - snapStart);
            QVERIFY(snapshotBlock.contains(
                QStringLiteral("provider: tile.provider")));
            QVERIFY(snapshotBlock.contains(
                QStringLiteral("gifId: tile.gifId")));
        }
        QVERIFY(!picker.contains(
            QStringLiteral("picker.gifChosen(gif.results.get(row))")));
        // v0.7 follow-up: the search field's Enter must never fall through
        // to a stale row while setQueryText()'s debounce is still pending —
        // it flushes the query and hands off focus to the grid, exactly
        // like the existing Down-arrow hand-off, and sends nothing itself.
        {
            const int returnStart = picker.indexOf(
                QStringLiteral("Keys.onReturnPressed: {"));
            const int returnEnd =
                picker.indexOf(QStringLiteral("IconButton {"), returnStart);
            QVERIFY(returnStart >= 0 && returnEnd > returnStart);
            const QString searchReturnBlock =
                picker.mid(returnStart, returnEnd - returnStart);
            QVERIFY(!searchReturnBlock.contains(QStringLiteral("picker.choose(")));
            QVERIFY(searchReturnBlock.contains(
                QStringLiteral("picker.gif.searchNow(searchField.text)")));
            QVERIFY(searchReturnBlock.contains(
                QStringLiteral("grid.forceActiveFocus()")));
        }
        // v0.7 follow-up: a highlighted row is plain mutable state that
        // outlives a model replacement (Qt does not remap it), so it must
        // be invalidated whenever the model contents are reset underneath
        // the grid — otherwise a later Return could resolve against
        // unrelated content that landed after a debounced search response.
        QVERIFY(picker.contains(QStringLiteral(
            "function onModelReset() { grid.currentIndex = -1 }")));
        // Favorite toggles without sending; recents are handed off in Phase 7.
        QVERIFY(picker.contains(QStringLiteral("gif.toggleFavorite(")));
        // Previews animate only while visible (offscreen/hidden → paused).
        QVERIFY(picker.contains(QStringLiteral("playing: picker.visible")));
        // Tiles use the PREVIEW variant, never the sendable original gifUrl.
        {
            // v0.6.6: a local-starred tile (provider === "local", rendered
            // on the picker's own Starred tab — see GifStarredStore) has no
            // provider previewUrl/stillUrl, so both the static Image and
            // the AnimatedImage branch on tile.provider now — the non-local
            // fallback is still exactly tile.stillUrl / tile.previewUrl,
            // never tile.gifUrl anywhere in the tile delegate.
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
            // tile.gifUrl DOES legitimately appear once, in snapshot()'s
            // "gifUrl: tile.gifUrl," field capture (send-time identity, not
            // a rendered source) — scoped to an actual `source:` binding.
            QVERIFY(!tileBlock.contains(QStringLiteral("source: tile.gifUrl")));
        }
        // State overlays cover missing-key / offline / rate-limit / error.
        QVERIFY(picker.contains(QStringLiteral("GifSearchController.MissingKey")));
        QVERIFY(picker.contains(QStringLiteral("GifSearchController.RateLimited")));
        // Keyboard: Enter/Escape/arrow navigation.
        QVERIFY(picker.contains(QStringLiteral("Keys.onReturnPressed")));
        QVERIFY(picker.contains(QStringLiteral(
            "Popup.CloseOnEscape | Popup.CloseOnPressOutside")));

        // Selecting a GIF routes to the destination-captured send pipeline:
        // the room composer to the room, the thread composer into the thread.
        QVERIFY(room.contains(QStringLiteral(
            "app.gifSend.sendToRoom(app.currentRoomId, result)")));
        QVERIFY(thread.contains(QStringLiteral("app.gifSend.sendToThread(")));
        QVERIFY(thread.contains(QStringLiteral("app.thread.rootEventId")));
        // The picker itself never sends to a room/thread directly.
        QVERIFY(!picker.contains(QStringLiteral("sendToRoom")));
        QVERIFY(!picker.contains(QStringLiteral("sendTextMessage")));
    }

    // v0.6.1: GIF autoplay is a tri-state (Always/OnHover/Never) honored by the
    // timeline and the picker, configured in Settings alongside safe-search,
    // provider, recents and clear actions with a privacy disclosure.
    void gifAutoplayAndSettingsWired()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString picker = read(QStringLiteral("GifPicker.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        // Timeline honors the tri-state (Never = static, OnHover = hover-gated).
        QVERIFY(delegate.contains(QStringLiteral("gifMode: app.settings.gifAutoplay")));
        QVERIFY(delegate.contains(QStringLiteral("gifMode === 0 || gifHovered")));
        QVERIFY(delegate.contains(QStringLiteral("app.settings.gifAutoplay !== 2")));
        // Picker previews honor it too.
        QVERIFY(picker.contains(QStringLiteral("app.settings.gifAutoplay")));
        // Settings expose the controls, bound to the settings model.
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifAutoplay = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifSafeSearch = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifPreferredProvider = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.storeRecentGifs = checked")));
        // Honest provider availability + privacy disclosure + confirmed clears.
        QVERIFY(settings.contains(QStringLiteral("providerConfigured(\"giphy\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "GIF searches are sent directly to the ")));
        QVERIFY(settings.contains(QStringLiteral("gifClearConfirm.open(\"favorites\")")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.favorites.clearAll()")));
        // v0.6.6 (review HIGH-2): the client-local starred-GIF store gets
        // its own visible count/size row and confirmed Clear All — never
        // folded into the Favorites/Recents clear actions above, since it
        // holds actual decrypted file bytes on this device rather than
        // small provider-CDN metadata rows.
        QVERIFY(settings.contains(QStringLiteral("objectName: \"starredGifsSummaryLabel\"")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.count")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.totalBytes")));
        QVERIFY(settings.contains(QStringLiteral("kept on this ")));
        // The copy must disclose the sign-out consequence (review finding:
        // sign-out deletes the store; "kept on this device only" alone
        // would read as a durability promise).
        QVERIFY(settings.contains(QStringLiteral("device only and removed ")));
        QVERIFY(settings.contains(QStringLiteral(
            "when you sign out of this ")));
        QVERIFY(settings.contains(QStringLiteral("starredGifsClearConfirm.open()")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.clearAll()")));
    }

    // v0.6.1: the thread root uses the Element-style summary card wired to the
    // SDK thread-summary roles, and activating it opens the real thread. The
    // old plain-text "reply(s) in thread" link and the redundant "· in thread"
    // reply label are gone.
    void threadRootUsesSummaryCard()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("ThreadSummaryCard {")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: model.isThreadRoot === true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "replyCount: model.threadReplyCount")));
        QVERIFY(delegate.contains(QStringLiteral(
            "latestKind: model.threadLatestKind")));
        QVERIFY(delegate.contains(QStringLiteral(
            "latestSender: model.threadLatestSenderDisplayName")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onActivated: app.thread.openThread(")));
        // The noisy legacy presentation must not come back.
        QVERIFY(!delegate.contains(QStringLiteral("reply(s) in thread")));
        QVERIFY(!delegate.contains(QStringLiteral("· in thread")));

        // The card renders a thread icon, elides its preview, never plays a
        // full GIF (still label only), and is keyboard-activable + accessible.
        const QString cardQml = read(QStringLiteral("ThreadSummaryCard.qml"));
        QVERIFY(!cardQml.isEmpty());
        QVERIFY(cardQml.contains(QStringLiteral("signal activated()")));
        // Design shell: interface chrome uses a Material Symbols glyph, never
        // an inline Canvas/SVG vector path.
        QVERIFY(cardQml.contains(QStringLiteral("name: \"forum\"")));
        QVERIFY(!cardQml.contains(QStringLiteral("Canvas {")));
        QVERIFY(cardQml.contains(QStringLiteral("elide: Text.ElideRight")));
        QVERIFY(cardQml.contains(QStringLiteral("maximumLineCount: 1")));
        QVERIFY(cardQml.contains(QStringLiteral("Keys.onReturnPressed")));
        QVERIFY(cardQml.contains(QStringLiteral("Keys.onSpacePressed")));
        QVERIFY(cardQml.contains(QStringLiteral("Accessible.role: Accessible.Button")));
        QVERIFY(cardQml.contains(QStringLiteral("qsTr(\"GIF\")")));
        QVERIFY(cardQml.contains(QStringLiteral("qsTr(\"Encrypted reply\")")));
        QVERIFY(cardQml.contains(QStringLiteral("qsTr(\"Message removed\")")));
        // Count is never invented: number only when the SDK count is > 0.
        QVERIFY(cardQml.contains(QStringLiteral(
            "replyCount > 0 ? qsTr(\"%n reply(s)\", \"\", replyCount)")));
    }

    void stateActivityUsesNeutralGroupedPresentation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("stateGroupEntries")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !root.isVirtualRow && !root.isStateActivity")));
        // No message-bubble-like card: the old Rectangle+cardElevated+border
        // treatment for the collapsed row must be gone, replaced with a
        // compact, clickable summary row.
        const QString activity = read(QStringLiteral("RoomActivityDelegate.qml"));
        QVERIFY(!activity.contains(QStringLiteral("AppTheme.cardElevated")));
        QVERIFY(activity.contains(QStringLiteral("summaryRow")));
        QVERIFY(activity.contains(QStringLiteral("modelData.description")));
        QVERIFY(activity.contains(QStringLiteral("model: expandedColumn.visible ? root.entries")));
        QVERIFY(!activity.contains(QStringLiteral("linkPreviews")));
        QVERIFY(!activity.contains(QStringLiteral("messageActions")));
    }

    // 0.5.14 checkpoint 2: clicking Expand did nothing because the summary
    // row referenced the bare `ListView.view` attached property, which is
    // only populated on the delegate's own root item, not on nested
    // children — every other action in this same file correctly qualifies
    // with `root.ListView.view`. Pin that convention for the state-activity
    // controls specifically, since that's exactly where it regressed.
    void stateActivityQualifiesListViewViewOnNestedControls()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateActivityBlock(delegate);
        QVERIFY(!block.isEmpty());
        // The delegate now reaches its host through root.timelineView,
        // which resolves the attached view ONCE in the delegate root's own
        // scope. The hazard this test exists for is unchanged and was
        // confirmed live: an attached property referenced from a nested
        // object attaches to THAT object, where the view is never populated,
        // and fails silently. One such reference (inside a Timer) disabled
        // the whole exact-height cache. So no nested block may name an
        // attached view at all.
        QVERIFY(block.count(QStringLiteral("root.timelineView")) > 0);
        QCOMPARE(block.count(QStringLiteral("ListView.view")), 0);
        QCOMPARE(block.count(QStringLiteral("TableView.view")), 0);
    }

    void roomActivitySettingIsPresentationOnly()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "!isRoutineActivity || app.settings.showRoomActivity")));
        // v0.6.0: the zero-height presentation filter also covers the
        // thread panel's pinned-root suppression — same mechanism, still
        // presentation-only.
        QVERIFY(delegate.contains(QStringLiteral("naturalImplicitHeight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "(!roomActivityVisible || suppressedAsThreadRoot) ? 0")));
        QVERIFY(settings.contains(QStringLiteral("Show room activity")));
        QVERIFY(settings.contains(QStringLiteral(
            "onToggled: app.settings.showRoomActivity = checked")));
    }

    // v0.6.0 checkpoint 8: the unable-to-decrypt placeholder exposes a
    // manual Retry (through the view-provided timeline model, so it works
    // in the thread panel too) and a Security settings jump — and never
    // renders raw session/ciphertext fields.
    void undecryptableRowsExposeRetryAndSecurityActions()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("Retry decryption")));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineModel.retryDecryption()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "app.showSettingsSection(\"security\")")));
        // No ciphertext/session-id MODEL fields are ever bound (the word in
        // a comment is fine; a binding would be model.<field>).
        QVERIFY(!delegate.contains(QStringLiteral("model.sessionId")));
        QVERIFY(!delegate.contains(QStringLiteral("model.ciphertext")));
    }

    // v0.6.0 checkpoint 9: the Sessions card lists devices read-only with
    // honest trust labels, no destructive remote sign-out (limitation is
    // stated), and never binds token-like fields.
    void sessionsCardIsReadOnlyAndHonest()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral(
            "onClicked: app.refreshSessionDevices()")));
        QVERIFY(settings.contains(QStringLiteral("model: app.sessionDevices")));
        QVERIFY(settings.contains(QStringLiteral("This session")));
        QVERIFY(settings.contains(QStringLiteral("Not verified")));
        QVERIFY(settings.contains(QStringLiteral("is not supported yet")));
        QVERIFY(!settings.contains(QStringLiteral("accessToken")));
        QVERIFY(!settings.contains(QStringLiteral("access_token")));
    }

    // v0.6.0 checkpoint 10: the recovery input is masked, accepts key or
    // passphrase, is wiped immediately after dispatch, and a successful
    // recovery re-reads SDK trust/backup state. No new-backup or
    // cross-signing SETUP button is faked (UIA limitation documented).
    void recoveryInputIsMaskedClearedAndHonest()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("echoMode: TextInput.Password")));
        QVERIFY(settings.contains(QStringLiteral("Recovery key or passphrase")));
        QVERIFY(settings.contains(QStringLiteral("recoveryField.text = \"\"")));
        QVERIFY(settings.contains(QStringLiteral("app.refreshCryptoHealth()")));
        QVERIFY(!settings.contains(QStringLiteral("Set up backup")));
        QVERIFY(!settings.contains(QStringLiteral("Set up cross-signing")));
    }

    void unreadNavigationUsesSdkMarkerAndBottomThreshold()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"unreadDivider\"")));
        QVERIFY(delegate.contains(QStringLiteral("qsTr(\"New messages\")")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"jumpToLatestButton\"")));
        QVERIFY(pane.contains(QStringLiteral("!timeline.stickToBottom")));
        // Bottom-follow is latched to user intent via a small slack (a reader
        // who scrolls up is never re-pinned by proximity), replacing the wide
        // 40px window that snapped the view back to the newest message.
        QVERIFY(pane.contains(QStringLiteral("function atBottomEdge()")));
        QVERIFY(pane.contains(QStringLiteral("wheelMinY() + bottomFollowSlack")));
        QVERIFY(!pane.contains(QStringLiteral("contentHeight - 40")));
        QVERIFY(!pane.contains(QStringLiteral("contentY + height === contentHeight")));
    }

    // Native-touchpad architecture: the deferred anchor correction must NEVER
    // run while the user's gesture owns the position, and the touchpad hot path
    // must NOT do per-delta geometry work.
    //   * The pixelDelta branch must NOT call captureViewAnchor() — the old
    //     per-delta indexAt/itemAtIndex/stableIdAt scan is gone; it must keep
    //     the scroll session alive with scrollSettleTimer.restart().
    //   * maintainViewAnchor() must BRANCH on userScrollActive: mid-gesture
    //     it applies a RELATIVE growth delta (round 3 — content resizing
    //     above the reader must not throw the view, the "an image pops in
    //     and I jump" defect), while the ABSOLUTE restore to the captured
    //     offset stays idle-only, since only an absolute write can disagree
    //     with where the gesture has since moved the view and fight it.
    //   * The anchor is (re)captured once the gesture settles — on the mouse
    //     path via onWheelMotionSettled and universally via scrollSettleTimer.
    // The offscreen QPA does not incubate ListView delegates, so the pixel
    // outcome is only provable on a physical touchpad; this scan guards the
    // wiring so a future edit cannot silently reintroduce the mid-gesture
    // absolute-write fight, nor drop the relative growth compensation.
    void touchpadScrollUsesTwoModeAnchorMaintenance()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));

        const int pixelBranch =
            pane.indexOf(QStringLiteral("if (event.pixelDelta.y !== 0)"));
        QVERIFY(pixelBranch >= 0);
        const int angleBranch = pane.indexOf(
            QStringLiteral("else if (event.angleDelta.y !== 0)"), pixelBranch);
        QVERIFY(angleBranch > pixelBranch);
        QString touchpad = pane.mid(pixelBranch, angleBranch - pixelBranch);
        // Strip comment lines before scanning: the branch's comment
        // legitimately names captureViewAnchor() when explaining where the
        // full re-derivation does happen (at settle), but a BARE
        // `captureViewAnchor()` call would resolve through the ListView's
        // scope chain and silently reintroduce the per-delta scan — so the
        // scan must stay broad enough to catch that, and precise enough not
        // to trip on prose.
        {
            QStringList codeOnly;
            const QList<QStringView> touchpadLines =
                QStringView(touchpad).split(QLatin1Char('\n'));
            for (const QStringView &line : touchpadLines) {
                if (!line.trimmed().startsWith(QLatin1String("//")))
                    codeOnly << line.toString();
            }
            touchpad = codeOnly.join(QLatin1Char('\n'));
        }
        // No per-delta anchor scan on the touchpad hot path.
        QVERIFY(!touchpad.contains(QStringLiteral("captureViewAnchor()")));
        // The session stays alive for the whole gesture.
        QVERIFY(touchpad.contains(QStringLiteral("scrollSettleTimer.restart()")));

        // The deferred correction is gated on the scroll session. The
        // scanned region starts at the userScrollActive branch, NOT at the
        // function declaration: the displaced-anchor resolve above it
        // legitimately calls positionViewAtIndex (an absolute view move used
        // purely to materialise a destroyed delegate, with the position
        // restored immediately after), and including it would make the
        // "no absolute write" assertion below false for the wrong reason.
        const int maintain =
            pane.indexOf(QStringLiteral("function maintainViewAnchor()"));
        QVERIFY(maintain >= 0);
        const int guardStart =
            pane.indexOf(QStringLiteral("if (userScrollActive) {"), maintain);
        QVERIFY(guardStart > maintain);
        const int maintainEnd =
            pane.indexOf(QStringLiteral("desired = anchorY + viewAnchorOffset"),
                         guardStart);
        QVERIFY(maintainEnd > guardStart);
        const QString maintainGuard =
            pane.mid(guardStart, maintainEnd - guardStart);
        // The mid-gesture path writes NOTHING. Applying the anchor delta
        // while input owns the viewport was tried twice and rejected twice by
        // physical testing — the second time with real measured heights and
        // with translateActiveMotion() carrying the wheel target along, so
        // neither "the quantity was noise" nor "the engine drove it back out"
        // explains it. anchorY moves both when rows resize under the reader
        // and when the view re-anchors its own loaded rows, and the raw delta
        // cannot tell those apart; feeding it into contentY pulled the reader
        // up and down, including with nothing loading at all.
        // The two contentY scans carry the whole contract; a scan for
        // translateActiveMotion would only match the comment in that branch
        // recording why the write was removed, which is worth keeping.
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY +=")));
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY =")));

        // userScrollActive covers the touchpad path via the settle timer, since
        // moving/wheelAnimating are both false there.
        QVERIFY(pane.contains(QStringLiteral(
            "moving || wheelAnimating || scrollSettleTimer.running")));

        // The anchor is captured on gesture settle (mouse path + settle timer).
        const int settled =
            pane.indexOf(QStringLiteral("function onWheelMotionSettled()"));
        QVERIFY(settled >= 0);
        const int settledEnd =
            pane.indexOf(QStringLiteral("scrollSettleTimer.restart()"), settled);
        QVERIFY(settledEnd > settled);
        const QString settledBlock = pane.mid(settled, settledEnd - settled);
        QVERIFY(settledBlock.contains(QStringLiteral("timeline.captureViewAnchor()")));
    }

    void directPreviewUsesControlledSourceAndOriginalUrlActivation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("previewImageSource")));
        QVERIFY(delegate.contains(QStringLiteral("previewAnimatedSource")));
        QVERIFY(delegate.contains(QStringLiteral("app.media.openWebUrl(card.p.url)")));
        QVERIFY(!delegate.contains(QStringLiteral("source: card.p.imageSource")));
        QVERIFY(!delegate.contains(QStringLiteral("openWebUrl(card.previewStatic")));
        QVERIFY(!delegate.contains(QStringLiteral("openWebUrl(card.previewAnimation")));
    }

    void messagesUseOneLeftAlignedSenderPresentation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"messagePresentationRow\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property real avatarGutterWidth: compactMode ? 8")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool showsIdentity: model.showSenderIdentity === true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "mxc: model.senderAvatarMxc || \"\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "name: model.senderDisplayName || model.senderInitials")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"senderName\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"senderTimestamp\"")));

        // Current-user status may affect metadata and permissions, never
        // horizontal flow in the Modern rows. The design's Bubbles mode is
        // the ONLY colored-bubble path and it must stay gated to
        // direct-message timelines behind the message-layout setting.
        QVERIFY(!delegate.contains(QStringLiteral("Qt.AlignRight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? (model.isOwn === true ? AppTheme.ownBubble")));
        // The Modern/Compact bubble stays transparent by default; the only
        // tint outside Bubbles is the sender-NEUTRAL mention highlight
        // (applies to any sender, never an own-message color or alignment
        // change). The wash base is the routed mentionHighlight token —
        // accent under legacy themes, the mention rose under Storm.
        QVERIFY(delegate.contains(QStringLiteral(
            "? Qt.alpha(AppTheme.mentionHighlight, 0.05)")));
        QVERIFY(delegate.contains(QStringLiteral(": \"transparent\"")));
        QVERIFY(delegate.contains(QStringLiteral("? AppTheme.radiusSm : 0")));
    }

    void continuationRowsStayCompactAndActionsFloat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!pane.isEmpty());

        // Continuations must not retain the former unconditional 36px avatar
        // height or a permanent timestamp line below the body.
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitHeight: root.showsIdentity ? 34 : bodyLabel.implicitHeight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"continuationTimestamp\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !root.showsIdentity && rowHover.hovered")));
        QVERIFY(delegate.contains(QStringLiteral("return \"\"")));
        QVERIFY(!delegate.contains(QStringLiteral(
            "return model.showSenderIdentity === true ? \"\" : ts")));

        // The toolbar overlays the unused right edge instead of taking a
        // RowLayout cell and narrowing the message column on hover.
        QVERIFY(delegate.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("spacing: 0")));
    }

    void wrappedBodiesHaveStableIncubationWidths()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(delegate.contains(QStringLiteral("bubble.width > 8")));
        QVERIFY(delegate.contains(QStringLiteral(": 560")));
        QVERIFY(pane.contains(QStringLiteral("available > 0 ? available : 640")));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"messageBody\"")));
    }

    void previewsAndMediaUseBoundedLeftAlignedColumns()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        const int mediaStart = delegate.indexOf(QStringLiteral("id: mediaBox"));
        const int bodyStart = delegate.indexOf(QStringLiteral("id: bodyLabel"),
                                               mediaStart);
        const int previewStart = delegate.indexOf(QStringLiteral("id: previewLoader"));
        const int metaStart = delegate.indexOf(QStringLiteral("id: metaRow"),
                                               previewStart);
        QVERIFY(mediaStart >= 0 && bodyStart > mediaStart);
        QVERIFY(previewStart >= 0 && metaStart > previewStart);
        const QString mediaBlock = delegate.mid(mediaStart,
                                                bodyStart - mediaStart);
        const QString previewBlock = delegate.mid(previewStart,
                                                  metaStart - previewStart);

        QVERIFY(mediaBlock.contains(QStringLiteral(
            "Layout.alignment: Qt.AlignLeft")));
        QVERIFY(mediaBlock.contains(QStringLiteral(
            "Layout.maximumWidth: bubble.width")));
        QVERIFY(!mediaBlock.contains(QStringLiteral("Layout.fillWidth: true")));
        QVERIFY(!mediaBlock.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(previewBlock.contains(QStringLiteral(
            "Layout.alignment: Qt.AlignLeft")));
        QVERIFY(previewBlock.contains(QStringLiteral(
            "item ? item.implicitWidth : 400")));
        QVERIFY(!previewBlock.contains(QStringLiteral("Layout.fillWidth: true")));

        // Video cards: responsive 72% column cap with hard bounds and the
        // control-surface floor (the old flat 360/320 caps clipped portrait
        // controls); file cards keep a bounded width.
        QVERIFY(delegate.contains(QStringLiteral(
            "var cap = Math.min(560, Math.max(280, bubble.width * 0.72))")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property real minControlW: Math.min(260, bubble.width)")));
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitWidth: Math.min(340, bubble.width)")));
    }

    void directGifUsesInlineMediaRenderer()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        QVERIFY(delegate.contains(QStringLiteral(
            "root.preview.isDirectMedia === true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? directMediaPreviewComponent")));
        const int start = delegate.indexOf(QStringLiteral(
            "id: directMediaPreviewComponent"));
        const int genericStart = delegate.indexOf(QStringLiteral(
            "id: linkPreviewComponent"), start);
        QVERIFY(start >= 0 && genericStart > start);
        const QString directBlock = delegate.mid(start, genericStart - start);

        QVERIFY(directBlock.contains(QStringLiteral(
            "objectName: \"directMediaPreview\"")));
        QVERIFY(directBlock.contains(QStringLiteral(
            "readonly property real maxWidth: Math.min(360, bubble.width - 8)")));
        QVERIFY(directBlock.contains(QStringLiteral("AnimatedImage {")));
        QVERIFY(directBlock.contains(QStringLiteral(
            "onClicked: app.media.openWebUrl(directMedia.p.url)")));
        QVERIFY(!directBlock.contains(QStringLiteral("card.p.siteName")));
        QVERIFY(!directBlock.contains(QStringLiteral("card.p.description")));
        QVERIFY(!directBlock.contains(QStringLiteral("card.p.host")));
        QVERIFY(!directBlock.contains(QStringLiteral(
            "color: AppTheme.accent\n")));
    }

    void messageContentAndActionsRemainInteractive()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("TextEdit {\n                        id: bodyLabel")));
        QVERIFY(delegate.contains(QStringLiteral("readOnly: true")));
        QVERIFY(delegate.contains(QStringLiteral("selectByMouse: true")));
        // Links stay interactive through the controlled web-open path;
        // mention links route to the member profile instead.
        QVERIFY(delegate.contains(QStringLiteral("app.media.openWebUrl(link)")));
        QVERIFY(delegate.contains(QStringLiteral("mention:")));
        QVERIFY(delegate.contains(QStringLiteral("id: replyBox")));
        QVERIFY(delegate.contains(QStringLiteral("id: actionBar")));
        QVERIFY(delegate.contains(QStringLiteral("id: previewLoader")));
        QVERIFY(delegate.contains(QStringLiteral("id: imageComponent")));
        // v0.7: reactions open the view-shared picker via the snapshotted
        // event id; the delegate owns no picker popup of its own.
        QVERIFY(delegate.contains(
            QStringLiteral("openReactionPickerFor(root.eventIdForActions()")));
        QVERIFY(!delegate.contains(QStringLiteral("id: reactionPicker")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.beginReply")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.beginEdit")));
        QVERIFY(delegate.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
        QVERIFY(delegate.contains(QStringLiteral("Qt.Key_Menu")));
        QVERIFY(delegate.contains(QStringLiteral("id: moreMenu")));
        QVERIFY(delegate.contains(QStringLiteral("Copy message link")));
        QVERIFY(delegate.contains(QStringLiteral("View details")));
        QVERIFY(delegate.contains(QStringLiteral("messageDetailsDialog")));
        QVERIFY(delegate.contains(QStringLiteral("root.menuEventId")));
    }

};

QTEST_MAIN(QmlBindingContractTest)
#include "QmlBindingContractTest.moc"
