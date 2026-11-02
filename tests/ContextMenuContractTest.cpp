
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

    static QString moreMenuBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: moreMenu"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("onTriggered: root.deleteThisMessage()"),
            start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    static QString roomMenuBlock(const QString &delegate)
    {
        if (!delegate.contains(QStringLiteral("RoomActionsMenu {")))
            return {};
        return read(QStringLiteral("RoomActionsMenu.qml"));
    }

private Q_SLOTS:

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

        QVERIFY(block.contains(QStringLiteral("QuickReactionStrip {")));
        QVERIFY(block.contains(QStringLiteral(
            "emojis: app.emojiCatalog.recentEmoji || []")));
        QVERIFY(block.contains(QStringLiteral("onPicked: (emoji) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.toggleReaction(root.menuEventId, emoji)")));
        QVERIFY(block.contains(QStringLiteral("onMorePressed: {")));
        QVERIFY(block.contains(QStringLiteral(
            "root.openReactionPickerFor(root.menuEventId, bubbleRow,")));

        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messagePermalink(\n"
            "                            root.menuEventId).length === 0")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messageDetails(\n"
            "                            root.menuEventId).redacted)")));

        const QStringList items =
            block.split(QStringLiteral("AppMenuItem {"));
        int glyphs = 0;
        for (const QString &item : items) {
            if (!item.contains(QStringLiteral("iconName: \"add_reaction\"")))
                continue;
            ++glyphs;
            QVERIFY2(item.contains(QStringLiteral(
                         "objectName: \"saveEmoticonsMenuItem\"")),
                     "a menu row other than the emoji-pack one uses the "
                     "add_reaction glyph: the standalone React row is back");
        }
        QVERIFY(glyphs <= 1);
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
        QVERIFY(block.count(QStringLiteral("app.composer.beginEdit(")) == 2);
    }

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
        QCOMPARE(block.count(QStringLiteral("radius: AppTheme.radiusControl")), 6);
        QVERIFY2(block.contains(QStringLiteral("objectName: \"messageEditButton\"")),
                 "the Edit control is not on the action bar");
        QVERIFY2(block.contains(QStringLiteral("objectName: \"messageHideMediaButton\"")),
                 "the local hide-image control is not on the action bar");
        QVERIFY(block.contains(QStringLiteral("active: root.moreMenuOpen")));
    }

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
        QVERIFY(block.contains(QStringLiteral(
            "onTriggered: root.setFavourite(!root.isFavourite)")));
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
        QCOMPARE(block.count(QStringLiteral("radio: true")), 4);
        QVERIFY(block.contains(
            QStringLiteral("text: qsTr(\"Follow account default\")")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 3")));
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
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(0)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(1)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(2)")));
        QVERIFY(!block.contains(QStringLiteral("app.settings.setRoomNotificationMode")));
        QVERIFY(!block.contains(QStringLiteral("app.setRoomNotificationMode")));
    }

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
            QVERIFY(!source.contains(QStringLiteral("Synced with")));
        }
        QVERIFY(block.contains(QStringLiteral("notificationsFlyout.syncFailed")));
        QVERIFY(block.contains(QStringLiteral(
            "app.roomNotificationModeSyncFailed(root.roomId)")));
        QVERIFY(roomInfo.contains(QStringLiteral("notificationModeCombo.syncFailed")));
        QVERIFY(roomInfo.contains(QStringLiteral(
            "app.roomNotificationModeSyncFailed(")));
        QVERIFY(roomInfo.contains(QStringLiteral("? qsTr(\"Notifications\")")));
        QVERIFY(roomInfo.contains(
            QStringLiteral(": qsTr(\"Notifications (this device)\")")));
        QVERIFY(settings.contains(capabilityGate));
        QVERIFY(settings.contains(QStringLiteral("are saved")));
        QVERIFY(settings.contains(QStringLiteral(
            "to your account's notification")));
        QVERIFY(settings.contains(QStringLiteral(
            "settings (server push rules).")));
        QVERIFY(settings.contains(QStringLiteral("apply to")));
        QVERIFY(settings.contains(QStringLiteral("this device only")));
        QVERIFY(settings.contains(QStringLiteral("Lightning does not register for")));
        QVERIFY(settings.contains(QStringLiteral("only while it is running")));
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
        const QString src = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!src.isEmpty());

        static const QRegularExpression call(
            QStringLiteral("\\b(?:dismiss|restore|request|retry|)"
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
        QCOMPARE(found, 8);
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
        QVERIFY(keysBlock.contains(QStringLiteral("root.openRowMenu()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "enabled: model.membership === \"joined\"\n"
            "                 || (root.isInviteRow && root.inviterUserId !== \"\")")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onTapped: root.openRowMenu()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool isInviteRow: model.membership === \"invited\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "const host = root.isInviteRow ? inviteMenuLoader : roomMenuLoader")));
    }

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
        const QString block = delegate.mid(start, (end - start) + 400);
        QVERIFY2(block.contains(QStringLiteral("messageDestructiveConfirmCancel")),
                 "the scanned extent is missing the cancel button");

        QVERIFY2(block.contains(QStringLiteral("Popup.CloseOnPressOutside")),
                 "the destructive confirm cannot be dismissed by clicking away");
        QVERIFY2(block.contains(QStringLiteral("kind: \"dangerPrimary\"")),
                 "the committing button must be dangerPrimary, not a bare Button");
        QVERIFY2(block.contains(QStringLiteral("kind: \"secondary\"")),
                 "the cancel button must be a secondary AppButton");
        QVERIFY2(!block.contains(QStringLiteral("\n                    Button {")),
                 "a bare Button is back in the destructive confirm; it renders "
                 "as the Qt Basic default and ignores the AppTheme ladder");
    }

    void copyTextIsWithheldForUndecryptableRows()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        const int item = block.indexOf(
            QStringLiteral("objectName: \"copyTextMenuItem\""));
        QVERIFY2(item >= 0, "the Copy text item lost its objectName");
        QVERIFY2(block.mid(item, 300).contains(
                     QStringLiteral("enabled: root.copyableText")),
                 "Copy text is not gated on root.copyableText");
        QVERIFY2(block.contains(QStringLiteral("if (root.copyableText) {")),
                 "the Ctrl+C shortcut does not share Copy text's gate");

        const int gate = delegate.indexOf(
            QStringLiteral("readonly property bool copyableText:"));
        QVERIFY2(gate >= 0, "root.copyableText is gone");
        QVERIFY2(delegate.mid(gate, 300).contains(
                     QStringLiteral("model.undecryptable !== true")),
                 "copyableText no longer refuses an undecryptable row");
    }

    void deleteTakesTheModeratorGrantFromTheSdk()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY2(!block.isEmpty(),
                 "the more-menu block could not be extracted: one of its "
                 "markers (id: moreMenu, or the Delete item's redactEvent "
                 "call) has moved, so every assertion below is testing an "
                 "empty string");
        const QString grantCall = QStringLiteral(
            "canRedactEvent(\n                             root.menuEventId, "
            "root.canRedactOthers)");
        QCOMPARE(block.count(grantCall), 3);
        QVERIFY2(delegate.contains(QStringLiteral(
                     "enabled: root.timelineModel.canRedactEvent(\n"
                     "                             root.menuEventId, "
                     "root.canRedactOthers)")),
                 "the Delete item does not pass the moderator grant");
        QVERIFY2(!block.contains(QStringLiteral(
                     "canRedactEvent(root.menuEventId)")),
                 "an own-only canRedactEvent call is left in the menu");

        const int grant = delegate.indexOf(
            QStringLiteral("readonly property bool canRedactOthers:"));
        QVERIFY2(grant >= 0, "root.canRedactOthers is gone");
        const int next = delegate.indexOf(
            QStringLiteral("readonly property"), grant + 1);
        QVERIFY2(next > grant, "the canRedactOthers binding has no end");
        const QString def = delegate.mid(grant, next - grant);
        QVERIFY2(def.contains(QStringLiteral(
                     "app.openRoomPermissions.canRedactOthers === true")),
                 "the open room's answer is not read, strictly true");
        QVERIFY2(def.contains(QStringLiteral(
                     "app.roomInfo.canRedactOthers === true")),
                 "canRedactOthers is not the SDK's answer, strictly true");
        QVERIFY2(def.contains(QStringLiteral(
                     "app.openRoomPermissions.roomId === app.currentRoomId")),
                 "another room's open-room facts could offer Delete here");
        QVERIFY2(def.contains(QStringLiteral(
                     "app.roomInfo.roomId === app.currentRoomId")),
                 "another room's grant could offer Delete in the open room");
        QVERIFY2(!def.contains(QStringLiteral("powerLevel")),
                 "power levels are being computed in QML");
    }
};

QTEST_GUILESS_MAIN(ContextMenuContractTest)
#include "ContextMenuContractTest.moc"
