// Regression cover for the 2026-08-18 tester report (Element chat export).
//
// One runtime case and a set of source contracts. The runtime case is the
// one defect that could only be seen by actually pressing the control; the
// contracts pin decisions whose failure mode is a silent revert of a
// one-line change (an emitted signal losing an argument, a Flow losing its
// width, a modal opening before it has anything to show).

#include <QtTest/QtTest>

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

    // "new message button does literally nothig".
    //
    // The Home surface emitted a two-parameter signal with one argument.
    // QML raises "Insufficient arguments" for that and the handler never
    // runs, so all three Home buttons were dead. Pressing it must open the
    // creation dialog.
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

    // The same defect, measured: thirty reactions on one message must WRAP
    // inside the row instead of running past its right edge.
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

        // Thirty distinct reactions — the tester's screenshot had about forty.
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

        // The row under test is the one that actually carries the chips —
        // every message has a Flow, and the fixture ships another message
        // with two reactions of its own.
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

    // "infinite reactions eina i sona": the chips ran off the right edge of
    // the window instead of wrapping, because a Flow without a width has
    // only its own single-row implicit width to wrap inside.
    // "when gif menu is opened the text doesn't disappear and gets half
    // hidden by the pop up": the picker opens ABOVE its button with the
    // pointer still over it, so `ToolTip.visible: hovered` kept the tooltip
    // up under the popup's bottom edge. Each picker button hides its
    // tooltip while its own picker is showing.
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

    // "could this button open above the send prompt so not to cover it
    // all": a bare popup() opens at the pointer, over the message box. The
    // menu is anchored to its button with a NEGATIVE y, i.e. above it.
    void theSendOptionsMenuOpensAboveTheComposer()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        const int at = bar.indexOf(QStringLiteral("id: sendOptionsButton"));
        QVERIFY(at > 0);
        const QString block = bar.mid(at, 2600);
        QVERIFY2(!block.contains(QStringLiteral("sendOptionsMenu.popup()")),
                 "a bare popup() lands on the pointer, over the message box");
        QVERIFY2(block.contains(QStringLiteral("sendOptionsMenu.popup("))
                     && block.contains(QStringLiteral("-sendOptionsMenu.height")),
                 "the menu must be placed above its button");
    }

    // "why is the lock so far away from the room name": the name label was
    // fillWidth and grew to its half-header cap, pushing the encryption lock
    // out to the far end. A non-fill label hugs its text; the maximumWidth
    // cap alone still elides a long name.
    void theEncryptionLockSitsBesideTheRoomName()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const int lock = pane.indexOf(QStringLiteral("id: encryptionLock"));
        QVERIFY(lock > 0);
        const QString before = pane.mid(lock - 2600, 2600);
        QVERIFY2(before.contains(QStringLiteral("Layout.fillWidth: false")),
                 "the room name must not fill the row");
        QVERIFY2(before.contains(QStringLiteral("Layout.maximumWidth: header.width * 0.5")),
                 "the cap is what still lets a long name elide");
        QVERIFY2(!before.mid(before.indexOf(QStringLiteral("Layout.fillWidth: false")))
                      .contains(QStringLiteral("Layout.fillWidth: true")),
                 "no fill between the name and the lock");
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

    // "you can pin \"message deleted\" useless": a redacted event has no
    // content left to pin. Unpin stays available for one pinned before it
    // was deleted.
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

    // "alt+v emoji bar neveikia": the shortcut existed only on a grid CELL,
    // which never has focus — the picker opens focused on its search field.
    void theEmojiPickerOwnsTheSkinToneShortcut()
    {
        const QString picker = read(QStringLiteral("EmojiPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY2(picker.contains(QStringLiteral("sequences: [\"Alt+V\"]")),
                 "Alt+V must be a picker-level shortcut, not a cell handler");
        QVERIFY2(picker.contains(QStringLiteral("openTonesForCurrentCell")),
                 "the shortcut needs a target-resolving entry point");
    }

    // "kai spaudi link atsidaro pop up langas milisekundei": a clicked room
    // link must resolve BEFORE the dialog is shown, so a link to a room the
    // user is already in never flashes a modal.
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

    // "kai sendini audio messages nera pause arba done mygtuko ir preview
    // yra tik send ir delete" — both composers get pause/resume and a Done
    // that lands in a preview the user can play back.
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

    // "still no middle click scrol": the timeline gets the desktop autoscroll
    // gesture, driven through the same bounds as the wheel path, and
    // declared OUTSIDE the rotated Flickable.
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

    // "audio slider klipinasi biski" / "neatsimena audio preferencu" / "kai
    // keiti audio garso greiti nera kaip grizti".
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
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TesterReportFixesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "TesterReportFixesTest.moc"
