// 2026-08-16 maintainer report ("scrolling feels slower and different in
// settings compared to normal chat boxes, make settings same as chat box
// scrolling"): source-contract proof for qml/SmoothWheelArea.qml, the
// shared component that gives Settings and the other converted panes the
// SAME mouse-wheel/touchpad feel as the room timeline
// (qml/TimelinePane.qml's timelineWheelHandler, src/models/
// TimelineScrollController.*). Modeled on ContextMenuContractTest.cpp's
// read()/bounded-block scanning style.
//
// Pins two separate things:
//   1. The component's own shape/policy — in particular that it NEVER
//      calls the stateful, shared-singleton parts of
//      TimelineScrollController's API (wheelNotch/animateTo/
//      pixelTargetY/wheelTargetY/cancel all mutate app.timelineScroll's
//      one instance of motion state — see the header comment in
//      SmoothWheelArea.qml for the read of TimelineScrollController.h/
//      .cpp that established this). Only the pure notchDistance() and
//      motionStep() reads
//      may cross that boundary. This is option "b" from the task and
//      must not silently drift back to option "a".
//   2. That the panes converted this round keep using the shared
//      component rather than reverting to a bare Flickable/ListView/
//      GridView with Qt's default wheel behaviour.
//
// Scope note: the sweep in noBareScrollSurfaceRegressionInConvertedPanes()
// is intentionally bounded to the files this round actually converted.
// A repository-wide grep at the time of writing also found Flickable/
// ListView/GridView content with no WheelHandler at all in AccountMenu,
// AppComboBox, DiscoverJoinDialog, GifPicker, MentionPopup,
// MessageSearchDialog, QuickSwitcher, RoomListPane, RoomsPanel,
// SpacesPanel, SpacesRail and UserPicker — none of those were in this
// round's assigned file list, so asserting SmoothWheelArea usage there
// would fail on pre-existing, out-of-scope code rather than catch a
// regression. Widening this test is real follow-up work, not a defect
// in this file.

#include <QtTest/QtTest>

#include <QFile>

class ScrollConsistencyContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // Returns the balanced-brace block starting at the '{' that follows
    // the first occurrence of `openToken` at or after `from`, e.g.
    // balancedBlock(text, "Flickable {") returns "Flickable { ... }"
    // with everything nested correctly accounted for. Empty string if
    // `openToken` is not found or never closes.
    static QString balancedBlock(const QString &text, const QString &openToken,
                                  int from = 0)
    {
        const int tokenStart = text.indexOf(openToken, from);
        if (tokenStart < 0)
            return {};
        const int braceStart = text.indexOf(QLatin1Char('{'), tokenStart);
        if (braceStart < 0)
            return {};
        int depth = 0;
        for (int i = braceStart; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('{')) {
                ++depth;
            } else if (text.at(i) == QLatin1Char('}')) {
                --depth;
                if (depth == 0)
                    return text.mid(tokenStart, i - tokenStart + 1);
            }
        }
        return {};
    }

    // The balanced block of `openToken` that CONTAINS `needle`.
    //
    // The obvious spelling -- balancedBlock(text, "Flickable {",
    // text.indexOf("id: foo")) -- cannot work: the id lives INSIDE the
    // block, so searching forward from it steps past the very "Flickable {"
    // we want and finds the next one, or nothing. Scan backwards from the
    // needle for the nearest opening token instead.
    static QString blockContaining(const QString &text, const QString &openToken,
                                   const QString &needle)
    {
        const int needleAt = text.indexOf(needle);
        if (needleAt < 0)
            return {};
        const int tokenAt = text.lastIndexOf(openToken, needleAt);
        if (tokenAt < 0)
            return {};
        const QString block = balancedBlock(text, openToken, tokenAt);
        // Only accept it if the block really does contain the anchor.
        return block.contains(needle) ? block : QString{};
    }

    // Every "Flickable {" / "ListView {" / "GridView {" balanced block in
    // `text`, in order of appearance. Used for the per-file regression
    // sweep below.
    static QStringList allScrollSurfaceBlocks(const QString &text)
    {
        QStringList blocks;
        for (const QString &token :
             { QStringLiteral("Flickable {"), QStringLiteral("ListView {"),
               QStringLiteral("GridView {") }) {
            int from = 0;
            for (;;) {
                const int tokenStart = text.indexOf(token, from);
                if (tokenStart < 0)
                    break;
                const QString block = balancedBlock(text, token, from);
                if (block.isEmpty())
                    break;
                blocks << block;
                from = tokenStart + token.size();
            }
        }
        return blocks;
    }

