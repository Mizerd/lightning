// Space moderation through SpaceModerationController against a fake client:
// the plan request covers the Space first, only rooms the plan offers are
// ever dispatched, steps run one at a time through the ordinary room
// moderation calls, every step's outcome is reported, and the confirmation
// text names what happens to the Space and to its rooms.

#include "spaces/SpaceModerationController.h"
#include "matrix/MatrixClient.h"

#include <QSet>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    // Rooms whose moderation call is refused synchronously (returns 0).
    QStringList refuseSend;

    int planCalls = 0;
    QStringList lastPlanRooms;
    QString lastPlanUser;
    QString lastPlanOp;
    quint64 lastPlanOpId = 0;

    struct Sent {
        quint64 opId;
        QString op;
        QString roomId;
        QString userId;
        QString reason;
    };
    QList<Sent> sent;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    // Changed by a test to stand for another account becoming active.
    QString userId = QStringLiteral("@me:example.org");
    QString currentUserId() const override { return userId; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    quint64 requestModerationPlan(const QStringList &roomIds,
                                  const QString &userId,
                                  const QString &op) override
    {
        ++planCalls;
        lastPlanRooms = roomIds;
        lastPlanUser = userId;
        lastPlanOp = op;
        lastPlanOpId = nextOp++;
        return lastPlanOpId;
    }
    quint64 record(const QString &op, const QString &roomId,
                   const QString &userId, const QString &reason)
    {
        if (refuseSend.contains(roomId))
            return 0;
        const quint64 id = nextOp++;
        sent.append({ id, op, roomId, userId, reason });
        return id;
    }
    quint64 kickUser(const QString &roomId, const QString &userId,
                     const QString &reason) override
    { return record(QStringLiteral("kick"), roomId, userId, reason); }
    quint64 banUser(const QString &roomId, const QString &userId,
                    const QString &reason) override
    { return record(QStringLiteral("ban"), roomId, userId, reason); }
    quint64 unbanUser(const QString &roomId, const QString &userId,
                      const QString &reason) override
    { return record(QStringLiteral("unban"), roomId, userId, reason); }

    // Answers the step in flight, as the backend would.
    void answerLast(bool ok, const QString &category = QString())
    {
        const Sent &s = sent.constLast();
        Q_EMIT moderationFinished(s.opId, s.roomId, s.userId, s.op, ok,
                                  ok ? QString() : category);
    }
};

const QString kSpace = QStringLiteral("!space:example.org");
const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");
const QString kRoomC = QStringLiteral("!c:example.org");
const QString kRoomD = QStringLiteral("!d:example.org");
const QString kTarget = QStringLiteral("@spammer:example.org");

QVariantMap planRow(const QString &roomId, const QString &reason,
                    const QString &name = QString())
{
    return QVariantMap{
        { QStringLiteral("roomId"), roomId },
        { QStringLiteral("name"), name.isEmpty() ? roomId : name },
        { QStringLiteral("isSpace"), roomId == kSpace },
        { QStringLiteral("membership"), QStringLiteral("joined") },
        { QStringLiteral("reason"), reason },
    };
}

QVariantMap roomRow(const SpaceModerationController &c, const QString &roomId)
{
    for (const QVariant &v : c.rooms()) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("roomId")).toString() == roomId)
            return row;
    }
    return {};
}

} // namespace

class SpaceModerationControllerTest : public QObject
{
    Q_OBJECT

private:
    FakeClient *m_client = nullptr;
    SpaceModerationController *m_ctl = nullptr;

    // Begins a flow and answers its plan: the Space and A offered, B not a
    // member, C outranks the viewer, D offered.
    void beginWithPlan(const QString &op, const QString &spaceReason = {})
    {
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), kTarget,
                     QStringLiteral("Spammer"), op);
        QCOMPARE(m_ctl->phase(), QStringLiteral("planning"));
        Q_EMIT m_client->moderationPlanReceived(
            m_client->lastPlanOpId, kTarget, op, false,
            QVariantList{
                planRow(kSpace, spaceReason, QStringLiteral("Lounge")),
                planRow(kRoomA, QString()),
                planRow(kRoomB, QStringLiteral("not_a_member")),
                planRow(kRoomC, QStringLiteral("outranked")),
                planRow(kRoomD, QString()),
            });
        QCOMPARE(m_ctl->phase(), QStringLiteral("ready"));
    }

