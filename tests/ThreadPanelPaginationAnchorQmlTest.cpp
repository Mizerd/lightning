// Regression coverage for the ThreadPanel.qml pagination-anchor port: before
// this checkpoint, qml/ThreadPanel.qml had NO scroll-anchor capture/restore
// mechanism at all for its own backward-pagination path
// (app.thread.model.requestOlder(), a plain ListView prepend) — the exact
// class of bug TimelinePane.qml's captureAnchor()/restoreCapturedAnchor()
// exists to prevent ("a fixed contentY would make the whole conversation
// jump", see TimelinePane.qml's own comment), left unaddressed in the
// sibling thread view. This test drives the REAL ThreadPanel.qml through a
// genuine near-top thread-reply pagination and proves the ported mechanism
// holds, including 9a0e41a's fix semantics: the restore reads the LIVE
// contentY at restore time and applies a RELATIVE shift, so a reader still
// scrolling when the page lands is never snapped back to a stale position.
//
// Mirrors TimelinePaneQmlTest.cpp's
// paginationAnchorRestorePreservesConcurrentScroll methodology (direct
// contentY write standing in for a continuing touchpad gesture — see that
// test's own comment for why that is a faithful stand-in), adapted to
// ThreadPanel's replyList/app.thread.model instead of
// TimelinePane's timeline/app.pagination.
#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
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
        // Sandbox BEFORE the first AppController exists. Without this the
        // test opens the developer's real matrix-client.conf: logging in as
        // the mock user calls setActiveAccountUserId() for an id with no
        // saved record, which removes the real `activeAccount` key — so
        // running the suite would silently sign the maintainer out.
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
        // Replace the small built-in fixture with 30 synthetic replies —
        // comfortably exceeds the test viewport, same reasoning as
        // TimelinePaneQmlTest.cpp's own fixture sizing. resetTimelineForTest
        // is a generic (roomId-string, events) store; feeding it the
        // COMPOSITE thread timeline id (already the model's roomId(), since
        // the thread is Ready) makes TimelineModel::onTimelineReset reload
        // exactly like it would for a real room, without disturbing
        // ThreadController's own state (it only reacts to timelineReset
        // while Opening, not once Ready).
        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
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

        // Real near-top request — exactly what ThreadPanel.qml's
        // onMovementEnded/afterWheelSettled dispatch. The mock flips
        // paginating() synchronously inside loadOlderMessages() (same
        // mechanism the room timeline relies on), so the new Connections{
        // target: app.thread.model }.onPaginationChanged handler in
        // ThreadPanel.qml calls captureAnchor() before this returns.
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
