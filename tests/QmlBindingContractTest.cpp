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

    void paginationVisibilityDoesNotDependOnGeometry()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("app.pagination.presentationState")));
        QVERIFY(pane.contains(QStringLiteral("PaginationController.Hidden ? 0 : 32")));
        QVERIFY(pane.contains(QStringLiteral("restoreScrollAnchor(app.currentRoomId)")));
        QVERIFY(pane.contains(QStringLiteral("saveScrollAnchor(")));
        QVERIFY(pane.contains(QStringLiteral("eventIdAt(row)")));

        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("jumpToEvent(model.replyToEventId")));
        QVERIFY(delegate.contains(QStringLiteral("highlightedEventId")));
        QVERIFY(pane.contains(QStringLiteral("viewportFillCheckScheduled")));
        QVERIFY(pane.contains(QStringLiteral("Qt.callLater(function()")));
        QVERIFY(pane.contains(QStringLiteral("app.pagination.requestViewportFill()")));
        QVERIFY(!pane.contains(QStringLiteral(
            "readonly property int paginationState")));
        QVERIFY(!pane.contains(QStringLiteral("showPaginationStatus")));
        QVERIFY(!pane.contains(QStringLiteral(
            "height: paginationHeader.visible ? paginationHeader.implicitHeight")));
    }

    void animatedGifSpinnerUsesActiveRendererState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "running: imageBox.animateGif")));
        QVERIFY(delegate.contains(QStringLiteral(
            "animatedImg.status === AnimatedImage.Loading")));
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
        QVERIFY(picker.contains(QStringLiteral("setActiveProvider(modelData)")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.attribution")));
        // Debounced search + categories + pagination through the controller.
        QVERIFY(picker.contains(QStringLiteral("gif.setQueryText(text)")));
        QVERIFY(picker.contains(QStringLiteral("gif.openCategory(modelData)")));
        QVERIFY(picker.contains(QStringLiteral("picker.gif.loadMore()")));
        // Grid binds to the active section model (results / favorites / recent).
        QVERIFY(picker.contains(QStringLiteral("model: picker.activeModel")));
        QVERIFY(picker.contains(QStringLiteral("gif.favorites")));
        QVERIFY(picker.contains(QStringLiteral("gif.recent")));
        // Favorite toggles without sending; recents are handed off in Phase 7.
        QVERIFY(picker.contains(QStringLiteral("gif.toggleFavorite(")));
        // Previews animate only while visible (offscreen/hidden → paused).
        QVERIFY(picker.contains(QStringLiteral("playing: picker.visible")));
        // Tiles use the PREVIEW variant, never the sendable original gifUrl.
        QVERIFY(picker.contains(QStringLiteral("source: tile.previewUrl")));
        QVERIFY(!picker.contains(QStringLiteral("source: tile.gifUrl")));
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

        // The card renders a bubble icon, elides its preview, never plays a
        // full GIF (still label only), and is keyboard-activable + accessible.
        const QString cardQml = read(QStringLiteral("ThreadSummaryCard.qml"));
        QVERIFY(!cardQml.isEmpty());
        QVERIFY(cardQml.contains(QStringLiteral("signal activated()")));
        QVERIFY(cardQml.contains(QStringLiteral("Canvas {")));       // vector icon
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
        const int qualifiedCount = block.count(QStringLiteral("root.ListView.view"));
        const int totalCount = block.count(QStringLiteral("ListView.view"));
        QVERIFY(qualifiedCount > 0);
        // A bare, unqualified "ListView.view" inside a nested child would
        // silently resolve to that child's own (never populated) attached
        // object instead of the delegate root's — every reference in this
        // block must be root-qualified.
        QCOMPARE(totalCount, qualifiedCount);
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
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitHeight: (!roomActivityVisible || suppressedAsThreadRoot) ? 0")));
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
        QVERIFY(pane.contains(QStringLiteral("contentHeight - 40")));
        QVERIFY(!pane.contains(QStringLiteral("contentY + height === contentHeight")));
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
            "readonly property real avatarGutterWidth: 40")));
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
        // horizontal flow or a colored speech bubble.
        QVERIFY(!delegate.contains(QStringLiteral("Qt.AlignRight")));
        QVERIFY(!delegate.contains(QStringLiteral("AppTheme.ownBubble")));
        QVERIFY(!delegate.contains(QStringLiteral("AppTheme.otherBubble")));
        // v0.6.0: the bubble stays transparent by default; the only tint is
        // the sender-NEUTRAL mention highlight (applies to any sender, never
        // an own-message color or alignment change).
        QVERIFY(delegate.contains(QStringLiteral("? Qt.alpha(AppTheme.accent, 0.14)")));
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

        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property real maxW: Math.min(360, bubble.width)")));
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitWidth: Math.min(320, bubble.width)")));
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
        QVERIFY(delegate.contains(QStringLiteral(
            "onLinkActivated: function(link) { app.media.openWebUrl(link) }")));
        QVERIFY(delegate.contains(QStringLiteral("id: replyBox")));
        QVERIFY(delegate.contains(QStringLiteral("id: actionBar")));
        QVERIFY(delegate.contains(QStringLiteral("id: previewLoader")));
        QVERIFY(delegate.contains(QStringLiteral("id: imageComponent")));
        QVERIFY(delegate.contains(QStringLiteral("id: reactionPicker")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.beginReply")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.beginEdit")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.reactTo")));
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
