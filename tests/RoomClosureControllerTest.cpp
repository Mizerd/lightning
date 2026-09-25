// Closing and deleting rooms through RoomClosureController against a fake
// client: a close is never presented as a delete, the plan is the gate for
// what a close may attempt, a Space cascades to its rooms with the Space
// last, a delete needs the server's own "administrator" answer and the typed
// name, and every room reports its own outcome.

#include "spaces/RoomClosureController.h"
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
    QStringList refuseSend;

    int planCalls = 0;
    QStringList lastPlanRooms;
    quint64 lastPlanOpId = 0;
    int adminCalls = 0;
    quint64 lastAdminOpId = 0;

    struct Sent {
        quint64 opId;
        QString kind; // "close" | "delete"
        QString roomId;
        QString reason;
        bool leave = false;
        QStringList unlist;
        bool block = false;
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

    QStringList lastPlanParents;
    quint64 requestRoomClosurePlan(const QStringList &roomIds,
                                   const QStringList &parentIds) override
    {
        ++planCalls;
        lastPlanRooms = roomIds;
        lastPlanParents = parentIds;
        lastPlanOpId = nextOp++;
        return lastPlanOpId;
    }
    quint64 closeRoom(const QString &roomId, const QString &reason, bool leave,
                      const QStringList &unlist) override
    {
        if (refuseSend.contains(roomId))
            return 0;
        const quint64 id = nextOp++;
        sent.append({ id, QStringLiteral("close"), roomId, reason, leave, unlist,
                      false });
        return id;
    }
    bool refuseAdmin = false;
    quint64 requestServerAdminStatus() override
    {
        ++adminCalls;
        if (refuseAdmin)
            return 0;
        lastAdminOpId = nextOp++;
        return lastAdminOpId;
    }
    quint64 adminDeleteRoom(const QString &roomId, bool block) override
    {
        if (refuseSend.contains(roomId))
            return 0;
        const quint64 id = nextOp++;
        sent.append({ id, QStringLiteral("delete"), roomId, {}, false, {}, block });
        return id;
    }

    void answerClose(const QString &outcome, int removed = 0, int staying = 0,
                     bool left = true)
    {
        const Sent &s = sent.constLast();
        Q_EMIT roomClosureFinished(
            s.opId, s.roomId,
            QVariantMap{
                { QStringLiteral("outcome"), outcome },
                { QStringLiteral("joinRule"),
                  outcome == QLatin1String("failed") ? QStringLiteral("failed")
                                                     : QStringLiteral("set") },
                { QStringLiteral("directory"), QStringLiteral("not_listed") },
                { QStringLiteral("removed"), removed },
                { QStringLiteral("removeFailed"),
                  outcome == QLatin1String("partial") ? 1 : 0 },
                { QStringLiteral("staying"), staying },
                { QStringLiteral("membersRead"), true },
                { QStringLiteral("left"),
                  left && s.leave && outcome == QLatin1String("closed") },
                { QStringLiteral("category"),
                  outcome == QLatin1String("failed") ? QStringLiteral("forbidden")
                                                     : QString() },
            });
    }
    void answerDelete(const QString &status, bool ok)
    {
        const Sent &s = sent.constLast();
        Q_EMIT adminRoomDeleteFinished(
            s.opId, s.roomId,
            QVariantMap{
                { QStringLiteral("status"), status },
                { QStringLiteral("ok"), ok },
                { QStringLiteral("removed"), 3 },
                { QStringLiteral("failedToRemove"), 0 },
                { QStringLiteral("category"), ok ? QString() : QStringLiteral("server") },
            });
    }
};

const QString kSpace = QStringLiteral("!space:example.org");
const QString kSub = QStringLiteral("!sub:example.org");
const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");
const QString kRoomC = QStringLiteral("!c:example.org");
const QString kShared = QStringLiteral("!shared:example.org");
const QString kOtherSpace = QStringLiteral("!other:example.org");

QVariantMap planRow(const QString &roomId, const QString &reason,
                    int removable = 2, int staying = 0,
                    const QStringList &stayingNames = {})
{
    return QVariantMap{
        { QStringLiteral("roomId"), roomId },
        { QStringLiteral("name"), roomId.mid(1, roomId.indexOf(QLatin1Char(':')) - 1) },
        { QStringLiteral("isSpace"), roomId == kSpace || roomId == kSub },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("removable"), removable },
        { QStringLiteral("staying"), staying },
        { QStringLiteral("stayingNames"), stayingNames },
    };
}

QVariantMap row(const RoomClosureController &c, const QString &roomId)
{
    for (const QVariant &v : c.rooms()) {
        const QVariantMap r = v.toMap();
        if (r.value(QStringLiteral("roomId")).toString() == roomId)
            return r;
    }
    return {};
}

QStringList sentRooms(const FakeClient &client)
{
    QStringList out;
    for (const auto &s : client.sent)
        out.append(s.roomId);
    return out;
}

} // namespace

class RoomClosureControllerTest : public QObject
{
    Q_OBJECT

private:
    FakeClient *m_client = nullptr;
    RoomClosureController *m_ctl = nullptr;

    void becomeAdmin(bool admin)
    {
        m_ctl->checkServerAdmin();
        Q_EMIT m_client->serverAdminStatusReceived(
            m_client->lastAdminOpId, admin,
            admin ? QString() : QStringLiteral("not_admin"));
    }

