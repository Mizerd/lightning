#include <functional>
#include <QMetaProperty>
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
#include "app/SettingsManager.h"
#include "models/TimelineScrollController.h"
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

public slots:
    void noteAnchorChanged() { if (m_onAnchorChanged) m_onAnchorChanged(); }

private:
    std::function<void()> m_onAnchorChanged;

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
        mock->resetTimelineForTest(timelineId, events,1);
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
                                          Q_ARG(int, 0)));
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

        constexpr double simulatedScrollDelta = 40.0;
        const double scrolledContentY = capturedContentY - simulatedScrollDelta;
        QVERIFY(replyList->setProperty("contentY", scrolledContentY));

        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("count").toInt() > countBefore,
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("anchorStableId").toString().isEmpty(),
            kSignalTimeoutMs);

        const int newRow =
            controller.thread()->model()->rowForStableId(capturedAnchorId);
        QVERIFY(newRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(QMetaObject::invokeMethod(replyList, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, anchorItem),
                                          Q_ARG(int, newRow)));
        QVERIFY(anchorItem != nullptr);

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

    void anOlderPageLandingMidGlideKeepsTheGlideGoing()
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

        mock->setPaginationDelayForTest(40);
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
        mock->resetTimelineForTest(timelineId, events,1);
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
                                          Q_ARG(int, 0)));
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

        TimelineScrollController *engineCtl = controller.threadScroll();
        bool sawRestore = false;
        bool glidingAtRestore = false;
        const QMetaProperty anchorProp = replyList->metaObject()->property(
            replyList->metaObject()->indexOfProperty("anchorStableId"));
        QVERIFY(anchorProp.hasNotifySignal());
        connect(replyList, anchorProp.notifySignal(), this,
                metaObject()->method(metaObject()->indexOfSlot("noteAnchorChanged()")));
        m_onAnchorChanged = [&] {
            if (replyList->property("anchorStableId").toString().isEmpty()
                    && !sawRestore) {
                sawRestore = true;
                glidingAtRestore = engineCtl->motionActive();
            }
        };
        QVariant lo, hi;
        QVERIFY(QMetaObject::invokeMethod(replyList, "wheelMinY",
                                          Q_RETURN_ARG(QVariant, lo)));
        QVERIFY(QMetaObject::invokeMethod(replyList, "wheelMaxY",
                                          Q_RETURN_ARG(QVariant, hi)));
        engineCtl->wheelNotch(360, replyList->property("contentY").toDouble(),
                              lo.toDouble(), hi.toDouble(),
                              replyList->property("height").toDouble());
        QVERIFY(engineCtl->motionActive());

        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("count").toInt() > countBefore,
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(sawRestore, kSignalTimeoutMs);
        m_onAnchorChanged = nullptr;
        QVERIFY2(glidingAtRestore,
                 "the anchor restore cancelled the reader's wheel glide");
        QCOMPARE(capturedAnchorId.isEmpty(), false);
        Q_UNUSED(anchorItemY);
        Q_UNUSED(capturedContentY);
        QTRY_VERIFY_WITH_TIMEOUT(!engineCtl->motionActive(), 5000);
        QCOMPARE(warnings, QStringList{});
    }

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
        mock->resetTimelineForTest(timelineId, events,0);
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

        const double minBefore = bounds("wheelMinY");
        QTest::keyClick(&window, Qt::Key_Up, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QVERIFY(replyList->property("contentY").toDouble() <= minBefore + 1.0);
        QCOMPARE(replyList->property("followLatest").toBool(), false);
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
        const double bottom = replyList->property("contentY").toDouble();
        QTest::keyClick(&window, Qt::Key_PageUp);
        QTRY_VERIFY_WITH_TIMEOUT(
            replyList->property("contentY").toDouble() < bottom - 100,
            kSignalTimeoutMs);

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

    void theThreadRootCollapsesToOneLineAndRemembersIt()
    {
        QSettings().remove(QStringLiteral("timeline/threadRootCollapsed"));
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const auto restore = qScopeGuard([&] {
            controller.settings()->setProperty("threadRootCollapsed", false);
        });

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
        window.resize(340, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, kSignalTimeoutMs));

        auto *card = root->findChild<QQuickItem *>(QStringLiteral("threadRootHeader"));
        auto *body = root->findChild<QQuickItem *>(QStringLiteral("threadRootBody"));
        auto *collapse = root->findChild<QQuickItem *>(
            QStringLiteral("threadRootCollapseButton"));
        auto *expand = root->findChild<QQuickItem *>(
            QStringLiteral("threadRootExpandButton"));
        auto *line = root->findChild<QQuickItem *>(
            QStringLiteral("threadRootCollapsedLine"));
        QVERIFY(card && body);
        QVERIFY2(collapse && expand && line, "the root card has no collapse toggle");

        QTRY_VERIFY(body->isVisible());
        QVERIFY(collapse->isVisible());
        QVERIFY(collapse->property("focusPolicy").toInt() & Qt::TabFocus);
        const qreal expandedHeight = card->height();

        collapse->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY(controller.settings()->property("threadRootCollapsed").toBool());
        QTRY_VERIFY(!body->isVisible());
        QVERIFY(line->isVisible());
        QVERIFY(expand->isVisible());
        QTRY_VERIFY2(card->height() < expandedHeight,
                     qPrintable(QStringLiteral("collapsed %1 px, expanded %2 px")
                                    .arg(card->height()).arg(expandedHeight)));
        {
            SettingsManager fresh;
            QVERIFY(fresh.property("threadRootCollapsed").toBool());
        }

        expand->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&window, Qt::Key_Return);
        QTRY_VERIFY(!controller.settings()->property("threadRootCollapsed").toBool());
        QTRY_VERIFY(body->isVisible());

        controller.settings()->setProperty("threadRootCollapsed", true);
        QTRY_VERIFY(line->isVisible());
        const QPointF centre = line->mapToScene(
            QPointF(line->width() / 4, line->height() / 2));
        QTest::mouseClick(&window, Qt::LeftButton, {}, centre.toPoint());
        QTRY_VERIFY(!controller.settings()->property("threadRootCollapsed").toBool());
        QTRY_VERIFY(body->isVisible());
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ThreadPanelPaginationAnchorQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}
#include "ThreadPanelPaginationAnchorQmlTest.moc"
