
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
        return block.contains(needle) ? block : QString{};
    }

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

    void componentExistsWithExpectedShape()
    {
        const QString src = read(QStringLiteral("SmoothWheelArea.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("WheelHandler {")));
        QVERIFY(src.contains(QStringLiteral("target: null")));
        QVERIFY(src.contains(QStringLiteral(
            "acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad")));
        QVERIFY(src.contains(QStringLiteral("event.pixelDelta.y !== 0")));
        QVERIFY(src.contains(QStringLiteral("event.angleDelta.y !== 0")));
        QVERIFY(src.contains(QStringLiteral("event.accepted = true")));
        QVERIFY(src.contains(QStringLiteral("function clampY(")));
        QVERIFY(src.contains(QStringLiteral("minContentY")));
        QVERIFY(src.contains(QStringLiteral("maxContentY")));
    }

    void neverDrivesSharedControllerMotionState()
    {
        const QString src = read(QStringLiteral("SmoothWheelArea.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("controller.notchDistance(")));
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
        QVERIFY(src.contains(QStringLiteral("property Timer ticker:")));
        QVERIFY(!src.contains(QStringLiteral("app.timelineScroll.motionActive")));
        QVERIFY(src.contains(QStringLiteral("controller.motionStep(")));
    }

    void settingsScreenContentPaneUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"),
                                             QStringLiteral("id: contentFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea {")));
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
        QVERIFY(block.contains(QStringLiteral("SmoothWheelArea { id: homeWheel }")));
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
        QVERIFY(src.contains(QStringLiteral("ScrollBar.horizontal: AppScrollBar")));
        QVERIFY(!src.contains(QStringLiteral("ScrollBar.horizontal.policy")));
    }

    void roomInfoPanelEveryScrollableSurfaceUsesSmoothWheelArea()
    {
        const QString src = read(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(!src.contains(QStringLiteral("ScrollView {")));

        const QString overview = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: overviewFlick"));
        QVERIFY(!overview.isEmpty());
        QVERIFY(overview.contains(QStringLiteral("SmoothWheelArea { enabled: !overviewFlick.comboOpen }")));

        const QString pinned = blockContaining(
            src, QStringLiteral("ListView {"),
            QStringLiteral("objectName: \"pinnedMessagesList\""));
        QVERIFY(!pinned.isEmpty());
        QVERIFY(pinned.contains(QStringLiteral("SmoothWheelArea {}")));

        const QString members = blockContaining(src, QStringLiteral("ListView {"), QStringLiteral("id: memberList"));
        QVERIFY(!members.isEmpty());
        QVERIFY(members.contains(QStringLiteral("SmoothWheelArea {}")));

        const QString browser = read(QStringLiteral("MediaBrowser.qml"));
        QVERIFY2(!browser.isEmpty(), "MediaBrowser.qml is gone");
        const QString mediaGrid = blockContaining(
            browser, QStringLiteral("GridView {"), QStringLiteral("id: grid"));
        QVERIFY(!mediaGrid.isEmpty());
        QVERIFY(mediaGrid.contains(QStringLiteral("SmoothWheelArea {}")));
        const QString mediaList = blockContaining(
            browser, QStringLiteral("ListView {"), QStringLiteral("id: list"));
        QVERIFY(!mediaList.isEmpty());
        QVERIFY(mediaList.contains(QStringLiteral("SmoothWheelArea {}")));

        const QString widgets = blockContaining(
            src, QStringLiteral("Flickable {"),
            QStringLiteral("visible: root.section === \"widgets\""));
        QVERIFY(!widgets.isEmpty());
        QVERIFY(widgets.contains(QStringLiteral("SmoothWheelArea {}")));

        QCOMPARE(src.count(QStringLiteral("SmoothWheelArea {")), 4);
        QCOMPARE(browser.count(QStringLiteral("SmoothWheelArea {")), 2);
    }

    void messageComposerBarInputSkipsSmoothWheelAreaByDesign()
    {
        const QString src = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!src.isEmpty());
        const QString block = blockContaining(src, QStringLiteral("Flickable {"), QStringLiteral("id: inputFlick"));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("TextArea.flickable: TextArea {")));
        QVERIFY(!block.contains(QStringLiteral("SmoothWheelArea")));
    }

    void updateAvailableDialogReleaseNotesSkipsSmoothWheelAreaByDesign()
    {
        const QString src = read(QStringLiteral("UpdateAvailableDialog.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("ScrollView {")));
        QVERIFY(!src.contains(QStringLiteral("SmoothWheelArea")));
    }

    void timelinePaneIsUnchangedReferenceImplementation()
    {
        const QString src = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("id: timelineWheelHandler")));
        QVERIFY(!src.contains(QStringLiteral("SmoothWheelArea")));
    }

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
