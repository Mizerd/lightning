// ThreadPanel.qml's backward pagination (app.thread.model.requestOlder(), a
// ListView prepend) must keep the reader's place, like TimelinePane's
// captureAnchor()/restoreCapturedAnchor(). The restore reads the live
// contentY and applies a relative shift, so a reader still scrolling when
// the page lands is not snapped back.
//
// Same method as TimelinePaneQmlTest's
// paginationAnchorRestorePreservesConcurrentScroll (a direct contentY write
// stands in for a continuing gesture), adapted to ThreadPanel's replyList.
#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

namespace {
constexpr int kSignalTimeoutMs = 2000;
const QString kGeneral = QStringLiteral("!general:mock.local");
}

class ThreadPanelPaginationAnchorQmlTest : public QObject
{
    Q_OBJECT

private:
    static bool login(AppController &controller)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        return loginSpy.wait(kSignalTimeoutMs);
    }

    // First fixture thread root of a room: the first event some other event
    // names as its threadRootId (mirrors ThreadControllerTest.cpp's
    // firstThreadRootId).
    static QString firstThreadRootId(MockMatrixClient &client,
                                     const QString &roomId)
    {
        const auto events = client.timeline(roomId);
        for (const auto &e : events) {
            if (!e.threadRootId.isEmpty())
                return e.threadRootId;
        }
        return {};
    }

