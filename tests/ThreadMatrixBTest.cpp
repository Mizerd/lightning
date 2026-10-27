// T38: an input method owns keys while a thread editor has uncommitted text.
// This source contract checks both QML editors because the regression was a
// missing guard in their separate Keys.onPressed handlers. Removing either
// guard makes this test fail against the pre-fix ThreadPanel.qml.

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QtTest/QtTest>
#include "app/RoomHistory.h"

class ThreadMatrixBTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void backForwardVisitsRoomThreadsAndSearchWithoutLoops()
    {
        using History = lightning::RoomHistory;
        const QString room = QStringLiteral("!room:example.org");
        const QString a = QStringLiteral("$rootA:example.org");
        const QString b = QStringLiteral("$rootB:example.org");
        const QString hit = QStringLiteral("$result:example.org");
        const History::Openable open = [](const QString &id) {
            return id == QStringLiteral("!room:example.org");
        };
        History history;
        history.visit(room);
        history.visitThread(room, a);
        history.visitThread(room, b);
        history.visitSearchResult(room, hit);
        QCOMPARE(history.places().size(), 4);
        for (int pass = 0; pass < 2; ++pass) {
            auto place = history.backPlace(open);
            QCOMPARE(place.kind, History::Place::Thread);
            QCOMPARE(place.rootEventId, b);
            place = history.backPlace(open);
            QCOMPARE(place.rootEventId, a);
            place = history.backPlace(open);
            QCOMPARE(place.kind, History::Place::Room);
            QCOMPARE(place.roomId, room);
            QVERIFY(history.backPlace(open).roomId.isEmpty());
            QCOMPARE(history.forwardPlace(open).rootEventId, a);
            QCOMPARE(history.forwardPlace(open).rootEventId, b);
            place = history.forwardPlace(open);
            QCOMPARE(place.kind, History::Place::SearchResult);
            QCOMPARE(place.eventId, hit);
            QVERIFY(history.forwardPlace(open).roomId.isEmpty());
            QCOMPARE(history.places().size(), 4);
        }
        for (const auto &place : history.places()) {
            QCOMPARE(place.roomId, room);
            QVERIFY(!place.roomId.contains(QChar(0x1f)));
        }
        QFile controller(QStringLiteral(SOURCE_DIR "/app/AppController.cpp"));
        QFile timeline(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY(controller.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(timeline.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString c = QString::fromUtf8(controller.readAll());
        const QString qml = QString::fromUtf8(timeline.readAll());
        QVERIFY(c.contains(QStringLiteral("m_roomHistory.visitThread(m_currentRoomId,")));
        QVERIFY(c.contains(QStringLiteral("m_roomHistory.backPlace(openable)")));
        QVERIFY(c.contains(QStringLiteral("m_pagination->jumpToEvent(place.eventId)")));
        QVERIFY(qml.contains(QStringLiteral("app.recordSearchResult(app.currentRoomId,")));
    }

    void roomFilterSurvivesOpeningAnIndependentThread()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/app/AppController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString source = QString::fromUtf8(file.readAll());
        const int start = source.indexOf(QStringLiteral("void AppController::showThread("));
        const int end = source.indexOf(QStringLiteral("void AppController::applyReceiptThreading("), start);
        QVERIFY(start >= 0 && end > start);
        const QString body = source.mid(start, end - start);
        QVERIFY(body.contains(QStringLiteral("m_thread->openThread(roomId, rootEventId)")));
        QVERIFY(!body.contains(QStringLiteral("setThreadFilterRootId")));
    }

    void threadSearchKeepsRootAndFiltersOnlyReplyRows()
    {
        QFile panel(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QFile delegate(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(panel.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(delegate.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString p = QString::fromUtf8(panel.readAll());
        const QString d = QString::fromUtf8(delegate.readAll());
        QVERIFY(p.contains(QStringLiteral("objectName: \"threadReplySearchField\"")));
        QVERIFY(p.contains(QStringLiteral("property string threadSearchQuery: panel.threadSearchQuery")));
        QVERIFY(p.contains(QStringLiteral("objectName: \"threadRootHeader\"")));
        QVERIFY(d.contains(QStringLiteral("id !== root.timelineView.suppressRootEventId")));
        QVERIFY(d.contains(QStringLiteral("root.timelineModel.visibleTextForEvent(id)")));
        QVERIFY(d.contains(QStringLiteral("!threadSearchVisible) ? 0")));
    }

    void unknownTotalNeverBecomesAnExactLabel()
    {
        QFile controller(QStringLiteral(SOURCE_DIR "/threads/ThreadController.cpp"));
        QFile panel(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(controller.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(panel.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString c = QString::fromUtf8(controller.readAll());
        const QString p = QString::fromUtf8(panel.readAll());
        const int start = c.indexOf(QStringLiteral("int ThreadController::exactReplyCount() const"));
        const int end = c.indexOf(QStringLiteral("void ThreadController::notifyReplyCountIfChanged()"), start);
        QVERIFY(start >= 0 && end > start);
        const QString exact = c.mid(start, end - start);
        QVERIFY(exact.contains(QStringLiteral("return known;")));
        QVERIFY(!exact.contains(QStringLiteral("realEventCount")));
        QVERIFY(p.contains(QStringLiteral("app.thread.exactReplyCount")));
        QVERIFY(p.contains(QStringLiteral(": qsTr(\"Replies\")")));
    }

    void changedRoomPermissionBlocksThreadPostingButKeepsHistory()
    {
        QFile file(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(source.contains(QStringLiteral("app.openRoomPermissions.sendKnown === true")));
        QVERIFY(source.contains(QStringLiteral("app.openRoomPermissions.roomId === panel.panelRoomId")));
        QVERIFY(source.contains(QStringLiteral("threadPostFacts.canSendMessages === false")));
        QVERIFY(source.contains(QStringLiteral("objectName: \"threadPostingBlockedReason\"")));
        QVERIFY(source.contains(QStringLiteral("visible: app.thread.active && !panel.threadPostBlocked")));
        QVERIFY(source.contains(QStringLiteral("|| panel.threadPostBlocked")));
        QVERIFY(source.contains(QStringLiteral("objectName: \"threadReplyList\"")));
    }

    void preeditHasPriorityInBothThreadEditors()
    {
        QFile file(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "ThreadPanel.qml must be readable");
        const QString source = QString::fromUtf8(file.readAll());

        const auto verifyEditor = [&](const QString &editorId,
                                      const QString &guard,
                                      const QString &firstAction) {
            const int editor = source.indexOf(editorId);
            QVERIFY2(editor >= 0, qPrintable(editorId));
            const int handler = source.indexOf(QStringLiteral("Keys.onPressed: (event) => {"),
                                               editor);
            QVERIFY2(handler >= 0, qPrintable(editorId));
            const int guardAt = source.indexOf(guard, handler);
            const int actionAt = source.indexOf(firstAction, handler);
            QVERIFY2(guardAt > handler && actionAt > guardAt,
                     qPrintable(editorId + QStringLiteral(": preedit must precede key actions")));
            const QString prefix = source.mid(guardAt, actionAt - guardAt);
            QVERIFY2(QRegularExpression(
                         QRegularExpression::escape(guard)
                         + QStringLiteral("\\s+return\\b"))
                         .match(prefix).hasMatch(),
                     qPrintable(editorId + QStringLiteral(": preedit must return without accepting the key")));
        };

        verifyEditor(QStringLiteral("id: threadRichInput"),
                     QStringLiteral("if (threadRichInput.preeditText !== \"\")"),
                     QStringLiteral("threadRichAutocorrect.handleKey(event)"));
        verifyEditor(QStringLiteral("id: threadComposerInput"),
                     QStringLiteral("if (threadComposerInput.preeditText !== \"\")"),
                     QStringLiteral("threadPlainAutocorrect.handleKey(event)"));
    }
};

QTEST_GUILESS_MAIN(ThreadMatrixBTest)
#include "ThreadMatrixBTest.moc"