    // Space -> sub -> A; Space -> B; Space -> C (not offered); shared room is
    // also listed by another Space.
    void beginSpaceClose()
    {
        QVERIFY(m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                             QStringLiteral("close")));
        QCOMPARE(m_ctl->phase(), QStringLiteral("planning"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false,
            QVariantList{
                planRow(kSpace, QString()),
                planRow(kSub, QString()),
                planRow(kRoomA, QString(), 5, 1, { QStringLiteral("Alice") }),
                planRow(kRoomB, QString()),
                planRow(kRoomC, QStringLiteral("no_permission")),
                planRow(kShared, QString()),
            });
        QCOMPARE(m_ctl->phase(), QStringLiteral("ready"));
    }

private slots:
    void init()
    {
        m_client = new FakeClient;
        m_ctl = new RoomClosureController;
        m_ctl->setClient(m_client);
        m_ctl->setScopeResolver([](const QString &spaceId) {
            return spaceId == kSpace
                ? QStringList{ kSpace, kSub, kRoomA, kRoomB, kRoomC, kShared }
                : QStringList{};
        });
        m_ctl->setParentResolver([](const QString &roomId) {
            if (roomId == kShared)
                return QStringList{ kSpace, kOtherSpace };
            if (roomId == kRoomA)
                return QStringList{ kSub };
            if (roomId == kRoomB || roomId == kSub || roomId == kRoomC)
                return QStringList{ kSpace };
            if (roomId == QLatin1String("!lone:example.org"))
                return QStringList{ kSpace, kOtherSpace };
            return QStringList{};
        });
        m_ctl->setRoomInfoResolver([](const QString &roomId) {
            if (roomId == kSpace)
                return QVariantMap{ { QStringLiteral("name"), QStringLiteral("Lounge") },
                                    { QStringLiteral("isSpace"), true } };
            if (roomId == kOtherSpace)
                return QVariantMap{ { QStringLiteral("name"), QStringLiteral("Other") },
                                    { QStringLiteral("isSpace"), true } };
            return QVariantMap{ { QStringLiteral("name"), roomId.mid(1, 1) } };
        });
    }

    void cleanup()
    {
        delete m_ctl;
        delete m_client;
        m_ctl = nullptr;
        m_client = nullptr;
    }