private Q_SLOTS:
    void initTestCase()
    {
        // Sandbox before the first AppController exists, or logging in as the
        // mock user would remove the real config's `activeAccount` key.
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("thread-panel-pagination-anchor-qml-test"));
        QSettings().clear();
    }

    void threadPaginationAnchorRestorePreservesConcurrentScroll()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        const QString rootId = firstThreadRootId(*mock, kGeneral);
        QVERIFY(!rootId.isEmpty());
        controller.thread()->openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        const QString timelineId =
            MatrixClient::threadTimelineId(kGeneral, rootId);

        mock->setPaginationDelayForTest(60);
        // Replace the built-in fixture with 30 replies, more than the
        // viewport. Resetting the composite thread timeline id reloads the
        // model like a real room. The first row is the thread ROOT: an open
        // thread whose root is not loaded fills its history on a reset
        // (break-it B022), which would spend this fixture's one page before
        // the request under test.
        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            if (i == 0)
                e.eventId = rootId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("reply %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(timelineId, events, /*paginationPages=*/1);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.thread()->model()->rowCount() >= 30, kSignalTimeoutMs);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("ThreadPanel"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(400, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *replyList = root->findChild<QQuickItem *>(
            QStringLiteral("threadReplyList"));
        QVERIFY(replyList != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(replyList->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);

        const int anchorRow = 15;
        QVERIFY(replyList->setProperty("followLatest", false));
        QVERIFY(QMetaObject::invokeMethod(replyList, "positionViewAtIndex",
                                          Q_ARG(int, anchorRow),
                                          Q_ARG(int, 0 /*ListView.Beginning*/)));
        QCoreApplication::processEvents();

        // A real near-top request. The mock flips paginating() synchronously,
        // so ThreadPanel's onPaginationChanged handler captures the anchor
        // before this returns.
        const int countBefore = replyList->property("count").toInt();
        controller.thread()->model()->requestOlder();
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            !replyList->property("anchorStableId").toString().isEmpty(), 2000);

        const QString capturedAnchorId =
            replyList->property("anchorStableId").toString();
        const double anchorItemY =
            replyList->property("anchorItemY").toDouble();
        const double capturedContentY =
            replyList->property("contentY").toDouble();

        // Simulate the reader continuing to scroll while the request is
        // still in flight: a direct contentY write, exactly what
        // threadWheelHandler's pixelDelta branch performs.
        constexpr double simulatedScrollDelta = 40.0;
        const double scrolledContentY = capturedContentY - simulatedScrollDelta;
        QVERIFY(replyList->setProperty("contentY", scrolledContentY));

        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("count").toInt() > countBefore,
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("anchorStableId").toString().isEmpty(),
            kSignalTimeoutMs);

        // Independently re-derive the anchor row's real post-prepend
        // geometry rather than trusting anything the panel itself reports.
        const int newRow =
            controller.thread()->model()->rowForStableId(capturedAnchorId);
        QVERIFY(newRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(QMetaObject::invokeMethod(replyList, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, anchorItem),
                                          Q_ARG(int, newRow)));
        QVERIFY(anchorItem != nullptr);

        // The correct relative restore: whatever contentY was at the moment
        // of restore (the reader's continued scroll), shifted by exactly
        // how far the prepend moved the anchor row's own y.
        const double expected =
            scrolledContentY + (anchorItem->y() - anchorItemY);
        const double actual = replyList->property("contentY").toDouble();
        QVERIFY2(qAbs(actual - expected) < 1.0,
                 qPrintable(QStringLiteral(
                     "thread pagination restore discarded concurrent "
                     "scroll: actual=%1 expected=%2")
                     .arg(actual).arg(expected)));
        QCOMPARE(warnings, QStringList{});
    }

    // GitHub #38 for the thread panel: PgUp/PgDn and Ctrl+Up/Down from the
    // thread composer drive the reply list (a plain, non-rotated ListView:
    // contentY grows towards the newest reply), and the list takes the same
    // keys when it has focus.
    void threadComposerForwardsThePagingKeysToTheReplyList()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString rootId = firstThreadRootId(*mock, kGeneral);
        QVERIFY(!rootId.isEmpty());
        controller.thread()->openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        const QString timelineId =
            MatrixClient::threadTimelineId(kGeneral, rootId);
        QList<TimelineEvent> events;
        for (int i = 0; i < 40; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("reply %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(timelineId, events, /*paginationPages=*/0);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.thread()->model()->rowCount() >= 40, kSignalTimeoutMs);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("ThreadPanel"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(400, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        auto *replyList = root->findChild<QQuickItem *>(
            QStringLiteral("threadReplyList"));
        QVERIFY(replyList != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(replyList->property("count").toInt() >= 40,
                                 kSignalTimeoutMs);

        QVariant editorVar;
        QVERIFY(QMetaObject::invokeMethod(root, "activeThreadEditor",
                                          Q_RETURN_ARG(QVariant, editorVar)));
        auto *editor = qobject_cast<QQuickItem *>(editorVar.value<QObject *>());
        QVERIFY(editor != nullptr);
        editor->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasActiveFocus(), kSignalTimeoutMs);

        auto bounds = [&](const char *fn) {
            QVariant v;
            QMetaObject::invokeMethod(replyList, fn, Q_RETURN_ARG(QVariant, v));
            return v.toDouble();
        };

        // Ctrl+Up: the oldest loaded reply; Ctrl+Down: the newest.
        const double minBefore = bounds("wheelMinY");
        QTest::keyClick(&window, Qt::Key_Up, Qt::ControlModifier);
        QCoreApplication::processEvents();
        // Against the bound as it was at the keypress: reaching the top asks
        // for older replies, whose header then moves the origin.
        QVERIFY(replyList->property("contentY").toDouble() <= minBefore + 1.0);
        QCOMPARE(replyList->property("followLatest").toBool(), false);
        // PgDn from an empty draft pages down (animated).
        const double top = replyList->property("contentY").toDouble();
        QTest::keyClick(&window, Qt::Key_PageDown);
        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("contentY").toDouble() > top + 100,
            kSignalTimeoutMs);
        const double maxBefore = bounds("wheelMaxY");
        QTest::keyClick(&window, Qt::Key_Down, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QVERIFY(replyList->property("contentY").toDouble() >= maxBefore - 1.0);
        QCOMPARE(replyList->property("followLatest").toBool(), true);
        // PgUp pages back up.
        const double bottom = replyList->property("contentY").toDouble();
        QTest::keyClick(&window, Qt::Key_PageUp);
        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("contentY").toDouble() < bottom - 100,
            kSignalTimeoutMs);

        // A multi-line draft with the caret on its first line keeps PgDn (the
        // caret can still move down), so the list stays at the top; on the
        // last line the same key pages the list.
        QVERIFY(QMetaObject::invokeMethod(replyList, "runKeyAction",
                    Q_ARG(QVariant, QStringLiteral("timeline.jumpToStart"))));
        QVERIFY(editor->setProperty("text", QStringLiteral("a\nb\nc\nd")));
        QVERIFY(editor->setProperty("cursorPosition", 0));
        QCoreApplication::processEvents();
        const double atTop = replyList->property("contentY").toDouble();
        QTest::keyClick(&window, Qt::Key_PageDown);
        QTest::qWait(300);
        QCOMPARE(replyList->property("contentY").toDouble(), atTop);
        QVERIFY(editor->setProperty("cursorPosition",
                                    editor->property("length").toInt()));
        QCoreApplication::processEvents();
        QTest::keyClick(&window, Qt::Key_PageDown);
        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("contentY").toDouble() > atTop + 100,
            kSignalTimeoutMs);
        QCOMPARE(warnings, QStringList{});
    }

    // ── The thread root card never draws wider than its column ───────────
    //
    // The body Label (a fillWidth child of a ColumnLayout anchored to the
    // card) came out at the layout's implicit width, wrapped for that, and
    // the card's `clip: true` cut the overhang out of the middle of the text.
    // Scale-free rule: nothing in the root card may be wider than its column.
    //
    // This is an invariant guard, not a regression test: offscreen, with the
    // panel as the window's root, the body gets the right width with or
    // without the caps. The overflow reproduced only as a SplitView child.
    void theThreadRootCardNeverDrawsWiderThanItsColumn()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        const QString rootId = firstThreadRootId(*mock, kGeneral);
        QVERIFY(!rootId.isEmpty());
        controller.thread()->openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("ThreadPanel"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        // Narrow on purpose: the overflow only appears once the card is too
        // small for the header's natural width.
        window.resize(340, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, kSignalTimeoutMs));

        auto *theme = engine.singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY2(theme, "no AppTheme singleton — this case cannot reach the "
                        "interface size the defect appears at");
        const qreal originalScale = theme->property("textScale").toReal();
        const auto restore = qScopeGuard([&] {
            theme->setProperty("textScale", originalScale);
        });

        auto *column = root->findChild<QQuickItem *>(
            QStringLiteral("threadRootColumn"));
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("threadRootHeaderRow"));
        auto *body = root->findChild<QQuickItem *>(
            QStringLiteral("threadRootBody"));
        QVERIFY2(column && header && body,
                 "the thread root card's column, header or body is gone — "
                 "re-anchor this case");

        // 100 % first, then the size the defect was found at. Both, because
        // a fix that only holds at one of them is not a fix.
        for (const qreal scale : { 1.0, 1.4 }) {
            theme->setProperty("textScale", scale);
            QCoreApplication::processEvents();
            QTest::qWait(80);

            const QVariantMap info = controller.thread()->rootInfo();
            QVERIFY2(column->width() > 0,
                     "the root card's column has no width, so this case is "
                     "comparing nothing");
            QVERIFY2(body->width() <= column->width() + 0.5,
                     qPrintable(QStringLiteral(
                         "at %1%% the thread root's BODY is %2px inside a "
                         "%3px column, so it wraps for a width the card then "
                         "clips — the message loses characters out of its "
                         "middle with nothing on screen to say so")
                         .arg(scale * 100).arg(body->width())
                         .arg(column->width())));
            QVERIFY2(header->width() <= column->width() + 0.5,
                     qPrintable(QStringLiteral(
                         "at %1%% the thread root's HEADER is %2px inside a "
                         "%3px column, which is what pushes `Open in room` "
                         "off the card's edge")
                         .arg(scale * 100).arg(header->width())
                         .arg(column->width())));
        }
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

int main(int argc, char *argv[])
{
    // Real Qt Quick item creation (even offscreen) needs a QGuiApplication,
    // matching main.cpp's application class exactly.
    QGuiApplication app(argc, argv);
    ThreadPanelPaginationAnchorQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}
#include "ThreadPanelPaginationAnchorQmlTest.moc"
