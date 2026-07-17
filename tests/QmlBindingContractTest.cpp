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
        QVERIFY(delegate.contains(QStringLiteral("color: \"transparent\"")));
        QVERIFY(delegate.contains(QStringLiteral("radius: 0")));
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
