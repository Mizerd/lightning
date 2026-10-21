// Labs (concurrent live accounts), settings half: the switch and its warm-slot
// default, that neither ever leaves the machine, the per-account pin and last-live time,
// the local-rule room set, the keyring-readability check a background host
// starts from, and flag-off parity (nothing here writes a key or emits a
// signal until something asks it to). Every case names the naive version it
// fails on.

#include "app/SettingsManager.h"
#include "settings/SettingsPortable.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace settingsport;

// Counts every read and write, so a case can prove that a settings-file
// answer read no secret. Optionally poses as a store shared between installs.
class CountingSecretStore final : public SecretStore
{
    Q_OBJECT

public:
    explicit CountingSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    bool shared = false;
    QString kind;
    mutable int reads = 0;
    int writes = 0;

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool isSharedBetweenInstalls() const override { return shared; }
    QString backendKind() const override { return kind; }

    bool storeSecret(const QString &userId, const QString &key, const QString &value) override
    {
        ++writes;
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
        ++writes;
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        ++writes;
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

// KeepRunningResult is a scoped enum: compare its value.
#define QCOMPARE_PIN(expr, want) \
    QCOMPARE(int(expr), int(SettingsManager::KeepRunningResult::want))

namespace {

const QString kHs = QStringLiteral("https://one.example");

QString uidN(int n)
{
    return QStringLiteral("@user%1:one.example").arg(n, 2, 10, QLatin1Char('0'));
}

void save(SettingsManager &s, const QString &uid, const QString &device = QStringLiteral("DEV"))
{
    s.saveSession(kHs, uid, device, QStringLiteral("token-fixture-") + uid);
}

QString accountKeyOf(const QString &uid, const char *sub)
{
    return QStringLiteral("accounts/") + matrix::app_data::safeUserSlug(uid) + QLatin1Char('/')
        + QLatin1String(sub);
}

QByteArray fileBytes(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class LabsSettingsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("LabsSettingsTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_configHome.path());
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ---- the switch -----------------------------------------------------------

    // Naive version: QVariant::toBool(), which reads "1", 1, "yes" and "on" as
    // true, so a hand edit (or another program) starts background sync for
    // every pinned account.
    void onlyAStoredTrueTurnsTheSwitchOn()
    {
        SettingsManager s;
        QCOMPARE(s.labsConcurrentAccounts(), false);
        QSettings raw;
        const QList<QVariant> notTrue = { QStringLiteral("1"), 1, QStringLiteral("yes"),
                                          QStringLiteral("on"), QStringLiteral("TRUE"),
                                          QStringLiteral("True"), 2, 1.0, QStringLiteral(" true") };
        for (const QVariant &v : notTrue) {
            raw.setValue(QStringLiteral("labs/concurrentAccounts"), v);
            QVERIFY2(!s.labsConcurrentAccounts(), qPrintable(v.toString()));
        }
        raw.setValue(QStringLiteral("labs/concurrentAccounts"), true);
        QVERIFY(s.labsConcurrentAccounts());
        // What QSettings hands back for a bool after the file is re-read.
        raw.setValue(QStringLiteral("labs/concurrentAccounts"), QStringLiteral("true"));
        QVERIFY(s.labsConcurrentAccounts());
    }

    // DECISION 3: the warm slot is on unless a stored false says otherwise.
    // Naive version: value(key, false), or toBool() on a damaged value.
    void theWarmSlotIsOnByDefault()
    {
        SettingsManager s;
        QCOMPARE(s.labsConcurrentAccountsWarmSlot(), true);
        QSettings raw;
        raw.setValue(QStringLiteral("labs/concurrentAccountsWarmSlot"), QStringLiteral("garbage"));
        QCOMPARE(s.labsConcurrentAccountsWarmSlot(), true);
        raw.setValue(QStringLiteral("labs/concurrentAccountsWarmSlot"), QStringLiteral("false"));
        QCOMPARE(s.labsConcurrentAccountsWarmSlot(), false);
        raw.setValue(QStringLiteral("labs/concurrentAccountsWarmSlot"), false);
        QCOMPARE(s.labsConcurrentAccountsWarmSlot(), false);
    }

    // Naive version: a setter without the equality guard, which would make
    // AppController tear down and rebuild the pool for a no-op write (an
    // import that carries the current value).
    void theSwitchesNotifyOnlyOnARealChange()
    {
        SettingsManager s;
        QSignalSpy on(&s, &SettingsManager::labsConcurrentAccountsChanged);
        QSignalSpy warm(&s, &SettingsManager::labsConcurrentAccountsWarmSlotChanged);
        s.setLabsConcurrentAccounts(false);
        QCOMPARE(on.count(), 0);
        s.setLabsConcurrentAccounts(true);
        s.setLabsConcurrentAccounts(true);
        QCOMPARE(on.count(), 1);
        QVERIFY(s.labsConcurrentAccounts());
        s.setLabsConcurrentAccounts(false);
        QCOMPARE(on.count(), 2);
        s.setLabsConcurrentAccountsWarmSlot(true);
        QCOMPARE(warm.count(), 0);
        s.setLabsConcurrentAccountsWarmSlot(false);
        s.setLabsConcurrentAccountsWarmSlot(false);
        QCOMPARE(warm.count(), 1);
        QVERIFY(!s.labsConcurrentAccountsWarmSlot());
    }

    // ---- portability ------------------------------------------------------------

    // The switches never leave the machine (liveacc port, 2026-10-20). The
    // design made them export-only; they are not portable at all, so that a
    // settings file exported with Labs off is byte-for-byte today's (the brief:
    // behaviour with the gate off is today's), and neither a synced blob nor
    // an imported file can switch on background sessions for every pinned
    // account. Naive versions: a schema row with flags 3 (synced: one
    // homeserver could start network activity for every account here), or
    // flags 1 (the export gains two keys on every install).
    void theSwitchesNeverLeaveTheMachine()
    {
        for (const char *n : { "labsConcurrentAccounts", "labsConcurrentAccountsWarmSlot" }) {
            QVERIFY2(!find(QString::fromLatin1(n)), n);
            QVERIFY2(!names(Sync).contains(QString::fromLatin1(n)), n);
            QVERIFY2(!names(Export).contains(QString::fromLatin1(n)), n);
        }

        // A synced blob carrying the switch never applies it.
        QJsonObject entry;
        entry.insert(QStringLiteral("v"), true);
        entry.insert(QStringLiteral("t"), 1.0);
        QJsonObject settings;
        settings.insert(QStringLiteral("labsConcurrentAccounts"), entry);
        QJsonObject blob;
        blob.insert(QStringLiteral("version"), 1);
        blob.insert(QStringLiteral("settings"), settings);
        const ParsedBlob p = parseBlob(blob, true);
        QVERIFY(!p.entries.contains(QStringLiteral("labsConcurrentAccounts")));
        QVariantMap local;
        local.insert(QStringLiteral("labsConcurrentAccounts"), false);
        const MergeResult m = merge(p, local, State(), 1000);
        QVERIFY(!m.applyLocal.contains(QStringLiteral("labsConcurrentAccounts")));

        // An imported file carrying it is "unknown" and never applied either.
        SettingsManager s;
        s.setLabsConcurrentAccounts(true);
        s.setLabsConcurrentAccountsWarmSlot(false);
        Document d;
        d.settings = readProperties(&s, Export);
        QVERIFY(!d.settings.contains(QStringLiteral("labsConcurrentAccounts")));
        QVERIFY(!d.settings.contains(QStringLiteral("labsConcurrentAccountsWarmSlot")));
        QJsonObject forged;
        forged.insert(QStringLiteral("format"), QStringLiteral("lightning-settings"));
        forged.insert(QStringLiteral("version"), 1);
        forged.insert(QStringLiteral("settings"),
                      QJsonObject{{QStringLiteral("labsConcurrentAccounts"), false},
                                  {QStringLiteral("labsConcurrentAccountsWarmSlot"), true}});
        const ParseResult r = parseDocument(QJsonDocument(forged).toJson());
        QVERIFY(r.ok);
        QVERIFY(r.unknown.contains(QStringLiteral("labsConcurrentAccounts")));
        QVERIFY(!r.doc.settings.contains(QStringLiteral("labsConcurrentAccounts")));
        QVERIFY(writeProperties(&s, r.doc.settings).isEmpty());
        QVERIFY(s.labsConcurrentAccounts());
        QVERIFY(!s.labsConcurrentAccountsWarmSlot());
    }

    // Pins and last-live times name which accounts exist here: never in the
    // file. Naive version: Q_PROPERTYs for them, or an exporter walking the
    // account groups.
    void perAccountKeysNeverLeaveTheMachine()
    {
        CountingSecretStore store;
        SettingsManager s;
        s.setSecretStore(&store);
        save(s, uidN(1));
        QCOMPARE_PIN(s.setKeepRunningFor(uidN(1), true), Ok);
        s.setLastLiveAtFor(uidN(1), 1760000000000);
        Document d;
        d.settings = readProperties(&s, Export);
        const QByteArray bytes = buildDocument(d, QStringLiteral("x"));
        for (const char *needle : { "keepRunning", "lastLiveAt", "1760000000000", "user01" })
            QVERIFY2(!bytes.contains(needle), needle);
    }

    // ---- the pin ------------------------------------------------------------------

    // Naive versions: no cap (a fifth pin stored), the cap counting only
    // non-foreground pins, or a pin written for an id with no record.
    void theFifthPinIsRefusedAndNothingIsStored()
    {
        CountingSecretStore store;
        SettingsManager s;
        s.setSecretStore(&store);
        for (int i = 1; i <= 6; ++i)
            save(s, uidN(i));
        s.setActiveAccountUserId(uidN(1)); // the foreground's pin counts too
        QCOMPARE(SettingsManager::kMaxPinnedAccounts, 4);
        QCOMPARE(SettingsManager::kMaxLiveAccounts, 5);

        // Pinned out of saved order, to prove keepRunningUserIds() reorders.
        for (int i : { 4, 1, 3, 2 })
            QCOMPARE_PIN(s.setKeepRunningFor(uidN(i), true), Ok);
        QCOMPARE_PIN(s.setKeepRunningFor(uidN(5), true), RefusedCap);
        QVERIFY(!s.keepRunningFor(uidN(5)));
        QVERIFY(!QSettings().contains(accountKeyOf(uidN(5), "keepRunning")));
        QStringList expected;
        for (const QString &uid : s.savedAccountUserIds()) {
            if (uid != uidN(5) && uid != uidN(6))
                expected.append(uid);
        }
        QCOMPARE(s.keepRunningUserIds(), expected);

        QCOMPARE_PIN(s.setKeepRunningFor(uidN(2), true), Unchanged);
        QCOMPARE_PIN(s.setKeepRunningFor(uidN(6), false), Unchanged);
        // Turning one off is never refused, and frees a place.
        QCOMPARE_PIN(s.setKeepRunningFor(uidN(3), false), Ok);
        QCOMPARE_PIN(s.setKeepRunningFor(uidN(5), true), Ok);
        QCOMPARE(s.keepRunningUserIds().size(), 4);

        QCOMPARE_PIN(s.setKeepRunningFor(QStringLiteral("@nobody:one.example"), true), RefusedNoRecord);
        QVERIFY(!s.keepRunningFor(QStringLiteral("@nobody:one.example")));
        QCOMPARE_PIN(s.setKeepRunningFor(QString(), true), RefusedNoRecord);
    }

    // keepRunningChanged(uid) on a real change only, and never accountsChanged
    // or sessionChanged (design R2-L3): the switcher and the session lifecycle
    // listen to those. Naive version: reuse upsert's accountsChanged.
    void aPinSignalsOnlyItself()
    {
        CountingSecretStore store;
        SettingsManager s;
        s.setSecretStore(&store);
        save(s, uidN(1));
        QSignalSpy pin(&s, &SettingsManager::keepRunningChanged);
        QSignalSpy accounts(&s, &SettingsManager::accountsChanged);
        QSignalSpy session(&s, &SettingsManager::sessionChanged);
        s.setKeepRunningFor(uidN(1), true);
        s.setKeepRunningFor(uidN(1), true);
        s.setLastLiveAtFor(uidN(1), 42);
        s.setKeepRunningFor(uidN(1), false);
        QCOMPARE(pin.count(), 2);
        QCOMPARE(pin.at(0).at(0).toString(), uidN(1));
        QCOMPARE(accounts.count(), 0);
        QCOMPARE(session.count(), 0);
    }

    // Written without QSettings::sync(): a host rewrites lastLiveAt every ten
    // minutes, and a flush per write is disk churn for every live account.
    // Naive version: copy setIndexAllOfferAnswer, which syncs.
    void pinsAndTimesAreWrittenWithoutAFlush()
    {
        CountingSecretStore store;
        const QString path = QSettings().fileName();
        {
            SettingsManager s;
            s.setSecretStore(&store);
            save(s, uidN(1));
            QVERIFY(!fileBytes(path).contains("keepRunning"));
            s.setKeepRunningFor(uidN(1), true);
            s.setLastLiveAtFor(uidN(1), 1760000000123);
            const QByteArray bytes = fileBytes(path);
            QVERIFY(!bytes.isEmpty()); // the record itself was flushed
            QVERIFY(!bytes.contains("keepRunning"));
            QVERIFY(!bytes.contains("lastLiveAt"));
        }
        // ...and they do reach the file: the check above could see them.
        QSettings().sync();
        const QByteArray after = fileBytes(path);
        QVERIFY(after.contains("keepRunning"));
        QVERIFY(after.contains("lastLiveAt"));
    }

    // They live inside the record and go with it. Naive version: a separate
    // labs/<slug>/ group that survives removal and re-pins a re-added account.
    void perAccountKeysGoWithTheRecord()
    {
        CountingSecretStore store;
        SettingsManager s;
        s.setSecretStore(&store);
        save(s, uidN(1));
        save(s, uidN(2));
        s.setKeepRunningFor(uidN(1), true);
        s.setLastLiveAtFor(uidN(1), 99);
        QVERIFY(s.clearSessionForAccount(uidN(1)));
        QVERIFY(!s.keepRunningFor(uidN(1)));
        QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(0));
        for (const QString &k : QSettings().allKeys()) {
            QVERIFY2(!k.endsWith(QLatin1String("/keepRunning")), qPrintable(k));
            QVERIFY2(!k.endsWith(QLatin1String("/lastLiveAt")), qPrintable(k));
        }
        save(s, uidN(1));
        QVERIFY(!s.keepRunningFor(uidN(1)));
        QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(0));
        QVERIFY(s.keepRunningUserIds().isEmpty());
    }

    // ---- lastLiveAt -------------------------------------------------------------------

    void lastLiveAtReadsZeroUnlessSane()
    {
        CountingSecretStore store;
        SettingsManager s;
        s.setSecretStore(&store);
        save(s, uidN(1));
        QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(0));
        s.setLastLiveAtFor(uidN(1), 1760000000000);
        QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(1760000000000));
        s.setLastLiveAtFor(uidN(1), -5);
        QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(0));
        QVERIFY(!QSettings().contains(accountKeyOf(uidN(1), "lastLiveAt")));
        QSettings().setValue(accountKeyOf(uidN(1), "lastLiveAt"), QStringLiteral("soon"));
        QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(0));
        // No record, nothing written anywhere.
        s.setLastLiveAtFor(QStringLiteral("@nobody:one.example"), 5);
        QCOMPARE(s.lastLiveAtFor(QStringLiteral("@nobody:one.example")), qint64(0));
        QVERIFY(!QSettings().contains(accountKeyOf(QStringLiteral("@nobody:one.example"),
                                                   "lastLiveAt")));
    }

    // ---- the local-rule room set ------------------------------------------------------

    // Equals roomNotificationModeDecidesLocallyFor() over every room either
    // account or the device ever stored a mode for, read for an account that
    // is NOT the active one. Naive versions: the account's unsynced keys only
    // (misses legacy device-only rooms), or the active account's scope.
    void theLocalRuleSetIsThePredicateForThatAccount()
    {
        CountingSecretStore store;
        SettingsManager s;
        s.setSecretStore(&store);
        const QString a = uidN(1), b = uidN(2);
        save(s, a);
        save(s, b);
        QSettings raw;
        // Legacy device-global modes from before modes were saved per account.
        raw.setValue(QStringLiteral("notifications/room-mode/!legacy:x"), 2);
        raw.setValue(QStringLiteral("notifications/room-mode/!legacyAll:x"), 0);
        raw.setValue(QStringLiteral("notifications/room-mode/!shadow:x"), 1);

        s.setActiveAccountUserId(a);
        s.setRoomNotificationModeUnsynced(QStringLiteral("!a1:x"), true, 0);
        s.setRoomNotificationMode(QStringLiteral("!a2:x"), 1);       // synced: server decides
        s.setRoomNotificationMode(QStringLiteral("!shadow:x"), 1);   // A's own key shadows legacy
        s.setActiveAccountUserId(b);
        s.setRoomNotificationModeUnsynced(QStringLiteral("!b1:x"), true, 1);

        // B is active; ask for A first.
        const QStringList forA = s.locallyDecidedNotificationRoomsFor(a);
        QCOMPARE(forA, (QStringList{ QStringLiteral("!a1:x"), QStringLiteral("!legacy:x") }));
        s.setActiveAccountUserId(a);
        const QStringList forB = s.locallyDecidedNotificationRoomsFor(b);
        QCOMPARE(forB, (QStringList{ QStringLiteral("!b1:x"), QStringLiteral("!legacy:x"),
                                     QStringLiteral("!shadow:x") }));
        // The same answer for the active account.
        QCOMPARE(s.locallyDecidedNotificationRoomsFor(a), forA);

        const QStringList every = { QStringLiteral("!a1:x"), QStringLiteral("!a2:x"),
                                    QStringLiteral("!b1:x"), QStringLiteral("!legacy:x"),
                                    QStringLiteral("!legacyAll:x"), QStringLiteral("!shadow:x") };
        for (const QString &uid : { a, b }) {
            const QStringList set = s.locallyDecidedNotificationRoomsFor(uid);
            for (const QString &room : every)
                QVERIFY2(set.contains(room) == s.roomNotificationModeDecidesLocallyFor(uid, room),
                         qPrintable(uid + QLatin1Char(' ') + room));
        }
        QVERIFY(s.locallyDecidedNotificationRoomsFor(QStringLiteral("@nobody:one.example")).isEmpty());
        QVERIFY(s.locallyDecidedNotificationRoomsFor(QString()).isEmpty());
    }

    // ---- what a host may read --------------------------------------------------------

    // A non-shared store: every recorded account. Read from the settings file
    // alone. Naive version: "a token is readable", which reads the keyring on
    // the GUI thread for every candidate at startup.
    void anUnsharedStoreIsDirectlyReadable()
    {
        CountingSecretStore store;
        SettingsManager s;
        QVERIFY(!s.keyringItemsDirectlyReadableFor(uidN(1))); // no store, no record
        s.setSecretStore(&store);
        save(s, uidN(1));
        const int reads = store.reads;
        QVERIFY(s.keyringItemsDirectlyReadableFor(uidN(1)));
        QVERIFY(!s.keyringItemsDirectlyReadableFor(QStringLiteral("@nobody:one.example")));
        QCOMPARE(store.reads, reads);
    }

    // A store shared between installs: only items this install wrote for the
    // recorded device, never adopted-unverified ones, never pre-scoping ones
    // (their first read ADOPTS them, a write), never ones declined as another
    // install's sign-in (this install's copy is gone; the foreground shows the
    // keyring_other_install card). Naive version: drop the adopted check or
    // the declined check, or ignore the device the items were written for.
    void aSharedStoreIsReadableOnlyForCurrentItems()
    {
        CountingSecretStore store;
        store.shared = true;
        SettingsManager s;
        s.setSecretStore(&store);
        const QString uid = uidN(1);
        save(s, uid, QStringLiteral("DEVNOW"));
        const int reads = store.reads;
        const int writes = store.writes;
        QSettings raw;
        QCOMPARE(raw.value(accountKeyOf(uid, "keyringItems")).toString(), QStringLiteral("own"));
        QVERIFY(s.keyringItemsDirectlyReadableFor(uid));

        raw.setValue(accountKeyOf(uid, "keyringItems"), QStringLiteral("adopted-unverified"));
        QVERIFY(s.keyringItemsAdoptedUnverified(uid));
        QVERIFY(!s.keyringItemsDirectlyReadableFor(uid));

        // Declined, and still current for the recorded device: the settings
        // file already says this install's copy was removed.
        raw.setValue(accountKeyOf(uid, "keyringItems"), QStringLiteral("declined-other-install"));
        QVERIFY(s.keyringItemsDeclinedForOtherInstall(uid));
        QVERIFY(!s.keyringItemsAdoptedUnverified(uid));
        QVERIFY(!s.keyringItemsDirectlyReadableFor(uid));

        raw.setValue(accountKeyOf(uid, "keyringItems"), QStringLiteral("own"));
        QVERIFY(s.keyringItemsDirectlyReadableFor(uid));
        raw.setValue(accountKeyOf(uid, "keyringDevice"), QStringLiteral("DEVBEFORE"));
        QVERIFY(!s.keyringItemsDirectlyReadableFor(uid));

        raw.remove(accountKeyOf(uid, "keyringItems"));
        raw.remove(accountKeyOf(uid, "keyringDevice"));
        QVERIFY(!s.keyringItemsDirectlyReadableFor(uid));

        QCOMPARE(store.reads, reads);
        QCOMPARE(store.writes, writes);
    }

    // What updateSessionTokens() records after a successful write, without
    // writing or reading anything. Naive version: an empty body (the backend
    // diagnostic then names the keyring the foreground last wrote).
    void tokensWrittenElsewhereRecordTheBackend()
    {
        CountingSecretStore store;
        store.kind = QStringLiteral("file");
        SettingsManager s;
        s.setSecretStore(&store);
        save(s, uidN(1));
        store.kind = QStringLiteral("service");
        const int reads = store.reads;
        const int writes = store.writes;
        s.noteSessionTokensWrittenElsewhere(uidN(1));
        QCOMPARE(s.keyringBackendFor(uidN(1)), QStringLiteral("service"));
        QCOMPARE(store.reads, reads);
        QCOMPARE(store.writes, writes);
    }

    // ---- flag-off parity -----------------------------------------------------------

    // Today's lifecycle on an install that never touched Labs writes no Labs
    // key at all, and the export carries no Labs key either (byte-for-byte
    // today's). Naive version: a constructor or a getter that writes its
    // default, or a portable schema row.
    void flagOffWritesNoLabsKey()
    {
        CountingSecretStore store;
        {
            SettingsManager s;
            s.setSecretStore(&store);
            save(s, uidN(1));
            save(s, uidN(2));
            s.setActiveAccountUserId(uidN(1));
            s.updateAccountProfile(uidN(1), QStringLiteral("One"), QString());
            s.setRoomNotificationMode(QStringLiteral("!r:x"), 2);
            // Every new reader, as a pool would call them: reading writes nothing.
            QVERIFY(!s.labsConcurrentAccounts());
            QVERIFY(s.labsConcurrentAccountsWarmSlot());
            QVERIFY(!s.keepRunningFor(uidN(1)));
            QVERIFY(s.keepRunningUserIds().isEmpty());
            QCOMPARE(s.lastLiveAtFor(uidN(1)), qint64(0));
            s.locallyDecidedNotificationRoomsFor(uidN(1));
            s.keyringItemsDirectlyReadableFor(uidN(1));
            QVERIFY(s.clearSessionForAccount(uidN(2)));

            const QVariantMap exported = readProperties(&s, Export);
            QVERIFY(!exported.contains(QStringLiteral("labsConcurrentAccounts")));
            QVERIFY(!exported.contains(QStringLiteral("labsConcurrentAccountsWarmSlot")));
            int labsFields = 0;
            for (const Field &f : fields()) {
                if (f.group == QLatin1String("Labs") || f.name.startsWith(QLatin1String("labs")))
                    ++labsFields;
            }
            QCOMPARE(labsFields, 0);
        }
        QSettings raw;
        raw.sync();
        for (const QString &k : raw.allKeys()) {
            QVERIFY2(!k.startsWith(QLatin1String("labs/")), qPrintable(k));
            QVERIFY2(!k.endsWith(QLatin1String("/keepRunning")), qPrintable(k));
            QVERIFY2(!k.endsWith(QLatin1String("/lastLiveAt")), qPrintable(k));
        }
        // The record keys today's code reads are untouched by the new ones.
        SettingsManager again;
        again.setSecretStore(&store);
        const QStringList recordKeys = again.accountRecord(uidN(1)).keys();
        QCOMPARE(recordKeys, (QStringList{ "addedAt", "avatarUrl", "deviceId", "displayName",
                                           "homeserver", "userId" }));
        again.setKeepRunningFor(uidN(1), true);
        again.setLastLiveAtFor(uidN(1), 7);
        QCOMPARE(again.accountRecord(uidN(1)).keys(), recordKeys);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(LabsSettingsTest)
#include "LabsSettingsTest.moc"
