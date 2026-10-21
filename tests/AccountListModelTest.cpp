// Labs (concurrent live accounts): AccountListModel, the switcher rows for up
// to a couple of hundred saved accounts. No secret is ever read to build them
// (AccountManager::accounts() reads every account's token); sections and
// order; the filter; needsSignIn from the record, the proof and the live
// state; reaction to the signals that change a row and to none that only
// change a badge. Also AccountManager's sessionRevokedChanged, and flag-off
// parity of today's AccountManager rows. Every case names the naive version
// it fails on.

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "models/AccountActivityModel.h"
#include "models/AccountListModel.h"
#include "storage/SecretStore.h"

#include <QAbstractItemModelTester>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

// Counts every read, so a case can prove the rows read no secret.
class CountingSecretStore final : public SecretStore
{
    Q_OBJECT

public:
    explicit CountingSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    mutable int reads = 0;

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key, const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        ++reads;
        return m_values.value(userId + QLatin1Char('/') + key);
    }
    QString readLegacySecret(const QString &, const QString &) const override
    {
        ++reads;
        return {};
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

namespace {

// Saved in this order within one second, so savedAccountUserIds() orders them
// by id: a < b < c < d < e.
const QString kA = QStringLiteral("@a-user:one.example");
const QString kB = QStringLiteral("@b-user:one.example");
const QString kC = QStringLiteral("@c-user:one.example");
const QString kD = QStringLiteral("@d-user:two.example");
const QString kE = QStringLiteral("@e-user:two.example");

QStringList idsOf(const QVariantList &rows)
{
    QStringList out;
    for (const QVariant &r : rows)
        out.append(r.toMap().value(QStringLiteral("userId")).toString());
    return out;
}

QVariantMap rowFor(const QVariantList &rows, const QString &uid)
{
    for (const QVariant &r : rows) {
        if (r.toMap().value(QStringLiteral("userId")).toString() == uid)
            return r.toMap();
    }
    return {};
}

} // namespace

class AccountListModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("account-list-model-test"));
    }

    void init()
    {
        QSettings raw;
        raw.clear();
        raw.sync();
        m_store = std::make_unique<CountingSecretStore>();
        m_settings = std::make_unique<SettingsManager>();
        m_settings->setSecretStore(m_store.get());
        m_accounts = std::make_unique<AccountManager>(m_settings.get());
        m_activity = std::make_unique<AccountActivityModel>();
        m_proofs.clear();
    }

    void cleanup()
    {
        m_tester.reset();
        m_model.reset();
        m_activity.reset();
        m_accounts.reset();
        m_settings.reset();
        m_store.reset();
    }

    // QML binds these names (WP1-Q).
    void roleNamesMatchTheContract()
    {
        makeModel();
        const QHash<int, QByteArray> n = m_model->roleNames();
        const QList<QByteArray> expected = { "userId", "displayName", "avatarUrl", "homeserver",
                                             "isActive", "needsSignIn", "sessionRevoked",
                                             "liveState", "keepRunning", "section", "lastLiveAt" };
        QCOMPARE(n.size(), expected.size());
        for (int i = 0; i < expected.size(); ++i)
            QCOMPARE(n.value(AccountListModel::UserIdRole + i), expected.at(i));
    }

    // The switcher at about 200 accounts: building, filtering and refreshing
    // the rows reads ZERO secrets, where today's accounts() reads one token
    // per account. Naive version: needsSignIn = AccountManager::needsSignIn(),
    // or rows copied from AccountManager::accounts().
    void twoHundredRowsReadNoSecret()
    {
        for (int i = 0; i < 200; ++i) {
            m_settings->saveSession(QStringLiteral("https://one.example"),
                                    QStringLiteral("@u%1:one.example").arg(i, 3, 10, QLatin1Char('0')),
                                    QStringLiteral("DEV"), QStringLiteral("token-fixture"));
        }
        const QString u5 = QStringLiteral("@u005:one.example");
        const QString u7 = QStringLiteral("@u007:one.example");
        const QString u9 = QStringLiteral("@u009:one.example");
        m_store->reads = 0;

        makeModel();
        QCOMPARE(m_model->rowCount(), 200);
        QCOMPARE(m_model->rowsSnapshot().size(), 200);
        m_model->setFilter(QStringLiteral("u01"));
        QCOMPARE(m_model->rowCount(), 10);
        m_model->setFilter(QString());
        m_activity->setLiveState(u5, AccountActivityModel::Running);
        m_accounts->markSessionRevoked(u7);
        m_settings->setKeepRunningFor(QStringLiteral("@u003:one.example"), true);
        m_settings->setActiveAccountUserId(u9);
        m_settings->updateAccountProfile(u9, QStringLiteral("Nine"), QString());
        for (int r = 0; r < m_model->rowCount(); ++r)
            m_model->data(m_model->index(r, 0), AccountListModel::NeedsSignInRole);
        m_model->rowsSnapshot();
        QCOMPARE(m_store->reads, 0);
        QCOMPARE(rowFor(m_model->rowsSnapshot(), u7).value("needsSignIn").toBool(), true);

        // The counter does see a token read: today's list reads every one.
        m_accounts->accounts();
        QVERIFY(m_store->reads >= 200);
    }

    // Running first (the active account at its top, then Starting, Running
    // and Stopping in saved order), then the others most recently live first.
    // Naive versions: saved order throughout (today's menu), lastLiveAt
    // ascending, or the running section sorted by lastLiveAt (rows jump while
    // the menu is open, because a host rewrites it every ten minutes).
    void sectionsAndOrder()
    {
        saveFive();
        m_settings->setActiveAccountUserId(kC);
        m_settings->setLastLiveAtFor(kA, 100);
        m_settings->setLastLiveAtFor(kB, 300);
        m_settings->setLastLiveAtFor(kD, 200);
        m_settings->setLastLiveAtFor(kE, 900);
        m_activity->setLiveState(kE, AccountActivityModel::Running);
        m_activity->setLiveState(kB, AccountActivityModel::Starting);
        makeModel();
        const QVariantList rows = m_model->rowsSnapshot();
        QCOMPARE(idsOf(rows), (QStringList{ kC, kB, kE, kD, kA }));
        QCOMPARE(rowFor(rows, kC).value("section").toString(), QStringLiteral("running"));
        QCOMPARE(rowFor(rows, kB).value("section").toString(), QStringLiteral("running"));
        QCOMPARE(rowFor(rows, kE).value("section").toString(), QStringLiteral("running"));
        QCOMPARE(rowFor(rows, kD).value("section").toString(), QStringLiteral("other"));
        QCOMPARE(rowFor(rows, kA).value("section").toString(), QStringLiteral("other"));
        QCOMPARE(rowFor(rows, kC).value("isActive").toBool(), true);
        QCOMPARE(rowFor(rows, kD).value("lastLiveAt").toLongLong(), qint64(200));
        // The model's own rows say the same.
        for (int r = 0; r < rows.size(); ++r)
            QCOMPARE(m_model->data(m_model->index(r, 0), AccountListModel::UserIdRole).toString(),
                     idsOf(rows).at(r));

        // A switch moves the new active account to the top (sessionChanged).
        QSignalSpy rowsChanged(m_model.get(), &AccountListModel::rowsChanged);
        m_settings->setActiveAccountUserId(kD);
        QVERIFY(rowsChanged.count() >= 1);
        QCOMPARE(idsOf(m_model->rowsSnapshot()).first(), kD);
        // Stopping still holds the store: it stays in the running section.
        m_activity->setLiveState(kA, AccountActivityModel::Stopping);
        QCOMPARE(rowFor(m_model->rowsSnapshot(), kA).value("section").toString(),
                 QStringLiteral("running"));
        m_activity->setLiveState(kA, AccountActivityModel::Paused);
        QCOMPARE(rowFor(m_model->rowsSnapshot(), kA).value("section").toString(),
                 QStringLiteral("other"));
    }

    // Case-insensitive substring on the display name and the user id. Naive
    // versions: case-sensitive, or the id only.
    void filterMatchesNameAndIdIgnoringCase()
    {
        saveFive();
        m_settings->updateAccountProfile(kA, QStringLiteral("Zebra Crossing"), QString());
        makeModel();
        QSignalSpy filterChanged(m_model.get(), &AccountListModel::filterChanged);
        QSignalSpy rowsChanged(m_model.get(), &AccountListModel::rowsChanged);
        m_model->setFilter(QStringLiteral("zEBRA"));
        QCOMPARE(filterChanged.count(), 1);
        QCOMPARE(rowsChanged.count(), 1);
        QCOMPARE(idsOf(m_model->rowsSnapshot()), (QStringList{ kA }));
        QCOMPARE(m_model->property("count").toInt(), 1);
        m_model->setFilter(QStringLiteral("B-USER"));
        QCOMPARE(idsOf(m_model->rowsSnapshot()), (QStringList{ kB }));
        m_model->setFilter(QStringLiteral("TWO.example"));
        QCOMPARE(idsOf(m_model->rowsSnapshot()), (QStringList{ kD, kE }));
        m_model->setFilter(QString());
        QCOMPARE(m_model->rowCount(), 5);
        QCOMPARE(m_model->property("count").toInt(), 5);
        m_model->setFilter(QString());
        QCOMPARE(filterChanged.count(), 4);
    }

    // needsSignIn = revoked this run || a revocation proof || the host said
    // so. Naive version: today's token check, or the revoked flag alone (a
    // proof from a host would then not show until the user clicked).
    void needsSignInFromFlagProofAndState()
    {
        saveFive();
        m_proofs.insert(kB);
        m_accounts->markSessionRevoked(kA);
        m_activity->setLiveState(kC, AccountActivityModel::NeedsSignIn);
        makeModel();
        const QVariantList rows = m_model->rowsSnapshot();
        QCOMPARE(rowFor(rows, kA).value("needsSignIn").toBool(), true);
        QCOMPARE(rowFor(rows, kA).value("sessionRevoked").toBool(), true);
        QCOMPARE(rowFor(rows, kB).value("needsSignIn").toBool(), true);
        QCOMPARE(rowFor(rows, kB).value("sessionRevoked").toBool(), false);
        QCOMPARE(rowFor(rows, kC).value("needsSignIn").toBool(), true);
        QCOMPARE(rowFor(rows, kD).value("needsSignIn").toBool(), false);
        QCOMPARE(rowFor(rows, kC).value("liveState").toInt(), int(AccountActivityModel::NeedsSignIn));
    }

    // The rows follow the four signals that change one. keepRunning in
    // particular emits NO accountsChanged (by design), so a model listening
    // only to accountsChanged would show a stale switch. Naive version: the
    // AccountManager wiring (accounts()/accountsChanged) alone.
    void rowsFollowPinsStatesAndRevocation()
    {
        saveFive();
        m_settings->setActiveAccountUserId(kA);
        makeModel();
        QSignalSpy rowsChanged(m_model.get(), &AccountListModel::rowsChanged);
        QSignalSpy reset(m_model.get(), &QAbstractItemModel::modelAboutToBeReset);
        QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);

        m_settings->setKeepRunningFor(kD, true);
        QCOMPARE(rowsChanged.count(), 1);
        QCOMPARE(reset.count(), 0);    // same order: one row changed in place
        QCOMPARE(changed.count(), 1);
        QCOMPARE(rowFor(m_model->rowsSnapshot(), kD).value("keepRunning").toBool(), true);

        m_accounts->markSessionRevoked(kE);
        QCOMPARE(rowsChanged.count(), 2);
        QCOMPARE(rowFor(m_model->rowsSnapshot(), kE).value("needsSignIn").toBool(), true);

        m_activity->setLiveState(kD, AccountActivityModel::Running);
        QCOMPARE(rowsChanged.count(), 3);
        QCOMPARE(idsOf(m_model->rowsSnapshot()).mid(0, 2), (QStringList{ kA, kD }));

        m_settings->updateAccountProfile(kB, QStringLiteral("Bee"), QString());
        QCOMPARE(rowsChanged.count(), 4);
        QCOMPARE(rowFor(m_model->rowsSnapshot(), kB).value("displayName").toString(),
                 QStringLiteral("Bee"));

        // A change that moves nothing says nothing.
        m_settings->setKeepRunningFor(kD, true);
        m_activity->setLiveState(kD, AccountActivityModel::Running);
        QCOMPARE(rowsChanged.count(), 4);
    }

    // rowsSnapshot() changes only with rowsChanged(), never with a badge.
    // Naive version: a row carrying the badge (mentionCount from the activity
    // model) refreshed on summaryChanged: QML re-reads the whole snapshot,
    // and every delegate in the open menu, on every unread change of every
    // background account.
    void aBadgeUpdateLeavesTheRowsAlone()
    {
        saveFive();
        m_activity->setLiveState(kB, AccountActivityModel::Running);
        makeModel();
        const QVariantList before = m_model->rowsSnapshot();
        QSignalSpy rowsChanged(m_model.get(), &AccountListModel::rowsChanged);
        QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);
        QSignalSpy reset(m_model.get(), &QAbstractItemModel::modelAboutToBeReset);
        QVERIFY(m_activity->setSummary(kB, 0, AccountActivityModel::Summary{ 5, 2, 1, true }));
        QVERIFY(m_activity->setSummary(kB, 0, AccountActivityModel::Summary{ 6, 3, 0, false }));
        m_activity->setActiveUserId(kB);
        QCOMPARE(rowsChanged.count(), 0);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(m_model->rowsSnapshot(), before);
    }

    // Today's accounts() keys, same values (tokens present, no proof), plus
    // the Labs roles. Naive version: different spellings, which would make
    // every QML consumer that switches sources read blanks.
    void theSnapshotIsTodaysRowsPlusTheLabsRoles()
    {
        saveFive();
        m_settings->setActiveAccountUserId(kB);
        m_settings->updateAccountProfile(kB, QStringLiteral("Bee"), QStringLiteral("mxc://one.example/x"));
        makeModel();
        const QVariantList today = m_accounts->accounts();
        const QVariantList labs = m_model->rowsSnapshot();
        QCOMPARE(labs.size(), today.size());
        for (const QVariant &t : today) {
            const QVariantMap todayRow = t.toMap();
            const QVariantMap labsRow = rowFor(labs, todayRow.value("userId").toString());
            QSet<QString> expectedKeys(todayRow.keyBegin(), todayRow.keyEnd());
            expectedKeys.unite(QSet<QString>{ "liveState", "keepRunning", "section", "lastLiveAt" });
            QCOMPARE(QSet<QString>(labsRow.keyBegin(), labsRow.keyEnd()), expectedKeys);
            for (auto it = todayRow.constBegin(); it != todayRow.constEnd(); ++it)
                QVERIFY2(labsRow.value(it.key()) == it.value(), qPrintable(it.key()));
        }
    }

    // ---- AccountManager ----------------------------------------------------------

    // Emitted beside accountsChanged, after it, on a real change only. Naive
    // version: no signal (the Labs list then never shows a revocation), or
    // one on every call.
    void sessionRevokedChangedFollowsAccountsChanged()
    {
        saveFive();
        QStringList order;
        connect(m_accounts.get(), &AccountManager::accountsChanged, this,
                [&order] { order.append(QStringLiteral("accounts")); });
        connect(m_accounts.get(), &AccountManager::sessionRevokedChanged, this,
                [&order](const QString &uid) { order.append(QStringLiteral("revoked ") + uid); });
        m_accounts->markSessionRevoked(kA);
        m_accounts->markSessionRevoked(kA);
        m_accounts->clearSessionRevoked(kA);
        m_accounts->clearSessionRevoked(kA);
        m_accounts->markSessionRevoked(QString());
        QCOMPARE(order, (QStringList{ "accounts", QStringLiteral("revoked ") + kA, "accounts",
                                      QStringLiteral("revoked ") + kA }));
    }

    // Flag-off parity for today's switcher source: Labs keys stored for an
    // account change nothing in accounts() or account(). Naive version:
    // accountRecord() growing the new keys, which every QML row would then
    // carry and every comparison of records would see move.
    void todaysAccountRowsIgnoreTheLabsKeys()
    {
        saveFive();
        m_settings->setActiveAccountUserId(kA);
        const QVariantList before = m_accounts->accounts();
        const QVariantMap oneBefore = m_accounts->account(kB);
        QSignalSpy accountsChanged(m_accounts.get(), &AccountManager::accountsChanged);
        m_settings->setLabsConcurrentAccounts(true);
        m_settings->setKeepRunningFor(kB, true);
        m_settings->setLastLiveAtFor(kB, 1760000000000);
        QCOMPARE(accountsChanged.count(), 0);
        QCOMPARE(m_accounts->accounts(), before);
        QCOMPARE(m_accounts->account(kB), oneBefore);
        QCOMPARE(QSet<QString>(before.first().toMap().keyBegin(), before.first().toMap().keyEnd()),
                 (QSet<QString>{ "userId", "homeserver", "displayName", "avatarUrl", "isActive",
                                 "needsSignIn", "sessionRevoked" }));
    }

