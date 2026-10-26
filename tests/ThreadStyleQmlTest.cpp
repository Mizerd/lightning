#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QFile>
#include <QFont>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

// The Thread style setting (Settings > Appearance) in the real mock shell:
//  * Inline (Nheko-like): thread replies interleave in the room timeline with
//    a thread tag; a tag opens the thread's own session (main view by
//    default, or the side panel); the header switch hides every thread reply;
//    a timeline narrowed through the model sends into the thread through the
//    thread send path.
//  * Thread panel (Element-like, the default): the panel's composer gets the
//    room composer's `:shortcode` completion, double-click to reply, and the
//    message-selection / composer.focus shortcuts.
//
// Only properties and QML-visible API are used, never a C++ symbol this
// change adds, so the same file builds against the tree before it and fails
// there (the regression proof).
class ThreadStyleQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    static constexpr auto kDevRoom = "!dev:lightning.example";

    static void collectItems(QQuickItem *root, const QString &name,
                             QList<QQuickItem *> &out)
    {
        if (root->objectName() == name)
            out.append(root);
        for (QQuickItem *child : root->childItems())
            collectItems(child, name, out);
    }

    QList<QQuickItem *> itemsNamed(const QString &name,
                                   QQuickItem *under = nullptr) const
    {
        QList<QQuickItem *> out;
        collectItems(under ? under : m_window->contentItem(), name, out);
        return out;
    }

    QQuickItem *item(const char *name) const
    {
        const auto found = itemsNamed(QString::fromLatin1(name));
        return found.isEmpty() ? nullptr : found.first();
    }

    QObject *objectNamed(const QString &name) const
    {
        const auto objects = m_window->findChildren<QObject *>();
        for (QObject *o : objects)
            if (o->objectName() == name)
                return o;
        // Popups are parented to the overlay; search the engine's objects too.
        const auto all = m_engine->findChildren<QObject *>();
        for (QObject *o : all)
            if (o->objectName() == name)
                return o;
        return nullptr;
    }

    MockMatrixClient *mock() const
    {
        return m_controller->findChild<MockMatrixClient *>();
    }

    TimelineModel *timeline() const { return m_controller->timeline(); }

    void setThreadStyle(int style)
    {
        QVERIFY(m_controller->settings()->setProperty("threadStyle", style));
        QTRY_COMPARE(timeline()->property("threadRepliesInline").toBool(),
                     style == 1);
    }

    void openDevRoom()
    {
        m_controller->openRoom(QString::fromLatin1(kDevRoom));
        QTRY_COMPARE(m_controller->currentRoomId(), QString::fromLatin1(kDevRoom));
        QTRY_VERIFY(timeline()->realEventCount() > 2);
        QTest::qWait(200);
    }

    // The demo room's thread root and its replies, as the room model holds
    // them (the replies are present only in the inline style).
    QString threadRoot() const
    {
        for (const TimelineEvent &e : timeline()->events())
            if (e.isThreadRoot)
                return e.eventId;
        return {};
    }

    QStringList repliesIn(const QString &root) const
    {
        QStringList out;
        for (const TimelineEvent &e : timeline()->events())
            if (e.threadRootId == root)
                out.append(e.eventId);
        return out;
    }

    QString plainMessage(const QString &bodyPrefix) const
    {
        for (const TimelineEvent &e : timeline()->events())
            if (e.body.startsWith(bodyPrefix))
                return e.eventId;
        return {};
    }

    QQuickItem *rowFor(const QString &eventId) const
    {
        auto *view = item("timelineListView");
        if (!view)
            return nullptr;
        QVariant row, value;
        QMetaObject::invokeMethod(view, "releasePendingRows");
        if (!QMetaObject::invokeMethod(view, "viewRowForStableId",
                Q_RETURN_ARG(QVariant, row), Q_ARG(QVariant, QVariant(eventId))))
            return nullptr;
        if (!QMetaObject::invokeMethod(view, "itemAtViewRow",
                Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, row)))
            return nullptr;
        return qobject_cast<QQuickItem *>(value.value<QObject *>());
    }

    qreal rowHeight(const QString &eventId) const
    {
        QQuickItem *row = rowFor(eventId);
        return row ? row->height() : -1;
    }

    QQuickItem *tagOn(const QString &eventId) const
    {
        QQuickItem *row = rowFor(eventId);
        if (!row)
            return nullptr;
        const auto tags = itemsNamed(QStringLiteral("threadTag"), row);
        return tags.isEmpty() ? nullptr : tags.first();
    }

    void click(QQuickItem *target)
    {
        QVERIFY(target);
        const QPointF centre =
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, {}, centre.toPoint());
    }

    // Element mode: open the demo thread in the side panel.
    void openThreadPanel(QString *rootOut)
    {
        openDevRoom();
        const QString root = threadRoot();
        QVERIFY(!root.isEmpty());
        m_controller->thread()->openThread(QString::fromLatin1(kDevRoom), root);
        QTRY_VERIFY(m_controller->thread()->active());
        QTRY_VERIFY(m_controller->thread()->model()->rowCount() > 2);
        QTest::qWait(300);
        *rootOut = root;
    }

    // QTest::keyClicks takes a QWidget only.
    void type(const QString &text)
    {
        for (const QChar c : text)
            QTest::keyClick(m_window, c.toLatin1());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + "/data").toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("thread-style-test"));
    }

    void init()
    {
        QSettings().clear();
        m_controller = new AppController(AppController::MockBackend, true);
        auto *client = mock();
        QVERIFY(client);
        client->setScreenshotDemoMode(true);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://lightning.example"),
                                    QStringLiteral("alex"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(4000));
        QTRY_VERIFY(m_controller->roomList()->rowCount() > 0);
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_controller);
        QQmlComponent scene(m_engine);
        scene.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 1500
    height: 900
    visible: true
    MainScreen { anchors.fill: parent }
}
)QML", QUrl(QStringLiteral("thread-style-scene.qml")));
        QObject *root = scene.create();
        QVERIFY2(root, qPrintable(scene.errorString()));
        m_window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanup()
    {
        delete m_window;
        delete m_engine;
        delete m_controller;
        m_window = nullptr;
        m_engine = nullptr;
        m_controller = nullptr;
    }

    // ── Inline (Nheko-like) ─────────────────────────────────────────────

    void threadStyleSettingPersistsAndSwitchesTheOpenTimeline()
    {
        QCOMPARE(m_controller->settings()->property("threadStyle").toInt(), 0);
        openDevRoom();
        const QString root = threadRoot();
        QVERIFY(!root.isEmpty());
        // The default (thread panel): replies live only in the thread.
        QCOMPARE(repliesIn(root).size(), 0);

        setThreadStyle(1);
        // The open room is rebuilt with its thread replies in it.
        QTRY_COMPARE(repliesIn(root).size(), 4);
        {
            SettingsManager fresh;
            QCOMPARE(fresh.property("threadStyle").toInt(), 1);
        }
        // The Settings control shows it.
        QVERIFY(m_controller->settings()->setProperty("threadStyle", 0));
        QTRY_COMPARE(repliesIn(root).size(), 0);
        QVERIFY(!timeline()->property("threadRepliesInline").toBool());
        {
            SettingsManager fresh;
            QCOMPARE(fresh.property("threadStyle").toInt(), 0);
        }
    }

    void inlineRepliesInterleaveInTimeOrderWithTags()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        const QStringList replies = repliesIn(root);
        QCOMPARE(replies.size(), 4);
        const QString later = plainMessage(QStringLiteral("Shipped: smooth"));
        QVERIFY(!later.isEmpty());
        // Time order, not "thread replies at the end": every reply sits after
        // its root and before the room message posted after them.
        const int rootRow = timeline()->rowForStableId(root);
        const int laterRow = timeline()->rowForStableId(later);
        for (const QString &id : replies) {
            const int row = timeline()->rowForStableId(id);
            QVERIFY2(row > rootRow && row < laterRow,
                     qPrintable(QStringLiteral("reply row %1 not between %2 and %3")
                                    .arg(row).arg(rootRow).arg(laterRow)));
            QTRY_VERIFY(rowHeight(id) > 0);
            QQuickItem *tag = tagOn(id);
            QVERIFY2(tag && tag->isVisible(), "a thread reply carries a thread tag");
        }
        QVERIFY2(tagOn(root) && tagOn(root)->isVisible(),
                 "the root carries its thread's tag");
        QVERIFY2(!tagOn(later) || !tagOn(later)->isVisible(),
                 "a room message is in no thread");
        // A thread reply is an ordinary row of this timeline: reply-eligible,
        // which is what keyboard selection and double-click reply key on.
        QVERIFY(rowFor(replies.first())->property("replyEligible").toBool());
    }

    // Until the reader chooses, where a focused thread is shown follows the
    // style (Panel -> beside the room, Inline -> the main column); a choice is
    // stored per account and outlives later style changes.
    void threadPlacementFollowsTheStyleUntilChosen()
    {
        SettingsManager *settings = m_controller->settings();
        QCOMPARE(settings->property("threadPlacement").toInt(), 0);
        QVERIFY(settings->setProperty("threadStyle", 1));
        QCOMPARE(settings->property("threadPlacement").toInt(), 1);
        QVERIFY(settings->setProperty("threadPlacement", 0));
        QCOMPARE(settings->property("threadPlacement").toInt(), 0);
        QVERIFY(settings->setProperty("threadStyle", 0));
        QVERIFY(settings->setProperty("threadStyle", 1));
        QCOMPARE(settings->property("threadPlacement").toInt(), 0);
        {
            SettingsManager fresh;
            QCOMPARE(fresh.property("threadPlacement").toInt(), 0);
        }
        // Anything but Main view is written as the side panel.
        QVERIFY(settings->setProperty("threadPlacement", 7));
        QCOMPARE(settings->property("threadPlacement").toInt(), 0);
    }

    // A thread tag opens the thread's own SDK session (every reply, its own
    // draft, thread receipts) -- not a narrowed copy of the loaded room rows.
    // Inline's default placement is the main column: the room column hides
    // (alive, and not read) and Back returns to it. Moving the thread between
    // the main view and the side panel is presentation only: the same session
    // stays open.
    void tagOpensTheThreadSessionInTheMainView()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        const QStringList replies = repliesIn(root);
        const QString later = plainMessage(QStringLiteral("Shipped: smooth"));
        QTRY_VERIFY(rowHeight(later) > 0);
        QObject *receipts =
            m_controller->property("readReceipts").value<QObject *>();
        QVERIFY(receipts);
        QTRY_VERIFY(receipts->property("timelineVisible").toBool());

        click(tagOn(replies.at(1)));
        QTRY_VERIFY2(m_controller->thread()->active(),
                     "a thread tag must open the thread session");
        QCOMPARE(m_controller->thread()->rootEventId(), root);
        QCOMPARE(timeline()->property("threadFilterRootId").toString(), QString());
        QTRY_VERIFY(m_controller->thread()->model()->rowCount() >= replies.size());
        QQuickItem *panel = item("threadPanel");
        QQuickItem *roomColumn = item("roomColumn");
        QVERIFY(panel && roomColumn);
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY2(!roomColumn->isVisible(),
                     "Main view: the thread takes the conversation column");
        // The panel fills the conversation pane (its RowLayout) once the
        // layout has run: measured against the pane, not a literal, since
        // the room list's width is not this test's concern, and polled,
        // since the RowLayout re-lays out on the next polish.
        QQuickItem *pane = panel->parentItem();
        QVERIFY(pane);
        QTRY_VERIFY2(panel->width() >= pane->width() - 2,
                     qPrintable(QStringLiteral("panel %1 in a %2 px pane")
                                    .arg(panel->width()).arg(pane->width())));
        // The room underneath is not on screen, so it cannot be read.
        QTRY_VERIFY(!receipts->property("timelineVisible").toBool());
        // The room composer stays the room's.
        QCOMPARE(m_controller->composer()->property("threadTargetRootId").toString(),
                 QString());

        // Side panel and back: the same session, never closed in between.
        QSignalSpy states(m_controller->thread(), SIGNAL(stateChanged()));
        click(item("threadPlacementButton"));
        QTRY_COMPARE(m_controller->settings()->property("threadPlacement").toInt(), 0);
        QTRY_VERIFY(roomColumn->isVisible());
        QVERIFY(panel->isVisible());
        // Beside the room: the room keeps at least its 320 px column.
        QTRY_VERIFY2(panel->width() <= pane->width() - 320,
                     qPrintable(QStringLiteral("panel %1 in a %2 px pane")
                                    .arg(panel->width()).arg(pane->width())));
        QTRY_VERIFY(receipts->property("timelineVisible").toBool());
        click(item("threadPlacementButton"));
        QTRY_COMPARE(m_controller->settings()->property("threadPlacement").toInt(), 1);
        QTRY_VERIFY(!roomColumn->isVisible());
        QVERIFY(m_controller->thread()->active());
        QCOMPARE(m_controller->thread()->rootEventId(), root);
        QCOMPARE(states.count(), 0);

        // Back returns to the room, which kept its rows.
        QQuickItem *back = item("threadBackToRoomButton");
        QVERIFY(back && back->isVisible());
        click(back);
        QTRY_VERIFY(!m_controller->thread()->active());
        QTRY_VERIFY(roomColumn->isVisible());
        QTRY_VERIFY(rowHeight(later) > 0);
    }

    // The Thread panel style with Main view chosen opens there too: placement
    // and room history policy are independent.
    // The room's Threads list is part of the thread surface too: it must open
    // (beside the room, whatever the placement), and a thread opened from it
    // follows the placement. A first version of the Main view change split the
    // surface condition and the list never showed (found in the GUI pass).
    void theThreadsListOpensBesideTheRoomInEveryPlacement()
    {
        openDevRoom();
        QVERIFY(m_controller->settings()->setProperty("threadPlacement", 1));
        m_controller->thread()->openList(QString::fromLatin1(kDevRoom));
        QTRY_VERIFY(m_controller->thread()->listOpen());
        QQuickItem *panel = item("threadPanel");
        QQuickItem *roomColumn = item("roomColumn");
        QVERIFY(panel && roomColumn);
        QTRY_VERIFY2(panel->isVisible(), "the Threads list must show");
        QVERIFY2(roomColumn->isVisible(),
                 "the Threads list stays beside the room in Main view");
        // A thread opened from the list takes the main column.
        m_controller->thread()->openThread(QString::fromLatin1(kDevRoom),
                                           threadRoot());
        QTRY_VERIFY(m_controller->thread()->active());
        QTRY_VERIFY(!roomColumn->isVisible());
    }

    void panelStyleCanOpenThreadsInTheMainView()
    {
        openDevRoom();
        QVERIFY(m_controller->settings()->setProperty("threadPlacement", 1));
        const QString root = threadRoot();
        QVERIFY(QMetaObject::invokeMethod(
            m_controller, "showThread",
            Q_ARG(QString, QString::fromLatin1(kDevRoom)),
            Q_ARG(QString, root)));
        QTRY_VERIFY(m_controller->thread()->active());
        QTRY_VERIFY(!item("roomColumn")->isVisible());
        // The style still keeps replies out of the room.
        QCOMPARE(repliesIn(root).size(), 0);
    }

    // Nheko compact in one click: the room renders in Small avatars -- a 20 px
    // avatar in the sender header above the body, no gutter avatar, 2 px
    // between continuations -- and the text size is the user's own.
    void nhekoCompactPresetRendersSmallAvatarRows()
    {
        openDevRoom();
        const QString later = plainMessage(QStringLiteral("Shipped: smooth"));
        QTRY_VERIFY(rowHeight(later) > 0);
        QQuickItem *row = rowFor(later);
        QVERIFY(row);
        const qreal bodyPixelSizeBefore = [&] {
            const auto bodies = itemsNamed(QStringLiteral("messageBody"), row);
            return bodies.isEmpty() ? -1.0
                                    : bodies.first()->property("font")
                                          .value<QFont>().pixelSize() * 1.0;
        }();

        QVERIFY(m_controller->settings()->setProperty("layoutPreset", 1));
        QTRY_COMPARE(m_controller->settings()->property("messageLayout").toInt(), 3);
        // Every identity row: header avatar shown, gutter avatar hidden.
        int identityRows = 0;
        int continuationRows = 0;
        for (const TimelineEvent &e : timeline()->events()) {
            QQuickItem *r = rowFor(e.eventId);
            if (!r || r->height() <= 0 || e.isVirtual())
                continue;
            const bool identity = r->property("showsIdentity").toBool();
            const auto gutter = itemsNamed(QStringLiteral("senderAvatar"), r);
            for (QQuickItem *g : gutter)
                QVERIFY2(!g->isVisible(), "Small avatars draws no gutter avatar");
            if (identity) {
                const auto head = itemsNamed(QStringLiteral("senderHeaderAvatar"), r);
                if (head.isEmpty())
                    continue; // a state/notice row has no sender header
                QVERIFY(head.first()->isVisible());
                QCOMPARE(head.first()->width(), 20.0);
                ++identityRows;
            } else if (r->property("messageTopSpacing").isValid()
                       && !r->property("isVirtualRow").toBool()) {
                QCOMPARE(r->property("messageTopSpacing").toReal(), 2.0);
                ++continuationRows;
            }
        }
        QVERIFY2(identityRows >= 2, qPrintable(QString::number(identityRows)));
        QVERIFY2(continuationRows >= 1, qPrintable(QString::number(continuationRows)));
        // Typography preserved.
        row = rowFor(later);
        const auto bodies = itemsNamed(QStringLiteral("messageBody"), row);
        if (bodyPixelSizeBefore > 0 && !bodies.isEmpty())
            QCOMPARE(bodies.first()->property("font").value<QFont>().pixelSize() * 1.0,
                     bodyPixelSizeBefore);

        // Back to Lightning: Modern rows again.
        QVERIFY(m_controller->settings()->setProperty("layoutPreset", 0));
        QTRY_COMPARE(m_controller->settings()->property("messageLayout").toInt(), 0);
        QTRY_VERIFY(itemsNamed(QStringLiteral("senderHeaderAvatar")).isEmpty());
    }

    void headerSwitchHidesThreadRepliesAndPersists()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        const QStringList replies = repliesIn(root);
        const QString later = plainMessage(QStringLiteral("Shipped: smooth"));
        for (const QString &id : replies)
            QTRY_VERIFY(rowHeight(id) > 0);

        QQuickItem *toggle = item("threadRepliesToggleButton");
        QVERIFY(toggle);
        QTRY_VERIFY(toggle->isVisible());
        click(toggle);
        QTRY_VERIFY(m_controller->settings()->property("hideThreadReplies").toBool());
        for (const QString &id : replies)
            QTRY_COMPARE(rowHeight(id), 0.0);
        QVERIFY(rowHeight(root) > 0);
        QVERIFY(rowHeight(later) > 0);
        {
            SettingsManager fresh;
            QVERIFY(fresh.property("hideThreadReplies").toBool());
        }

        // The root's tag still opens the whole thread, and Back returns to
        // the room as the reader keeps it (replies hidden).
        click(tagOn(root));
        QTRY_VERIFY(m_controller->thread()->active());
        QTRY_VERIFY(m_controller->thread()->model()->rowCount() >= replies.size());
        click(item("threadBackToRoomButton"));
        QTRY_VERIFY(!m_controller->thread()->active());
        for (const QString &id : replies)
            QTRY_COMPARE(rowHeight(id), 0.0);
        QTRY_VERIFY(rowHeight(later) > 0);

        // The same switch shows them again.
        click(toggle);
        QTRY_VERIFY(!m_controller->settings()->property("hideThreadReplies").toBool());
        for (const QString &id : replies)
            QTRY_VERIFY(rowHeight(id) > 0);
    }

    void narrowedComposerSendsIntoTheThread()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        const QStringList replies = repliesIn(root);
        // Narrowed through the model (no tag narrows any more).
        timeline()->setProperty("threadFilterRootId", root);
        QTRY_COMPARE(timeline()->property("threadFilterRootId").toString(), root);
        auto *editor = item("composerInput");
        QVERIFY(editor);
        QTRY_VERIFY(editor->hasActiveFocus());

        const int before = timeline()->events().size();
        const int roomSendsBefore = mock()->property("roomTextSends").toInt();
        type(QStringLiteral("into the thread"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(timeline()->events().size(), before + 1);
        const TimelineEvent sent = timeline()->events().last();
        QCOMPARE(sent.body, QStringLiteral("into the thread"));
        // The thread send path: an m.thread reply to this root, not a room
        // message.
        QCOMPARE(sent.threadRootId, root);
        QCOMPARE(sent.replyToEventId, QString());
        QTRY_VERIFY(rowHeight(sent.eventId) > 0);
        // Still targeting the thread after the send.
        QCOMPARE(m_controller->composer()->property("threadTargetRootId").toString(),
                 root);

        // A reply to a message of the narrowed thread: an in-thread reply.
        QVERIFY(QMetaObject::invokeMethod(rowFor(replies.first()), "beginReply",
                                          Q_ARG(QVariant, QVariant(replies.first()))));
        QTRY_VERIFY(m_controller->composer()->isReplying());
        editor->forceActiveFocus();
        type(QStringLiteral("agreed"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(timeline()->events().size(), before + 2);
        const TimelineEvent reply = timeline()->events().last();
        QCOMPARE(reply.threadRootId, root);
        QCOMPARE(reply.replyToEventId, replies.first());
        // Neither went out as a room message (§8: no room-send fallback).
        QCOMPARE(mock()->property("roomTextSends").toInt(), roomSendsBefore);

        // Back to the room: a plain message goes to the room again.
        timeline()->setProperty("threadFilterRootId", QString());
        QTRY_COMPARE(m_controller->composer()->property("threadTargetRootId").toString(),
                     QString());
        editor->forceActiveFocus();
        type(QStringLiteral("room again"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(timeline()->events().size(), before + 3);
        QCOMPARE(timeline()->events().last().threadRootId, QString());
        QCOMPARE(mock()->property("roomTextSends").toInt(), roomSendsBefore + 1);
    }

    void narrowedComposerSendsAttachmentsAndVoiceIntoTheThread()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        timeline()->setProperty("threadFilterRootId", root);
        QTRY_COMPARE(m_controller->composer()->property("threadTargetRootId").toString(),
                     root);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString notes = dir.filePath(QStringLiteral("notes.txt"));
        {
            QFile f(notes);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("thread notes");
        }
        const int threadBefore = mock()->threadAttachmentCallsForTest();
        const int roomBefore = mock()->property("roomAttachmentCalls").toInt();
        m_controller->composer()->addAttachment(QUrl::fromLocalFile(notes));
        QTRY_VERIFY(m_controller->composer()->hasAttachments());
        m_controller->composer()->send();
        QTRY_COMPARE(mock()->threadAttachmentCallsForTest(), threadBefore + 1);
        QCOMPARE(mock()->property("roomAttachmentCalls").toInt(), roomBefore);

        const QString voice = dir.filePath(QStringLiteral("voice.ogg"));
        {
            QFile f(voice);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(512, 'v'));
        }
        m_controller->composer()->sendVoiceMessage(
            voice, QStringLiteral("audio/ogg"), 1500,
            QVariantList{ 10, 40, 20 });
        QTRY_COMPARE(mock()->property("threadVoiceCalls").toInt(), 1);
        QCOMPARE(mock()->property("roomAttachmentCalls").toInt(), roomBefore);
        QCOMPARE(timeline()->events().last().threadRootId, root);
    }

    // The automatic receipt may only cover what the reader can see. Inline and
    // whole: unthreaded at the newest row. Narrowed to a thread: none at all.
    // Thread replies hidden: a visible (non-thread) row, on the main thread.
    void automaticReceiptNeverCoversHiddenThreadReplies()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        const QString later = plainMessage(QStringLiteral("Shipped: smooth"));
        // Make a thread reply the newest event of the room.
        // Narrowed through the model (no tag narrows any more).
        timeline()->setProperty("threadFilterRootId", root);
        QTRY_COMPARE(timeline()->property("threadFilterRootId").toString(), root);
        auto *editor = item("composerInput");
        QTRY_VERIFY(editor->hasActiveFocus());
        type(QStringLiteral("newest is a reply"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(timeline()->events().last().body,
                     QStringLiteral("newest is a reply"));
        const QString newestReply = timeline()->events().last().eventId;
        QCOMPARE(timeline()->events().last().threadRootId, root);
        QTRY_VERIFY(!newestReply.startsWith(QLatin1String("local:")));

        // Narrowed: nothing may be read for the room from this view.
        QCOMPARE(timeline()->latestReadableEventId(), QString());
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(false));

        // Whole room, replies shown: everything is on screen.
        timeline()->setProperty("threadFilterRootId", QString());
        QTRY_COMPARE(timeline()->latestReadableEventId(), newestReply);
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(true));

        // Replies hidden: the newest VISIBLE row, as a main-thread receipt.
        QVERIFY(m_controller->settings()->setProperty("hideThreadReplies", true));
        QTRY_COMPARE(timeline()->latestReadableEventId(), later);
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(false));
        QVERIFY(m_controller->settings()->setProperty("hideThreadReplies", false));
        QTRY_COMPARE(mock()->property("unthreadedReadReceipts"), QVariant(true));

        // The thread panel style never sends an unthreaded receipt.
        setThreadStyle(0);
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(false));
    }

    // ── Thread panel (Element-like) parity ──────────────────────────────

    void threadPanelOffersShortcodeCompletion()
    {
        QString root;
        openThreadPanel(&root);
        if (QTest::currentTestFailed())
            return;
        auto *editor = item("threadComposerInput");
        QVERIFY(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        type(QStringLiteral("hi :smil"));
        QObject *popup = objectNamed(QStringLiteral("threadEmojiCompletionPopup"));
        QVERIFY2(popup, "the thread composer has no shortcode completion popup");
        QTRY_VERIFY(popup->property("visible").toBool());
        QVERIFY(popup->property("count").toInt() > 0);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        const QString text = editor->property("text").toString();
        QVERIFY2(!text.contains(QLatin1String(":smil")), qPrintable(text));
        QVERIFY2(text.startsWith(QLatin1String("hi ")) && text.size() > 3,
                 qPrintable(text));
        // Nothing was sent: Return went to the popup.
        QCOMPARE(m_controller->thread()->property("text").toString(), text);

        // A complete `:alias: ` converts as it does in the room composer.
        type(QStringLiteral(":thumbsup: "));
        QTRY_VERIFY(!editor->property("text").toString().contains(
            QLatin1String(":thumbsup:")));
    }

    void threadPanelDoubleClickStartsAReply()
    {
        QString root;
        openThreadPanel(&root);
        if (QTest::currentTestFailed())
            return;
        auto *list = item("threadReplyList");
        QVERIFY(list);
        // The newest reply: a short line with free space beside it.
        QQuickItem *row = nullptr;
        QString id;
        auto *model = m_controller->thread()->model();
        for (int r = model->rowCount() - 1; r >= 0 && !row; --r) {
            const QString candidate = model->eventIdAt(r);
            if (candidate.isEmpty() || candidate == root)
                continue;
            QVariant value;
            QMetaObject::invokeMethod(list, "positionViewAtIndex",
                                      Q_ARG(int, r), Q_ARG(int, 3));
            QTest::qWait(50);
            QQuickItem *found = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, found),
                                      Q_ARG(int, r));
            if (found && found->height() > 0) {
                row = found;
                id = candidate;
            }
        }
        QVERIFY(row);
        const auto bodies = itemsNamed(QStringLiteral("messageBody"), row);
        QVERIFY(!bodies.isEmpty());
        QQuickItem *body = bodies.first();
        auto *bubble = itemsNamed(QStringLiteral("messageContentColumn"), row).value(0);
        QVERIFY(bubble);
        // The empty part of the bubble beside the text (as in the room).
        QPointF point = body->mapToScene(QPointF(body->width() + 24,
                                                 body->height() / 2));
        if (bubble->mapFromScene(point).x() >= bubble->width() - 2)
            point = bubble->mapToScene(QPointF(bubble->width() - 6,
                                               bubble->mapFromScene(point).y()));
        QTest::mouseDClick(m_window, Qt::LeftButton, {}, point.toPoint());
        QTRY_COMPARE(m_controller->thread()->property("replyToEventId").toString(),
                     id);
    }

    void threadPanelTakesTheSelectionAndComposerShortcuts()
    {
        QString root;
        openThreadPanel(&root);
        if (QTest::currentTestFailed())
            return;
        auto *list = item("threadReplyList");
        QVERIFY(list);
        // timeline.selectLatestMessage: the newest reply, in the panel.
        QTest::keyClick(m_window, Qt::Key_Space,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(list->hasActiveFocus());
        auto *model = m_controller->thread()->model();
        const QString newest = model->eventIdAt(model->rowCount() - 1);
        QTRY_COMPARE(list->property("keyboardSelectedEventId").toString(), newest);
        // K: the older message; J back.
        QTest::keyClick(m_window, Qt::Key_K);
        const QString older = list->property("keyboardSelectedEventId").toString();
        QVERIFY(!older.isEmpty() && older != newest && older != root);
        QTest::keyClick(m_window, Qt::Key_J);
        QCOMPARE(list->property("keyboardSelectedEventId").toString(), newest);
        // R: an in-thread reply to it, typed in the thread composer.
        QTest::keyClick(m_window, Qt::Key_K);
        QTest::keyClick(m_window, Qt::Key_R);
        QTRY_COMPARE(m_controller->thread()->property("replyToEventId").toString(),
                     older);
        auto *editor = item("threadComposerInput");
        QTRY_VERIFY(editor->hasActiveFocus());
        // composer.focus from the list returns to the thread composer.
        QTest::keyClick(m_window, Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(editor->hasActiveFocus());
    }
};

QTEST_MAIN(ThreadStyleQmlTest)
#include "ThreadStyleQmlTest.moc"
