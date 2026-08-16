// v0.6.5 (SPEC 1a/1d): source-contract proof for the redesigned message and
// room context menus. Pins the exact grouping/conditionals/wiring that
// MenuSystemQmlTest cannot see (it proves the shared AppMenu/AppMenuItem
// *language*, not which application surface uses which rows). Modeled on
// QmlBindingContractTest.cpp's read()/bounded-block scanning style; never
// weakens an assertion, only adds new ones for the new SPEC 1a/1d rows.

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

    // The `moreMenu` AppMenu through the Delete item's own action — the
    // entire message-context-menu row list, so a bounded absence check (no
    // standalone "React" row) can never false-positive on the earlier hover
    // action bar's own React IconButton (which also uses "add_reaction").
    static QString moreMenuBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: moreMenu"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("app.composer.redact(root.menuEventId)"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    // The `roomMenu` AppMenu, including the Leave room item's own action —
    // bounded by the trailing comment after the AppMenu's closing brace so
    // the block is never truncated exactly at content it also asserts.
    static QString roomMenuBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: roomMenu"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("Design shell: no per-row hairline"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

private Q_SLOTS:
    // ---- 1a: message context menu ----

    void moreMenuUsesSpecWidth()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString block = moreMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "menuWidth: AppTheme.menuWidthMessage")));
    }

    void quickReactionStripReplacesStandaloneReactRow()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY(!block.isEmpty());

        // The strip is wired with the picked/morePressed handlers.
        QVERIFY(block.contains(QStringLiteral("QuickReactionStrip {")));
        QVERIFY(block.contains(QStringLiteral(
            "emojis: app.emojiCatalog.recentEmoji || []")));
        QVERIFY(block.contains(QStringLiteral("onPicked: (emoji) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "app.composer.reactTo(root.menuEventId, emoji)")));
        QVERIFY(block.contains(QStringLiteral("onMorePressed: {")));
        QVERIFY(block.contains(QStringLiteral(
            "root.openReactionPickerFor(root.menuEventId, bubbleRow)")));

        // The liveness guard matches React's real current condition
        // (permalink non-empty AND not redacted), not a permalink-only check.
        // v0.7 perf round: the menu moved to a lazily-created root-level
        // Component (16 spaces shallower), so the continuation indent is 28.
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messagePermalink(\n"
            "                            root.menuEventId).length === 0")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messageDetails(\n"
            "                            root.menuEventId).redacted)")));

        // The standalone "React" row is gone from the dropdown — bounded to
        // this block so it cannot match the earlier, unrelated hover action
        // bar's own React IconButton (also "add_reaction", outside this
        // block since it precedes "id: moreMenu" in the file).
        QCOMPARE(block.count(QStringLiteral("iconName: \"add_reaction\"")), 0);
    }

    void messageMenuAcceleratorsArePresent()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("accel: \"R\"")));
        QVERIFY(block.contains(QStringLiteral("accel: \"T\"")));
        QVERIFY(block.contains(QStringLiteral("accel: \"Ctrl+C\"")));
        // The mock hints ↑ on Edit (a composer-history convention this app
        // does not have; a Key_Up shortcut would also steal menu arrow
        // navigation) — the honest binding and keycap are E.
        QVERIFY(block.contains(QStringLiteral("accel: \"E\"")));
        QVERIFY(!block.contains(QStringLiteral("accel: \"↑\"")));
    }

    void messageMenuKeyboardAcceleratorsCallExistingActionsOnly()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        // Keys cannot attach to a Menu (a Popup, not an Item) — the
        // accelerators are Shortcuts scoped to the open menu, one per key,
        // and never a Key_Up binding (menu navigation owns that).
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
        // Edit's keyboard path reuses the exact same three-argument call the
        // Edit row's onTriggered uses.
        QVERIFY(block.count(QStringLiteral("app.composer.beginEdit(")) == 2);
    }

    // v0.6.6 UX rework: GIF starring is no longer a dropdown row at all — it
    // moved to a Discord-style hover star overlaid on the GIF media itself
    // (see GifHoverStarContractTest.cpp). Pin the absence so it can never
    // silently come back as a second, redundant activation surface
    // alongside the hover star.
    void starGifIsNotAMenuItem()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = moreMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(!block.contains(QStringLiteral("starGifMenuItem")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Star GIF\")")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Unstar GIF\")")));
    }

    // v0.7.1: the crash-fix round split this in two. A short/continuation
    // room-timeline row cannot contain the ~32px bar under root's clip
    // (see MessageDelegate.qml line 13), so that host's bar is now ONE
    // shared instance in TimelinePane.qml (id: sharedMessageActionBar,
    // covered by MessageActionBarFitTest.cpp), leaving only the thread
    // panel's own always-safe (clip: false, a real ListView) in-row bar —
    // and its React/Reply/More, never Thread, which was always hidden
    // there (`visible: !root.inThreadPanel`) — inside this file.
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
        QCOMPARE(block.count(QStringLiteral("radius: AppTheme.radiusControl")), 3);
        // v0.7 perf round: the menu is created lazily, so the open state is
        // surfaced through the delegate's moreMenuOpen proxy property.
        QVERIFY(block.contains(QStringLiteral("active: root.moreMenuOpen")));
    }

    // ---- 1d: room context menu ----

    void roomMenuUsesSpecWidth()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString block = roomMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "menuWidth: AppTheme.menuWidthRoom")));
    }

    void notificationsFlyoutUsesRealSettingAndPureRadioBindings()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString block = roomMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "menuWidth: AppTheme.menuWidthFlyout")));
        QVERIFY(block.contains(QStringLiteral(
            "submenuIconName: \"notifications\"")));
        // Re-queried on show and on the settings manager's own change signal;
        // opening also re-polls the server rule (a guarded no-op on
        // backends without server push-rule support).
        QVERIFY(block.contains(QStringLiteral(
            "currentMode = app.settings.roomNotificationMode(model.roomId)")));
        const int aboutToShow =
            block.indexOf(QStringLiteral("onAboutToShow: {"));
        QVERIFY(aboutToShow >= 0);
        const QString showBlock = block.mid(aboutToShow, 500);
        QVERIFY(showBlock.contains(QStringLiteral("refreshMode()")));
        QVERIFY(showBlock.contains(QStringLiteral(
            "app.requestRoomNotificationMode(model.roomId)")));
        QVERIFY(block.contains(QStringLiteral(
            "function onRoomNotificationModeChanged(roomId) {")));
        // FOUR radio rows since v0.7, each a pure binding — never an
        // imperative assignment (AppMenuItem itself never self-toggles
        // radioSelected; the owner must not either). Mode 1 is labeled for
        // what the SDK rule actually does: mentions AND keyword rules keep
        // firing. Mode 3 ("Follow account default") was added so this
        // flyout can represent the same states Room Information offers —
        // without it a room set to mode 3 showed NO selected radio here.
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
        // The writes are signal-routed, not direct app/app.settings calls.
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(0)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(1)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(2)")));
        QVERIFY(!block.contains(QStringLiteral("app.settings.setRoomNotificationMode")));
        QVERIFY(!block.contains(QStringLiteral("app.setRoomNotificationMode")));
    }

    // Every surface that talks about per-room notification modes phrases
    // itself from the backend's real capability
    // (app.serverRoomNotificationModes): "saved to your account" wording
    // iff the backend writes the account's push rules — demoted to an
    // honest "kept on this device" while the room's last write is known to
    // have failed — and the exact pre-existing local-only wording
    // otherwise. Identical picker strings in the flyout and Room
    // Information; the Settings → Notifications caption carries the same
    // conditional, with its push-registration sentence unconditional
    // (that stays true on every backend).
    void notificationDisclaimersAreBackendHonest()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString roomInfo = read(QStringLiteral("RoomInfoPanel.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!roomInfo.isEmpty());
        QVERIFY(!settings.isEmpty());
        const QString block = roomMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        const QString savedFragment1 =
            QStringLiteral("Saved to your account's notification");
        const QString savedFragment2 =
            QStringLiteral("settings (server push rules).");
        const QString failedFragment1 =
            QStringLiteral("Couldn't save to the server");
        // v0.7: the failure line now also states that the write is retried
        // on reconnect. The admission of failure must still come FIRST and
        // unqualified, which is what this fragment pins; the retry promise
        // is checked separately below.
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
            // Never over-promise: no "synced with" phrasing anywhere (there
            // is no live push-rule watcher yet).
            QVERIFY(!source.contains(QStringLiteral("Synced with")));
        }
        // The failed-write demotion is driven by the per-room state the
        // controller tracks, re-queried through each surface's refresh.
        QVERIFY(block.contains(QStringLiteral("notificationsFlyout.syncFailed")));
        QVERIFY(block.contains(QStringLiteral(
            "app.roomNotificationModeSyncFailed(model.roomId)")));
        QVERIFY(roomInfo.contains(QStringLiteral("notificationModeCombo.syncFailed")));
        QVERIFY(roomInfo.contains(QStringLiteral(
            "app.roomNotificationModeSyncFailed(")));
        // Room Information's heading drops "(this device)" only when the
        // backend really saves the mode to the account.
        QVERIFY(roomInfo.contains(QStringLiteral("? qsTr(\"Notifications\")")));
        QVERIFY(roomInfo.contains(
            QStringLiteral(": qsTr(\"Notifications (this device)\")")));
        // Settings → Notifications: same capability gate, its own "are
        // saved to your account" / device-only variants, and the
        // unconditional push-registration sentence.
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
        QVERIFY(!block.isEmpty());
        QVERIFY(!block.contains(QStringLiteral("push_pin")));
        QVERIFY(!block.contains(QStringLiteral("Pin to favourites")));
    }

    void copyRoomLinkAndLeaveRoomAreSignalRouted()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString block = roomMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
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
        // app.roomInfo.roomId is never touched by this delegate — the leave
        // adapter must act on an explicit id, never the panel's own binding.
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
        const int keysEnd = delegate.indexOf(QStringLiteral("id: roomMenu"), keysStart);
        QVERIFY(keysEnd > keysStart);
        const QString keysBlock = delegate.mid(keysStart, keysEnd - keysStart);
        QVERIFY(keysBlock.contains(QStringLiteral("model.membership === \"joined\"")));
        QVERIFY(keysBlock.contains(QStringLiteral("roomMenu.popup()")));
        // The pre-existing right-click gate is untouched.
        QVERIFY(delegate.contains(QStringLiteral(
            "enabled: model.membership === \"joined\"")));
        QVERIFY(delegate.contains(QStringLiteral("onTapped: roomMenu.popup()")));
    }
};

QTEST_GUILESS_MAIN(ContextMenuContractTest)
#include "ContextMenuContractTest.moc"
