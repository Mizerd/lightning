// Labs (concurrent live accounts): AccountActivityModel, the per-account
// badge rows the background hosts feed. Epoch-stamped summaries (a late
// result from a retired host is refused, also after removal), one-row
// updates that never reset the model, "only a Running account shows
// anything", and totals that leave the active account out. Every case names
// the naive version it fails on.

#include "models/AccountActivityModel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

namespace {

using Summary = AccountActivityModel::Summary;

Summary sum(int unreadRooms, int mentions, int invites = 0, bool lowerBound = false)
{
    Summary s;
    s.unreadRooms = unreadRooms;
    s.mentions = mentions;
    s.invites = invites;
    s.mentionsLowerBound = lowerBound;
    return s;
}

const QString kA = QStringLiteral("@a:one.example");
const QString kB = QStringLiteral("@b:one.example");
const QString kC = QStringLiteral("@c:two.example");

QVariant roleOf(const AccountActivityModel &m, const QString &uid, int role)
{
    for (int r = 0; r < m.rowCount(); ++r) {
        const QModelIndex idx = m.index(r, 0);
        if (m.data(idx, AccountActivityModel::UserIdRole).toString() == uid)
            return m.data(idx, role);
    }
    return {};
}

int rowOf(const AccountActivityModel &m, const QString &uid)
{
    for (int r = 0; r < m.rowCount(); ++r) {
        if (m.data(m.index(r, 0), AccountActivityModel::UserIdRole).toString() == uid)
            return r;
    }
    return -1;
}

} // namespace

class AccountActivityModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_model = std::make_unique<AccountActivityModel>();
        m_tester = std::make_unique<QAbstractItemModelTester>(
            m_model.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    }

    void cleanup()
    {
        m_tester.reset();
        m_model.reset();
    }

    // QML binds these names (WP1-Q); a rename is a silent blank badge.
    void roleNamesMatchTheContract()
    {
        const QHash<int, QByteArray> names = m_model->roleNames();
        QCOMPARE(names.value(AccountActivityModel::UserIdRole), QByteArray("userId"));
        QCOMPARE(names.value(AccountActivityModel::LiveStateRole), QByteArray("liveState"));
        QCOMPARE(names.value(AccountActivityModel::MentionCountRole), QByteArray("mentionCount"));
        QCOMPARE(names.value(AccountActivityModel::HasUnreadRole), QByteArray("hasUnread"));
        QCOMPARE(names.value(AccountActivityModel::MentionsLowerBoundRole),
                 QByteArray("mentionsLowerBound"));
        QCOMPARE(names.value(AccountActivityModel::UnreadRoomsRole), QByteArray("unreadRooms"));
        QCOMPARE(names.value(AccountActivityModel::InvitesRole), QByteArray("invites"));
        QCOMPARE(names.value(AccountActivityModel::UpdatedAtMsRole), QByteArray("updatedAtMs"));
        QCOMPARE(names.size(), 8);
        const QVariantMap a = m_model->activityFor(QStringLiteral("@nobody:x"));
        QCOMPARE(a.keys(), (QStringList{ "hasUnread", "invites", "liveState", "mentionCount",
                                         "mentionsLowerBound" }));
        QCOMPARE(a.value("liveState").toInt(), int(AccountActivityModel::NotLive));
        QCOMPARE(a.value("mentionCount").toInt(), 0);
        QCOMPARE(a.value("hasUnread").toBool(), false);
    }

    // A badge update is ONE row's dataChanged. Naive version: rebuild the
    // list with a model reset, which re-creates every delegate in the open
    // account menu on every summary.
    void aSummaryIsOneRowNeverAReset()
    {
        m_model->setLiveState(kA, AccountActivityModel::Running);
        m_model->setLiveState(kB, AccountActivityModel::Running);
        m_model->setLiveState(kC, AccountActivityModel::Running);
        QSignalSpy reset(m_model.get(), &QAbstractItemModel::modelAboutToBeReset);
        QSignalSpy layout(m_model.get(), &QAbstractItemModel::layoutAboutToBeChanged);
        QSignalSpy inserted(m_model.get(), &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(m_model.get(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);
        QSignalSpy summary(m_model.get(), &AccountActivityModel::summaryChanged);

        QVERIFY(m_model->setSummary(kB, 1, sum(3, 1)));
        QCOMPARE(changed.count(), 1);
        const QModelIndex tl = changed.at(0).at(0).value<QModelIndex>();
        const QModelIndex br = changed.at(0).at(1).value<QModelIndex>();
        QCOMPARE(tl.row(), rowOf(*m_model, kB));
        QCOMPARE(br.row(), tl.row());
        QCOMPARE(summary.count(), 1);
        QCOMPARE(summary.at(0).at(0).toString(), kB);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(layout.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);

        // The same summary again changes nothing and says nothing.
        QVERIFY(m_model->setSummary(kB, 1, sum(3, 1)));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(summary.count(), 1);
    }

    // A summary older than the row's epoch is a late result from a retired
    // host. Naive version: last writer wins, so a retiring host's final take
    // overwrites the new host's fresh counts.
    void aLateSummaryIsRefused()
    {
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kA, 5, sum(2, 1)));
        QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);
        QSignalSpy summary(m_model.get(), &AccountActivityModel::summaryChanged);
        QVERIFY(!m_model->setSummary(kA, 4, sum(9, 9, 9)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 2);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionCountRole).toInt(), 1);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(summary.count(), 0);
        // The same epoch is the same host: accepted.
        QVERIFY(m_model->setSummary(kA, 5, sum(4, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 4);
        // A newer one raises the bar.
        QVERIFY(m_model->setSummary(kA, 6, sum(1, 0)));
        QVERIFY(!m_model->setSummary(kA, 5, sum(7, 0)));
        QCOMPARE(m_model->epochOf(kA), quint64(6));
    }

    // Zero the counts, keep the row and its state, raise the epoch. Naive
    // versions: remove the row (the switcher loses the account's state), or
    // zero without raising the epoch (the retired host's next take restores
    // stale counts).
    void resetZeroesAndRaisesTheEpoch()
    {
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kA, 2, sum(5, 2, 1, true)));
        QSignalSpy summary(m_model.get(), &AccountActivityModel::summaryChanged);
        m_model->resetAccount(kA, 3);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->liveStateOf(kA), AccountActivityModel::Running);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionCountRole).toInt(), 0);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::InvitesRole).toInt(), 0);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionsLowerBoundRole).toBool(), false);
        QCOMPARE(summary.count(), 1);
        QVERIFY(!m_model->setSummary(kA, 2, sum(5, 2)));
        QVERIFY(m_model->setSummary(kA, 3, sum(1, 0)));
        // A reset with an older epoch is itself late.
        m_model->resetAccount(kA, 1);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 1);
    }

    // Removal is a tombstone (contract §6.3, DECISION 7: activity->
    // removeAccount() runs BEFORE pool->retireForRemoval(), so the retiring
    // host still reports Stopping and NotLive, and may still deliver a last
    // summary). Nothing it reports may bring the row back, at any epoch;
    // only an explicit reopen by a new host does. Naive versions: a floor
    // that only summaries check (setLiveState(Stopping) re-creates the row,
    // which then sits in the "running" section), or a removal that leaves no
    // trace when the account had no row yet (a Starting host that had not
    // reported creates one afterwards).
    void aRemovedAccountStaysRemoved()
    {
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kA, 4, sum(3, 1)));
        QSignalSpy removed(m_model.get(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(m_model.get(), &QAbstractItemModel::rowsInserted);
        QSignalSpy live(m_model.get(), &AccountActivityModel::liveStateChanged);
        m_model->removeAccount(kA);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(live.count(), 1);
        QCOMPARE(m_model->rowCount(), 0);
        QVERIFY(m_model->isRemoved(kA));

        // The retiring host walks Stopping -> NotLive and delivers a last
        // take, at its own epoch and at a later one: all refused.
        m_model->setLiveState(kA, AccountActivityModel::Stopping);
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QVERIFY(!m_model->setSummary(kA, 4, sum(3, 1)));
        QVERIFY(!m_model->setSummary(kA, 9, sum(3, 1)));
        m_model->setLiveState(kA, AccountActivityModel::NotLive);
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(live.count(), 1);
        QCOMPARE(m_model->activityFor(kA).value("liveState").toInt(),
                 int(AccountActivityModel::NotLive));
        QCOMPARE(m_model->totalsExcluding(QString()).unreadRooms, 0);
        // resetAccount() (a retire's cleanup) never lifts a removal.
        m_model->resetAccount(kA, 5);
        QVERIFY(m_model->isRemoved(kA));
        m_model->setLiveState(kA, AccountActivityModel::Stopping);
        QCOMPARE(m_model->rowCount(), 0);

        // An account removed while it had no row (a Starting host that had
        // reported nothing) is tombstoned all the same.
        QVERIFY(!m_model->isRemoved(kB));
        m_model->removeAccount(kB);
        QVERIFY(m_model->isRemoved(kB));
        QVERIFY(!m_model->setSummary(kB, 0, sum(2, 2)));
        m_model->setLiveState(kB, AccountActivityModel::Stopping);
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(inserted.count(), 0);

        // Reopen: a new host for the re-added account, at the pool's epoch.
        // Below the floor it is refused and the tombstone stays: 3 is below
        // the removed row's epoch (4), and 4 is below the 5 the post-removal
        // resetAccount() raised the floor to.
        QVERIFY(!m_model->reopenAccount(kA, 3));
        QVERIFY(m_model->isRemoved(kA));
        QVERIFY(!m_model->reopenAccount(kA, 4));
        QVERIFY(m_model->isRemoved(kA));
        QVERIFY(m_model->reopenAccount(kA, 6));
        QVERIFY(!m_model->isRemoved(kA));
        QVERIFY(m_model->isRemoved(kB)); // one account's reopen is not another's
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->epochOf(kA), quint64(6));
        // The old host's epoch is still late for the new row.
        QVERIFY(!m_model->setSummary(kA, 5, sum(3, 1)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
        QVERIFY(m_model->setSummary(kA, 6, sum(1, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 1);

        // Reopening an account that was never removed changes nothing.
        QVERIFY(m_model->reopenAccount(kC, 0));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->epochOf(kC), quint64(0));
    }

    // Review round 2 (N1): reopenAccount(uid, epochFor(uid)) must succeed on
    // the two paths where the pool's epoch did NOT move between the row's
    // last epoch and the removal. Naive version: the removal sets the floor
    // to the row's epoch + 1, so the re-added account's reopen is refused for
    // good and it never gets a row again in that process.
    void aReAddedAccountReopensAtThePoolsEpoch()
    {
        // (a) A row created by setLiveState() alone, at epoch 0, for an
        // account that never ran a host (keyring locked); removed while not
        // live, so the pool's epoch is still 0.
        m_model->setLiveState(kB, AccountActivityModel::KeyringLocked);
        QCOMPARE(m_model->epochOf(kB), quint64(0));
        m_model->removeAccount(kB);
        QVERIFY(m_model->isRemoved(kB));
        QVERIFY(m_model->reopenAccount(kB, 0));
        QVERIFY(!m_model->isRemoved(kB));
        m_model->setLiveState(kB, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kB, 0, sum(2, 1)));
        QCOMPARE(roleOf(*m_model, kB, AccountActivityModel::UnreadRoomsRole).toInt(), 2);

        // (b) The documented protocol: the host ran at 2 and retired (the
        // pool bumped to 3 and called resetAccount(uid, 3)); the account is
        // then removed while not live, which bumps nothing.
        m_model->setLiveState(kC, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kC, 2, sum(4, 0)));
        m_model->resetAccount(kC, 3);
        m_model->setLiveState(kC, AccountActivityModel::NotLive);
        m_model->removeAccount(kC);
        QVERIFY(m_model->isRemoved(kC));
        QVERIFY(!m_model->reopenAccount(kC, 2)); // below the row's epoch
        QVERIFY(m_model->isRemoved(kC));
        QVERIFY(m_model->reopenAccount(kC, 3));
        QVERIFY(!m_model->isRemoved(kC));
        m_model->setLiveState(kC, AccountActivityModel::Running);
        QCOMPARE(m_model->epochOf(kC), quint64(3));
        // The retired host's epoch is still late for the new row.
        QVERIFY(!m_model->setSummary(kC, 2, sum(4, 0)));
        QCOMPARE(roleOf(*m_model, kC, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
        QVERIFY(m_model->setSummary(kC, 3, sum(1, 0)));
        QCOMPARE(roleOf(*m_model, kC, AccountActivityModel::UnreadRoomsRole).toInt(), 1);
    }

    // DECISION §14.5: a paused, stopped or non-live account shows nothing.
    // Naive version: the roles read the stored summary whatever the state,
    // so a paused account keeps a badge from hours ago.
    void onlyARunningAccountShowsItsSummary()
    {
        QSignalSpy summary(m_model.get(), &AccountActivityModel::summaryChanged);
        QSignalSpy live(m_model.get(), &AccountActivityModel::liveStateChanged);
        QVERIFY(m_model->setSummary(kA, 1, sum(2, 3, 1, true))); // creates a NotLive row
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(summary.count(), 0); // nothing visible changed
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionCountRole).toInt(), 0);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::HasUnreadRole).toBool(), false);
        QCOMPARE(m_model->activityFor(kA).value("mentionCount").toInt(), 0);
        QCOMPARE(m_model->totalsExcluding(QString()).mentions, 0);

        m_model->setLiveState(kA, AccountActivityModel::Running);
        QCOMPARE(live.count(), 1);
        QCOMPARE(summary.count(), 1);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionCountRole).toInt(), 3);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::HasUnreadRole).toBool(), true);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionsLowerBoundRole).toBool(), true);
        QCOMPARE(m_model->activityFor(kA).value("mentionCount").toInt(), 3);
        QCOMPARE(m_model->activityFor(kA).value("liveState").toInt(),
                 int(AccountActivityModel::Running));
        QCOMPARE(m_model->totalsExcluding(QString()).mentions, 3);

        for (AccountActivityModel::LiveState s :
             { AccountActivityModel::Paused, AccountActivityModel::Stopped,
               AccountActivityModel::NeedsSignIn, AccountActivityModel::Starting,
               AccountActivityModel::Stopping, AccountActivityModel::Active }) {
            m_model->setLiveState(kA, s);
            QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionCountRole).toInt(), 0);
            QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
            QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::InvitesRole).toInt(), 0);
            QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::HasUnreadRole).toBool(), false);
            QCOMPARE(m_model->activityFor(kA).value("mentionCount").toInt(), 0);
            QCOMPARE(m_model->totalsExcluding(QString()).accountsWithActivity, 0);
            m_model->setLiveState(kA, AccountActivityModel::Running);
        }
        // Every hide and every show was announced: QML reads activityFor()
        // only from these handlers.
        QCOMPARE(summary.count(), 1 + 2 * 6);
    }

    // hasUnread is an unread room or an invite; a mention alone is a number.
    void hasUnreadIsAnUnreadRoomOrAnInvite()
    {
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kA, 0, sum(0, 0, 1)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::HasUnreadRole).toBool(), true);
        QVERIFY(m_model->setSummary(kA, 0, sum(1, 0, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::HasUnreadRole).toBool(), true);
        QVERIFY(m_model->setSummary(kA, 0, sum(0, 0, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::HasUnreadRole).toBool(), false);
        QVERIFY(m_model->setSummary(kA, 0, sum(-3, -1, -2)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::MentionCountRole).toInt(), 0);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::InvitesRole).toInt(), 0);
    }

    // The rail dot counts OTHER accounts; the foreground has its own badges.
    // Naive version: sum every row, so the foreground's own unread lights
    // "new messages on other accounts".
    void totalsLeaveTheActiveAccountOut()
    {
        for (const QString &uid : { kA, kB, kC })
            m_model->setLiveState(uid, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kA, 0, sum(2, 1)));
        QVERIFY(m_model->setSummary(kB, 0, sum(0, 0, 1)));
        QSignalSpy totals(m_model.get(), &AccountActivityModel::totalsChanged);
        m_model->setActiveUserId(kA);
        QCOMPARE(totals.count(), 1);
        QCOMPARE(m_model->otherAccountsWithActivity(), 1);
        AccountActivityModel::Totals t = m_model->totalsExcluding(kA);
        QCOMPARE(t.unreadRooms, 0);
        QCOMPARE(t.mentions, 0);
        QCOMPARE(t.accountsWithActivity, 1);
        t = m_model->totalsExcluding(QString());
        QCOMPARE(t.unreadRooms, 2);
        QCOMPARE(t.mentions, 1);
        QCOMPARE(t.accountsWithActivity, 2);

        // A change on the active account moves no total.
        QVERIFY(m_model->setSummary(kA, 0, sum(7, 4)));
        QCOMPARE(totals.count(), 1);
        // A change elsewhere does.
        QVERIFY(m_model->setSummary(kC, 0, sum(0, 2)));
        QCOMPARE(totals.count(), 2);
        QCOMPARE(m_model->otherAccountsWithActivity(), 2);
        m_model->setActiveUserId(kC);
        QCOMPARE(m_model->otherAccountsWithActivity(), 2); // A and B
        QCOMPARE(m_model->totalsExcluding(kC).mentions, 4);
    }

    // updatedAtMs moves with a real change only.
    void updatedAtFollowsTheClock()
    {
        qint64 clock = 1000;
        m_model->setClockForTest([&clock] { return clock; });
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kA, 0, sum(1, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UpdatedAtMsRole).toLongLong(), qint64(1000));
        clock = 2000;
        QVERIFY(m_model->setSummary(kA, 0, sum(1, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UpdatedAtMsRole).toLongLong(), qint64(1000));
        QVERIFY(m_model->setSummary(kA, 0, sum(2, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UpdatedAtMsRole).toLongLong(), qint64(2000));
        m_model->setClockForTest(nullptr);
    }

private:
    std::unique_ptr<AccountActivityModel> m_model;
    std::unique_ptr<QAbstractItemModelTester> m_tester;
};

QTEST_GUILESS_MAIN(AccountActivityModelTest)
#include "AccountActivityModelTest.moc"