private:
    // la1-M L2: an account that is not revoked but whose keyring lost its
    // token showed no "Needs sign-in" in the Labs switcher (today's menu reads
    // every token on the GUI thread to say so). The probe answers off the
    // GUI thread: lost -> the chip; "cannot tell" -> nothing; the active and
    // running rows are never asked; answers are forgotten when a record
    // changes. Still no secret read while building rows. Mutation: drop the
    // m_lostSignIn term from needsSignIn and kB never shows the chip.
    void aProbedLostSignInShowsNeedsSignIn()
    {
        saveFive();
        m_settings->setActiveAccountUserId(kA);
        m_activity->setLiveState(kC, AccountActivityModel::Running);
        makeModel();
        m_store->reads = 0;
        QStringList asked;
        QHash<QString, std::function<void(bool)>> pending;
        m_model->setSignInProbe([&](const QString &uid, std::function<void(bool)> done) {
            asked << uid;
            pending.insert(uid, std::move(done));
        });
        // At most four at a time, never the active or a running account.
        QCOMPARE(asked.size(), 3);   // kB, kD, kE (kA active, kC running)
        QVERIFY(!asked.contains(kA));
        QVERIFY(!asked.contains(kC));
        QCOMPARE(m_store->reads, 0);
        QVERIFY(!rowFor(m_model->rowsSnapshot(), kB).value("needsSignIn").toBool());
        QSignalSpy changed(m_model.get(), &AccountListModel::rowsChanged);
        pending.take(kB)(true);    // the keyring answered: nothing there
        pending.take(kD)(false);   // "cannot tell", or a token is there
        QVERIFY(changed.count() >= 1);
        QVERIFY(rowFor(m_model->rowsSnapshot(), kB).value("needsSignIn").toBool());
        QVERIFY(!rowFor(m_model->rowsSnapshot(), kD).value("needsSignIn").toBool());
        // Each account is asked once while nothing changes.
        m_model->refresh();
        QCOMPARE(asked.count(kB), 1);
        QCOMPARE(asked.count(kD), 1);
        QCOMPARE(m_store->reads, 0);
        // A late answer from before a record changed is ignored, and every
        // account is asked again.
        std::function<void(bool)> stale = pending.take(kE);
        m_settings->updateAccountProfile(kE, QStringLiteral("E"), QString());
        m_settings->saveSession(QStringLiteral("https://one.example"), kB, QStringLiteral("DEV"),
                                QStringLiteral("token-fixture"));
        m_settings->setActiveAccountUserId(kA);
        stale(true);
        QVERIFY(!rowFor(m_model->rowsSnapshot(), kE).value("needsSignIn").toBool());
        QVERIFY(!rowFor(m_model->rowsSnapshot(), kB).value("needsSignIn").toBool());
        QVERIFY(asked.count(kB) >= 2);
    }

    void saveFive()
    {
        for (const QString &uid : { kA, kB, kC, kD, kE }) {
            const QString hs = uid.endsWith(QLatin1String("two.example"))
                ? QStringLiteral("https://two.example")
                : QStringLiteral("https://one.example");
            m_settings->saveSession(hs, uid, QStringLiteral("DEV"), QStringLiteral("token-fixture"));
        }
        // saveSession makes each one active; start from none.
        m_settings->setActiveAccountUserId(QString());
    }

    void makeModel()
    {
        QSet<QString> *proofs = &m_proofs;
        m_model = std::make_unique<AccountListModel>(
            m_settings.get(), m_accounts.get(), m_activity.get(),
            [proofs](const QString &uid) { return proofs->contains(uid); });
        m_tester = std::make_unique<QAbstractItemModelTester>(
            m_model.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    }

    QTemporaryDir m_configHome;
    std::unique_ptr<CountingSecretStore> m_store;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AccountActivityModel> m_activity;
    std::unique_ptr<AccountListModel> m_model;
    std::unique_ptr<QAbstractItemModelTester> m_tester;
    QSet<QString> m_proofs;
};

QTEST_GUILESS_MAIN(AccountListModelTest)
#include "AccountListModelTest.moc"