private Q_SLOTS:
    // ---- SmoothWheelArea.qml itself ----

    void componentExistsWithExpectedShape()
    {
        const QString src = read(QStringLiteral("SmoothWheelArea.qml"));
        QVERIFY(!src.isEmpty());
        // Non-visual PointerHandler root — never an Item/MouseArea that
        // could swallow clicks meant for content underneath it.
        QVERIFY(src.contains(QStringLiteral("WheelHandler {")));
        QVERIFY(src.contains(QStringLiteral("target: null")));
        QVERIFY(src.contains(QStringLiteral(
            "acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad")));
        // Handles both wheel input classes, and always accepts.
        QVERIFY(src.contains(QStringLiteral("event.pixelDelta.y !== 0")));
        QVERIFY(src.contains(QStringLiteral("event.angleDelta.y !== 0")));
        QVERIFY(src.contains(QStringLiteral("event.accepted = true")));
        // Clamps to the target's own live bounds on every write.
        QVERIFY(src.contains(QStringLiteral("function clampY(")));
        QVERIFY(src.contains(QStringLiteral("minContentY")));
        QVERIFY(src.contains(QStringLiteral("maxContentY")));
    }

    // Pins choice "b": only the pure per-notch-distance read may cross
    // into the shared TimelineScrollController singleton. Calling any of
    // the stateful, motion-mutating entry points from a second,
    // simultaneously visible surface would corrupt the timeline's own
    // in-flight glide (see the header comment in SmoothWheelArea.qml).
    void neverDrivesSharedControllerMotionState()
    {
        const QString src = read(QStringLiteral("SmoothWheelArea.qml"));
        QVERIFY(!src.isEmpty());
        // The one permitted, side-effect-free read.
        QVERIFY(src.contains(QStringLiteral("controller.notchDistance(")));
        // Every stateful entry point on the shared controller must be
        // absent — not just unused, literally never named, so a future
        // edit that reaches for the "obvious" API is caught immediately.
        for (const QString &forbidden :
             { QStringLiteral(".wheelNotch("), QStringLiteral(".animateTo("),
               QStringLiteral(".pixelTargetY("), QStringLiteral(".wheelTargetY("),
               QStringLiteral(".cancel("), QStringLiteral(".notifyBoundReached("),
               QStringLiteral(".translateActiveMotion(") }) {
            QVERIFY2(!src.contains(forbidden),
                     qPrintable(QStringLiteral(
                         "SmoothWheelArea.qml must never call the shared "
                         "TimelineScrollController's stateful %1 — it "
                         "mutates app.timelineScroll's single instance of "
                         "motion state.").arg(forbidden)));
        }
        // The glide is this instance's OWN ticker, never a reference into
        // app.timelineScroll's motion-active flag.
        QVERIFY(src.contains(QStringLiteral("property Timer ticker:")));
        QVERIFY(!src.contains(QStringLiteral("app.timelineScroll.motionActive")));
        // ...and it steps on the controller's REAL curve. motionStep is
        // const and stateless, so sharing it cannot disturb the timeline's
        // own motion — and sharing it is the only way the two can actually
        // feel the same rather than merely similar. Two attempts to
        // approximate with a QML easing curve were both reported as feeling
        // wrong (SmoothedAnimation eased in; OutExpo restarted its
        // deceleration every notch, i.e. scrolling "in blocks").
        QVERIFY(src.contains(QStringLiteral("controller.motionStep(")));
    }

    // ---- Applied panes: each scroll surface carries the component ----

    void settingsScreenContentPaneUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"),
                                             QStringLiteral("id: contentFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {")));
        // The section-switch jump-to-top stops the glide first so a
        // residual motion from the previous section cannot fight it.
        QVERIFY(block.contains(QStringLiteral("settingsWheelArea.stopGlide()")));
    }

    void createPollDialogBodyUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("CreatePollDialog.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: pollBodyFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {}")));
    }

    void homePaneUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("HomePane.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = balancedBlock(src, QStringLiteral("Flickable {"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {}")));
    }

    void invitePeopleDialogBodyUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("InvitePeopleDialog.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: inviteBodyFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {}")));
    }

    void loginScreenUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("LoginScreen.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: loginFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {}")));
    }

    void newConversationDialogTabUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("NewConversationDialog.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: tabFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {}")));
    }

    void emojiPickerGridUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("EmojiPicker.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("GridView {"), QStringLiteral("id: emojiGrid"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {}")));
        // The horizontal category rail (a ScrollView, not this grid)
        // deliberately keeps Qt's default behaviour — vertical wheel
        // feel does not apply to a horizontal-only strip.
        QVERIFY(src.contains(QStringLiteral("ScrollBar.horizontal.policy")));
    }

    // RoomInfoPanel has four independently scrollable surfaces: Overview
    // (converted from ScrollView to an explicit Flickable so a
    // WheelHandler has something reachable to attach to — ScrollView
    // auto-wraps non-Flickable content in an internal, unreachable
    // Flickable), Pinned, People and Media.
    void roomInfoPanelAllFourSurfacesUseSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(!src.isEmpty());
        // The ScrollView this pane used for Overview is gone entirely.
        QVERIFY(!src.contains(QStringLiteral("ScrollView {")));

        const QString overview = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: overviewFlick"));
        QVERIFY(!overview.isEmpty());
        QVERIFY(overview.contains(QStringLiteral("SmoothWheelArea {}")));

        const QString pinned = blockContaining(
            src, QStringLiteral("ListView {"),
            QStringLiteral("objectName: \"pinnedMessagesList\""));
        QVERIFY(!pinned.isEmpty());
        QVERIFY(pinned.contains(QStringLiteral("SmoothWheelArea {}")));

        const QString members = blockContaining(src, QStringLiteral("ListView {"), QStringLiteral("id: memberList"));
        QVERIFY(!members.isEmpty());
        QVERIFY(members.contains(QStringLiteral("SmoothWheelArea {}")));

        const QString media = blockContaining(src, QStringLiteral("ListView {"), QStringLiteral("id: mediaList"));
        QVERIFY(!media.isEmpty());
        QVERIFY(media.contains(QStringLiteral("SmoothWheelArea {}")));

        QCOMPARE(src.count(QStringLiteral("SmoothWheelArea {")), 4);
    }

    // ---- Deliberate skips, documented ----

    // The composer's own input box is a small (<= ~6 lines, 140px cap)
    // TextArea.flickable — caret-follow scrolling, not a content pane.
    // A coalescing glide would fight cursor positioning while typing, so
    // it deliberately keeps Qt's default TextArea/Flickable wheel
    // behaviour.
    void messageComposerBarInputSkipsSmoothWheelAreaByDesign()
    {
        const QString src = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: inputFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("TextArea.flickable: TextArea {")));
        QVERIFY(!block.contains(QStringLiteral("SmoothWheelArea")));
    }

    // The release-notes preview is a small (<=160px), read-only,
    // auto-sized ScrollView wrapping a TextArea — Qt's ScrollView/TextArea
    // cooperation creates its own internal Flickable that is not
    // reachable to attach a WheelHandler to without first converting the
    // control the same way RoomInfoPanel's Overview was (a change judged
    // not worth making for a rarely-scrolled update-notes box).
    void updateAvailableDialogReleaseNotesSkipsSmoothWheelAreaByDesign()
    {
        const QString src = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("ScrollView {")));
        QVERIFY(!src.contains(QStringLiteral("SmoothWheelArea")));
    }

    // The room timeline is the reference behaviour and must stay exactly
    // as it was — it is not touched by this round at all.
    void timelinePaneIsUnchangedReferenceImplementation()
    {
        const QString src = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("id: timelineWheelHandler")));
        QVERIFY(!src.contains(QStringLiteral("SmoothWheelArea")));
    }

    // ---- Regression guard for the panes this round converted ----

    // Any genuine scroll surface (one with a visible ScrollBar.vertical,
    // or — for the ScrollBar-less Home/composer-adjacent panes — any
    // Flickable/ListView/GridView block at all) in a converted file must
    // carry the shared component rather than reverting to Qt's bare
    // wheel behaviour. Scoped to exactly the files this round touched;
    // see the file header for why a repository-wide sweep would be
    // premature.
    void noBareScrollSurfaceRegressionInConvertedPanes()
    {
        const QStringList convertedFiles = {
            QStringLiteral("SettingsScreen.qml"),
            QStringLiteral("CreatePollDialog.qml"),
            QStringLiteral("EmojiPicker.qml"),
            QStringLiteral("HomePane.qml"),
            QStringLiteral("InvitePeopleDialog.qml"),
            QStringLiteral("LoginScreen.qml"),
            QStringLiteral("NewConversationDialog.qml"),
            QStringLiteral("RoomInfoPanel.qml"),
        };
        for (const QString &file : convertedFiles) {
            const QString src = read(file);
            QVERIFY2(!src.isEmpty(), qPrintable(file));
            const QStringList blocks = allScrollSurfaceBlocks(src);
            QVERIFY2(!blocks.isEmpty(), qPrintable(
                QStringLiteral("%1: expected at least one scroll surface")
                    .arg(file)));
            for (const QString &block : blocks) {
                QVERIFY2(block.contains(QStringLiteral("SmoothWheelArea")),
                         qPrintable(QStringLiteral(
                             "%1: a Flickable/ListView/GridView block has "
                             "no SmoothWheelArea — bare Qt wheel "
                             "behaviour would regress").arg(file)));
            }
        }
    }
};

QTEST_GUILESS_MAIN(ScrollConsistencyContractTest)
#include "ScrollConsistencyContractTest.moc"
