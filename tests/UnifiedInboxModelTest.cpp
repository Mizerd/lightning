#include "models/AccountActivityModel.h"
#include "models/UnifiedInboxModel.h"

#include <QHash>
#include <QPointer>
#include <QSignalSpy>
#include <QtTest>

namespace {

const QString kAlice = QStringLiteral("@alice:hs1.example");
const QString kBob = QStringLiteral("@bob:hs2.example");
const QString kCarol = QStringLiteral("@carol:hs3.example");

UnifiedInboxModel::Entry entry(const QString &uid, const QString &eventId, qint64 ts,
                               const QString &roomId = QStringLiteral("!room:hs.example"))
{
    UnifiedInboxModel::Entry e;
    e.accountUserId = uid;
    e.accountLabel = uid.mid(1, uid.indexOf(QLatin1Char(':')) - 1);
    e.roomId = roomId;
    e.roomName = QStringLiteral("Room");
    e.senderId = QStringLiteral("@dave:hs.example");
    e.senderName = QStringLiteral("Dave");
    e.preview = QStringLiteral("hello ") + eventId;
    e.eventId = eventId;
    e.timestampMs = ts;
    return e;
}

QStringList column(const UnifiedInboxModel &m, int role)
{
    QStringList out;
    for (int i = 0; i < m.rowCount(); ++i)
        out << m.data(m.index(i, 0), role).toString();
    return out;
}

}

