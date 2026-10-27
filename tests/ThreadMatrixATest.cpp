#include "matrix/MockMatrixClient.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
const QString roomId = QStringLiteral("!general:mock.local");

bool login(MockMatrixClient &client)
{
    QSignalSpy ready(&client, &MatrixClient::loginSucceeded);
    client.login(QStringLiteral("https://mock.local"),
                 QStringLiteral("alice"), QStringLiteral("unused"));
    if (!ready.wait(2000))
        return false;
    client.startSync();
    return true;
}

QString fixtureRoot(const MockMatrixClient &client)
{
    for (const auto &event : client.timeline(roomId))
        if (!event.threadRootId.isEmpty())
            return event.threadRootId;
    return {};
}

QString firstReply(const TimelineModel &model, const QString &root)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex index = model.index(row, 0);
        const QString id = model.data(index, TimelineModel::EventIdRole).toString();
        if (id != root && model.data(index, TimelineModel::ThreadRootIdRole).toString() == root)
            return id;
    }
    return {};
}

int rowOf(const TimelineModel &model, const QString &eventId)
{
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row, 0), TimelineModel::EventIdRole).toString() == eventId)
            return row;
    return -1;
}
}

class ThreadMatrixATest : public QObject
{
    Q_OBJECT
private slots:
    void rapidOlderRequestsProduceOnePage()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController thread;
        thread.setClient(&client);
        const QString root = fixtureRoot(client);
        QVERIFY(!root.isEmpty());
        thread.openThread(roomId, root);
        QTRY_COMPARE(thread.state(), ThreadController::Ready);
        const QString id = MatrixClient::threadTimelineId(roomId, root);
        client.setPaginationDelayForTest(50);
        client.resetTimelineForTest(id, client.timeline(id), 2);
        const int before = thread.model()->rowCount();
        thread.model()->requestOlder();
        thread.model()->requestOlder();
        QTRY_VERIFY(!client.paginating(id));
        QCOMPARE(thread.model()->rowCount(), before + 3);
        QVERIFY(client.canPaginate(id));
    }

    void redactingRootRetainsReplies()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController thread;
        thread.setClient(&client);
        const QString root = fixtureRoot(client);
        QVERIFY(!root.isEmpty());
        thread.openThread(roomId, root);
        QTRY_COMPARE(thread.state(), ThreadController::Ready);
        auto *model = thread.model();
        const QString reply = firstReply(*model, root);
        QVERIFY(!reply.isEmpty());
        const int before = model->rowCount();
        model->redactEvent(root, QStringLiteral("removed"));
        QCOMPARE(thread.state(), ThreadController::Ready);
        QCOMPARE(model->rowCount(), before);
        const int rootRow = rowOf(*model, root);
        const int replyRow = rowOf(*model, reply);
        QVERIFY(rootRow >= 0);
        QVERIFY(replyRow >= 0);
        QVERIFY(model->data(model->index(rootRow), TimelineModel::RedactedRole).toBool());
        QCOMPARE(model->data(model->index(replyRow), TimelineModel::EventIdRole).toString(), reply);
    }

    void reactingToReplyPreservesUnrelatedComposerDraft()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController thread;
        thread.setClient(&client);
        const QString root = fixtureRoot(client);
        QVERIFY(!root.isEmpty());
        thread.openThread(roomId, root);
        QTRY_COMPARE(thread.state(), ThreadController::Ready);
        const QString reply = firstReply(*thread.model(), root);
        QVERIFY(!reply.isEmpty());
        thread.setText(QStringLiteral("new reply still being composed"));
        thread.model()->toggleReaction(reply, QStringLiteral("👍"));
        QCOMPARE(thread.text(), QStringLiteral("new reply still being composed"));
        const auto events = client.timeline(MatrixClient::threadTimelineId(roomId, root));
        bool reacted = false;
        for (const auto &event : events)
            if (event.eventId == reply)
                for (const auto &reaction : event.reactions)
                    reacted |= reaction.key == QStringLiteral("👍") && reaction.byMe;
        QVERIFY(reacted);
    }
};

QTEST_MAIN(ThreadMatrixATest)
#include "ThreadMatrixATest.moc"
