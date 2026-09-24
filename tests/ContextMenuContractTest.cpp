// Source contract for the message and room context menus: the grouping,
// conditionals and wiring that MenuSystemQmlTest (which proves the shared
// AppMenu/AppMenuItem language) cannot see. Uses bounded-block scanning in
// the style of QmlBindingContractTest.cpp.

#include <QRegularExpression>
#include <QtTest/QtTest>

#include <QFile>

class ContextMenuContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // The `moreMenu` AppMenu through the Delete item's action: the whole row
    // list, so an absence check cannot match the hover bar's React button
    // (which also uses "add_reaction").
    static QString moreMenuBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: moreMenu"));
        if (start < 0) return {};
        // The Delete item's action is the menu's last row. It goes through the
        // timeline model's redactEvent (app.composer is the room composer and
        // cannot see thread items) inside the confirmation's closure. Each
        // QVERIFY below names this marker so a missing block reports clearly.
        const int end = delegate.indexOf(
            QStringLiteral("root.timelineModel.redactEvent(id)"),
            start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    // The room menu lives in RoomActionsMenu.qml, shared by both room-list
    // layouts, so this reads the whole component. `delegate` is still taken
    // so the row is required to exist and own the menu.
    static QString roomMenuBlock(const QString &delegate)
    {
        if (!delegate.contains(QStringLiteral("RoomActionsMenu {")))
            return {};
        return read(QStringLiteral("RoomActionsMenu.qml"));
    }

private Q_SLOTS:
    // ---- message context menu ----

    void moreMenuUsesSpecWidth()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(block.contains(QStringLiteral(
            "menuWidth: AppTheme.menuWidthMessage")));
    }

    void quickReactionStripReplacesStandaloneReactRow()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");

        // The strip is wired with the picked/morePressed handlers.
        QVERIFY(block.contains(QStringLiteral("QuickReactionStrip {")));
        QVERIFY(block.contains(QStringLiteral(
            "emojis: app.emojiCatalog.recentEmoji || []")));
        QVERIFY(block.contains(QStringLiteral("onPicked: (emoji) => {")));
        // Through the model, not the room composer: in the thread panel
        // app.composer addresses the room timeline, which hides threaded
        // events.
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.toggleReaction(root.menuEventId, emoji)")));
        QVERIFY(block.contains(QStringLiteral("onMorePressed: {")));
        QVERIFY(block.contains(QStringLiteral(
            "root.openReactionPickerFor(root.menuEventId, bubbleRow)")));

        // The liveness guard matches React's real condition (permalink
        // non-empty and not redacted). The menu is a lazily created root-level
        // Component, so the continuation indent is 28.
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messagePermalink(\n"
            "                            root.menuEventId).length === 0")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messageDetails(\n"
            "                            root.menuEventId).redacted)")));

        // No standalone "React" row in the dropdown; bounded to this block so
        // the hover bar's React button (before "id: moreMenu") cannot match.
        QCOMPARE(block.count(QStringLiteral("iconName: \"add_reaction\"")), 0);
    }

    void messageMenuAcceleratorsArePresent()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(block.contains(QStringLiteral("accel: \"R\"")));
        QVERIFY(block.contains(QStringLiteral("accel: \"T\"")));
        QVERIFY(block.contains(QStringLiteral("accel: \"Ctrl+C\"")));
        // Edit's accelerator is E: a Key_Up shortcut would steal menu arrow
        // navigation, and there is no composer-history convention here.
        QVERIFY(block.contains(QStringLiteral("accel: \"E\"")));
        QVERIFY(!block.contains(QStringLiteral("accel: \"↑\"")));
    }

    void messageMenuKeyboardAcceleratorsCallExistingActionsOnly()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        // Keys cannot attach to a Menu (a Popup), so accelerators are
        // Shortcuts scoped to the open menu, never a Key_Up binding.
        QVERIFY(!block.contains(QStringLiteral("Keys.onPressed")));
        QVERIFY(block.count(QStringLiteral("Shortcut {")) == 4);
        QVERIFY(block.count(QStringLiteral("enabled: moreMenu.opened")) == 4);
        QVERIFY(block.count(QStringLiteral(
            "context: Qt.ApplicationShortcut")) == 4);
        QVERIFY(block.contains(QStringLiteral("sequence: \"R\"")));
        QVERIFY(block.contains(QStringLiteral("sequence: \"T\"")));
        QVERIFY(block.contains(QStringLiteral("sequence: \"E\"")));
        QVERIFY(block.contains(QStringLiteral("sequence: \"Ctrl+C\"")));
        QVERIFY(!block.contains(QStringLiteral("Qt.Key_Up")));
        QVERIFY(block.count(QStringLiteral(
            "root.copyToClipboard(\n")) >= 1);
        // Edit's keyboard path reuses the row's exact three-argument call.
        QVERIFY(block.count(QStringLiteral("app.composer.beginEdit(")) == 2);
    }

    // GIF starring is a hover star on the media (see
    // GifHoverStarContractTest.cpp), not a menu row; a second activation
    // surface must not return.
    void starGifIsNotAMenuItem()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(!block.contains(QStringLiteral("starGifMenuItem")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Star GIF\")")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Unstar GIF\")")));
    }

    // Room-timeline rows share one action bar in TimelinePane.qml (id:
    // sharedMessageActionBar, covered by MessageActionBarFitTest.cpp), since a
    // short row cannot contain it under root's clip. This file keeps only the
    // thread panel's in-row bar (React/Reply/More, never Thread).
    void messageActionBarUsesSpecChrome()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int start = delegate.indexOf(QStringLiteral("id: messageActionBar"));
        const int end = delegate.indexOf(QStringLiteral("id: moreMenu"), start);
        QVERIFY(start >= 0 && end > start);
        const QString block = delegate.mid(start, end - start);
        QVERIFY(block.contains(QStringLiteral("radius: AppTheme.radiusTile")));
        QVERIFY(block.contains(QStringLiteral("border.color: AppTheme.borderStrong")));
        // Five buttons: Hide (image and sticker rows only, leading, as in
        // Element), React, Reply, Edit (own editable messages only), More.
        QCOMPARE(block.count(QStringLiteral("radius: AppTheme.radiusControl")), 5);
        QVERIFY2(block.contains(QStringLiteral("objectName: \"messageEditButton\"")),
                 "the Edit control is not on the action bar");
        QVERIFY2(block.contains(QStringLiteral("objectName: \"messageHideMediaButton\"")),
                 "the local hide-image control is not on the action bar");
        // The menu is created lazily, so its open state is surfaced through the
        // delegate's moreMenuOpen property.
        QVERIFY(block.contains(QStringLiteral("active: root.moreMenuOpen")));
    }

    // ---- room context menu ----

    void roomMenuUsesSpecWidth()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString block = roomMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(block.contains(QStringLiteral(
            "menuWidth: AppTheme.menuWidthRoom")));
    }

    // The Favourite toggle is hidden where the backend cannot write room tags,
    // and it sends the value to write rather than toggling locally.
    void roomMenuOffersFavouritesOnlyWhereTheBackendCanWriteTheTag()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString block = roomMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(block.contains(QStringLiteral("objectName: \"roomFavouriteItem\"")));
        QVERIFY(block.contains(QStringLiteral(
            "visible: app.roomList.roomFavouritesSupported")));
        // The row asks for the opposite of what the model reports and waits
        // for the backend; a local assignment would be an optimistic apply.
        QVERIFY(block.contains(QStringLiteral(
            "onTriggered: root.setFavourite(!root.isFavourite)")));
        // An assignment only; the `root.isFavourite ?` read must not trip this.
        QVERIFY(!block.contains(
            QRegularExpression(QStringLiteral("root\\.isFavourite\\s*=(?!=)"))));
        QVERIFY(delegate.contains(QStringLiteral("signal setFavourite(bool on)")));
    }

    void notificationsFlyoutUsesRealSettingAndPureRadioBindings()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString block = roomMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(block.contains(QStringLiteral(
            "menuWidth: AppTheme.menuWidthFlyout")));
        QVERIFY(block.contains(QStringLiteral(
            "submenuIconName: \"notifications\"")));
        // Re-queried on show and on the settings manager's change signal;
        // opening also re-polls the server rule (a no-op without server push
        // rules).
        QVERIFY(block.contains(QStringLiteral(
            "currentMode = app.settings.roomNotificationMode(root.roomId)")));
        const int aboutToShow =
            block.indexOf(QStringLiteral("onAboutToShow: {"));
        QVERIFY(aboutToShow >= 0);
        const QString showBlock = block.mid(aboutToShow, 500);
        QVERIFY(showBlock.contains(QStringLiteral("refreshMode()")));
        QVERIFY(showBlock.contains(QStringLiteral(
            "app.requestRoomNotificationMode(root.roomId)")));
        QVERIFY(block.contains(QStringLiteral(
            "function onRoomNotificationModeChanged(roomId) {")));
        // Four radio rows, each a pure binding (AppMenuItem never
        // self-toggles, and the owner must not either). Mode 1 is labelled for
        // what the SDK rule does (mentions and keywords still fire); mode 3
        // ("Follow account default") matches Room Information's states.
        QCOMPARE(block.count(QStringLiteral("radio: true")), 4);
        QVERIFY(block.contains(
            QStringLiteral("text: qsTr(\"Follow account default\")")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 3")));
        // Offered only where there is a real account rule to defer to.
        QVERIFY(block.contains(
            QStringLiteral("visible: app.serverRoomNotificationModes")));
        QVERIFY(block.contains(QStringLiteral("text: qsTr(\"All messages\")")));
        QVERIFY(block.contains(QStringLiteral(
            "text: qsTr(\"Mentions & keywords\")")));
        QVERIFY(block.contains(QStringLiteral("text: qsTr(\"Muted\")")));
        QVERIFY(!block.contains(QStringLiteral("Mentions only")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 0")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 1")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 2")));
        QVERIFY(!block.contains(QStringLiteral("radioSelected =")));
        // Writes are signal-routed, not direct app/app.settings calls.
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(0)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(1)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(2)")));
        QVERIFY(!block.contains(QStringLiteral("app.settings.setRoomNotificationMode")));
        QVERIFY(!block.contains(QStringLiteral("app.setRoomNotificationMode")));
    }

    // A non-MenuItem child of an AppMenu must size itself, or a wrapping Text
    // takes its own implicitWidth, paints past the panel and does not elide.
    // A source contract: the offscreen QQuickMenu sizes this item where the
    // running app does not.
    void theNotificationDisclaimerBindsItsWidthToTheFlyout()
    {
        const QString menu = read(QStringLiteral("RoomActionsMenu.qml"));
        QVERIFY2(!menu.isEmpty(), "RoomActionsMenu.qml not readable");
        const int label =
            menu.indexOf(QStringLiteral("objectName: \"roomNotificationDisclaimer\""));
        QVERIFY2(label >= 0, "the disclaimer Label has been renamed");
        const int end = menu.indexOf(QStringLiteral("wrapMode:"), label);
        QVERIFY2(end > label, "the disclaimer no longer declares a wrapMode");
        const QString block = menu.mid(label, end - label);
        QVERIFY2(block.contains(QStringLiteral("width: notificationsFlyout.width")),
                 "the disclaimer does not bind its width to the flyout, so "
                 "its wrapMode wraps at its own implicitWidth and the text "
                 "paints through the panel border");
        QVERIFY(block.contains(QStringLiteral("notificationsFlyout.leftPadding")));
        QVERIFY(block.contains(QStringLiteral("notificationsFlyout.rightPadding")));
    }

    void notificationDisclaimersAreBackendHonest()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString roomInfo = read(QStringLiteral("RoomInfoPanel.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!roomInfo.isEmpty());
        QVERIFY(!settings.isEmpty());
        const QString block = roomMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        const QString savedFragment1 =
            QStringLiteral("Saved to your account's notification");
        const QString savedFragment2 =
            QStringLiteral("settings (server push rules).");
        const QString failedFragment1 =
            QStringLiteral("Couldn't save to the server");
        // Notification-mode wording follows the backend's real capability
        // (app.serverRoomNotificationModes): "saved to your account" only when
        // push rules are written, "kept on this device" while the last write
        // failed, the local-only wording otherwise; the same in the flyout,
        // Room Information and Settings. The failure line also promises a
        // retry on reconnect, but the admission of failure must come first.
        const QString failedFragment2 =
            QStringLiteral("kept on this device.");
        const QString retryFragment =
            QStringLiteral("Retried when you reconnect.");
        const QString localFragment1 =
            QStringLiteral("Local setting: it does not change this");
        const QString localFragment2 =
            QStringLiteral("room's server push rules.");
        const QString capabilityGate =
            QStringLiteral("app.serverRoomNotificationModes");
        for (const QString &source : { block, roomInfo }) {
            QVERIFY(source.contains(capabilityGate));
            QVERIFY(source.contains(savedFragment1));
            QVERIFY(source.contains(savedFragment2));
            QVERIFY(source.contains(failedFragment1));
            QVERIFY(source.contains(failedFragment2));
            QVERIFY(source.contains(retryFragment));
            QVERIFY(source.contains(localFragment1));
            QVERIFY(source.contains(localFragment2));
            // No "synced with" wording: there is no live push-rule watcher.
            QVERIFY(!source.contains(QStringLiteral("Synced with")));
        }
        // The failed-write demotion follows the per-room state the controller
        // tracks.
        QVERIFY(block.contains(QStringLiteral("notificationsFlyout.syncFailed")));
        QVERIFY(block.contains(QStringLiteral(
            "app.roomNotificationModeSyncFailed(root.roomId)")));
        QVERIFY(roomInfo.contains(QStringLiteral("notificationModeCombo.syncFailed")));
        QVERIFY(roomInfo.contains(QStringLiteral(
            "app.roomNotificationModeSyncFailed(")));
        // Room Information's heading drops "(this device)" only when the
        // backend really saves the mode to the account.
        QVERIFY(roomInfo.contains(QStringLiteral("? qsTr(\"Notifications\")")));
        QVERIFY(roomInfo.contains(
            QStringLiteral(": qsTr(\"Notifications (this device)\")")));
        // Settings > Notifications: same capability gate, its own variants, and
        // the unconditional push-registration sentence.
        QVERIFY(settings.contains(capabilityGate));
        QVERIFY(settings.contains(QStringLiteral("are saved")));
        QVERIFY(settings.contains(QStringLiteral(
            "to your account's notification")));
        QVERIFY(settings.contains(QStringLiteral(
            "settings (server push rules).")));
        QVERIFY(settings.contains(QStringLiteral("apply to")));
        QVERIFY(settings.contains(QStringLiteral("this device only")));
        QVERIFY(settings.contains(QStringLiteral("Push registration for")));
        QVERIFY(settings.contains(QStringLiteral("implemented.")));
        // Storm skin: the flyout disclaimer rides the faint storm mono ink.
        QVERIFY(block.contains(QStringLiteral("color: AppTheme.stormTextFaint")));
        QVERIFY(block.contains(QStringLiteral("font.pixelSize: AppTheme.fontMicro")));
        QVERIFY(block.contains(QStringLiteral("wrapMode: Text.WordWrap")));
    }

    void roomMenuOmitsPinToFavourites()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString block = roomMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(!block.contains(QStringLiteral("push_pin")));
        QVERIFY(!block.contains(QStringLiteral("Pin to favourites")));
    }

    void everyPreviewRestoreUsesTheKeyTheDismissalUsed()
    {
        // A dismissed preview is remembered under a key built from a room id
        // and the event's action key, and a lookup of a key nobody dismissed is
        // a silent no-op. Every verb must use `previewRoomId`
        // (app.currentRoomId), not the view's roomId.
        const QString src = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!src.isEmpty());

        // Anchored on the call, not a fixed window after a name. Every verb
        // that keys the store (dismiss, request, retry, lookup, restore) is
        // checked.
        static const QRegularExpression call(
            QStringLiteral("(?:dismiss|restore|request|retry|)"
                           "[Pp]reviewForEvent\\s*\\(([^)]*)\\)"));
        auto it = call.globalMatch(src);
        int found = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            ++found;
            const QString args = m.captured(1);
            QVERIFY2(args.contains(QStringLiteral("previewRoomId")),
                     qPrintable(QStringLiteral(
                         "a preview restore keys on something other than "
                         "previewRoomId, so it can never find the dismissal "
                         "it is meant to undo: %1").arg(args.simplified())));
        }
        // An exact count: a floor would let a new unchecked site slip in.
        QCOMPARE(found, 6);
    }

    void copyRoomLinkAndLeaveRoomAreSignalRouted()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString block = roomMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        QVERIFY(block.contains(QStringLiteral("iconName: \"link\"")));
        QVERIFY(block.contains(QStringLiteral(
            "text: qsTr(\"Copy room link\")")));
        QVERIFY(block.contains(QStringLiteral("onTriggered: root.copyRoomLink()")));
        QVERIFY(block.contains(QStringLiteral("iconName: \"logout\"")));
        QVERIFY(block.contains(QStringLiteral("text: qsTr(\"Leave room\")")));
        QVERIFY(block.contains(QStringLiteral("danger: true")));
        QVERIFY(block.contains(QStringLiteral(
            "onTriggered: root.leaveRoomRequested()")));
    }

    void roomDelegateStaysSignalOnlyForMutations()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("signal setNotificationMode(int mode)")));
        QVERIFY(delegate.contains(QStringLiteral("signal copyRoomLink()")));
        QVERIFY(delegate.contains(QStringLiteral("signal leaveRoomRequested()")));
        // The delegate never touches app.roomInfo.roomId; leave acts on an
        // explicit id.
        QVERIFY(!delegate.contains(QStringLiteral("app.roomInfo.roomId")));
        QVERIFY(!delegate.contains(QStringLiteral("app.roomInfo.leaveRoom")));
    }

    void roomMenuKeyboardAndRightClickStayJoinedOnly()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("activeFocusOnTab: true")));
        const int keysStart = delegate.indexOf(QStringLiteral("Keys.onPressed: (event) => {"));
        QVERIFY(keysStart >= 0);
        const int keysEnd =
            delegate.indexOf(QStringLiteral("RoomActionsMenu {"), keysStart);
        QVERIFY(keysEnd > keysStart);
        const QString keysBlock = delegate.mid(keysStart, keysEnd - keysStart);
        QVERIFY(keysBlock.contains(QStringLiteral("model.membership === \"joined\"")));
        QVERIFY(keysBlock.contains(QStringLiteral("roomMenu.popup()")));
        // The existing right-click gate is untouched.
        QVERIFY(delegate.contains(QStringLiteral(
            "enabled: model.membership === \"joined\"")));
        QVERIFY(delegate.contains(QStringLiteral("onTapped: roomMenu.popup()")));
    }

    // The delete confirmation uses AppButton (`dangerPrimary` for the
    // destructive action) rather than bare Qt Basic buttons, and closes on a
    // press outside; that is safe because committing needs an explicit press
    // on the destructive button. Scoped to the dialog's extent.
    void theDestructiveConfirmUsesAppButtonsAndClosesOnPressOutside()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        const int start = delegate.indexOf(
            QStringLiteral("objectName: \"messageDestructiveConfirmDialog\""));
        QVERIFY2(start > 0, "the destructive confirm dialog was not found");
        const int end = delegate.indexOf(
            QStringLiteral("messageDestructiveConfirmAccept"), start);
        QVERIFY2(end > start, "the accept button was not found after the dialog");
        // The whole block including the accept button's body.
        const QString block = delegate.mid(start, (end - start) + 400);
        QVERIFY2(block.contains(QStringLiteral("messageDestructiveConfirmCancel")),
                 "the scanned extent is missing the cancel button");

        QVERIFY2(block.contains(QStringLiteral("Popup.CloseOnPressOutside")),
                 "the destructive confirm cannot be dismissed by clicking away");
        QVERIFY2(block.contains(QStringLiteral("kind: \"dangerPrimary\"")),
                 "the committing button must be dangerPrimary, not a bare Button");
        QVERIFY2(block.contains(QStringLiteral("kind: \"secondary\"")),
                 "the cancel button must be a secondary AppButton");
        // No plain `Button {` declaration in this block (`AppButton {` does not
        // match).
        QVERIFY2(!block.contains(QStringLiteral("\n                    Button {")),
                 "a bare Button is back in the destructive confirm; it renders "
                 "as the Qt Basic default and ignores the AppTheme ladder");
    }
};

QTEST_GUILESS_MAIN(ContextMenuContractTest)
#include "ContextMenuContractTest.moc"
