
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

}

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

        QVERIFY(m_model->setSummary(kB, 1, sum(3, 1)));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(summary.count(), 1);
    }

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
        QVERIFY(m_model->setSummary(kA, 5, sum(4, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 4);
        QVERIFY(m_model->setSummary(kA, 6, sum(1, 0)));
        QVERIFY(!m_model->setSummary(kA, 5, sum(7, 0)));
        QCOMPARE(m_model->epochOf(kA), quint64(6));
    }

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
        m_model->resetAccount(kA, 1);
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 1);
    }

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
        m_model->resetAccount(kA, 5);
        QVERIFY(m_model->isRemoved(kA));
        m_model->setLiveState(kA, AccountActivityModel::Stopping);
        QCOMPARE(m_model->rowCount(), 0);

        QVERIFY(!m_model->isRemoved(kB));
        m_model->removeAccount(kB);
        QVERIFY(m_model->isRemoved(kB));
        QVERIFY(!m_model->setSummary(kB, 0, sum(2, 2)));
        m_model->setLiveState(kB, AccountActivityModel::Stopping);
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(inserted.count(), 0);

        QVERIFY(!m_model->reopenAccount(kA, 3));
        QVERIFY(m_model->isRemoved(kA));
        QVERIFY(!m_model->reopenAccount(kA, 4));
        QVERIFY(m_model->isRemoved(kA));
        QVERIFY(m_model->reopenAccount(kA, 6));
        QVERIFY(!m_model->isRemoved(kA));
        QVERIFY(m_model->isRemoved(kB));
        m_model->setLiveState(kA, AccountActivityModel::Running);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->epochOf(kA), quint64(6));
        QVERIFY(!m_model->setSummary(kA, 5, sum(3, 1)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
        QVERIFY(m_model->setSummary(kA, 6, sum(1, 0)));
        QCOMPARE(roleOf(*m_model, kA, AccountActivityModel::UnreadRoomsRole).toInt(), 1);

        QVERIFY(m_model->reopenAccount(kC, 0));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->epochOf(kC), quint64(0));
    }

    void aReAddedAccountReopensAtThePoolsEpoch()
    {
        m_model->setLiveState(kB, AccountActivityModel::KeyringLocked);
        QCOMPARE(m_model->epochOf(kB), quint64(0));
        m_model->removeAccount(kB);
        QVERIFY(m_model->isRemoved(kB));
        QVERIFY(m_model->reopenAccount(kB, 0));
        QVERIFY(!m_model->isRemoved(kB));
        m_model->setLiveState(kB, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kB, 0, sum(2, 1)));
        QCOMPARE(roleOf(*m_model, kB, AccountActivityModel::UnreadRoomsRole).toInt(), 2);

        m_model->setLiveState(kC, AccountActivityModel::Running);
        QVERIFY(m_model->setSummary(kC, 2, sum(4, 0)));
        m_model->resetAccount(kC, 3);
        m_model->setLiveState(kC, AccountActivityModel::NotLive);
        m_model->removeAccount(kC);
        QVERIFY(m_model->isRemoved(kC));
        QVERIFY(!m_model->reopenAccount(kC, 2));
        QVERIFY(m_model->isRemoved(kC));
        QVERIFY(m_model->reopenAccount(kC, 3));
        QVERIFY(!m_model->isRemoved(kC));
        m_model->setLiveState(kC, AccountActivityModel::Running);
        QCOMPARE(m_model->epochOf(kC), quint64(3));
        QVERIFY(!m_model->setSummary(kC, 2, sum(4, 0)));
        QCOMPARE(roleOf(*m_model, kC, AccountActivityModel::UnreadRoomsRole).toInt(), 0);
        QVERIFY(m_model->setSummary(kC, 3, sum(1, 0)));
        QCOMPARE(roleOf(*m_model, kC, AccountActivityModel::UnreadRoomsRole).toInt(), 1);
    }

    void onlyARunningAccountShowsItsSummary()
    {
        QSignalSpy summary(m_model.get(), &AccountActivityModel::summaryChanged);
        QSignalSpy live(m_model.get(), &AccountActivityModel::liveStateChanged);
        QVERIFY(m_model->setSummary(kA, 1, sum(2, 3, 1, true)));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(summary.count(), 0);
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
        QCOMPARE(summary.count(), 1 + 2 * 6);
    }

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

        QVERIFY(m_model->setSummary(kA, 0, sum(7, 4)));
        QCOMPARE(totals.count(), 1);
        QVERIFY(m_model->setSummary(kC, 0, sum(0, 2)));
        QCOMPARE(totals.count(), 2);
        QCOMPARE(m_model->otherAccountsWithActivity(), 2);
        m_model->setActiveUserId(kC);
        QCOMPARE(m_model->otherAccountsWithActivity(), 2);
        QCOMPARE(m_model->totalsExcluding(kC).mentions, 4);
    }

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