    // Closing is never called deleting, and the confirmation says what stays.
    void aCloseIsNeverPresentedAsADelete()
    {
        QVERIFY(m_ctl->begin(QStringLiteral("!lone:example.org"),
                             QStringLiteral("General"), false,
                             QStringLiteral("close")));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false,
            QVariantList{ planRow(QStringLiteral("!lone:example.org"), QString()) });
        QCOMPARE(m_ctl->title(), QStringLiteral("Close General?"));
        QCOMPARE(m_ctl->confirmLabel(), QStringLiteral("Close room"));
        const QString text = m_ctl->consequenceText();
        QVERIFY(text.contains(QStringLiteral("invite-only")));
        QVERIFY(text.contains(QStringLiteral("does not delete anything")));
        QVERIFY(text.contains(QStringLiteral("history stays on every server")));
        QVERIFY(text.contains(QStringLiteral("Everyone you cannot remove stays")));
        QVERIFY(text.contains(QStringLiteral("anyone can read stays readable")));
        QVERIFY(!m_ctl->title().contains(QStringLiteral("Delete")));
        QVERIFY(!m_ctl->confirmLabel().contains(QStringLiteral("Delete")));
        // Leaving is part of the promise only while it is chosen.
        QVERIFY(text.contains(QStringLiteral("then you leave")));
        m_ctl->setLeaveAfter(false);
        QVERIFY(m_ctl->consequenceText().contains(QStringLiteral("You stay.")));
        QVERIFY(!m_ctl->consequenceText().contains(QStringLiteral("then you leave")));
    }

    // One room: its parents are offered for unlisting, and the choice is
    // what the backend is asked to do.
    void aSingleRoomCloseCarriesItsOptions()
    {
        const QString lone = QStringLiteral("!lone:example.org");
        QVERIFY(m_ctl->begin(lone, QStringLiteral("General"), false,
                             QStringLiteral("close")));
        // The parents are asked about too, for their child permission only.
        QCOMPARE(m_client->lastPlanRooms, QStringList{ lone });
        QCOMPARE(m_client->lastPlanParents, (QStringList{ kSpace, kOtherSpace }));
        QVariantMap editable = planRow(kSpace, QStringLiteral("parent"));
        editable.insert(QStringLiteral("canEditChildren"), true);
        QVariantMap locked = planRow(kOtherSpace, QStringLiteral("parent"));
        locked.insert(QStringLiteral("canEditChildren"), false);
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false,
            QVariantList{ planRow(lone, QString()), editable, locked });
        // Parents are not rows.
        QVERIFY(m_ctl->rooms().isEmpty());
        // Only a parent whose children the viewer may change is offered;
        // the other is named, so a close can still finish.
        QCOMPARE(m_ctl->parentSpaceNames(), QStringList{ QStringLiteral("Lounge") });
        QCOMPARE(m_ctl->lockedParentNames(), QStringList{ QStringLiteral("Other") });
        m_ctl->confirm(QStringLiteral("  wound down  "));
        QCOMPARE(m_client->sent.size(), 1);
        const auto &s = m_client->sent.constFirst();
        QCOMPARE(s.kind, QStringLiteral("close"));
        QCOMPARE(s.reason, QStringLiteral("wound down"));
        QCOMPARE(s.leave, true);
        QCOMPARE(s.unlist, QStringList{ kSpace });

        cleanup();
        init();
        QVERIFY(m_ctl->begin(lone, QString(), false, QStringLiteral("close")));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false, QVariantList{ planRow(lone, QString()) });
        m_ctl->setUnlistFromParents(false);
        m_ctl->setLeaveAfter(false);
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.constFirst().unlist, QStringList{});
        QCOMPARE(m_client->sent.constFirst().leave, false);
    }

    // The plan is the gate: a room it does not offer is never sent, whatever
    // the view asks, and a room another Space also lists starts unselected.
    void onlyOfferedRoomsAreClosedAndTheSpaceGoesLast()
    {
        beginSpaceClose();
        QCOMPARE(m_client->lastPlanRooms,
                 (QStringList{ kSpace, kSub, kRoomA, kRoomB, kRoomC, kShared }));
        QVERIFY(m_ctl->targetEligible());
        QCOMPARE(m_ctl->eligibleRoomCount(), 4);
        QCOMPARE(row(*m_ctl, kShared).value(QStringLiteral("alsoElsewhere")).toInt(), 1);
        QCOMPARE(row(*m_ctl, kShared).value(QStringLiteral("selected")).toBool(), false);
        QCOMPARE(m_ctl->selectedRoomCount(), 3);
        m_ctl->setRoomSelected(kRoomC, true);
        QCOMPARE(row(*m_ctl, kRoomC).value(QStringLiteral("selected")).toBool(), false);
        QCOMPARE(row(*m_ctl, kRoomC).value(QStringLiteral("reasonText")).toString(),
                 RoomClosureController::reasonText(QStringLiteral("no_permission")));
        QCOMPARE(m_ctl->title(), QStringLiteral("Close Lounge and its rooms?"));

        m_ctl->confirm(QString());
        QCOMPARE(m_ctl->phase(), QStringLiteral("running"));
        // One at a time.
        QCOMPARE(m_client->sent.size(), 1);
        for (int i = 0; i < 4; ++i)
            m_client->answerClose(QStringLiteral("closed"));
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        // Deepest first, the Space last; no Space unlisting inside a cascade.
        QCOMPARE(sentRooms(*m_client), (QStringList{ kRoomB, kRoomA, kSub, kSpace }));
        for (const auto &s : m_client->sent)
            QVERIFY(s.unlist.isEmpty());
        QCOMPARE(row(*m_ctl, kRoomC).value(QStringLiteral("status")).toString(),
                 QStringLiteral("skipped"));
        QCOMPARE(row(*m_ctl, kShared).value(QStringLiteral("status")).toString(),
                 QStringLiteral("skipped"));
        QCOMPARE(m_ctl->statusText(), QStringLiteral("Done: 4 closed."));
    }

    void withoutTheCascadeOnlyTheSpaceIsClosed()
    {
        beginSpaceClose();
        m_ctl->setCascade(false);
        QCOMPARE(m_ctl->title(), QStringLiteral("Close Lounge?"));
        m_ctl->confirm(QString());
        QCOMPARE(sentRooms(*m_client), QStringList{ kSpace });
    }

    // Every room reports its own outcome; a partial close is not a success
    // and a refusal does not stop the others.
    void eachRoomReportsItsOwnOutcome()
    {
        m_client->refuseSend = { kRoomA };
        beginSpaceClose();
        m_ctl->confirm(QString());
        m_client->answerClose(QStringLiteral("partial"), 3, 0);  // B
        // A could not be sent at all.
        m_client->answerClose(QStringLiteral("failed"));         // sub
        m_client->answerClose(QStringLiteral("closed"), 4, 1);   // Space
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QCOMPARE(m_ctl->succeededCount(), 1);
        QCOMPARE(m_ctl->partialCount(), 1);
        QCOMPARE(m_ctl->failedCount(), 2);

        const QVariantMap b = row(*m_ctl, kRoomB);
        QCOMPARE(b.value(QStringLiteral("status")).toString(), QStringLiteral("partial"));
        QVERIFY(b.value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("you stayed so you can run it again")));
        const QVariantMap sub = row(*m_ctl, kSub);
        QCOMPARE(sub.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(sub.value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("Nothing was changed")));
        QCOMPARE(row(*m_ctl, kRoomA).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        // Nothing beneath the subspace or the Space finished cleanly, so the
        // viewer stays in both: once out of a Space it cannot be cascaded
        // again.
        QCOMPARE(sentRooms(*m_client), (QStringList{ kRoomB, kSub, kSpace }));
        QCOMPARE(m_client->sent.at(0).leave, true);
        QCOMPARE(m_client->sent.at(1).leave, false);
        QCOMPARE(m_client->sent.at(2).leave, false);
        const QString space =
            m_ctl->targetRow().value(QStringLiteral("message")).toString();
        QVERIFY(space.contains(QStringLiteral("4 removed")));
        QVERIFY(space.contains(QStringLiteral("1 stay")));
        QVERIFY(!space.contains(QStringLiteral("you left")));
        QVERIFY(space.contains(QStringLiteral("you stayed: rooms inside it")));
        QVERIFY(m_ctl->statusText().contains(QStringLiteral("1 partly closed")));
    }

    // A subspace is left when everything beneath IT closed, whatever
    // happened to rooms elsewhere in the Space; the Space itself is not.
    void aSpaceIsLeftOnlyWhenEverythingBeneathItClosed()
    {
        beginSpaceClose();
        m_ctl->confirm(QString());
        m_client->answerClose(QStringLiteral("failed"));  // B
        m_client->answerClose(QStringLiteral("closed"));  // A (in sub)
        m_client->answerClose(QStringLiteral("closed"));  // sub
        m_client->answerClose(QStringLiteral("closed"));  // Space
        QCOMPARE(sentRooms(*m_client), (QStringList{ kRoomB, kRoomA, kSub, kSpace }));
        QCOMPARE(m_client->sent.at(2).leave, true);
        QCOMPARE(m_client->sent.at(3).leave, false);
    }

    // A step that never answers is marked "not known yet" and the rest go
    // on; its late answer is not counted.
    void aStepThatNeverAnswersDoesNotHoldTheRest()
    {
        // No wall clock: the watchdog is checked armed, then expired through
        // the test hook.
        beginSpaceClose();
        m_ctl->confirm(QString());
        const auto first = m_client->sent.constFirst();
        QVERIFY(m_ctl->stepWatchdogArmedForTest());
        QCOMPARE(m_ctl->stepWatchdogIntervalForTest(),
                 RoomClosureController::kStepSilenceMs);
        // Progress for the step in flight keeps it armed.
        Q_EMIT m_client->roomClosureProgress(first.opId, first.roomId, 0, 0);
        QVERIFY(m_ctl->stepWatchdogArmedForTest());
        QCOMPARE(row(*m_ctl, first.roomId).value(QStringLiteral("progress")).toString(),
                 QStringLiteral("Working…"));
        m_ctl->expireStepForTest();
        QCOMPARE(m_client->sent.size(), 2);
        // The next step has its own watchdog.
        QVERIFY(m_ctl->stepWatchdogArmedForTest());
        QCOMPARE(row(*m_ctl, first.roomId).value(QStringLiteral("status")).toString(),
                 QStringLiteral("unknown"));
        QVERIFY(row(*m_ctl, first.roomId).value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("check the room")));
        QCOMPARE(m_ctl->unknownCount(), 1);
        Q_EMIT m_client->roomClosureFinished(
            first.opId, first.roomId,
            QVariantMap{ { QStringLiteral("outcome"), QStringLiteral("closed") } });
        QCOMPARE(m_ctl->succeededCount(), 0);
        // Something not known beneath the Space keeps the viewer in it.
        m_client->answerClose(QStringLiteral("closed"));
        m_client->answerClose(QStringLiteral("closed"));
        m_client->answerClose(QStringLiteral("closed"));
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QCOMPARE(m_client->sent.constLast().roomId, kSpace);
        QCOMPARE(m_client->sent.constLast().leave, false);
        QVERIFY(m_ctl->statusText().contains(QStringLiteral("1 not known yet")));
        QVERIFY(!m_ctl->stepWatchdogArmedForTest());
    }

    // Hiding the dialog keeps the flow; reopening shows it, and a flow that
    // ended while hidden is shown once before a new one can start.
    void hidingTheDialogKeepsTheFlow()
    {
        beginSpaceClose();
        m_ctl->confirm(QString());
        m_ctl->viewClosed();
        QCOMPARE(m_ctl->phase(), QStringLiteral("running"));
        QVERIFY(!m_ctl->begin(kRoomB, QStringLiteral("B"), false,
                              QStringLiteral("close")));
        QVERIFY(m_ctl->notice().contains(QStringLiteral("still running")));
        m_ctl->viewClosed();
        for (int i = 0; i < 4; ++i)
            m_client->answerClose(QStringLiteral("closed"));
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QVERIFY(!m_ctl->begin(kRoomB, QStringLiteral("B"), false,
                              QStringLiteral("close")));
        QCOMPARE(m_ctl->targetId(), kSpace);
        QVERIFY(m_ctl->notice().contains(QStringLiteral("how the earlier")));
        // Seen: closing the dialog now resets, and a new flow starts.
        m_ctl->viewClosed();
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        QVERIFY(m_ctl->begin(kRoomB, QStringLiteral("B"), false,
                             QStringLiteral("close")));
    }

    // Without the kick permission nobody is removed, and every text says so.
    void withoutKickPermissionEveryoneStaysAndItIsSaid()
    {
        QVariantMap plan = planRow(kRoomB, QString(), 0, 3);
        plan.insert(QStringLiteral("canKick"), false);
        QCOMPARE(RoomClosureController::closeSummary(plan),
                 QStringLiteral("You can't remove members here: all 3 stay"));
        plan.insert(QStringLiteral("worldReadable"), true);
        QVERIFY(RoomClosureController::closeSummary(plan)
                    .contains(QStringLiteral("readable by anyone")));

        const QString lone = QStringLiteral("!lone:example.org");
        m_ctl->begin(lone, QString(), false, QStringLiteral("close"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false, QVariantList{ planRow(lone, QString()) });
        m_ctl->confirm(QString());
        const auto &s = m_client->sent.constFirst();
        Q_EMIT m_client->roomClosureFinished(
            s.opId, s.roomId,
            QVariantMap{ { QStringLiteral("outcome"), QStringLiteral("closed") },
                         { QStringLiteral("staying"), 3 },
                         { QStringLiteral("canKick"), false },
                         { QStringLiteral("membersRead"), true } });
        const QString message =
            m_ctl->targetRow().value(QStringLiteral("message")).toString();
        QVERIFY(message.contains(QStringLiteral("3 stay (you can't remove members here)")));
        QVERIFY(!message.contains(QStringLiteral("role not below yours")));
    }

    // "Not an administrator" is believed; "could not ask" is asked again.
    void anUnreachableAdminAnswerIsAskedAgain()
    {
        m_ctl->setAdminRetryForTest(0);
        m_ctl->checkServerAdmin();
        Q_EMIT m_client->serverAdminStatusReceived(m_client->lastAdminOpId, false,
                                                   QStringLiteral("unavailable"));
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("no"));
        m_ctl->checkServerAdmin();
        QCOMPARE(m_client->adminCalls, 2);
        Q_EMIT m_client->serverAdminStatusReceived(m_client->lastAdminOpId, false,
                                                   QStringLiteral("not_admin"));
        m_ctl->checkServerAdmin();
        QCOMPARE(m_client->adminCalls, 2);
    }

    void aRejectedSessionSaysSignInAgain()
    {
        becomeAdmin(true);
        const QString lone = QStringLiteral("!lone:example.org");
        m_ctl->begin(lone, QString(), false, QStringLiteral("delete"));
        m_ctl->setTypedConfirmation(lone);
        m_ctl->confirm(QString());
        const auto &s = m_client->sent.constFirst();
        Q_EMIT m_client->adminRoomDeleteFinished(
            s.opId, s.roomId,
            QVariantMap{ { QStringLiteral("status"), QStringLiteral("failed") },
                         { QStringLiteral("ok"), false },
                         { QStringLiteral("category"), QStringLiteral("unauthorized") } });
        QVERIFY(m_ctl->targetRow().value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("Sign in again")));
    }

    // Add-account never signs the previous account out. Nothing more may go
    // out through the client once it speaks for another account.
    void aFlowNeverContinuesUnderAnotherAccount()
    {
        beginSpaceClose();
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.size(), 1);
        m_client->userId = QStringLiteral("@other:example.org");
        // The old account's answer never comes; the watchdog fires.
        m_ctl->expireStepForTest();
        QCOMPARE(m_client->sent.size(), 1);
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
    }

    void anAdminAnswerAboutAnotherAccountIsIgnored()
    {
        m_ctl->checkServerAdmin();
        m_client->userId = QStringLiteral("@other:example.org");
        Q_EMIT m_client->serverAdminStatusReceived(m_client->lastAdminOpId, true,
                                                   QString());
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("unknown"));
        QVERIFY(!m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                              QStringLiteral("delete")));

        // A "yes" for the first account does not carry to the next either.
        cleanup();
        init();
        becomeAdmin(true);
        m_client->userId = QStringLiteral("@other:example.org");
        QVERIFY(!m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                              QStringLiteral("delete")));
    }

    void resettingForAnAccountChangeForgetsEverything()
    {
        becomeAdmin(true);
        beginSpaceClose();
        m_ctl->confirm(QString());
        m_ctl->resetForAccountChange();
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("unknown"));
        m_client->answerClose(QStringLiteral("closed"));
        QCOMPARE(m_client->sent.size(), 1);
    }

    // A room the plan could not assess may still need closing, so the Space
    // above it is not left.
    // The whole scope has rows, so nothing but C's reason can hold the leave;
    // the no_permission pass is the control that leaves.
    void anUnassessedRoomKeepsTheViewerInTheSpace()
    {
        for (const QString &reasonC :
             { QStringLiteral("no_permission"), QStringLiteral("not_checked"),
               QStringLiteral("unknown") }) {
            cleanup();
            init();
            QVERIFY(m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                                 QStringLiteral("close")));
            Q_EMIT m_client->roomClosurePlanReceived(
                m_client->lastPlanOpId, false,
                QVariantList{ planRow(kSpace, QString()), planRow(kSub, QString()),
                              planRow(kRoomA, QString()), planRow(kRoomB, QString()),
                              planRow(kRoomC, reasonC), planRow(kShared, QString()) });
            m_ctl->confirm(QString());
            for (int i = 0; i < 4; ++i)
                m_client->answerClose(QStringLiteral("closed")); // B, A, sub, Space
            QCOMPARE(sentRooms(*m_client),
                     (QStringList{ kRoomB, kRoomA, kSub, kSpace }));
            QCOMPARE(m_client->sent.at(3).leave,
                     reasonC == QLatin1String("no_permission"));
        }
    }

    // A delete whose request got no answer is not a failure: nobody knows.
    void aDeleteWithNoAnswerIsNotKnownRatherThanFailed()
    {
        becomeAdmin(true);
        const QString lone = QStringLiteral("!lone:example.org");
        m_ctl->begin(lone, QString(), false, QStringLiteral("delete"));
        m_ctl->setTypedConfirmation(lone);
        m_ctl->confirm(QString());
        const auto &s = m_client->sent.constFirst();
        Q_EMIT m_client->adminRoomDeleteFinished(
            s.opId, s.roomId,
            QVariantMap{ { QStringLiteral("status"), QStringLiteral("unknown") },
                         { QStringLiteral("ok"), false },
                         { QStringLiteral("category"), QStringLiteral("unknown") } });
        QCOMPARE(m_ctl->targetRow().value(QStringLiteral("status")).toString(),
                 QStringLiteral("unknown"));
        QVERIFY(m_ctl->targetRow().value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("Whether the server deleted it is "
                                             "not known")));
        QVERIFY(m_ctl->targetRow().value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("Check the room before trying again")));
        QCOMPARE(m_ctl->failedCount(), 0);
        QCOMPARE(m_ctl->unknownCount(), 1);

        cleanup();
        init();
        becomeAdmin(true);
        m_ctl->begin(lone, QString(), false, QStringLiteral("delete"));
        m_ctl->setTypedConfirmation(lone);
        m_ctl->confirm(QString());
        const auto &t = m_client->sent.constFirst();
        Q_EMIT m_client->adminRoomDeleteFinished(
            t.opId, t.roomId,
            QVariantMap{ { QStringLiteral("status"), QStringLiteral("failed") },
                         { QStringLiteral("ok"), false },
                         { QStringLiteral("category"), QStringLiteral("invalid") } });
        QVERIFY(m_ctl->targetRow().value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("may already be running")));
    }

    // An administrator question that could not even be sent is asked again.
    void anUnsentAdminQuestionIsAskedAgain()
    {
        m_client->refuseAdmin = true;
        m_ctl->setAdminRetryForTest(0);
        m_ctl->checkServerAdmin();
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("no"));
        m_ctl->checkServerAdmin();
        QCOMPARE(m_client->adminCalls, 2);
    }

    // A name nobody can type does not lock the delete: the id works, always.
    void theRoomIdAlwaysConfirmsADelete()
    {
        becomeAdmin(true);
        const QString lone = QStringLiteral("!lone:example.org");
        m_ctl->begin(lone, QStringLiteral("Gen\u200Beral"), false,
                     QStringLiteral("delete"));
        QVERIFY(!m_ctl->confirmationNameUsable());
        QCOMPARE(m_ctl->confirmationPhrase(), lone);
        m_ctl->setTypedConfirmation(lone);
        QVERIFY(m_ctl->canConfirm());

        cleanup();
        init();
        becomeAdmin(true);
        m_ctl->begin(lone, QStringLiteral("General 🎉"), false,
                     QStringLiteral("delete"));
        QVERIFY(m_ctl->confirmationNameUsable());
        m_ctl->setTypedConfirmation(QStringLiteral("General 🎉"));
        QVERIFY(m_ctl->canConfirm());
        m_ctl->setTypedConfirmation(lone);
        QVERIFY(m_ctl->canConfirm());
    }

    // A plan cut at its cap has no row for the rooms past it; those were
    // never closed, so the Space above them is not left.
    void aTruncatedPlanKeepsTheViewerInTheSpace()
    {
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), true, QStringLiteral("close"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, /*truncated=*/true,
            QVariantList{ planRow(kSpace, QString()), planRow(kRoomB, QString()) });
        QVERIFY(m_ctl->planTruncated());
        m_ctl->confirm(QString());
        m_client->answerClose(QStringLiteral("closed")); // B
        m_client->answerClose(QStringLiteral("closed")); // Space
        QCOMPARE(sentRooms(*m_client), (QStringList{ kRoomB, kSpace }));
        QCOMPARE(m_client->sent.at(0).leave, true);
        QCOMPARE(m_client->sent.at(1).leave, false);
    }

    // A room listed under two subspaces runs under only one of them; the
    // other is not left while it is still waiting.
    void aSubspaceIsNotLeftBeforeASharedRoomRuns()
    {
        const QString kSub2 = QStringLiteral("!sub2:example.org");
        m_ctl->setScopeResolver([kSub2](const QString &) {
            return QStringList{ kSpace, kSub, kRoomA, kSub2 };
        });
        m_ctl->setParentResolver([kSub2](const QString &roomId) {
            if (roomId == kRoomA)
                return QStringList{ kSub, kSub2 };
            if (roomId == kSub || roomId == kSub2)
                return QStringList{ kSpace };
            return QStringList{};
        });
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), true, QStringLiteral("close"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false,
            QVariantList{ planRow(kSpace, QString()), planRow(kSub, QString()),
                          planRow(kRoomA, QString()), planRow(kSub2, QString()) });
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.constFirst().roomId, kSub2);
        QCOMPARE(m_client->sent.constFirst().leave, false);
        for (int i = 0; i < 4; ++i)
            m_client->answerClose(QStringLiteral("closed"));
        QCOMPARE(sentRooms(*m_client), (QStringList{ kSub2, kRoomA, kSub, kSpace }));
        QCOMPARE(m_client->sent.at(2).leave, true);
    }

    // The administrator answer is about one account: shown to another, it
    // reads "unknown" (so no delete button) even if no reset ran, and that
    // account is asked for itself.
    void anAdminAnswerNeverShowsForAnotherAccount()
    {
        becomeAdmin(true);
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("yes"));
        m_client->userId = QStringLiteral("@other:example.org");
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("unknown"));
        m_ctl->checkServerAdmin();
        QCOMPARE(m_client->adminCalls, 2);
        Q_EMIT m_client->serverAdminStatusReceived(m_client->lastAdminOpId, false,
                                                   QStringLiteral("not_admin"));
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("no"));
    }

    // A join-rule change that timed out may have landed: not known, not
    // "nothing was changed".
    void aTimedOutJoinRuleIsNotKnown()
    {
        const QString lone = QStringLiteral("!lone:example.org");
        m_ctl->begin(lone, QString(), false, QStringLiteral("close"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false, QVariantList{ planRow(lone, QString()) });
        m_ctl->confirm(QString());
        const auto &s = m_client->sent.constFirst();
        Q_EMIT m_client->roomClosureFinished(
            s.opId, s.roomId,
            QVariantMap{ { QStringLiteral("outcome"), QStringLiteral("failed") },
                         { QStringLiteral("joinRule"), QStringLiteral("unknown") },
                         { QStringLiteral("category"), QStringLiteral("unknown") } });
        QCOMPARE(m_ctl->targetRow().value(QStringLiteral("status")).toString(),
                 QStringLiteral("unknown"));
        const QString message =
            m_ctl->targetRow().value(QStringLiteral("message")).toString();
        QVERIFY(message.contains(QStringLiteral("may or may not have changed")));
        QVERIFY(!message.contains(QStringLiteral("Nothing was changed")));
    }

    // A room the viewer cannot make invite-only is not offered at all.
    void aRoomThatCannotBeRestrictedIsNotOffered()
    {
        const QString lone = QStringLiteral("!lone:example.org");
        m_ctl->begin(lone, QStringLiteral("General"), false, QStringLiteral("close"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false,
            QVariantList{ planRow(lone, QStringLiteral("no_permission")) });
        QVERIFY(!m_ctl->targetEligible());
        QVERIFY(!m_ctl->canConfirm());
        m_ctl->confirm(QString());
        QVERIFY(m_client->sent.isEmpty());
        // A row with no reason at all fails closed too.
        cleanup();
        init();
        m_ctl->begin(lone, QStringLiteral("General"), false, QStringLiteral("close"));
        QVariantMap noReason = planRow(lone, QString());
        noReason.remove(QStringLiteral("reason"));
        Q_EMIT m_client->roomClosurePlanReceived(m_client->lastPlanOpId, false,
                                                 QVariantList{ noReason });
        QVERIFY(!m_ctl->canConfirm());
    }

    void thePlanSummaryNamesWhoStays()
    {
        beginSpaceClose();
        QCOMPARE(row(*m_ctl, kRoomA).value(QStringLiteral("summary")).toString(),
                 QStringLiteral("Removes 5 · 1 stay: Alice"));
        QCOMPARE(RoomClosureController::closeSummary(planRow(kRoomB, QString(), 0, 0)),
                 QStringLiteral("Only you are here"));
        QCOMPARE(RoomClosureController::closeSummary(planRow(
                     kRoomB, QString(), 0, 4,
                     { QStringLiteral("A"), QStringLiteral("B") })),
                 QStringLiteral("Removes nobody · 4 stay: A, B and 2 more"));
    }

    // A delete is not offered until the server says the account is an
    // administrator, and the question is asked once.
    void aDeleteNeedsTheServersOwnAnswer()
    {
        QVERIFY(!m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                              QStringLiteral("delete")));
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        QCOMPARE(m_client->adminCalls, 0);

        m_ctl->checkServerAdmin();
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("checking"));
        m_ctl->checkServerAdmin();
        QCOMPARE(m_client->adminCalls, 1);
        // A stale answer is ignored.
        Q_EMIT m_client->serverAdminStatusReceived(m_client->lastAdminOpId + 7,
                                                   true, QString());
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("checking"));
        Q_EMIT m_client->serverAdminStatusReceived(m_client->lastAdminOpId, false,
                                                   QStringLiteral("unavailable"));
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("no"));
        QVERIFY(!m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                              QStringLiteral("delete")));
        m_ctl->checkServerAdmin();
        QCOMPARE(m_client->adminCalls, 1);

        // Signing out forgets the answer.
        m_client->logout();
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("unknown"));
    }

    // Deleting needs the typed name, names the server, and says what it does
    // not reach.
    void aDeleteIsConfirmedByTypingTheName()
    {
        becomeAdmin(true);
        QCOMPARE(m_ctl->serverAdmin(), QStringLiteral("yes"));
        QVERIFY(m_ctl->begin(kSpace, QStringLiteral("Lounge"), true,
                             QStringLiteral("delete")));
        QCOMPARE(m_ctl->phase(), QStringLiteral("ready"));
        QCOMPARE(m_client->planCalls, 0);
        // The cascade is named with its size, not as "it".
        QCOMPARE(m_ctl->title(),
                 QStringLiteral("Delete Lounge and 4 rooms from example.org?"));
        QVERIFY(m_ctl->consequenceText().contains(QStringLiteral("cannot be undone")));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("the space and the 4 rooms selected below")));
        QVERIFY(m_ctl->consequenceText().contains(
            QStringLiteral("Members on other servers keep the rooms")));
        m_ctl->setCascade(false);
        QCOMPARE(m_ctl->title(), QStringLiteral("Delete Lounge from example.org?"));
        QVERIFY(m_ctl->consequenceText().contains(QStringLiteral("deletes it from")));
        m_ctl->setCascade(true);
        QCOMPARE(m_ctl->confirmationPhrase(), QStringLiteral("Lounge"));
        QVERIFY(!m_ctl->canConfirm());
        m_ctl->confirm(QString());
        QVERIFY(m_client->sent.isEmpty());
        m_ctl->setTypedConfirmation(QStringLiteral("lounge"));
        QVERIFY(!m_ctl->canConfirm());
        m_ctl->setTypedConfirmation(QStringLiteral(" Lounge "));
        QVERIFY(m_ctl->canConfirm());

        m_ctl->setBlock(false);
        m_ctl->confirm(QString());
        // The shared room was not selected; the rest go first, the Space last.
        QCOMPARE(m_client->sent.size(), 1);
        for (int i = 0; i < 5; ++i)
            m_client->answerDelete(QStringLiteral("complete"), true);
        QCOMPARE(m_ctl->phase(), QStringLiteral("done"));
        QCOMPARE(sentRooms(*m_client),
                 (QStringList{ kRoomC, kRoomB, kRoomA, kSub, kSpace }));
        for (const auto &s : m_client->sent) {
            QCOMPARE(s.kind, QStringLiteral("delete"));
            QCOMPARE(s.block, false);
        }
        QVERIFY(m_ctl->targetRow().value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("Deleted from the server")));
    }

    // A delete the server has not finished is not reported as done or failed.
    void aDeleteStillRunningOnTheServerIsSaidSo()
    {
        becomeAdmin(true);
        const QString lone = QStringLiteral("!lone:example.org");
        QVERIFY(m_ctl->begin(lone, QString(), false, QStringLiteral("delete")));
        QCOMPARE(m_ctl->confirmationPhrase(), lone);
        m_ctl->setTypedConfirmation(lone);
        m_ctl->confirm(QString());
        QCOMPARE(m_client->sent.constFirst().block, true);
        Q_EMIT m_client->adminRoomDeleteProgress(m_client->sent.constFirst().opId,
                                                 lone, QStringLiteral("active"));
        QCOMPARE(m_ctl->targetRow().value(QStringLiteral("progress")).toString(),
                 QStringLiteral("The server is deleting it"));
        m_client->answerDelete(QStringLiteral("following"), false);
        QCOMPARE(m_ctl->targetRow().value(QStringLiteral("status")).toString(),
                 QStringLiteral("following"));
        QCOMPARE(m_ctl->partialCount(), 1);
        QCOMPARE(m_ctl->failedCount(), 0);
        QVERIFY(m_ctl->statusText().contains(
            QStringLiteral("1 still being deleted by the server")));
    }

    void anAnswerForAnotherStepIsIgnored()
    {
        beginSpaceClose();
        m_ctl->confirm(QString());
        const auto step = m_client->sent.constLast();
        Q_EMIT m_client->roomClosureFinished(
            step.opId + 40, step.roomId,
            QVariantMap{ { QStringLiteral("outcome"), QStringLiteral("closed") } });
        Q_EMIT m_client->roomClosureFinished(
            step.opId, kRoomC,
            QVariantMap{ { QStringLiteral("outcome"), QStringLiteral("closed") } });
        // A delete answer cannot finish a close step.
        Q_EMIT m_client->adminRoomDeleteFinished(
            step.opId, step.roomId,
            QVariantMap{ { QStringLiteral("status"), QStringLiteral("complete") },
                         { QStringLiteral("ok"), true } });
        QCOMPARE(m_client->sent.size(), 1);
        QCOMPARE(m_ctl->succeededCount(), 0);
        QCOMPARE(m_ctl->phase(), QStringLiteral("running"));
    }

    void aNewFlowWhileOneRunsIsRefusedVisibly()
    {
        beginSpaceClose();
        m_ctl->confirm(QString());
        QVERIFY(!m_ctl->begin(kRoomB, QStringLiteral("B"), false,
                              QStringLiteral("close")));
        QCOMPARE(m_ctl->targetId(), kSpace);
        QVERIFY(m_ctl->notice().contains(QStringLiteral("still running")));
        QCOMPARE(m_client->planCalls, 1);
    }

    void signingOutDropsTheFlow()
    {
        beginSpaceClose();
        m_ctl->confirm(QString());
        const auto step = m_client->sent.constLast();
        m_client->logout();
        QCOMPARE(m_ctl->phase(), QStringLiteral("idle"));
        Q_EMIT m_client->roomClosureFinished(
            step.opId, step.roomId,
            QVariantMap{ { QStringLiteral("outcome"), QStringLiteral("closed") } });
        QCOMPARE(m_ctl->succeededCount(), 0);
        QCOMPARE(m_client->sent.size(), 1);
    }

    void aPlanThatNeverAnswersFails()
    {
        m_ctl->setPlanTimeoutForTest(30);
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), true, QStringLiteral("close"));
        QTRY_COMPARE_WITH_TIMEOUT(m_ctl->phase(), QStringLiteral("failed"), 10000);
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false, QVariantList{ planRow(kSpace, QString()) });
        QCOMPARE(m_ctl->phase(), QStringLiteral("failed"));
        QVERIFY(!m_ctl->canConfirm());
    }

    void roomsNotCheckedInTimeAreNeverClosed()
    {
        m_ctl->begin(kSpace, QStringLiteral("Lounge"), true, QStringLiteral("close"));
        Q_EMIT m_client->roomClosurePlanReceived(
            m_client->lastPlanOpId, false,
            QVariantList{ planRow(kSpace, QString()),
                          planRow(kRoomB, QStringLiteral("not_checked")) });
        QCOMPARE(m_ctl->uncheckedRoomCount(), 1);
        QVERIFY(m_ctl->statusText().contains(QStringLiteral("1 of the rooms")));
        m_ctl->setRoomSelected(kRoomB, true);
        m_ctl->confirm(QString());
        m_client->answerClose(QStringLiteral("closed"));
        QCOMPARE(sentRooms(*m_client), QStringList{ kSpace });
    }

    void everyReasonHasItsOwnText()
    {
        const QStringList reasons{
            QStringLiteral("not_joined"), QStringLiteral("no_permission"),
            QStringLiteral("unknown"), QStringLiteral("not_checked"),
        };
        QSet<QString> texts;
        for (const QString &reason : reasons)
            texts.insert(RoomClosureController::reasonText(reason));
        QCOMPARE(texts.size(), reasons.size());
        QVERIFY(!texts.contains(RoomClosureController::reasonText(
            QStringLiteral("something-new"))));
    }
};

QTEST_GUILESS_MAIN(RoomClosureControllerTest)
#include "RoomClosureControllerTest.moc"