private slots:
    void init()
    {
        m_client = new FakeClient;
        m_ctl = new SpaceModerationController;
        m_ctl->setClient(m_client);
        m_ctl->setScopeResolver([](const QString &spaceId) {
            // Deliberately without the Space: the controller puts it first.
            return spaceId == kSpace
                ? QStringList{ kRoomA, kRoomB, kRoomC, kRoomD }
                : QStringList{};
        });
    }

    void cleanup()
    {
        delete m_ctl;
        delete m_client;
        m_ctl = nullptr;
        m_client = nullptr;
    }

    void thePlanCoversTheSpaceFirstAndSendsNothing()
    {
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), kTarget,
                     QStringLiteral("Spammer"), QStringLiteral("kick"));
        QCOMPARE(m_client->planCalls, 1);
        QCOMPARE(m_client->lastPlanRooms,
                 (QStringList{ kSpace, kRoomA, kRoomB, kRoomC, kRoomD }));
        QCOMPARE(m_client->lastPlanUser, kTarget);
        QCOMPARE(m_client->lastPlanOp, QStringLiteral("kick"));
        QVERIFY(m_client->sent.isEmpty());
        QVERIFY(!m_ctl->canConfirm());
    }

    void anUnknownOpIsRefusedBeforeAskingAnything()
    {
        m_ctl->begin(kSpace, QString(), kTarget, QString(),
                     QStringLiteral("redact"));
        QCOMPARE(m_client->planCalls, 0);
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        QVERIFY(!m_ctl->canConfirm());
    }

    void aStaleOrForeignPlanIsIgnored()
    {
        m_ctl->begin(kSpace, QString(), kTarget, QString(),
                     QStringLiteral("ban"));
        const quint64 op = m_client->lastPlanOpId;
        // Another user, then another op id: neither may fill this flow.
        Q_EMIT m_client->moderationPlanReceived(
            op, QStringLiteral("@other:example.org"), QStringLiteral("ban"),
            false, QVariantList{ planRow(kSpace, QString()) });
        Q_EMIT m_client->moderationPlanReceived(
            op + 100, kTarget, QStringLiteral("ban"), false,
            QVariantList{ planRow(kSpace, QString()) });
        QCOMPARE(m_ctl->phase(), QStringLiteral("planning"));
        QVERIFY(m_ctl->spaceRow().isEmpty());
    }

    // Power-level gating: the backend's plan is the gate, and the view cannot
    // talk the controller into a room the plan did not offer.
    void onlyOfferedRoomsAreEverDispatched()
    {
        beginWithPlan(QStringLiteral("ban"));
        QVERIFY(m_ctl->spaceEligible());
        QCOMPARE(m_ctl->eligibleRoomCount(), 2);
        QCOMPARE(m_ctl->skippedRoomCount(), 2);
        QCOMPARE(m_ctl->selectedRoomCount(), 2);

        // Selecting a room that is not offered changes nothing.
        m_ctl->setRoomSelected(kRoomC, true);
        m_ctl->setRoomSelected(kRoomB, true);
        QCOMPARE(roomRow(*m_ctl, kRoomC).value(QStringLiteral("selected")).toBool(),
                 false);
        QCOMPARE(m_ctl->selectedRoomCount(), 2);
        QCOMPARE(roomRow(*m_ctl, kRoomC).value(QStringLiteral("reasonText")).toString(),
                 SpaceModerationController::reasonText(QStringLiteral("outranked")));

        m_ctl->confirm(QStringLiteral("  spam  "));
        QCOMPARE(m_ctl->phase(), QStringLiteral("running"));
        // One step at a time.
        QCOMPARE(m_client->sent.size(), 1);
        m_client->answerLast(true);
        QCOMPARE(m_client->sent.size(), 2);
        m_client->answerLast(true);
        QCOMPARE(m_client->sent.size(), 3);
        m_client->answerLast(true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));

        QStringList rooms;
        for (const auto &s : m_client->sent) {
            rooms.append(s.roomId);
            QCOMPARE(s.op, QStringLiteral("ban"));
            QCOMPARE(s.userId, kTarget);
            QCOMPARE(s.reason, QStringLiteral("spam"));
        }
        QCOMPARE(rooms, (QStringList{ kSpace, kRoomA, kRoomD }));
        QCOMPARE(roomRow(*m_ctl, kRoomB).value(QStringLiteral("status")).toString(),
                 QStringLiteral("skipped"));
        QCOMPARE(roomRow(*m_ctl, kRoomC).value(QStringLiteral("status")).toString(),
                 QStringLiteral("skipped"));
    }

    void aSpaceTheViewerCannotModerateIsSkippedNotSent()
    {
        beginWithPlan(QStringLiteral("kick"), QStringLiteral("no_permission"));
        QVERIFY(!m_ctl->spaceEligible());
        // The confirmation must not promise a Space removal that will not
        // happen.
        QVERIFY(m_ctl->title().contains(QStringLiteral("from rooms in")));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("space itself is unchanged")));
        QVERIFY(!m_ctl->consequenceText().contains(
            QStringLiteral("removed from the space")));
        // Still confirmable for the rooms the viewer can act in.
        QVERIFY(m_ctl->canConfirm());
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.constFirst().roomId, kRoomA);
        QCOMPARE(m_ctl->spaceRow().value(QStringLiteral("status")).toString(),
                 QStringLiteral("skipped"));

        // With the cascade off there is nothing left to do.
        cleanup();
        init();
        beginWithPlan(QStringLiteral("kick"), QStringLiteral("no_permission"));
        m_ctl->setCascade(false);
        QVERIFY(!m_ctl->canConfirm());
        m_ctl->confirm(QString());
        QVERIFY(m_client->sent.isEmpty());
        QCOMPARE(m_ctl->phase(), QStringLiteral("ready"));
    }

    void withoutTheCascadeOnlyTheSpaceIsTouched()
    {
        beginWithPlan(QStringLiteral("kick"));
        m_ctl->setCascade(false);
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.size(), 1);
        QCOMPARE(m_client->sent.constFirst().roomId, kSpace);
        m_client->answerLast(true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QCOMPARE(m_ctl->succeededCount(), 1);
        QCOMPARE(roomRow(*m_ctl, kRoomA).value(QStringLiteral("status")).toString(),
                 QStringLiteral("skipped"));
    }

    void anUnselectedRoomIsLeftAlone()
    {
        beginWithPlan(QStringLiteral("kick"));
        m_ctl->setRoomSelected(kRoomD, false);
        QCOMPARE(m_ctl->selectedRoomCount(), 1);
        m_ctl->confirm(QString());
        m_client->answerLast(true);
        m_client->answerLast(true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QCOMPARE(m_client->sent.size(), 2);
        QCOMPARE(m_client->sent.at(1).roomId, kRoomA);
    }

    // Every step reports its own outcome: a refusal in one room neither stops
    // the others nor hides behind an overall success.
    void eachRoomReportsItsOwnOutcome()
    {
        m_client->refuseSend = { kRoomD };
        beginWithPlan(QStringLiteral("ban"));
        QSignalSpy finished(m_ctl, &SpaceModerationController::finished);
        m_ctl->confirm(QString());
        m_client->answerLast(true);                          // Space
        m_client->answerLast(false, QStringLiteral("forbidden")); // A
        // D could not be sent at all; the flow still ends.
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QCOMPARE(m_ctl->succeededCount(), 1);
        QCOMPARE(m_ctl->failedCount(), 2);

        QCOMPARE(m_ctl->spaceRow().value(QStringLiteral("status")).toString(),
                 QStringLiteral("ok"));
        const QVariantMap a = roomRow(*m_ctl, kRoomA);
        QCOMPARE(a.value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QVERIFY(a.value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("permission")));
        const QVariantMap d = roomRow(*m_ctl, kRoomD);
        QCOMPARE(d.value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QVERIFY(!d.value(QStringLiteral("message")).toString().isEmpty());

        QCOMPARE(m_ctl->statusText(),
                 QStringLiteral("Done with errors: 1 succeeded, 2 failed. The "
                                "failed rooms are marked below."));
        QCOMPARE(finished.count(), 1);
        const QList<QVariant> args = finished.constFirst();
        QCOMPARE(args.at(0).toString(), kSpace);
        QCOMPARE(args.at(3).toInt(), 1);
        QCOMPARE(args.at(4).toInt(), 2);
    }

    void anAnswerForAnotherStepOrUserIsIgnored()
    {
        beginWithPlan(QStringLiteral("kick"));
        m_ctl->confirm(QString());
        const auto &step = m_client->sent.constLast();
        Q_EMIT m_client->moderationFinished(step.opId + 50, step.roomId,
                                            step.userId, step.op, true, {});
        Q_EMIT m_client->moderationFinished(step.opId, step.roomId,
                                            QStringLiteral("@x:example.org"),
                                            step.op, true, {});
        QCOMPARE(m_client->sent.size(), 1);
        QCOMPARE(m_ctl->succeededCount(), 0);
        QCOMPARE(m_ctl->phase(), QStringLiteral("running"));
    }

    void signingOutDropsTheFlow()
    {
        beginWithPlan(QStringLiteral("ban"));
        m_ctl->confirm(QString());
        const auto step = m_client->sent.constLast();
        m_client->logout();
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        // The answer to the step already sent is not counted afterwards.
        Q_EMIT m_client->moderationFinished(step.opId, step.roomId,
                                            step.userId, step.op, true, {});
        QCOMPARE(m_ctl->succeededCount(), 0);
        QCOMPARE(m_client->sent.size(), 1);
    }

    void aNewFlowCannotStartWhileOneIsRunning()
    {
        beginWithPlan(QStringLiteral("ban"));
        m_ctl->confirm(QString());
        QVERIFY(m_ctl->notice().isEmpty());
        const bool started = m_ctl->begin(
            kSpace, QString(), QStringLiteral("@other:example.org"), QString(),
            QStringLiteral("kick"));
        QVERIFY(!started);
        QCOMPARE(m_ctl->userId(), kTarget);
        QCOMPARE(m_ctl->phase(), QStringLiteral("running"));
        QCOMPARE(m_client->planCalls, 1);
        // The refusal is visible, not silent.
        QVERIFY(m_ctl->notice().contains(QStringLiteral("still running")));
        // And it goes once the running flow has finished.
        m_client->answerLast(true);
        m_client->answerLast(true);
        m_client->answerLast(true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QVERIFY(m_ctl->notice().isEmpty());
        QVERIFY(m_ctl->begin(kSpace, QString(), kTarget, QString(),
                             QStringLiteral("kick")));
    }

    // Add-account never signs the previous account out: nothing more goes
    // out through the client once it speaks for another account.
    void aFlowNeverContinuesUnderAnotherAccount()
    {
        beginWithPlan(QStringLiteral("ban"));
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.size(), 1);
        m_client->userId = QStringLiteral("@other:example.org");
        m_client->answerLast(true);
        QCOMPARE(m_client->sent.size(), 1);
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));

        cleanup();
        init();
        beginWithPlan(QStringLiteral("kick"));
        m_ctl->confirm(QString());
        m_ctl->resetForAccountChange();
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        m_client->answerLast(true);
        QCOMPARE(m_client->sent.size(), 1);
    }

    // A step that never answers does not hold the (modal) dialog for the
    // session: the watchdog marks it not known and the rest go on. No wall
    // clock: the hook expires it.
    void aStepThatNeverAnswersDoesNotHoldTheDialog()
    {
        beginWithPlan(QStringLiteral("ban"));
        m_ctl->confirm(QString());
        QVERIFY(m_ctl->stepWatchdogArmedForTest());
        QCOMPARE(m_ctl->stepWatchdogIntervalForTest(),
                 SpaceModerationController::kStepTimeoutMs);
        const auto first = m_client->sent.constFirst();
        m_ctl->expireStepForTest();
        QCOMPARE(m_client->sent.size(), 2);
        QCOMPARE(m_ctl->spaceRow().value(QStringLiteral("status")).toString(),
                 QStringLiteral("unknown"));
        QCOMPARE(m_ctl->unknownCount(), 1);
        // Its late answer is not counted.
        Q_EMIT m_client->moderationFinished(first.opId, first.roomId,
                                            first.userId, first.op, true, {});
        QCOMPARE(m_ctl->succeededCount(), 0);
        m_client->answerLast(true);
        m_client->answerLast(true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QVERIFY(!m_ctl->stepWatchdogArmedForTest());
        QVERIFY(m_ctl->statusText().contains(QStringLiteral("1 not known")));
    }

    // Rooms the backend ran out of time for are listed, never offered, and
    // the status line says how many.
    void roomsNotCheckedInTimeAreNeverOffered()
    {
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), kTarget,
                     QStringLiteral("Spammer"), QStringLiteral("kick"));
        Q_EMIT m_client->moderationPlanReceived(
            m_client->lastPlanOpId, kTarget, QStringLiteral("kick"), false,
            QVariantList{
                planRow(kSpace, QString(), QStringLiteral("Lounge")),
                planRow(kRoomA, QString()),
                planRow(kRoomB, QStringLiteral("not_checked")),
                planRow(kRoomC, QStringLiteral("not_checked")),
            });
        QCOMPARE(m_ctl->phase(), QStringLiteral("ready"));
        QCOMPARE(m_ctl->uncheckedRoomCount(), 2);
        QCOMPARE(m_ctl->eligibleRoomCount(), 1);
        const QVariantMap b = roomRow(*m_ctl, kRoomB);
        QCOMPARE(b.value(QStringLiteral("eligible")).toBool(), false);
        QCOMPARE(b.value(QStringLiteral("reasonText")).toString(),
                 SpaceModerationController::reasonText(
                     QStringLiteral("not_checked")));
        QVERIFY(m_ctl->statusText().contains(QStringLiteral("2 of the rooms")));

        m_ctl->setRoomSelected(kRoomB, true);
        m_ctl->confirm(QString());
        m_client->answerLast(true);
        m_client->answerLast(true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QStringList rooms;
        for (const auto &s : m_client->sent)
            rooms.append(s.roomId);
        QCOMPARE(rooms, (QStringList{ kSpace, kRoomA }));
    }

    // A plan that never answers ends the wait instead of spinning for ever.
    void aPlanThatNeverAnswersFails()
    {
        m_ctl->setPlanTimeoutForTest(30);
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), kTarget,
                     QStringLiteral("Spammer"), QStringLiteral("ban"));
        QCOMPARE(m_ctl->phase(), QStringLiteral("planning"));
        QTRY_COMPARE_WITH_TIMEOUT(m_ctl->phase(), QStringLiteral("failed"), 10000);
        QVERIFY(m_ctl->statusText().contains(QStringLiteral("Nothing was changed")));
        // A late answer does not revive it.
        Q_EMIT m_client->moderationPlanReceived(
            m_client->lastPlanOpId, kTarget, QStringLiteral("ban"), false,
            QVariantList{ planRow(kSpace, QString()) });
        QCOMPARE(m_ctl->phase(), QStringLiteral("failed"));
        QVERIFY(!m_ctl->canConfirm());
        QVERIFY(m_client->sent.isEmpty());
    }

    // An answer in time stops the watchdog.
    void aPlanThatAnswersIsNotFailedLater()
    {
        m_ctl->setPlanTimeoutForTest(30);
        beginWithPlan(QStringLiteral("kick"));
        QTest::qWait(80);
        QCOMPARE(m_ctl->phase(), QStringLiteral("ready"));
    }

    // What the user is asked to confirm: who, where, what happens to the
    // Space, and that the rooms are a separate choice.
    void theConfirmationStatesTheConsequences()
    {
        beginWithPlan(QStringLiteral("kick"));
        QCOMPARE(m_ctl->title(), QStringLiteral("Kick Spammer from Lounge?"));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("can join again if invited")));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("does not remove them from its rooms")));
        QCOMPARE(m_ctl->cascadeLabel(),
                 QStringLiteral("Also kick them from rooms in this space"));
        QCOMPARE(m_ctl->confirmLabel(), QStringLiteral("Kick"));

        cleanup();
        init();
        beginWithPlan(QStringLiteral("ban"));
        QCOMPARE(m_ctl->title(), QStringLiteral("Ban Spammer from Lounge?"));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("cannot join it again until someone unbans them")));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("does not remove them from its rooms")));
        QCOMPARE(m_ctl->cascadeLabel(),
                 QStringLiteral("Also ban them from rooms in this space"));

        cleanup();
        init();
        beginWithPlan(QStringLiteral("unban"));
        QCOMPARE(m_ctl->title(), QStringLiteral("Unban Spammer in Lounge?"));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("not invited back")));
        QCOMPARE(m_ctl->confirmLabel(), QStringLiteral("Unban"));
    }

    void everyPlanReasonHasItsOwnText()
    {
        const QStringList reasons{
            QStringLiteral("not_a_member"), QStringLiteral("already_banned"),
            QStringLiteral("not_banned"), QStringLiteral("no_permission"),
            QStringLiteral("outranked"), QStringLiteral("not_joined"),
            QStringLiteral("unknown"), QStringLiteral("not_checked"),
        };
        QSet<QString> texts;
        for (const QString &reason : reasons)
            texts.insert(SpaceModerationController::reasonText(reason));
        // Distinct per reason, and none is the catch-all.
        QCOMPARE(texts.size(), reasons.size());
        QVERIFY(!texts.contains(SpaceModerationController::reasonText(
            QStringLiteral("something-new"))));
    }
};

QTEST_GUILESS_MAIN(SpaceModerationControllerTest)
#include "SpaceModerationControllerTest.moc"
