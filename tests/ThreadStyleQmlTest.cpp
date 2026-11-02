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

    void type(const QString &text)
    {
        for (const QChar c : text)
            QTest::keyClick(m_window, c.toLatin1());
    }

private slots:
    void replyDividerUsesLiveKnownCount()
    {
        QString root;
        openThreadPanel(&root);
        const QString composite = MatrixClient::threadTimelineId(
            QString::fromLatin1(kDevRoom), root);
        const auto events = mock()->timeline(composite);
        int rootIndex = -1;
        for (int i = 0; i < events.size(); ++i)
            if (events.at(i).eventId == root) { rootIndex = i; break; }
        QVERIFY(rootIndex >= 0);
        TimelineEvent changed = events.at(rootIndex);
        changed.threadReplyCount = 1;
        mock()->changeEventAtForTest(composite, rootIndex, changed);
        auto *label = item("threadReplyCountLabel");
        QVERIFY(label);
        const int live = m_controller->thread()->replyCount();
        QVERIFY(live > 1);
        QTRY_COMPARE(label->property("replies").toInt(), live);
    }

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

    void threadStyleSettingPersistsAndSwitchesTheOpenTimeline()
    {
        QCOMPARE(m_controller->settings()->property("threadStyle").toInt(), 0);
        openDevRoom();
        const QString root = threadRoot();
        QVERIFY(!root.isEmpty());
        QCOMPARE(repliesIn(root).size(), 0);

        setThreadStyle(1);
        QTRY_COMPARE(repliesIn(root).size(), 4);
        {
            SettingsManager fresh;
            QCOMPARE(fresh.property("threadStyle").toInt(), 1);
        }
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
        QVERIFY(rowFor(replies.first())->property("replyEligible").toBool());
    }

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
        QVERIFY(settings->setProperty("threadPlacement", 7));
        QCOMPARE(settings->property("threadPlacement").toInt(), 0);
    }

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
        QQuickItem *pane = panel->parentItem();
        QVERIFY(pane);
        QTRY_VERIFY2(panel->width() >= pane->width() - 2,
                     qPrintable(QStringLiteral("panel %1 in a %2 px pane")
                                    .arg(panel->width()).arg(pane->width())));
        QTRY_VERIFY(!receipts->property("timelineVisible").toBool());
        QCOMPARE(m_controller->composer()->property("threadTargetRootId").toString(),
                 QString());

        QSignalSpy states(m_controller->thread(), SIGNAL(stateChanged()));
        click(item("threadPlacementButton"));
        QTRY_COMPARE(m_controller->settings()->property("threadPlacement").toInt(), 0);
        QTRY_VERIFY(roomColumn->isVisible());
        QVERIFY(panel->isVisible());
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

        QQuickItem *back = item("threadBackToRoomButton");
        QVERIFY(back && back->isVisible());
        click(back);
        QTRY_VERIFY(!m_controller->thread()->active());
        QTRY_VERIFY(roomColumn->isVisible());
        QTRY_VERIFY(rowHeight(later) > 0);
    }

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
        QCOMPARE(repliesIn(root).size(), 0);
    }

    void nhekoCompactPresetRendersSmallAvatarRows()
    {
        openDevRoom();
        QQuickItem *header = item("roomHeaderBand");
        QVERIFY(header);
        QTRY_COMPARE(header->height(), 60.0);
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
        QTRY_COMPARE(header->height(), 48.0);
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
                    continue;
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
        row = rowFor(later);
        const auto bodies = itemsNamed(QStringLiteral("messageBody"), row);
        if (bodyPixelSizeBefore > 0 && !bodies.isEmpty())
            QCOMPARE(bodies.first()->property("font").value<QFont>().pixelSize() * 1.0,
                     bodyPixelSizeBefore);

        QVERIFY(m_controller->settings()->setProperty("layoutPreset", 0));
        QTRY_COMPARE(m_controller->settings()->property("messageLayout").toInt(), 0);
        QTRY_COMPARE(header->height(), 60.0);
        QTRY_VERIFY(itemsNamed(QStringLiteral("senderHeaderAvatar")).isEmpty());
        QObject *theme = m_engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY(theme);
        QCOMPARE(theme->property("menuItemHeight").toInt(), 32);
        QCOMPARE(theme->property("dialogInset").toInt(), 20);
        QCOMPARE(theme->property("channelRowMinHeight").toInt(), 32);
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

        click(tagOn(root));
        QTRY_VERIFY(m_controller->thread()->active());
        QTRY_VERIFY(m_controller->thread()->model()->rowCount() >= replies.size());
        click(item("threadBackToRoomButton"));
        QTRY_VERIFY(!m_controller->thread()->active());
        for (const QString &id : replies)
            QTRY_COMPARE(rowHeight(id), 0.0);
        QTRY_VERIFY(rowHeight(later) > 0);

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
        QCOMPARE(sent.threadRootId, root);
        QCOMPARE(sent.replyToEventId, QString());
        QTRY_VERIFY(rowHeight(sent.eventId) > 0);
        QCOMPARE(m_controller->composer()->property("threadTargetRootId").toString(),
                 root);

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
        QCOMPARE(mock()->property("roomTextSends").toInt(), roomSendsBefore);

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

    void automaticReceiptNeverCoversHiddenThreadReplies()
    {
        setThreadStyle(1);
        openDevRoom();
        const QString root = threadRoot();
        const QString later = plainMessage(QStringLiteral("Shipped: smooth"));
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

        QCOMPARE(timeline()->latestReadableEventId(), QString());
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(false));

        timeline()->setProperty("threadFilterRootId", QString());
        QTRY_COMPARE(timeline()->latestReadableEventId(), newestReply);
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(true));

        QVERIFY(m_controller->settings()->setProperty("hideThreadReplies", true));
        QTRY_COMPARE(timeline()->latestReadableEventId(), later);
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(false));
        QVERIFY(m_controller->settings()->setProperty("hideThreadReplies", false));
        QTRY_COMPARE(mock()->property("unthreadedReadReceipts"), QVariant(true));

        setThreadStyle(0);
        QCOMPARE(mock()->property("unthreadedReadReceipts"), QVariant(false));
    }

    static bool atBottomEdge(QQuickItem *list)
    {
        QVariant r;
        QMetaObject::invokeMethod(list, "atBottomEdge", Q_RETURN_ARG(QVariant, r));
        return r.toBool();
    }

    void aFollowedThreadStaysOnItsNewestReplyWhenARowGrowsByViewports()
    {
        QString root;
        openThreadPanel(&root);
        auto *list = item("threadReplyList");
        QVERIFY(list);
        QTRY_VERIFY(list->property("followLatest").toBool());
        QTRY_VERIFY(atBottomEdge(list));
        const int count = list->property("count").toInt();
        QVERIFY(count >= 2);
        for (int index : { count - 2, count - 1 }) {
            QQuickItem *row = nullptr;
            QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex",
                                              Q_RETURN_ARG(QQuickItem *, row),
                                              Q_ARG(int, index)));
            QVERIFY(row);
            row->setProperty("heightSeed",
                             row->implicitHeight() + 3 * list->height());
            row->setProperty("heightSeedActive", true);
            QTRY_VERIFY2(atBottomEdge(list),
                         qPrintable(QStringLiteral("row %1 grew by 3 viewports "
                                                   "and the thread left its "
                                                   "newest reply").arg(index)));
            QTest::qWait(300);
            QVERIFY(atBottomEdge(list));
        }
        QVERIFY(list->property("followLatest").toBool());
    }

    void aFollowedThreadStaysOnItsNewestReplyWhileRowsGrow()
    {
        QString root;
        openThreadPanel(&root);
        auto *list = item("threadReplyList");
        QVERIFY(list);
        QTRY_VERIFY(list->property("followLatest").toBool());
        QTRY_VERIFY(list->property("atYEnd").toBool());
        const int count = list->property("count").toInt();
        QVERIFY(count >= 2);
        QQuickItem *row = nullptr;
        QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, row),
                                          Q_ARG(int, count - 2)));
        QVERIFY(row);
        const qreal natural = row->implicitHeight();
        row->setProperty("heightSeed", natural + list->height());
        row->setProperty("heightSeedActive", true);
        QTRY_VERIFY(list->property("contentHeight").toReal() > list->height() + 100);
        QTRY_VERIFY2(atBottomEdge(list),
                     "a followed thread must stay on its newest reply");
        QVERIFY(list->property("followLatest").toBool());
    }

    void scrollingAFollowedThreadUpIsNotSnappedBack()
    {
        QString root;
        openThreadPanel(&root);
        auto *list = item("threadReplyList");
        QVERIFY(list);
        QTRY_VERIFY(list->property("followLatest").toBool());
        QTRY_VERIFY(list->property("atYEnd").toBool());
        // The mock thread fits in the view; make it taller than the view so
        // there is somewhere to scroll (the pin keeps the end in view).
        const int count = list->property("count").toInt();
        QVERIFY(count >= 2);
        QQuickItem *row = nullptr;
        QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, row),
                                          Q_ARG(int, count - 2)));
        QVERIFY(row);
        row->setProperty("heightSeed", row->implicitHeight() + list->height());
        row->setProperty("heightSeedActive", true);
        QTRY_VERIFY(list->property("contentHeight").toReal() > list->height() + 100);
        QTRY_VERIFY(atBottomEdge(list));
        const QPointF top = list->mapToScene(QPointF(list->width() / 2, 40));
        const QPointF bottom =
            list->mapToScene(QPointF(list->width() / 2, list->height() - 40));
        QTest::mousePress(m_window, Qt::LeftButton, {}, top.toPoint());
        for (int i = 1; i <= 10; ++i) {
            const QPointF p = top + (bottom - top) * (i / 10.0);
            QTest::mouseMove(m_window, p.toPoint(), 16);
        }
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, bottom.toPoint(), 16);
        QTRY_VERIFY(!list->property("moving").toBool());
        QTest::qWait(400);
        QVERIFY2(!list->property("atYEnd").toBool(),
                 "a thread scrolled up must stay where the reader left it");
        QVERIFY(!list->property("followLatest").toBool());
    }

    void aReplyArrivingWhileInHistoryIsCountedNotJumpedTo()
    {
        QString root;
        openThreadPanel(&root);
        auto *list = item("threadReplyList");
        QVERIFY(list);
        QTRY_VERIFY(list->property("atYEnd").toBool());
        // Make the thread taller than the view so the top is genuinely in
        // history (the mock thread fits, so its top would also be its end).
        const int count = list->property("count").toInt();
        QVERIFY(count >= 2);
        QQuickItem *row = nullptr;
        QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, row),
                                          Q_ARG(int, count - 2)));
        QVERIFY(row);
        row->setProperty("heightSeed", row->implicitHeight() + 2 * list->height());
        row->setProperty("heightSeedActive", true);
        QTRY_VERIFY(list->property("contentHeight").toReal() > list->height() * 2);
        QTRY_VERIFY(atBottomEdge(list));
        QTest::qWait(100);
        list->setProperty("followLatest", false);
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTRY_VERIFY(!list->property("atYEnd").toBool());
        const qreal parkedAt = list->property("contentY").toReal()
                               - list->property("originY").toReal();
        const int readsBefore = mock()->markThreadReadCallsForTest();
        QQuickItem *pill = item("threadJumpToLatestButton");
        QVERIFY(pill);
        QTRY_VERIFY(pill->isVisible());

        m_controller->thread()->sendText(QStringLiteral("arrived below"));
        QTRY_COMPARE(list->property("unseenReplies").toInt(), 1);
        QVERIFY(!list->property("followLatest").toBool());
        QTest::qWait(300);
        const qreal nowAt = list->property("contentY").toReal()
                            - list->property("originY").toReal();
        QVERIFY2(qAbs(nowAt - parkedAt) < 1.0,
                 qPrintable(QStringLiteral("the reader must keep their place "
                                           "in the history: parked %1, now %2")
                                .arg(parkedAt).arg(nowAt)));
        QCOMPARE(mock()->markThreadReadCallsForTest(), readsBefore);

        click(pill);
        QTRY_VERIFY(list->property("followLatest").toBool());
        QTRY_COMPARE(list->property("unseenReplies").toInt(), 0);
        QTRY_VERIFY(!pill->isVisible());
    }

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
        QCOMPARE(m_controller->thread()->property("text").toString(), text);

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
        QTest::keyClick(m_window, Qt::Key_Space,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(list->hasActiveFocus());
        auto *model = m_controller->thread()->model();
        const QString newest = model->eventIdAt(model->rowCount() - 1);
        QTRY_COMPARE(list->property("keyboardSelectedEventId").toString(), newest);
        QTest::keyClick(m_window, Qt::Key_K);
        const QString older = list->property("keyboardSelectedEventId").toString();
        QVERIFY(!older.isEmpty() && older != newest && older != root);
        QTest::keyClick(m_window, Qt::Key_J);
        QCOMPARE(list->property("keyboardSelectedEventId").toString(), newest);
        QTest::keyClick(m_window, Qt::Key_K);
        QTest::keyClick(m_window, Qt::Key_R);
        QTRY_COMPARE(m_controller->thread()->property("replyToEventId").toString(),
                     older);
        auto *editor = item("threadComposerInput");
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(editor->hasActiveFocus());
    }
};

QTEST_MAIN(ThreadStyleQmlTest)
#include "ThreadStyleQmlTest.moc"
