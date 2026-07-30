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
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messagePermalink(\n"
            "                                            root.menuEventId).length === 0")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineModel.messageDetails(\n"
            "                                            root.menuEventId).redacted)")));

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

    void messageActionBarUsesSpecChrome()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int start = delegate.indexOf(QStringLiteral("id: actionBar"));
        const int end = delegate.indexOf(QStringLiteral("id: moreMenu"), start);
        QVERIFY(start >= 0 && end > start);
        const QString block = delegate.mid(start, end - start);
        QVERIFY(block.contains(QStringLiteral("radius: AppTheme.radiusTile")));
        QVERIFY(block.contains(QStringLiteral("border.color: AppTheme.borderStrong")));
        QCOMPARE(block.count(QStringLiteral("radius: AppTheme.radiusControl")), 4);
        QVERIFY(block.contains(QStringLiteral("active: moreMenu.opened")));
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
        // Re-queried on show and on the settings manager's own change signal.
        QVERIFY(block.contains(QStringLiteral(
            "currentMode = app.settings.roomNotificationMode(model.roomId)")));
        QVERIFY(block.contains(QStringLiteral("onAboutToShow: refreshMode()")));
        QVERIFY(block.contains(QStringLiteral(
            "function onRoomNotificationModeChanged(roomId) {")));
        // Three radio rows, each a pure binding — never an imperative
        // assignment (AppMenuItem itself never self-toggles radioSelected;
        // the owner must not either).
        QCOMPARE(block.count(QStringLiteral("radio: true")), 3);
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 0")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 1")));
        QVERIFY(block.contains(QStringLiteral(
            "radioSelected: notificationsFlyout.currentMode === 2")));
        QVERIFY(!block.contains(QStringLiteral("radioSelected =")));
        // The writes are signal-routed, not direct app.settings calls.
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(0)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(1)")));
        QVERIFY(block.contains(QStringLiteral("root.setNotificationMode(2)")));
        QVERIFY(!block.contains(QStringLiteral("app.settings.setRoomNotificationMode")));
    }

    void notificationsFlyoutCarriesTheExactExistingDisclaimer()
    {
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        const QString roomInfo = read(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!roomInfo.isEmpty());
        const QString block = roomMenuBlock(delegate);
        QVERIFY(!block.isEmpty());
        const QString disclaimerFragment1 =
            QStringLiteral("Local setting: it does not change this");
        const QString disclaimerFragment2 =
            QStringLiteral("room's server push rules.");
        // Exact existing wording from RoomInfoPanel.qml, reused verbatim.
        QVERIFY(roomInfo.contains(disclaimerFragment1));
        QVERIFY(roomInfo.contains(disclaimerFragment2));
        QVERIFY(block.contains(disclaimerFragment1));
        QVERIFY(block.contains(disclaimerFragment2));
        QVERIFY(block.contains(QStringLiteral("color: AppTheme.textDisabled")));
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
