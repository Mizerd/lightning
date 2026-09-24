// Regression cover for a batch of fixed defects. One runtime case
// (a defect only visible by pressing the control) and source contracts for
// decisions whose failure mode is a silent one-line revert.

#include <QtTest/QtTest>

#include <QRegularExpression>

#include <QDirIterator>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/AppController.h"
#include "models/MessageComposer.h"
#include "models/TimelineModel.h"

namespace {

QString read(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                          : QString{};
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 1100
    height: 720
    visible: true
    color: AppTheme.background

    MainScreen {
        objectName: "mainScreen"
        anchors.fill: parent
    }
}
)QML";

} // namespace

class TesterReportFixesTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    static void collect(QQuickItem *parent, const QString &name,
                        QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->objectName() == name)
            out.append(parent);
        const auto children = parent->childItems();
        for (QQuickItem *child : children)
            collect(child, name, out);
    }

    // Every row carries a (usually empty, invisible) reaction Flow; the one
    // under test is the visible one.
    QQuickItem *visibleItem(const char *name) const
    {
        QList<QQuickItem *> all;
        collect(m_window->contentItem(), QLatin1String(name), all);
        for (QQuickItem *candidate : std::as_const(all)) {
            if (candidate->isVisible() && candidate->width() > 0)
                return candidate;
        }
        return nullptr;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("testerreport.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QTest::qWait(80);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    // The Home "new message" button opens the creation dialog. Emitting a
    // two-parameter signal with one argument makes QML refuse the call and
    // the handler never runs.
    void theHomeNewMessageButtonOpensTheCreationDialog()
    {
        m_controller->setCurrentRoomId(QString());
        QTest::qWait(80);
        auto *button = item("homeNewMessageButton");
        QVERIFY2(button, "the Home surface's New message button is missing");
        auto *dialog =
            m_root->findChild<QObject *>(QStringLiteral("newConversationDialog"));
        QVERIFY(dialog);
        QVERIFY2(!dialog->property("visible").toBool(),
                 "premise: the dialog is closed before the press");

        QMetaObject::invokeMethod(button, "clicked");
        QTest::qWait(120);
        QVERIFY2(dialog->property("visible").toBool(),
                 "pressing New message did nothing");
        QMetaObject::invokeMethod(dialog, "close");
        QTest::qWait(60);
    }

    // Thirty reactions on one message wrap inside the row instead of running
    // past its right edge (a Flow needs a width to wrap within).
    void manyReactionsWrapInsteadOfLeavingTheRow()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(150);
        auto *timeline = m_controller->timeline();
        QVERIFY(timeline);
        QString target;
        for (int row = 0; row < timeline->rowCount(); ++row) {
            const QString id = timeline->eventIdAt(row);
            if (!id.isEmpty() && !id.startsWith(QLatin1String("local:"))) {
                target = id;
                break;
            }
        }
        QVERIFY2(!target.isEmpty(), "no real event to react to in the fixture");

        // Thirty distinct reactions.
        static const char *kEmoji[] = {
            "\U0001F600", "\U0001F601", "\U0001F602", "\U0001F603",
            "\U0001F604", "\U0001F605", "\U0001F606", "\U0001F607",
            "\U0001F608", "\U0001F609", "\U0001F60A", "\U0001F60B",
            "\U0001F60C", "\U0001F60D", "\U0001F60E", "\U0001F60F",
            "\U0001F610", "\U0001F611", "\U0001F612", "\U0001F613",
            "\U0001F614", "\U0001F615", "\U0001F616", "\U0001F617",
            "\U0001F618", "\U0001F619", "\U0001F61A", "\U0001F61B",
            "\U0001F61C", "\U0001F61D",
        };
        for (const char *emoji : kEmoji)
            m_controller->composer()->reactTo(target, QString::fromUtf8(emoji));
        QTest::qWait(300);

        // The row under test is the one carrying the chips; every message has
        // a Flow and the fixture has another message with two reactions.
        QList<QQuickItem *> flows;
        collect(m_window->contentItem(), QStringLiteral("reactionsFlow"),
                flows);
        QQuickItem *flow = nullptr;
        int mostChips = 0;
        for (QQuickItem *candidate : std::as_const(flows)) {
            const int chips = candidate->childItems().size();
            if (candidate->isVisible() && chips > mostChips) {
                mostChips = chips;
                flow = candidate;
            }
        }
        QVERIFY2(flow, "no reaction row was rendered");
        qInfo() << "chips on the row under test" << mostChips;
        QVERIFY2(mostChips >= 20, "the reactions did not reach the row");
        QQuickItem *row = flow->parentItem();
        QVERIFY(row);
        qInfo() << "reaction flow" << flow->width() << "x" << flow->height()
                << "in a row of" << row->width();
        QVERIFY2(flow->width() <= row->width() + 1,
                 "the reaction row is wider than the message row it lives in");
        QVERIFY2(flow->height() > 40,
                 "thirty chips did not wrap onto more than one line");
    }

    // A picker opens above its button with the pointer still over it, so each
    // picker button hides its tooltip while its own picker is showing.
    void pickerButtonsDropTheirTooltipWhileTheirPickerIsUp()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!bar.isEmpty());
        const int gif = bar.indexOf(QStringLiteral("onClicked: root.openMediaPicker(true)"));
        QVERIFY(gif > 0);
        QVERIFY2(bar.mid(gif - 600, 600).contains(
                     QStringLiteral("ToolTip.visible: hovered && !gifPicker.visible")),
                 "the GIF button's tooltip must go when the picker is up");
        const int emoji = bar.indexOf(QStringLiteral("onClicked: root.openEmojiPicker(true)"));
        QVERIFY(emoji > 0);
        QVERIFY2(bar.mid(emoji - 600, 600).contains(
                     QStringLiteral("ToolTip.visible: hovered && !emojiPicker.visible")),
                 "the emoji button's tooltip must go when the picker is up");
    }

    // The enclosing declaration of `at`: everything up to the first line that
    // closes at a shallower indent. Byte windows go stale as code moves;
    // callers still assert the extent is plausible before relying on it.
    static QString declBlock(const QString &source, int at)
    {
        if (at < 0)
            return {};
        const int lineStart = source.lastIndexOf(QLatin1Char('\n'), at) + 1;
        int indent = 0;
        while (lineStart + indent < source.size()
               && source.at(lineStart + indent) == QLatin1Char(' ')) {
            ++indent;
        }
        int pos = source.indexOf(QLatin1Char('\n'), at);
        while (pos >= 0) {
            const int next = source.indexOf(QLatin1Char('\n'), pos + 1);
            const QString line = source.mid(pos + 1,
                                            (next < 0 ? source.size() : next) - pos - 1);
            const QString trimmed = line.trimmed();
            int lead = 0;
            while (lead < line.size() && line.at(lead) == QLatin1Char(' '))
                ++lead;
            if (!trimmed.isEmpty() && lead < indent
                && (trimmed.startsWith(QLatin1Char('}'))
                    || trimmed.startsWith(QLatin1Char(')')))) {
                return source.mid(at, pos - at);
            }
            pos = next;
        }
        return source.mid(at);
    }

    // The send-options menu is anchored to its button with a negative y, so it
    // opens above the composer instead of at the pointer over the message box.
    void theSendOptionsMenuOpensAboveTheComposer()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        const int at = bar.indexOf(QStringLiteral("id: sendOptionsButton"));
        QVERIFY(at > 0);
        const QString block = declBlock(bar, at);
        QVERIFY2(block.size() > 200,
                 qPrintable(QStringLiteral("the sendOptionsButton block scanned "
                                           "only %1 chars").arg(block.size())));
        QVERIFY2(!block.contains(QStringLiteral("sendOptionsMenu.popup(")),
                 "popup(x, y) computes the place on the click, when the menu's "
                 "height is still 0 on its first open — it landed on the bar");
        QVERIFY2(block.contains(QStringLiteral("sendOptionsMenu.open()")),
                 "open() lets the menu's own bindings place it");
        const int menu = bar.indexOf(QStringLiteral("id: sendOptionsMenu"));
        QVERIFY(menu > 0);
        const QString decl = bar.mid(menu, 900);
        QVERIFY2(decl.contains(QStringLiteral("y: -height - 4")),
                 "the menu must bind its y above the composer");
        QVERIFY2(decl.contains(QStringLiteral("parent: composerCard")),
                 "above the composer CARD, the way the GIF picker anchors");
        QVERIFY2(decl.contains(QStringLiteral("x: Math.max(0, composerCard.width - width)")),
                 "right-aligned through the card's observable width, so a "
                 "resize moves it");
        QVERIFY2(!decl.contains(QStringLiteral("mapToItem")),
                 "mapToItem() is not observable: it parked the menu at x = 0");
    }

    // The encryption lock sits beside the room name: a label that fills width
    // pushes the lock to the far end.
    void theEncryptionLockSitsBesideTheRoomName()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const int lock = pane.indexOf(QStringLiteral("id: encryptionLock"));
        QVERIFY(lock > 0);
        // From the room-name label down to the lock, derived from the label.
        const int name = pane.lastIndexOf(QStringLiteral("objectName: \"roomHeaderTitle\""), lock);
        QVERIFY2(name > 0, "the room title label was not found above the lock");
        const QString before = pane.mid(name, lock - name);
        QVERIFY2(before.size() > 100 && before.size() < 8000,
                 qPrintable(QStringLiteral("the title-to-lock extent is %1 chars, which is not a plausible "
                                "distance between two items in one row")
                                .arg(before.size())));
        // The name fills, so a narrow header can shrink it.
        QVERIFY2(before.contains(QStringLiteral("Layout.fillWidth: true")),
                 "the name must fill so a narrow header can shrink it");
        // Title width versus the header icons is covered geometrically by
        // TimelinePaneQmlTest::theRoomTitleOutranksTheHeaderIconRowAtEveryWidth.
    }

    // A TextField parks its cursor at the end when text is set, so every edit
    // field in the panel that receives remote text rewinds to the start when
    // not being typed in.
    void editRoomFieldsShowTheStartOfALongValue()
    {
        const QString panel = read(QStringLiteral("RoomInfoPanel.qml"));
        int at = panel.indexOf(QStringLiteral("placeholderText: qsTr(\"Room name\")"));
        QVERIFY(at > 0);
        QVERIFY2(panel.mid(at, 700).contains(QStringLiteral("cursorPosition = 0")),
                 "the room name field must rewind to its start");
        at = panel.indexOf(QStringLiteral("placeholderText: qsTr(\"Topic\")"));
        QVERIFY(at > 0);
        QVERIFY2(panel.mid(at, 700).contains(QStringLiteral("cursorPosition = 0")),
                 "the topic field must rewind to its start");
    }

    // A server-route preview delivers og:image as an mxc the homeserver cached;
    // both preview cards route it through the media bridge's mxc path.
    void serverPreviewImagesGoThroughTheMediaRoute()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        int from = 0;
        int cards = 0;
        while (true) {
            const int at = delegate.indexOf(QStringLiteral("if (src.indexOf(\"mxc://\") === 0)"), from);
            if (at < 0)
                break;
            QVERIFY2(delegate.mid(at, 200).contains(QStringLiteral("app.mediaBridge.mxcImageSource(src, 512)")),
                     "an mxc preview image must use the media route");
            ++cards;
            from = at + 1;
        }
        QCOMPARE(cards, 2);
    }

    // A compact composer row hides emoji, GIF and microphone buttons behind one
    // "…" overflow menu carrying all the actions.
    void aCompactComposerRowOffersAnOverflowMenu()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        const int mic = bar.indexOf(QStringLiteral("objectName: \"composerMicButton\""));
        QVERIFY(mic > 0);
        QVERIFY2(bar.mid(mic, 1600).contains(QStringLiteral("&& !root.compactInputRow")),
                 "the microphone must yield to the overflow in a compact row");
        const int more = bar.indexOf(QStringLiteral("objectName: \"composerOverflowButton\""));
        QVERIFY(more > 0);
        QVERIFY2(bar.mid(more, 900).contains(QStringLiteral("visible: root.compactInputRow")),
                 "the overflow button exists only in a compact row");
        const int menu = bar.indexOf(QStringLiteral("id: composerOverflowMenu"));
        QVERIFY(menu > 0);
        // The menu's own block (from the `AppMenu {` line's indent to the line
        // closing at that indent), not a fixed byte window; the extent is
        // asserted before anything is read from it.
        const int declLine = bar.lastIndexOf(QStringLiteral("\n"), menu) + 1;
        const int openLine = bar.lastIndexOf(QStringLiteral("\n"), declLine - 2) + 1;
        QVERIFY2(bar.mid(openLine, declLine - openLine)
                     .contains(QStringLiteral("AppMenu {")),
                 "composerOverflowMenu is no longer the first line of an "
                 "AppMenu block; this scan cannot find its extent");
        QString indent;
        for (int i = openLine; i < bar.size() && bar.at(i).isSpace()
                               && bar.at(i) != QLatin1Char('\n'); ++i)
            indent.append(bar.at(i));
        QVERIFY(!indent.isEmpty());
        const int close = bar.indexOf(QStringLiteral("\n") + indent
                                          + QStringLiteral("}"), menu);
        QVERIFY2(close > menu,
                 "the composerOverflowMenu block has no closing brace at its "
                 "own indent");
        const QString decl = bar.mid(menu, close - menu);
        // The extent is real: long enough to hold a menu, and ending before
        // the next top-level declaration.
        QVERIFY2(decl.size() > 400,
                 qPrintable(QStringLiteral("the menu block scanned to only %1 "
                                           "characters").arg(decl.size())));
        QVERIFY2(!decl.contains(QStringLiteral("id: sendOptionsMenu")),
                 "the scan ran past composerOverflowMenu into its sibling");
        for (const char *item : { "composerOverflowFormattingItem",
                                  "composerOverflowEmojiItem",
                                  "composerOverflowMediaItem",
                                  "composerOverflowVoiceItem" }) {
            QVERIFY2(decl.contains(QLatin1String(item)), item);
        }
        // Send later stays under the send button's chevron, not here.
        QVERIFY(!decl.contains(QStringLiteral("composerOverflowSendLaterItem")));
        QVERIFY2(decl.contains(QStringLiteral("y: -height - 4")), "the menu opens above the composer");
    }

    void theReactionRowFillsItsRowSoItCanWrap()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int flow = delegate.indexOf(QStringLiteral("id: reactionsFlow"));
        QVERIFY(flow > 0);
        const QString block = delegate.mid(flow, 1800);
        QVERIFY2(block.contains(QStringLiteral("Layout.fillWidth: true")),
                 "the reactions Flow needs a real width or it cannot wrap");
    }

    // A redacted message cannot be pinned (nothing is left to pin); unpin
    // stays available for one pinned before deletion.
    void pinningIsNotOfferedForARedactedMessage()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const int pin = delegate.indexOf(QStringLiteral("text: qsTr(\"Pin message\")"));
        QVERIFY(pin > 0);
        const int unpin =
            delegate.indexOf(QStringLiteral("text: qsTr(\"Unpin message\")"));
        QVERIFY(unpin > pin);
        const QString pinBlock = delegate.mid(pin, unpin - pin);
        QVERIFY2(pinBlock.contains(QStringLiteral("model.redacted !== true")),
                 "Pin must be withheld for a redacted event");
        const QString unpinBlock = delegate.mid(unpin, 500);
        QVERIFY2(!unpinBlock.contains(QStringLiteral("model.redacted !== true")),
                 "Unpin must stay available for a redacted pinned message");
    }

    // The emoji picker owns the skin-tone shortcut: grid cells never have
    // focus (the picker opens on its search field).
    void theEmojiPickerOwnsTheSkinToneShortcut()
    {
        const QString picker = read(QStringLiteral("EmojiPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY2(picker.contains(QStringLiteral("sequences: [\"Alt+V\"]")),
                 "Alt+V must be a picker-level shortcut, not a cell handler");
        QVERIFY2(picker.contains(QStringLiteral("openTonesForCurrentCell")),
                 "the shortcut needs a target-resolving entry point");
    }

    // A clicked room link resolves before the dialog is shown, so a link to a
    // joined room never flashes a modal.
    void aClickedRoomLinkResolvesBeforeShowingTheDialog()
    {
        const QString dialog = read(QStringLiteral("DiscoverJoinDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        const int fn = dialog.indexOf(QStringLiteral("function openForLink("));
        QVERIFY(fn > 0);
        const int end = dialog.indexOf(QStringLiteral("\n    }"), fn);
        QVERIFY(end > fn);
        const QString body = dialog.mid(fn, end - fn);
        QVERIFY2(!body.contains(QStringLiteral("\n        open()")),
                 "openForLink must not open the dialog before resolving");
        QVERIFY2(body.contains(QStringLiteral("linkResolveGrace.restart()")),
                 "a slow resolve still has to surface the dialog");
    }

    // Both composers offer pause/resume for voice recording and a Done that
    // leads to a playable preview.
    void bothComposersOfferPauseDoneAndAPreview()
    {
        const QString composer = read(QStringLiteral("MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(!composer.isEmpty() && !thread.isEmpty());
        QVERIFY(composer.contains(QStringLiteral("composerVoicePauseButton")));
        QVERIFY(composer.contains(QStringLiteral("composerVoiceDoneButton")));
        QVERIFY(composer.contains(QStringLiteral("VoicePreviewBar")));
        QVERIFY(thread.contains(QStringLiteral("threadVoicePauseButton")));
        QVERIFY(thread.contains(QStringLiteral("threadVoiceDoneButton")));
        QVERIFY(thread.contains(QStringLiteral("VoicePreviewBar")));
        const QString preview = read(QStringLiteral("VoicePreviewBar.qml"));
        QVERIFY2(preview.contains(QStringLiteral("voicePreviewPlayButton")),
                 "a preview the user cannot listen to is not a preview");
    }

    // The timeline has the middle-click autoscroll gesture, using the wheel
    // path's bounds, declared outside the rotated Flickable.
    void theTimelineCarriesTheMiddleClickScroller()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        const int at = pane.indexOf(QStringLiteral("MiddleClickScroller {"));
        QVERIFY2(at > 0, "the timeline has no middle-click autoscroll");
        const QString block = pane.mid(at, 700);
        QVERIFY2(block.contains(QStringLiteral("view: timeline")),
                 "the scroller must be given its view explicitly");
        QVERIFY2(block.contains(QStringLiteral("inverted: true")),
                 "the timeline is rotated; the direction must be inverted");
        QVERIFY2(block.contains(QStringLiteral("wheelMinY()"))
                     && block.contains(QStringLiteral("wheelMaxY()")),
                 "autoscroll must obey the same bounds as the wheel");
    }

    // The audio card fits its slider, remembers volume and speed, and offers a
    // way back to normal speed.
    void theAudioCardFitsItsSliderAndRemembersPreferences()
    {
        const QString card = read(QStringLiteral("AudioPlayerCard.qml"));
        QVERIFY(!card.isEmpty());
        QVERIFY2(card.contains(QStringLiteral("id: seekSlider")),
                 "the seek slider needs its own compact handle to fit");
        QVERIFY2(card.contains(QStringLiteral("volume: app.settings.mediaVolume")),
                 "playback volume must come from the remembered setting");
        QVERIFY2(card.contains(
                     QStringLiteral("playbackRate: app.settings.mediaPlaybackRate")),
                 "playback speed must come from the remembered setting");
        QVERIFY2(card.contains(QStringLiteral("audioSpeedMenu")),
                 "the speed control must be selectable, not cycle-only");
    }

    // Every player that writes the remembered media level also starts at it.
    // Derived from the source, so new players are covered. If an in-app
    // ringtone ever trips this, exempt it here: `mediaVolume` is for media the
    // user chose to play, not alerts.
    void everyPlayerStartsAtTheRememberedVolume()
    {
        QDir dir(QStringLiteral(QML_DIR));
        QVERIFY2(dir.exists(), QML_DIR);
        const auto files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());

        QStringList players;
        QStringList deaf;
        for (const QString &name : files) {
            const QString body = read(name);
            if (!body.contains(QStringLiteral("AudioOutput {")))
                continue;
            players.append(name);
            if (!body.contains(
                    QStringLiteral("volume: app.settings.mediaVolume")))
                deaf.append(name);
        }

        // Present-token control: if `AudioOutput {` stops matching, this sweep
        // would pass vacuously.
        QVERIFY2(players.size() >= 3,
                 qPrintable(QStringLiteral("only %1 AudioOutput declarations "
                                           "found — the sweep is matching the "
                                           "wrong thing")
                                .arg(players.size())));
        QVERIFY2(deaf.isEmpty(),
                 qPrintable(QStringLiteral(
                                "these players ignore the remembered media "
                                "volume and will start at their own literal: "
                                "%1")
                                .arg(deaf.join(QStringLiteral(", ")))));
    }


    void theHoverBarOffersEdit()
    {
        const QString src = read(QStringLiteral("MessageDelegate.qml"));
        const int edit = src.indexOf(QStringLiteral("objectName: \"messageEditButton\""));
        const int more = src.indexOf(QStringLiteral("id: threadMoreButton"));
        QVERIFY2(edit > 0, "the hover bar has no Edit button");
        QVERIFY2(more > edit, "Edit sits before More on the bar");
        const QString block = src.mid(edit, more - edit);
        QVERIFY2(block.contains(QStringLiteral("canEditEvent(")),
                 "Edit must use the same gate as the context menu's Edit");
        QVERIFY2(block.contains(QStringLiteral("app.composer.beginEdit(")),
                 "Edit must start the composer's edit, not a menu");
    }

    void thePopoutCanFillItselfWithTheShare()
    {
        const QString src = read(QStringLiteral("CallPipWindow.qml"));
        QVERIFY(src.contains(QStringLiteral("objectName: \"pipShareFillButton\"")));
        QVERIFY(src.contains(QStringLiteral("property bool shareFills")));
        QVERIFY(src.contains(QStringLiteral("objectName: \"pipShareFill\"")));
        QVERIFY2(src.contains(QStringLiteral(
                     "active: root.visible && root.groupLive && !root.shareFillActive")),
                 "the tile grid must step aside while the share fills the window");
        QVERIFY2(src.contains(QStringLiteral(
                     "onFillShareShownChanged: if (root.fillShareShown.length === 0) "
                     "root.shareFills = false")),
                 "the fill mode must drop when the share ends");
        // A click fills, a click restores, Escape restores, and the bar hides
        // while the share fills the window.
        QVERIFY(src.contains(QStringLiteral("root.fillShareId = shareId\n"
                                            "                        root.shareFills = true")));
        QVERIFY(src.contains(QStringLiteral("onActivated: root.shareFills = false")));
        QVERIFY(src.contains(QStringLiteral("visible: !root.shareFillActive\n"
                                            "            color: AppTheme.surface")));
    }

    void theProfileCardFramesItsBannerAndSizesItsChips()
    {
        const QString src = read(QStringLiteral("MemberProfilePopover.qml"));
        const int banner = src.indexOf(QStringLiteral("objectName: \"profileBanner\""));
        QVERIFY(banner > 0);
        const QString bannerBlock = src.mid(banner, 900);
        QVERIFY2(bannerBlock.contains(QStringLiteral("width: parent.width - 2 * frame")),
                 "the banner must sit inside the popover's border");
        // Smooth corners: the mask must not be a hard step on its alpha.
        QVERIFY2(src.contains(QStringLiteral("maskSpreadAtMin: 1.0")),
                 "the banner mask must carry its antialiased edge through");
        QVERIFY2(src.contains(QStringLiteral("maskThresholdMin: 0.5")),
                 "the low-side ramp is [(t-1)(1+s)+1, t(1+s)]: only t=0.5, s=1 maps 0->0 and 1->1");
        QVERIFY(bannerBlock.contains(QStringLiteral("antialiasing: true")));
        const int chip = src.indexOf(QStringLiteral("objectName: \"profileMembershipChip\""));
        QVERIFY(chip > 0);
        QVERIFY2(src.mid(chip, 400).contains(QStringLiteral("height: profileChipRow.uniformHeight")),
                 "the membership chip must share the row's height");
        const int dot = src.indexOf(QStringLiteral("objectName: \"profileAvatarPresenceDot\""));
        QVERIFY(dot > 0);
        const QString dotBlock = src.mid(dot, 2400);
        QVERIFY2(dotBlock.contains(QStringLiteral("hoverStatus: false")),
                 "the dot's own tooltip opens on top of the avatar");
        QVERIFY2(dotBlock.contains(QStringLiteral("x: avatarPresenceDot.x + avatarPresenceDot.width")),
                 "the presence tip must sit beside the dot it explains");
        QVERIFY2(src.contains(QStringLiteral("_fillFromServer()")),
                 "a member the roster cannot name must be asked from the server");
        QVERIFY(src.contains(QStringLiteral("app.userProfiles.lookup(userId)")));
    }

    void mediaRowsWaitForTheBand()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("property bool mediaInBand: true")));
        QVERIFY2(delegate.contains(QStringLiteral("if (!root.mediaInBand) return")),
                 "an image row outside the band must not ask the bridge");
        QVERIFY2(delegate.contains(QStringLiteral("function onMediaInBandChanged()")),
                 "a row entering the band must ask then");
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(pane.contains(QStringLiteral("function refreshMediaBand()")));
        // Moved at discrete moments, never bound to contentY (which would
        // re-run every row's comparison each frame); an index range assigned
        // by the row loader, not a geometry test in the delegate.
        QVERIFY(pane.contains(QStringLiteral("property int mediaBandFirstRow: 0")));
        QVERIFY(pane.contains(QStringLiteral("mediaInBand: index >= timeline.mediaBandFirstRow")));
        QVERIFY(pane.count(QStringLiteral("refreshMediaBand()")) >= 5);
    }

    void shareTilesFrameThePictureAndFadeTheNameplate()
    {
        const QString src = read(QStringLiteral("CallShareTile.qml"));
        const int frame = src.indexOf(QStringLiteral("objectName: \"callShareVideoFrame\""));
        QVERIFY2(frame > 0, "the painted video has no frame");
        QVERIFY2(src.mid(frame, 900).contains(QStringLiteral("output.videoSink.videoSize.width")),
                 "the frame must follow the painted picture, not the tile bounds");
        QVERIFY2(src.contains(QStringLiteral("(pictureFramed ? 0 : (root.focused ? 2 : 1))")),
                 "the tile edge must step aside while the picture frame draws");
        QVERIFY(src.contains(QStringLiteral("objectName: \"callSharePlateIdleTimer\"")));
        const int plate = src.indexOf(QStringLiteral("objectName: \"callShareNameplate\""));
        QVERIFY(plate > 0);
        QVERIFY2(src.mid(plate, 400).contains(QStringLiteral(
                     "opacity: surface.plateAutoHides && surface.plateIdle ? 0 : 1")),
                 "the nameplate must fade once the surface is idle");
    }

    void theFillFollowsTheControllerNotATimer()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const int at = pane.indexOf(QStringLiteral("function onStateChanged() {"));
        QVERIFY(at > 0);
        QVERIFY2(pane.mid(at, 900).contains(QStringLiteral(
                     "if (!app.pagination.busy)\n"
                     "                            timeline.maybeFillViewport()")),
                 "a finished batch must re-check the fill at once, not after the retry timer");
        // And the band never closes on the newest side.
        QVERIFY(pane.contains(QStringLiteral("mediaBandFirstRow = 0\n")));
        // The fill is bounded by rows as well as invisible pages: pages that
        // each add a little height defeat the page budget.
        QVERIFY(pane.contains(QStringLiteral("readonly property int maxViewportFillRows: 240")));
        QVERIFY2(pane.contains(QStringLiteral("return \"rowBudget\"")),
                 "the fill must decline on the row cap, by name");
    }

    // Selection circles follow the count and own a gutter; the hovered row's
    // time, "Home" eliding, the voice bar's hang-up button and popped-out
    // shares are covered by the cases below.
    void theSelectionCircleFollowsTheCountAndOwnsTheGutter()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY2(delegate.contains(QStringLiteral("&& app.forward.selectedCount > 0\n"
                                                  "        && app.forward.isSelected(")),
                 "rowSelected must name a NOTIFYing property: isSelected() is a plain call");
        const int loader = delegate.indexOf(
            QStringLiteral("active: !root.showsIdentity && rowHover.hovered"));
        QVERIFY(loader > 0);
        QVERIFY2(delegate.mid(loader, 160).contains(QStringLiteral("!root.selectionMode")),
                 "the hover timestamp must yield the gutter to the selection circle");
        // The circle has its own column (content shifts right while
        // selecting), and only messages are selectable.
        QVERIFY(delegate.contains(QStringLiteral(
            "x: root.selectionMode && root.rowSelectable ? root.selectionGutterWidth : 0")));
        QVERIFY(delegate.contains(QStringLiteral("&& !root.isCallEvent && !root.isStateActivity\n"
                                                 "        && root.eventIdForActions() !== \"\"")));
    }

    // A Layout floors a fractional bound (53.28 becomes 53), so any
    // `Layout.maximumWidth` bound to a text's own measure must be rounded up.
    void theRoomTitleNeverElidesByAFraction()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        static const QRegularExpression cap(
            QStringLiteral("Layout\\.maximumWidth:\\s*([^\\n]+)"));
        auto it = cap.globalMatch(pane);
        int checked = 0;
        while (it.hasNext()) {
            const QString expr = it.next().captured(1).trimmed();
            ++checked;
            const bool rounded = expr.contains(QStringLiteral("Math.ceil"))
                              || expr.contains(QStringLiteral("Math.floor"))
                              || expr.contains(QStringLiteral("Math.round"));
            // Only caps bound to a text's own measure matter: there the
            // fraction decides whether the text fits. A proportional cap
            // losing a sub-pixel is harmless.
            const bool boundToOwnText =
                expr.contains(QStringLiteral("implicitWidth"))
                || expr.contains(QStringLiteral("contentWidth"))
                || expr.contains(QStringLiteral("paintedWidth"));
            if (!boundToOwnText)
                continue;
            QVERIFY2(rounded,
                     qPrintable(QStringLiteral(
                         "Layout.maximumWidth: %1 is bound to the text's own "
                         "measure and is not rounded; a Layout floors it and "
                         "the text elides one pixel early")
                             .arg(expr)));
        }
        QVERIFY2(checked > 0, "no Layout.maximumWidth found to check");
    }

    void theVoiceBarKeepsItsButtonsInsideItself()
    {
        const QString bar = read(QStringLiteral("VoiceConnectedBar.qml"));
        const int status = bar.indexOf(QStringLiteral("qsTr(\"Voice connected\")"));
        QVERIFY(status > 0);
        QVERIFY2(bar.mid(status, 500).contains(QStringLiteral("elide: Text.ElideRight")),
                 "the status text must yield before the hang-up button leaves the bar");
        QVERIFY2(bar.count(QStringLiteral("Layout.minimumWidth: 0")) >= 2,
                 "the text column must be shrinkable");
    }

    // The channel row's favourite star: filled while favourite, an outline on
    // hover, a click toggles the tag, placed so the pill never moves.
    void theChannelRowOffersAFavouriteStar()
    {
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        const int star = row.indexOf(QStringLiteral("objectName: \"channelFavouriteStar\""));
        QVERIFY2(star > 0, "no favourite star in the channel row");
        const QString block = declBlock(row, star);
        QVERIFY2(block.size() > 200,
                 qPrintable(QStringLiteral("the favourite-star block scanned "
                                           "only %1 chars").arg(block.size())));
        QVERIFY(block.contains(QStringLiteral("&& (root.isFavourite || root.hovered)")));
        QVERIFY2(block.contains(QStringLiteral("onClicked: root.setFavourite(!root.isFavourite)")),
                 "the star must toggle the same tag the menu writes");
        QVERIFY(block.contains(QStringLiteral("readonly property bool filled: root.isFavourite")));
        QVERIFY2(row.contains(QStringLiteral("anchors.right: favouriteStar.left")),
                 "the name must yield to the star, not the pill");
    }

    // A completed rich-mode pill is not a token, and a query with a second
    // space is not a name.
    void aCompletedMentionDoesNotReopenThePopup()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY2(bar.contains(QStringLiteral(
                     "richInput.getFormattedText(tok.start, tok.start + 1)")),
                 "the rich editor must refuse a token that starts inside a pill anchor");
        QVERIFY(bar.contains(QStringLiteral(".indexOf(\"matrix.to/#/\") >= 0")));
    }

    void thePopoutFillTilesLiveOnlyWhileTheWindowShows()
    {
        const QString src = read(QStringLiteral("CallPipWindow.qml"));
        QVERIFY2(src.contains(QStringLiteral(
                     "model: root.visible && root.shareFillActive ? root.shareModel : null")),
                 "fill tiles that outlive a pop-in come back unattached: a grey box");
    }

    void emojiSurfacesNameTheColourFace()
    {
        const QString panel = read(QStringLiteral("VerificationPanel.qml"));
        const int symbol = panel.indexOf(QStringLiteral("text: modelData.symbol || \"\""));
        QVERIFY(symbol > 0);
        QVERIFY2(panel.mid(symbol, 600).contains(QStringLiteral("font.family: app.emojiFontFamily")),
                 "verification emoji must name the colour face (Qt 6.8 fallback is monochrome)");
        const QString icon = read(QStringLiteral("Icon.qml"));
        QVERIFY(icon.contains(QStringLiteral("renderType: Text.NativeRendering")));
    }

    // A key arriving retries decryption in place (retryDecryption()) instead of
    // rebuilding the room via reloadCurrentRoomTimeline(), which re-paginates,
    // re-fetches media and closes an open thread. No automatic C++ caller of
    // the reload may exist, and both automatic triggers must call the retry.
    // Asserted as call counts, since the definition alone would satisfy a
    // `contains`.
    void aKeyArrivingRetriesInPlaceInsteadOfRebuildingTheRoom()
    {
        QString src;
        int filesRead = 0;
        QDirIterator it(QStringLiteral(LIGHTNING_SRC_DIR),
                        { QStringLiteral("*.cpp"), QStringLiteral("*.h") },
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile f(it.next());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            src += QString::fromUtf8(f.readAll()) + QLatin1Char('\n');
            ++filesRead;
        }
        // The extent must be real before anything is concluded.
        QVERIFY2(filesRead > 50,
                 qPrintable(QStringLiteral("only %1 source files were scanned")
                                .arg(filesRead)));

        // Strip comments, which name the hazard, so the counts are of code.
        QString code;
        const auto lines = src.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (!line.trimmed().startsWith(QLatin1String("//")))
                code += line + QLatin1Char('\n');
        }

        // Only the definition may name it in C++ (QML's Settings "Refresh"
        // button is the one legitimate caller). Scanned across all of src/,
        // since it is a public Q_INVOKABLE.
        const int defs = code.count(
            QStringLiteral("void AppController::reloadCurrentRoomTimeline"));
        QCOMPARE(defs, 1);
        // The header declaration is a mention, not a call.
        const int decls = code.count(
            QStringLiteral("void reloadCurrentRoomTimeline"));
        QCOMPARE(decls, 1);
        const int mentions =
            code.count(QStringLiteral("reloadCurrentRoomTimeline"));
        // Everything else would be a call, and there must be none in C++.
        QVERIFY2(mentions - decls == defs,
                 qPrintable(QStringLiteral(
                     "reloadCurrentRoomTimeline is called %1 time(s) from C++; "
                     "rebuilding the room to retry decryption throws away the "
                     "reader's history and every delegate with it — use "
                     "retryDecryptionInCurrentRoom()")
                         .arg(mentions - decls - defs)));

        // The replacement is actually called: count calls, since the
        // definition's signature contains the same substring.
        const int retryDefs = code.count(
            QStringLiteral("void AppController::retryDecryptionInCurrentRoom"));
        QCOMPARE(retryDefs, 1);
        const int retryDecls = code.count(
            QStringLiteral("void retryDecryptionInCurrentRoom"));
        QCOMPARE(retryDecls, 1);
        const int retryCalls =
            code.count(QStringLiteral("retryDecryptionInCurrentRoom()"))
            - retryDefs - retryDecls;
        QVERIFY2(retryCalls == 2,
                 qPrintable(QStringLiteral(
                     "the in-place decryption retry is called %1 time(s); both "
                     "the key-backup-recovery and the verification-done "
                     "handlers must use it")
                         .arg(retryCalls)));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TesterReportFixesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "TesterReportFixesTest.moc"