class UnifiedInboxModelTest : public QObject
{
    Q_OBJECT

private:
    // Two live background accounts (alice, bob); carol is the foreground one.
    struct Fixture {
        AccountActivityModel activity;
        UnifiedInboxModel *inbox = new UnifiedInboxModel(&activity);
        QHash<QString, quint64> epochs{ { kAlice, 1 }, { kBob, 1 }, { kCarol, 1 } };
        Fixture()
        {
            activity.setActiveUserId(kCarol);
            activity.setLiveState(kCarol, AccountActivityModel::Active);
            activity.setLiveState(kAlice, AccountActivityModel::Running);
            activity.setLiveState(kBob, AccountActivityModel::Running);
            inbox->setEpochSource([this](const QString &uid) { return epochs.value(uid); });
        }
    };

private Q_SLOTS:
    void rowsFromTwoAccountsInterleaveNewestFirstWithTheirLabels()
    {
        Fixture f;
        QSignalSpy count(f.inbox, &UnifiedInboxModel::countChanged);
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$a1"), 100)));
        QVERIFY(f.inbox->add(1, entry(kBob, QStringLiteral("$b1"), 300)));
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$a2"), 200)));
        QCOMPARE(f.inbox->count(), 3);
        QCOMPARE(count.size(), 3);
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::EventIdRole),
                 (QStringList{ "$b1", "$a2", "$a1" }));
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::AccountLabelRole),
                 (QStringList{ "bob", "alice", "alice" }));
        const QVariantMap first = f.inbox->entryAt(0);
        QCOMPARE(first.value(QStringLiteral("accountUserId")).toString(), kBob);
        QCOMPARE(first.value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!room:hs.example"));
        QVERIFY(f.inbox->entryAt(9).isEmpty());
    }

    void onlyARunningBackgroundAccountOfTheCurrentEpochMayAdd()
    {
        Fixture f;
        QVERIFY2(!f.inbox->add(1, entry(kCarol, QStringLiteral("$c"), 1)),
                 "the foreground account has its own Activity list");
        f.activity.setLiveState(kBob, AccountActivityModel::Paused);
        QVERIFY(!f.inbox->add(1, entry(kBob, QStringLiteral("$b"), 1)));
        QVERIFY(!f.inbox->add(1, entry(QStringLiteral("@nobody:hs.example"),
                                       QStringLiteral("$n"), 1)));
        f.epochs.insert(kAlice, 2);
        QVERIFY2(!f.inbox->add(1, entry(kAlice, QStringLiteral("$late"), 1)),
                 "a retired host's late event must not land");
        QVERIFY(f.inbox->add(2, entry(kAlice, QStringLiteral("$now"), 1)));
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::EventIdRole), QStringList{ "$now" });
    }

    void leavingRunningDropsThatAccountsRowsOnly()
    {
        Fixture f;
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$a"), 1)));
        QVERIFY(f.inbox->add(1, entry(kBob, QStringLiteral("$b"), 2)));
        // Switching to alice: she becomes the foreground account.
        f.activity.setLiveState(kAlice, AccountActivityModel::Active);
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::EventIdRole), QStringList{ "$b" });
        f.activity.setLiveState(kBob, AccountActivityModel::Stopped);
        QCOMPARE(f.inbox->count(), 0);
    }

    void aRemovedAccountIsDroppedAndStaysOut()
    {
        Fixture f;
        QVERIFY(f.inbox->add(1, entry(kBob, QStringLiteral("$b"), 1)));
        f.activity.removeAccount(kBob);
        QCOMPARE(f.inbox->count(), 0);
        f.activity.setLiveState(kBob, AccountActivityModel::Running);
        QVERIFY(!f.inbox->add(1, entry(kBob, QStringLiteral("$b2"), 2)));
        QCOMPARE(f.inbox->count(), 0);
    }

    void aSecondCopyReplacesTheFirstInPlace()
    {
        Fixture f;
        UnifiedInboxModel::Entry utd = entry(kAlice, QStringLiteral("$e"), 5);
        utd.undecryptable = true;
        utd.preview.clear();
        QVERIFY(f.inbox->add(1, utd));
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$x"), 9)));
        QSignalSpy changed(f.inbox, &QAbstractItemModel::dataChanged);
        QSignalSpy inserted(f.inbox, &QAbstractItemModel::rowsInserted);
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$e"), 5)));
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(f.inbox->count(), 2);
        QCOMPARE(f.inbox->data(f.inbox->index(1, 0), UnifiedInboxModel::UndecryptableRole)
                     .toBool(), false);
        QCOMPARE(f.inbox->data(f.inbox->index(1, 0), UnifiedInboxModel::PreviewRole)
                     .toString(), QStringLiteral("hello $e"));
    }

    void readAndRedactedRowsGoPerAccount()
    {
        Fixture f;
        const QString shared = QStringLiteral("!shared:hs.example");
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$a1"), 1, shared)));
        QVERIFY(f.inbox->add(1, entry(kAlice, QStringLiteral("$a2"), 2, shared)));
        QVERIFY(f.inbox->add(1, entry(kBob, QStringLiteral("$b1"), 3, shared)));
        QVERIFY(f.inbox->add(1, entry(kBob, QStringLiteral("$a1"), 4)));
        f.inbox->dropEvent(kAlice, QStringLiteral("$a1"));
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::EventIdRole),
                 (QStringList{ "$a1", "$b1", "$a2" }));
        f.inbox->dropRoom(kAlice, shared);
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::EventIdRole),
                 (QStringList{ "$a1", "$b1" }));
        QCOMPARE(column(*f.inbox, UnifiedInboxModel::AccountUserIdRole),
                 (QStringList{ kBob, kBob }));
    }

    void theListIsBoundedAndKeepsTheNewest()
    {
        Fixture f;
        for (int i = 0; i < UnifiedInboxModel::kMaxEntries + 20; ++i)
            f.inbox->add(1, entry(kAlice, QStringLiteral("$e%1").arg(i), 1000 + i));
        QCOMPARE(f.inbox->count(), UnifiedInboxModel::kMaxEntries);
        QCOMPARE(f.inbox->entryAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$e%1").arg(UnifiedInboxModel::kMaxEntries + 19));
        QVERIFY2(!f.inbox->add(1, entry(kAlice, QStringLiteral("$old"), 1)),
                 "an entry older than a full list is not kept");
        QCOMPARE(f.inbox->count(), UnifiedInboxModel::kMaxEntries);
    }

    void itDiesWithTheActivityModel()
    {
        auto *activity = new AccountActivityModel;
        QPointer<UnifiedInboxModel> inbox = new UnifiedInboxModel(activity);
        delete activity;
        QVERIFY(inbox.isNull());
    }
};

QTEST_GUILESS_MAIN(UnifiedInboxModelTest)
#include "UnifiedInboxModelTest.moc"
