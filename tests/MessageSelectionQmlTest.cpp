#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/QuickSwitcherModel.h"
#include "models/MessageComposer.h"
#include "models/PaginationController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/ReverseListProxyModel.h"

class MessageSelectionQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name) const
    {
        return m_window->findChild<QQuickItem *>(QString::fromLatin1(name));
    }

    QObject *switcher() const
    {
        const auto objects = m_window->findChildren<QObject *>();
        for (QObject *object : objects) {
            if (QByteArray(object->metaObject()->className())
                    .startsWith("QuickSwitcher"))
                return object;
        }
        return nullptr;
    }

    void openSwitcher()
    {
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTRY_VERIFY(item("quickSwitcherField")->hasActiveFocus());
    }

    int resultRow(bool invite = false, bool space = false) const
    {
        auto *model = m_controller->quickSwitcher();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariantMap result = model->resultAt(row);
            if (result.value(QStringLiteral("isInvite")).toBool() == invite
                && result.value(QStringLiteral("isSpace")).toBool() == space)
                return row;
        }
        return -1;
    }

private:
    void openJoinedRoom()
    {
        openSwitcher();
        const int row = resultRow();
        QVERIFY(row >= 0);
        QVERIFY(item("quickSwitcherList")->setProperty("currentIndex", row));
        const QString roomId = m_controller->quickSwitcher()->resultAt(row)
                                   .value(QStringLiteral("roomId")).toString();
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_controller->currentRoomId(), roomId);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        QTRY_VERIFY(m_controller->timeline()->realEventCount() > 2);
        // Let upstream's one-shot automatic composer focus settle. It must
        // not make an absent explicit shortcut look as if it worked.
        QTest::qWait(300);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + "/data").toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("composer-focus-test"));
    }

    void init()
    {
        QSettings().clear();
        // In-memory secrets and deterministic rooms, Spaces and invitations;
        // the screenshot UI itself is not enabled.
        m_controller = new AppController(AppController::MockBackend, true);
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setScreenshotDemoMode(true);
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
    width: 1100
    height: 800
    visible: true
    MainScreen { anchors.fill: parent }
}
)QML", QUrl(QStringLiteral("composer-focus-scene.qml")));
        QObject *root = scene.create();
        QVERIFY2(root, qPrintable(scene.errorString()));
        m_window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QVERIFY(switcher());
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

    void resumeShortcutLeavesComposeWithoutLosingTheDraft_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("markdown") << QStringLiteral("markdown");
        QTest::newRow("rich") << QStringLiteral("rich");
    }

    void resumeShortcutLeavesComposeWithoutLosingTheDraft()
    {
        QFETCH(QString, mode);
        openJoinedRoom();
        QVERIFY(m_controller->settings()->setProperty("composerMode", mode));
        m_controller->composer()->setText(QStringLiteral("keep this draft"));
        auto *editor = item(mode == QStringLiteral("rich")
                                ? "composerRichInput" : "composerInput");
        QVERIFY(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->property("text").toString().contains("keep this draft"));
        const QString draft = m_controller->composer()->text();
        QTRY_VERIFY(editor->hasActiveFocus());
        QVERIFY(item("timelinePane")->property("timelineFocusable").toBool());
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QCOMPARE(m_controller->composer()->text(), draft);
        QVERIFY(!item("timelinePane")->property("keyboardSelectedEventId")
                     .toString().isEmpty());
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(editor->hasActiveFocus());
        QTRY_COMPARE(item("timelinePane")->property("keyboardSelectedEventId")
                         .toString(), QString());
        QCOMPARE(m_controller->composer()->text(), draft);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTRY_VERIFY(!item("timelinePane")->property("keyboardSelectedEventId")
                         .toString().isEmpty());
    }

    void invalidReplyTargetsAreSkipped_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"redacted", "local", "state", "call"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void invalidReplyTargetsAreSkipped()
    {
        QFETCH(QString, kind);
        openJoinedRoom();
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        TimelineEvent event;
        event.eventId = kind == "local" ? "local:review" : "$review-invalid";
        event.sender = "@peer:example.org";
        event.body = "Not a reply target";
        event.timestamp = QDateTime::currentDateTimeUtc();
        event.redacted = kind == "redacted";
        event.isLocalEcho = kind == "local";
        if (kind == "state") event.type = TimelineEvent::StateChange;
        if (kind == "call") event.type = TimelineEvent::CallEvent;
        mock->appendEventForTest(m_controller->currentRoomId(), event);
        QTest::qWait(100);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTest::qWait(50);
        const auto selected = item("timelinePane")->property("keyboardSelectedEventId").toString();
        QVERIFY(!selected.isEmpty());
        QVERIFY(selected != event.eventId);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(item("timelinePane")->property("keyboardSelectedEventId").toString(), selected);
    }

    void selectionEscapesAnExcludedViewport_data()
    {
        QTest::addColumn<bool>("older");
        QTest::newRow("older") << true;
        QTest::newRow("newer") << false;
    }

    void selectionEscapesAnExcludedViewport()
    {
        QFETCH(bool, older);
        openJoinedRoom();
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QList<TimelineEvent> events;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent event;
            event.eventId = QStringLiteral("$excluded-viewport-%1").arg(i);
            event.sender = "@peer:example.org";
            event.body = "Selection boundary";
            event.timestamp = QDateTime::currentDateTimeUtc().addSecs(i);
            if (i == 1)
                event.type = TimelineEvent::CallEvent;
            events.append(event);
        }
        Q_EMIT mock->eventsInsertedAt(m_controller->currentRoomId(),
                                      m_controller->timeline()->events().size(), events);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        auto *view = item("timelineListView");
        auto *pane = item("timelinePane");
        QTRY_VERIFY(view->hasActiveFocus());
        QTest::qWait(50);
        QVariant row, value;
        QVERIFY(QMetaObject::invokeMethod(view, "viewRowForStableId",
            Q_RETURN_ARG(QVariant, row), Q_ARG(QVariant, QVariant(events.at(1).eventId))));
        QVERIFY(QMetaObject::invokeMethod(view, "itemAtViewRow",
            Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, row)));
        auto *card = qobject_cast<QQuickItem *>(value.value<QObject *>());
        QVERIFY(card);
        QVERIFY(!card->property("replyEligible").toBool());
        // Make the excluded region taller than the viewport without depending
        // on theme-specific call-card sizing or creating a real call.
        card->setHeight(view->height() * 3);
        QTest::qWait(100);
        QVERIFY(view->setProperty("contentY", card->y() + view->height() * 0.5));
        QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
        const qreal before = view->property("contentY").toReal();
        QVERIFY(before > card->y());
        QVERIFY(before + view->height() < card->y() + card->height());
        QVERIFY(view->property("selectionDetached").toBool());
        QTest::keyClick(m_window, older ? Qt::Key_K : Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(),
                 older ? events.first().eventId : events.last().eventId);
        QVERIFY(older ? view->property("contentY").toReal() > before
                      : view->property("contentY").toReal() < before);
    }

    void redactingSelectedMessagePreventsReply()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(50);
        const auto selected = item("timelinePane")->property("keyboardSelectedEventId").toString();
        const int row = m_controller->timeline()->rowForStableId(selected);
        QVERIFY(row >= 0);
        auto event = m_controller->timeline()->events().at(row);
        event.redacted = true;
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        Q_EMIT mock->eventChangedAt(m_controller->currentRoomId(), row, event);
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QVERIFY(!m_controller->composer()->isReplying());
    }

    void rowFocusCannotReplyToHiddenSelection()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(50);
        auto *view = item("timelineListView");
        QVariant value;
        QVERIFY(QMetaObject::invokeMethod(view, "itemAtViewRow",
            Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, QVariant(1))));
        auto *row = qobject_cast<QQuickItem *>(value.value<QObject *>());
        QVERIFY(row);
        row->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(row->hasActiveFocus());
        QVERIFY(!view->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QVERIFY(!m_controller->composer()->isReplying());
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(view->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QVERIFY(m_controller->composer()->isReplying());
    }

    void latestEntryKeepsPresentationBounded()
    {
        openJoinedRoom();
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QList<TimelineEvent> events;
        for (int i = 0; i < 900; ++i) {
            TimelineEvent event;
            event.eventId = QStringLiteral("$bounded-%1").arg(i);
            event.sender = "@peer:example.org";
            event.body = "Window regression message";
            event.timestamp = QDateTime::currentDateTimeUtc().addSecs(i);
            events.append(event);
        }
        Q_EMIT mock->eventsInsertedAt(m_controller->currentRoomId(),
                                      m_controller->timeline()->events().size(), events);
        auto *proxy = qobject_cast<ReverseListProxyModel *>(m_controller->timelineView());
        QVERIFY(proxy);
        proxy->setWindow(600, 100);
        int peak = proxy->rowCount();
        QObject observer;
        auto sample = [&] { peak = qMax(peak, proxy->rowCount()); };
        connect(proxy, &QAbstractItemModel::rowsInserted, &observer, sample);
        connect(proxy, &QAbstractItemModel::modelReset, &observer, sample);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(100);
        QCOMPARE(proxy->windowSkip(), 0);
        QVERIFY2(peak <= 320, qPrintable(QStringLiteral("Peak exposed rows: %1").arg(peak)));
        QCOMPARE(item("timelinePane")->property("keyboardSelectedEventId").toString(),
                 events.last().eventId);
    }

    void halfPageDownCrossesWindowBoundaryWithoutSettling()
    {
        openJoinedRoom();
        auto *proxy = qobject_cast<ReverseListProxyModel *>(m_controller->timelineView());
        QVERIFY(proxy);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(50);
        auto *view = item("timelineListView");
        proxy->setWindow(5, 10);
        QTest::qWait(30);
        QVariant minimum;
        QVERIFY(QMetaObject::invokeMethod(view, "wheelMinY", Q_RETURN_ARG(QVariant, minimum)));
        QVERIFY(view->setProperty("contentY", minimum));
        QCOMPARE(proxy->windowSkip(), 5);
        const auto selected = item("timelinePane")->property("keyboardSelectedEventId");
        // No event-loop pause: a held key must not rely on the settle timer.
        for (int i = 0; i < 8; ++i)
            QTest::keyClick(m_window, Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(proxy->windowSkip(), 0);
        QCOMPARE(item("timelinePane")->property("keyboardSelectedEventId"), selected);
    }

    void newerMessageCrossesWindowBoundary()
    {
        openJoinedRoom();
        auto *proxy = qobject_cast<ReverseListProxyModel *>(m_controller->timelineView());
        QVERIFY(proxy);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(50);
        auto *view = item("timelineListView");
        proxy->setWindow(5, 10);
        QTest::qWait(30);
        QVariant minimum, selected;
        QVERIFY(QMetaObject::invokeMethod(view, "wheelMinY", Q_RETURN_ARG(QVariant, minimum)));
        QVERIFY(view->setProperty("contentY", minimum));
        QVERIFY(QMetaObject::invokeMethod(view, "eventIdAtViewRow",
            Q_RETURN_ARG(QVariant, selected), Q_ARG(QVariant, QVariant(0))));
        QVERIFY(!selected.toString().isEmpty());
        QVERIFY(item("timelinePane")->setProperty("keyboardSelectedEventId", selected));
        const int sourceRow = m_controller->timeline()->rowForStableId(selected.toString());
        QVERIFY(sourceRow >= 0);
        const auto expected = m_controller->timeline()->events().at(sourceRow + 1).eventId;
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(proxy->windowSkip(), 0);
        QCOMPARE(item("timelinePane")->property("keyboardSelectedEventId").toString(), expected);
    }

    void removedSelectionCannotReplyToAnotherRow()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        auto *pane = item("timelinePane");
        const QString selected = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(!selected.isEmpty());
        const int row = m_controller->timeline()->rowForStableId(selected);
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        Q_EMIT mock->eventRemovedAt(m_controller->currentRoomId(), row);
        QTRY_COMPARE(pane->property("keyboardSelectedEventId").toString(), QString());
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QVERIFY(!m_controller->composer()->isReplying());
    }

    void resumePreservesHistoricalSelectionAndDraft_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("markdown") << QStringLiteral("markdown");
        QTest::newRow("rich") << QStringLiteral("rich");
    }

    void resumePreservesHistoricalSelectionAndDraft()
    {
        QFETCH(QString, mode);
        openJoinedRoom();
        QVERIFY(m_controller->settings()->setProperty("composerMode", mode));
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        auto *pane = item("timelinePane");
        auto *view = item("timelineListView");
        QTRY_VERIFY(view->hasActiveFocus());
        QTest::qWait(50);
        const QString latest = pane->property("keyboardSelectedEventId").toString();
        for (int step = 0; step < 5; ++step)
            QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        const QString history = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(!history.isEmpty());
        QVERIFY(history != latest);
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        auto *editor = item(mode == QStringLiteral("rich")
                                ? "composerRichInput" : "composerInput");
        QTRY_VERIFY(editor->hasActiveFocus());
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), QString());
        m_controller->composer()->setText(QStringLiteral("historical draft"));
        QTRY_VERIFY(editor->property("text").toString().contains("historical draft"));
        const QString draft = m_controller->composer()->text();
        const qreal y = view->property("contentY").toReal();
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(view->hasActiveFocus());
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), history);
        QCOMPARE(view->property("contentY").toReal(), y);
        QCOMPARE(m_controller->composer()->text(), draft);
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(pane->property("keyboardSelectedEventId").toString(), latest);
    }

    void resumeShortcutPreservesReplyAndDoesNotStealSwitcherFocus()
    {
        openJoinedRoom();
        QCOMPARE(m_controller->shortcuts()->setBinding(
                     QStringLiteral("timeline.resumeMessageSelection"), QStringLiteral("Ctrl+Alt+N")),
                 QString());
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        auto *pane = item("timelinePane");
        auto *view = item("timelineListView");
        QTRY_VERIFY(view->hasActiveFocus());
        QTest::qWait(50);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        const QString history = pane->property("keyboardSelectedEventId").toString();
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        QCOMPARE(m_controller->composer()->replyingToEventId(), history);
        QTest::keyClick(m_window, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(view->hasActiveFocus());
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), history);
        QCOMPARE(m_controller->composer()->replyingToEventId(), history);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        const QString nextReply = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(nextReply != history);
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        QCOMPARE(m_controller->composer()->replyingToEventId(), nextReply);
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        openSwitcher();
        QTest::keyClick(m_window, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(item("quickSwitcherField")->hasActiveFocus());
    }

    void resumeFromUnselectedHistoryDoesNotJumpToLatest()
    {
        openJoinedRoom();
        auto *pane = item("timelinePane");
        auto *view = item("timelineListView");
        QVERIFY(QMetaObject::invokeMethod(view, "moveKeyboardViewport",
            Q_ARG(QVariant, QVariant(view->property("contentY").toReal()
                                     + view->height() * 0.5))));
        QTest::qWait(100);
        const qreal y = view->property("contentY").toReal();
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(view->hasActiveFocus());
        QCOMPARE(view->property("contentY").toReal(), y);
        QVERIFY(!pane->property("keyboardSelectedEventId").toString().isEmpty());
    }

    void removedHistoryIsNotRememberedForResume()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        auto *pane = item("timelinePane");
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTest::qWait(50);
        const QString selected = pane->property("keyboardSelectedEventId").toString();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(pane->property("resumeSelectedEventId").toString(), selected);
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        Q_EMIT mock->eventRemovedAt(m_controller->currentRoomId(),
                                    m_controller->timeline()->rowForStableId(selected));
        QTRY_COMPARE(pane->property("resumeSelectedEventId").toString(), QString());
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QVERIFY(pane->property("keyboardSelectedEventId").toString() != selected);
    }

    void keyboardStepsAndRepliesToTheSelectedMessage()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        auto *pane = item("timelinePane");
        const QString initial = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(!initial.isEmpty());
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(pane->property("keyboardSelectedEventId").toString() != initial);
        QVERIFY(!switcher()->property("visible").toBool());
        const QString older = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(m_controller->timeline()->rowForStableId(older)
                < m_controller->timeline()->rowForStableId(initial));
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QTRY_COMPARE(pane->property("keyboardSelectedEventId").toString(), initial);
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QTRY_COMPARE(m_controller->composer()->replyingToEventId(), initial);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
    }

    void entrySelectsLatestAndVisibleStepsDoNotPage()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        auto *pane = item("timelinePane");
        auto *view = item("timelineListView");
        const QString selected = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(!selected.isEmpty());
        const int selectedRow = m_controller->timeline()->rowForStableId(selected);
        for (int row = selectedRow + 1; row < m_controller->timeline()->rowCount(); ++row) {
            const QString id = m_controller->timeline()->eventIdAt(row);
            const QVariantMap details = m_controller->timeline()->messageDetails(id);
            QVERIFY(details.isEmpty() || details.value("redacted").toBool());
        }
        QTest::qWait(50); // Drain the bounded row-window exposure callback.
        const qreal before = view->property("contentY").toReal();
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(pane->property("keyboardSelectedEventId").toString() != selected);
        QCOMPARE(view->property("contentY").toReal(), before);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), selected);
        QCOMPARE(view->property("contentY").toReal(), before);
    }

    void halfPageKeepsSelectionUntilAMessageKeyIsUsed_data()
    {
        QTest::addColumn<bool>("older");
        QTest::newRow("older-from-newest-visible") << true;
        QTest::newRow("newer-from-oldest-visible") << false;
    }

    void halfPageKeepsSelectionUntilAMessageKeyIsUsed()
    {
        QFETCH(bool, older);
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTest::qWait(50);
        auto *pane = item("timelinePane");
        auto *view = item("timelineListView");
        const QString before = pane->property("keyboardSelectedEventId").toString();
        QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), before);
        const qreal pageY = view->property("contentY").toReal();
        QTest::qWait(300);
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), before);
        QString edge;
        for (int index = 0; index < view->property("count").toInt(); ++index) {
            QVariant value;
            QVERIFY(QMetaObject::invokeMethod(view, "itemAtViewRow",
                Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, QVariant(index))));
            auto *message = qobject_cast<QQuickItem *>(value.value<QObject *>());
            if (!message || message->metaObject()->indexOfMethod(
                                "replyOnDoubleTap(QVariant,QVariant)") < 0
                || message->y() >= pageY + view->height()
                || message->y() + message->height() <= pageY)
                continue;
            QVariant id;
            QVERIFY(QMetaObject::invokeMethod(view, "eventIdAtViewRow",
                Q_RETURN_ARG(QVariant, id), Q_ARG(QVariant, QVariant(index))));
            edge = id.toString();
            if (older)
                break;
        }
        QVERIFY(!edge.isEmpty());
        QTest::keyClick(m_window, older ? Qt::Key_K : Qt::Key_J, Qt::ControlModifier);
        const QString selected = pane->property("keyboardSelectedEventId").toString();
        QCOMPARE(selected, edge);
        QVERIFY(!selected.isEmpty());
        QVERIFY(selected != before);
        QVariant row;
        QVERIFY(QMetaObject::invokeMethod(view, "viewRowForStableId",
            Q_RETURN_ARG(QVariant, row), Q_ARG(QVariant, QVariant(selected))));
        QVariant value;
        QVERIFY(QMetaObject::invokeMethod(view, "itemAtViewRow",
            Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, row)));
        auto *message = qobject_cast<QQuickItem *>(value.value<QObject *>());
        QVERIFY(message);
        const qreal y = view->property("contentY").toReal();
        // Paging selects the newest intersecting message, including a row
        // clipped at the edge, without scrolling back to reveal it in full.
        QVERIFY(message->y() < y + view->height());
        QVERIFY(message->y() + message->height() > y);
        QCOMPARE(y, pageY);
        QTest::qWait(300); // Settling must not reselect a center row.
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), selected);
    }

    void halfPagingAdvancesThroughTallMessage_data()
    {
        QTest::addColumn<qreal>("heightInViewports");
        QTest::addColumn<int>("pagesWithinMessage");
        QTest::newRow("media-shorter-than-viewport") << qreal(0.75) << 1;
        QTest::newRow("viewport-sized") << qreal(1) << 1;
        QTest::newRow("several-pages") << qreal(3) << 3;
    }

    void halfPagingAdvancesThroughTallMessage()
    {
        QFETCH(qreal, heightInViewports);
        QFETCH(int, pagesWithinMessage);
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        auto *view = item("timelineListView");
        auto *pane = item("timelinePane");
        QTRY_VERIFY(view->hasActiveFocus());
        QTest::qWait(50);
        const QString selected = pane->property("keyboardSelectedEventId").toString();
        QVariant row;
        QVERIFY(QMetaObject::invokeMethod(view, "viewRowForStableId",
            Q_RETURN_ARG(QVariant, row), Q_ARG(QVariant, QVariant(selected))));
        QVariant value;
        QVERIFY(QMetaObject::invokeMethod(view, "itemAtViewRow",
            Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, row)));
        auto *message = qobject_cast<QQuickItem *>(value.value<QObject *>());
        QVERIFY(message);
        // A media row can span several half-pages. Keep its selection while
        // scrolling through it; selecting it must not undo viewport progress.
        message->setHeight(view->height() * heightInViewports);
        QTest::qWait(100);
        QVERIFY(QMetaObject::invokeMethod(view, "enterMessageNavigation"));
        QTest::qWait(50);
        for (int page = 0; page < pagesWithinMessage; ++page) {
            const qreal before = view->property("contentY").toReal();
            QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
            QVERIFY2(view->property("contentY").toReal() > before + 1,
                     "Selecting a tall row must not undo an upward half-page");
            QTest::qWait(100);
            QVERIFY(view->property("contentY").toReal() > before + 1);
            QCOMPARE(pane->property("keyboardSelectedEventId").toString(), selected);
        }
        const qreal before = view->property("contentY").toReal();
        QTest::keyClick(m_window, Qt::Key_D, Qt::ControlModifier);
        QVERIFY(view->property("contentY").toReal() < before - 1);
        // Continue beyond the media row into older messages, rather than only
        // proving that the viewport can move within the same selection.
        for (int page = 0; page < 6; ++page) {
            QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
            QTest::qWait(30);
        }
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), selected);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(pane->property("keyboardSelectedEventId").toString() != selected);
    }

    void halfPagingDoesNotToggleThePeoplePanel()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        const bool panel = m_controller->settings()->memberPanelOpen();
        const qreal before = item("timelineListView")->property("contentY").toReal();
        QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
        QTRY_VERIFY(item("timelineListView")->property("contentY").toReal() > before);
        QCOMPARE(m_controller->settings()->memberPanelOpen(), panel);
    }

    void reboundNavigationMatchesOnlyInsideTheTimeline()
    {
        openJoinedRoom();
        auto *registry = m_controller->shortcuts();
        QCOMPARE(registry->setBinding(QStringLiteral("timeline.olderMessage"),
                                      QStringLiteral("Ctrl+N")), QString());
        QCOMPARE(registry->messageSelectionActionForKey(Qt::Key_N, Qt::ControlModifier),
                 QStringLiteral("timeline.olderMessage"));
        QVERIFY(registry->messageSelectionActionForKey(Qt::Key_K,
                                               Qt::ControlModifier).isEmpty());
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        auto *pane = item("timelinePane");
        const QString initial = pane->property("keyboardSelectedEventId").toString();
        QTest::keyClick(m_window, Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(pane->property("keyboardSelectedEventId").toString() != initial);
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        const QString selected = pane->property("keyboardSelectedEventId").toString();
        QTest::keyClick(m_window, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), selected);
    }

    void plainNavigationKeysLeaveComposerTypingAlone_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("markdown") << QStringLiteral("markdown");
        QTest::newRow("rich") << QStringLiteral("rich");
    }

    void plainNavigationKeysLeaveComposerTypingAlone()
    {
        QFETCH(QString, mode);
        openJoinedRoom();
        QVERIFY(m_controller->settings()->setProperty("composerMode", mode));
        auto *registry = m_controller->shortcuts();
        QCOMPARE(registry->setBinding(QStringLiteral("timeline.olderMessage"),
                                      QStringLiteral("K")), QString());
        QCOMPARE(registry->setBinding(QStringLiteral("timeline.newerMessage"),
                                      QStringLiteral("J")), QString());
        QCOMPARE(registry->setBinding(QStringLiteral("timeline.reply"),
                                      QStringLiteral("R")), QString());
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        auto *pane = item("timelinePane");
        const QString newest = pane->property("keyboardSelectedEventId").toString();
        QVERIFY(!newest.isEmpty());
        QTest::keyClick(m_window, Qt::Key_K);
        QTRY_VERIFY(pane->property("keyboardSelectedEventId").toString() != newest);
        QTest::keyClick(m_window, Qt::Key_J);
        QTRY_COMPARE(pane->property("keyboardSelectedEventId").toString(), newest);
        QTest::keyClick(m_window, Qt::Key_R);
        QTRY_COMPARE(m_controller->composer()->replyingToEventId(), newest);
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        auto *editor = item(mode == QStringLiteral("rich")
                                ? "composerRichInput" : "composerInput");
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_J);
        QTest::keyClick(m_window, Qt::Key_K);
        QTest::keyClick(m_window, Qt::Key_R);
        QTRY_VERIFY(editor->property("text").toString().contains(QStringLiteral("jkr")));
        QVERIFY(pane->property("keyboardSelectedEventId").toString().isEmpty());
    }

    void enteringSelectionCancelsQueuedAndDelayedComposerFocus()
    {
        openJoinedRoom();
        m_controller->requestComposerFocus(m_controller->currentRoomId());
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTest::qWait(350);
        QVERIFY(item("timelineListView")->hasActiveFocus());
        QVERIFY(!item("timelinePane")->property("keyboardSelectedEventId").toString().isEmpty());

        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        m_controller->requestComposerFocus(m_controller->currentRoomId());
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTest::qWait(350);
        QVERIFY(item("timelineListView")->hasActiveFocus());
    }

    void roomPickerRestoresLatestIntent_data()
    {
        QTest::addColumn<QString>("entry");
        QTest::newRow("select-latest") << QStringLiteral("selection");
        QTest::newRow("upstream-latest") << QStringLiteral("upstream");
        QTest::newRow("preserve-history") << QStringLiteral("history");
    }

    void roomPickerRestoresLatestIntent()
    {
        QFETCH(QString, entry);
        openJoinedRoom();
        const QString roomA = m_controller->currentRoomId();
        auto *view = item("timelineListView");
        auto *pagination = m_controller->pagination();
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(view);
        QVERIFY(mock);

        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(view->hasActiveFocus());
        QTest::qWait(50);
        QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
        QTRY_VERIFY(!view->property("stickToBottom").toBool());
        // First save a genuine historical viewport, with no pending scroll
        // timer left to incidentally save a subsequent latest jump.
        QTest::qWait(350);
        if (entry == QStringLiteral("selection")) {
            QTest::keyClick(m_window, Qt::Key_Space,
                           Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY(view->property("stickToBottom").toBool());
        } else if (entry == QStringLiteral("upstream")) {
            QVERIFY(QMetaObject::invokeMethod(view, "goToLatest"));
            QTRY_VERIFY(view->property("stickToBottom").toBool());
        }
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        QTest::qWait(350);

        openSwitcher();
        int otherRow = -1;
        for (int row = 0; row < m_controller->quickSwitcher()->rowCount(); ++row) {
            const auto result = m_controller->quickSwitcher()->resultAt(row);
            if (result.value(QStringLiteral("roomId")).toString() != roomA
                    && !result.value(QStringLiteral("isSpace")).toBool()
                    && !result.value(QStringLiteral("isInvite")).toBool()) {
                otherRow = row;
                break;
            }
        }
        QVERIFY(otherRow >= 0);
        const QString roomB = m_controller->quickSwitcher()->resultAt(otherRow)
                                  .value(QStringLiteral("roomId")).toString();
        QVERIFY(item("quickSwitcherList")->setProperty("currentIndex", otherRow));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_controller->currentRoomId(), roomB);
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        QTest::qWait(350);

        TimelineEvent incoming;
        incoming.eventId = QStringLiteral("$arrived-while-away");
        incoming.sender = QStringLiteral("@peer:example.org");
        incoming.body = QStringLiteral("New message while viewing another room");
        incoming.timestamp = QDateTime::currentDateTimeUtc();
        mock->appendEventForTest(roomA, incoming);
        QSignalSpy latest(pagination, &PaginationController::restoreLatestRequested);
        QSignalSpy historical(pagination, &PaginationController::targetLocated);
        openSwitcher();
        int returnRow = -1;
        for (int row = 0; row < m_controller->quickSwitcher()->rowCount(); ++row) {
            if (m_controller->quickSwitcher()->resultAt(row)
                    .value(QStringLiteral("roomId")).toString() == roomA) {
                returnRow = row;
                break;
            }
        }
        QVERIFY(returnRow >= 0);
        QVERIFY(item("quickSwitcherList")->setProperty("currentIndex", returnRow));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_controller->currentRoomId(), roomA);
        QTRY_VERIFY(m_controller->timeline()->rowForStableId(incoming.eventId) >= 0);
        if (entry == QStringLiteral("history")) {
            QTRY_VERIFY(!historical.isEmpty());
            QCOMPARE(latest.count(), 0);
            QTRY_VERIFY(!view->property("stickToBottom").toBool());
        } else {
            QTRY_VERIFY(!latest.isEmpty());
            QCOMPARE(historical.count(), 0);
            QTRY_VERIFY(view->property("stickToBottom").toBool());
        }
    }

    void roomChangesClearRememberedSelection()
    {
        openJoinedRoom();
        QTest::keyClick(m_window, Qt::Key_Space,
                       Qt::ControlModifier | Qt::ShiftModifier);
        auto *pane = item("timelinePane");
        QTRY_VERIFY(item("timelineListView")->hasActiveFocus());
        QTRY_VERIFY(!pane->property("keyboardSelectedEventId").toString().isEmpty());
        const QString previous = pane->property("keyboardSelectedEventId").toString();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(pane->property("resumeSelectedEventId").toString(), previous);
        QString otherRoom;
        auto *rooms = m_controller->roomList();
        for (int row = 0; row < rooms->rowCount(); ++row) {
            const QString roomId = rooms->data(rooms->index(row),
                                               RoomListModel::RoomIdRole).toString();
            const auto result = rooms->findRoom(roomId);
            if (result.value(QStringLiteral("membership")).toString()
                    == QStringLiteral("joined")
                && !result.value(QStringLiteral("isSpace")).toBool()
                && roomId != m_controller->currentRoomId()) {
                otherRoom = roomId;
                break;
            }
        }
        QVERIFY(!otherRoom.isEmpty());
        m_controller->openRoom(otherRoom);
        QTRY_COMPARE(m_controller->currentRoomId(), otherRoom);
        QCOMPARE(pane->property("keyboardSelectedEventId").toString(), QString());
        QCOMPARE(pane->property("resumeSelectedEventId").toString(), QString());
    }

    void shortcutCaptureAcceptsPlainTimelineKeysWithoutWarning()
    {
        QQmlComponent component(m_engine);
        component.setData(R"QML(
import QtQuick
import MatrixClient
ShortcutRow {
    width: 600
    actionId: "nav.quickSwitcher"
    description: "Test shortcut capture"
}
)QML", QUrl(QStringLiteral("timeline-shortcut-capture.qml")));
        auto *row = qobject_cast<QQuickItem *>(component.create());
        QVERIFY2(row, qPrintable(component.errorString()));
        row->setParent(m_window);
        row->setParentItem(m_window->contentItem());
        row->setZ(100);
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QTest::keyClick(m_window, Qt::Key_K);
        QTRY_VERIFY(!row->property("captureError").toString().isEmpty());
        QTRY_VERIFY(row->findChild<QQuickItem *>(
            QStringLiteral("shortcutCaptureError_nav.quickSwitcher")));
        QVERIFY(row->setProperty("actionId", QStringLiteral("timeline.olderMessage")));
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QTest::keyClick(m_window, Qt::Key_K);
        QTRY_VERIFY(!row->property("capturing").toBool());
        QVERIFY(row->property("captureError").toString().isEmpty());
        QTRY_VERIFY(!row->findChild<QQuickItem *>(
            QStringLiteral("shortcutCaptureError_timeline.olderMessage")));
        QCOMPARE(m_controller->shortcuts()->sequenceFor(
                     QStringLiteral("timeline.olderMessage")), QStringLiteral("K"));
    }

};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MessageSelectionQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MessageSelectionQmlTest.moc"
