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

    // ── 2026-09-18: the thread root card lost characters out of the middle
    //    of the message ──────────────────────────────────────────────────
    //
    // Seen on the running client at 140 %: a root reading "discriminator ping
    // for the space-tree refresh test" rendered as "discriminator ping for
    // the space-t" / "refresh test". Not elided and not clipped-but-present —
    // the characters "ree" were not drawn anywhere, and nothing on screen
    // said so.
    //
    // Instrumented rather than guessed at. A probe on the live item reported
    // `body w=377  colW=292  cardW=316  contentW=372`: the body Label, a
    // `Layout.fillWidth` child of a ColumnLayout that is ANCHORED to a 316px
    // card, came out 377px wide — the layout's own IMPLICIT width, which the
    // header row's contents set. So the text wrapped for 377 and the card's
    // `clip: true` ate the overhang. The same probe after the fix reads
    // `body w=292  colW=292`.
    //
    // The rule asserted here is the one that was violated, and it is scale-
    // free: **nothing in the root card may be wider than the column that
    // holds it.** Pixel values are deliberately absent — they are a property
    // of the window size this test happens to use.
    //
    // AND THIS IS AN INVARIANT GUARD, NOT A REGRESSION TEST. Removing either
    // cap leaves it PASSING: offscreen, with the panel as the window's root
    // item, the layout gives the body 292 whether or not it is capped. The
    // live panel is a SplitView child, and only there did the body come out
    // at its own content width. Both mutations were run (each with the QML
    // module rebuilt, since a load test reads the COMPILED module) and both
    // passed, so this case must not be quoted as coverage for the defect —
    // it holds the invariant, and the numbers above are the evidence.
    // What it CAN catch is the same shape reappearing under conditions the
    // harness does reach, and the day it starts failing it means something.
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
        // NARROW ON PURPOSE. The panel is 340px on the maintainer's desktop
        // at 140 %, and the overflow is only visible once the card is too
        // small for the header's natural width — at 700px everything fits
        // and a broken build passes.
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
