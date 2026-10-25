// Drafts: account/room/thread scoping, the encrypted-room memory-only policy
// (never QSettings, fail-closed for unknown rooms), debounce staleness (a
// late timer cannot resurrect a cleared or switched-away draft), retirement
// on send/clear, reply-target and mention-ref restoration, and account
// isolation.

#include "app/DraftStore.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageComposer.h"
#include "storage/SecretStore.h"
#include "threads/ThreadController.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 3000;
// Seeded mock rooms: !general is unencrypted, !devs is encrypted.
const QString kPlainRoom = QStringLiteral("!general:mock.local");
const QString kEncryptedRoom = QStringLiteral("!devs:mock.local");

class DraftFakeSecretStore final : public SecretStore
{
public:
    explicit DraftFakeSecretStore(QObject *parent = nullptr)
        : SecretStore(parent)
    {
    }
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
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

QVariantMap textDraft(const QString &text)
{
    QVariantMap draft;
    draft.insert(QStringLiteral("text"), text);
    return draft;
}
} // namespace

class DraftStoreTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;

    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

    static void activateAccount(SettingsManager &settings,
                                const QString &userId)
    {
        settings.saveSession(QStringLiteral("https://mock.local"), userId,
                             QStringLiteral("DEVICE"),
                             QStringLiteral("token-fixture"));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("draft-store-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // Photo metadata policy (a privacy setting that lives beside the composer
    // ones): defaults to removing the location, round-trips, and an
    // unreadable stored value reads as the default, never as "keep".
    void imageMetadataModeDefaultsRoundTripsAndNeverReadsAsKeep()
    {
        SettingsManager settings;
        QCOMPARE(settings.imageMetadataMode(), QStringLiteral("location"));
        QSignalSpy changed(&settings, &SettingsManager::imageMetadataModeChanged);
        settings.setImageMetadataMode(QStringLiteral("all"));
        QCOMPARE(settings.imageMetadataMode(), QStringLiteral("all"));
        settings.setImageMetadataMode(QStringLiteral("keep"));
        QCOMPARE(settings.imageMetadataMode(), QStringLiteral("keep"));
        QVERIFY(changed.count() >= 2);
        settings.setImageMetadataMode(QStringLiteral("something-newer"));
        QCOMPARE(settings.imageMetadataMode(), QStringLiteral("location"));
        {
            QSettings raw;
            raw.setValue(QStringLiteral("composer/imageMetadataMode"),
                         QStringLiteral("garbage"));
            raw.sync();
        }
        QCOMPARE(settings.imageMetadataMode(), QStringLiteral("location"));
    }

    void settingsRoundTripRequiresAnActiveAccount()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        // No account: writes refused, reads empty.
        settings.setRoomDraft(kPlainRoom, textDraft(QStringLiteral("lost")));
        QVERIFY(settings.roomDraft(kPlainRoom).isEmpty());

        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        settings.setRoomDraft(kPlainRoom, textDraft(QStringLiteral("kept")));
        QCOMPARE(settings.roomDraft(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("kept"));
        // An empty map removes.
        settings.setRoomDraft(kPlainRoom, {});
        QVERIFY(settings.roomDraft(kPlainRoom).isEmpty());
    }

    void encryptedAndUnknownRoomsNeverPersist()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));

        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);

        // Unencrypted room: persists (survives the memory wipe).
        store.save(kPlainRoom, kPlainRoom, textDraft(QStringLiteral("plain")));
        store.clearMemoryDrafts();
        QCOMPARE(store.load(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("plain"));

        // Encrypted room: memory only; gone after the wipe and never in
        // QSettings.
        store.save(kEncryptedRoom, kEncryptedRoom,
                   textDraft(QStringLiteral("secret sentence")));
        QCOMPARE(store.load(kEncryptedRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("secret sentence"));
        QVERIFY(settings.roomDraft(kEncryptedRoom).isEmpty());
        store.clearMemoryDrafts();
        QVERIFY(store.load(kEncryptedRoom).isEmpty());

        // Unknown room: fail closed to memory.
        const QString unknown = QStringLiteral("!nowhere:mock.local");
        store.save(unknown, unknown, textDraft(QStringLiteral("limbo")));
        QVERIFY(settings.roomDraft(unknown).isEmpty());
        store.clearMemoryDrafts();
        QVERIFY(store.load(unknown).isEmpty());

        // A listed room whose encryption state has not synced
        // (encryptionKnown=false, as in the first-sync window) also fails
        // closed: encrypted=false without encryptionKnown is unknown.
        const QString fresh = QStringLiteral("!fresh:mock.local");
        QSignalSpy joined(&client, &MatrixClient::roomJoinFinished);
        client.joinRoomByIdOrAlias(fresh, {});
        QTRY_VERIFY(joined.count() >= 1);
        QVERIFY(!client.roomInfo(fresh).id.isEmpty());
        store.save(fresh, fresh, textDraft(QStringLiteral("first words")));
        QVERIFY(settings.roomDraft(fresh).isEmpty());
        QCOMPARE(store.load(fresh)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("first words"));
        store.clearMemoryDrafts();
        QVERIFY(store.load(fresh).isEmpty());
    }

    void composerSavesAndRestoresAcrossRoomSwitches()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);

        composer.setRoomId(kPlainRoom);
        composer.setText(QStringLiteral("half-written thought"));
        composer.setRoomId(kEncryptedRoom);
        QVERIFY(composer.text().isEmpty());
        composer.setText(QStringLiteral("encrypted-room words"));
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("half-written thought"));
        // The Settings round-trip (room cleared, then restored).
        composer.setRoomId(QString());
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("half-written thought"));
        // The encrypted room's draft survived the switches in memory.
        composer.setRoomId(kEncryptedRoom);
        QCOMPARE(composer.text(), QStringLiteral("encrypted-room words"));
    }

    void sendAndExplicitClearRetireTheDraftEvenAgainstTheTimer()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);

        composer.setRoomId(kPlainRoom);
        composer.setText(QStringLiteral("about to send"));
        composer.send();
        QVERIFY(composer.text().isEmpty());
        // Outwait the debounce: the stopped timer must not resurrect.
        QTest::qWait(1300);
        composer.setRoomId(QString());
        composer.setRoomId(kPlainRoom);
        QVERIFY(composer.text().isEmpty());

        composer.setText(QStringLiteral("typed then cleared"));
        composer.clear();
        QTest::qWait(1300);
        composer.setRoomId(QString());
        composer.setRoomId(kPlainRoom);
        QVERIFY(composer.text().isEmpty());
    }

    void staleTimerCannotWriteAcrossRooms()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);

        composer.setRoomId(kPlainRoom);
        composer.setText(QStringLiteral("room A text"));
        // Switch immediately: the save happens synchronously and the pending
        // timer is stopped, so nothing lands under B's key.
        composer.setRoomId(kEncryptedRoom);
        QTest::qWait(1300);
        QVERIFY(store.load(kEncryptedRoom).isEmpty());
        QCOMPARE(store.load(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("room A text"));
    }

    void replyTargetRestoresAndEditModeNeverSaves()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);

        composer.setRoomId(kPlainRoom);
        composer.beginReply(QStringLiteral("$target"),
                            QStringLiteral("Bob"),
                            QStringLiteral("the original"),
                            QStringLiteral("$target"));
        composer.setText(QStringLiteral("replying to that"));
        composer.setRoomId(QString());
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("replying to that"));
        QCOMPARE(composer.replyingToEventId(), QStringLiteral("$target"));
        QCOMPARE(composer.replyingToSender(), QStringLiteral("Bob"));
        // The reply banner's thumbnail media key survives the round trip like
        // the text fields.
        QCOMPARE(composer.replyingToMediaKey(), QStringLiteral("$target"));

        // Edit mode replaces the text with the edited body; leaving the room
        // then must not save that as a draft; the pre-edit draft survives.
        composer.beginEdit(QStringLiteral("$edited"),
                           QStringLiteral("old message body"));
        composer.setRoomId(QString());
        composer.setRoomId(kPlainRoom);
        QVERIFY(composer.text() != QStringLiteral("old message body"));
    }

    void mentionRefsRestoreFailClosed()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);

        // A valid ref whose slice matches, and a corrupted one that does not;
        // only the valid one comes back.
        QVariantMap draft;
        draft.insert(QStringLiteral("text"),
                     QStringLiteral("hi @Bob and @Eve"));
        QVariantList refs;
        refs.append(QVariantMap{
            { QStringLiteral("userId"), QStringLiteral("@bob:mock.local") },
            { QStringLiteral("displayText"), QStringLiteral("@Bob") },
            { QStringLiteral("start"), 3 },
            { QStringLiteral("length"), 4 },
        });
        refs.append(QVariantMap{
            { QStringLiteral("userId"), QStringLiteral("@eve:mock.local") },
            { QStringLiteral("displayText"), QStringLiteral("@Someone") },
            { QStringLiteral("start"), 12 },
            { QStringLiteral("length"), 4 },
        });
        draft.insert(QStringLiteral("mentions"), refs);
        store.save(kPlainRoom, kPlainRoom, draft);

        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("hi @Bob and @Eve"));
        QCOMPARE(composer.mentionRanges().size(), 1);
    }

    void threadDraftsAreScopedPerThreadAndRetiredOnSend()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        ThreadController thread;
        thread.setClient(&client);
        thread.setDraftStore(&store);

        // Any event id works as a root for composer-draft purposes.
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        thread.setText(QStringLiteral("thread one draft"));
        thread.openThread(kPlainRoom, QStringLiteral("$root2"));
        QVERIFY(thread.text().isEmpty());
        thread.setText(QStringLiteral("thread two draft"));
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        QCOMPARE(thread.text(), QStringLiteral("thread one draft"));
        // Close keeps the draft; reopening restores it.
        thread.close();
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        QCOMPARE(thread.text(), QStringLiteral("thread one draft"));
        // A dispatched send retires it.
        thread.sendText(thread.text());
        QVERIFY(thread.text().isEmpty());
        thread.close();
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        QVERIFY(thread.text().isEmpty());
    }

    // Inline thread focus retargets the ROOM composer (setThreadTargetRootId)
    // instead of opening the thread panel. Each conversation keeps its own
    // draft: room text must not ride into thread A (and be sent there), A's
    // text must not appear in B, and the thread key is the one the panel
    // uses, so a draft follows the thread between placements.
    void inlineThreadTargetsKeepPerConversationDrafts()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);

        composer.setRoomId(kPlainRoom);
        composer.setText(QStringLiteral("room text"));
        composer.beginReply(QStringLiteral("$target"),
                            QStringLiteral("Bob"),
                            QStringLiteral("the original"),
                            QStringLiteral("$target"));

        composer.setThreadTargetRootId(QStringLiteral("$rootA"));
        QVERIFY2(composer.text().isEmpty(),
                 "the room draft followed the composer into thread A");
        QVERIFY2(composer.replyingToEventId().isEmpty(),
                 "the room's reply chip followed the composer into thread A");
        QCOMPARE(composer.draftKey(),
                 MatrixClient::threadTimelineId(kPlainRoom,
                                                QStringLiteral("$rootA")));
        composer.setText(QStringLiteral("thread A text"));

        composer.setThreadTargetRootId(QStringLiteral("$rootB"));
        QVERIFY2(composer.text().isEmpty(),
                 "thread A's draft appeared in thread B");
        composer.setText(QStringLiteral("thread B text"));

        composer.setThreadTargetRootId(QStringLiteral("$rootA"));
        QCOMPARE(composer.text(), QStringLiteral("thread A text"));

        composer.setThreadTargetRootId(QString());
        QCOMPARE(composer.text(), QStringLiteral("room text"));
        QCOMPARE(composer.replyingToEventId(), QStringLiteral("$target"));

        // Leaving the room while focused on B keeps B's draft under B's key.
        composer.setThreadTargetRootId(QStringLiteral("$rootB"));
        composer.setRoomId(kEncryptedRoom);
        QVERIFY(composer.threadTargetRootId().isEmpty());
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("room text"));

        // The thread panel reads the same key: B's inline draft is there.
        ThreadController thread;
        thread.setClient(&client);
        thread.setDraftStore(&store);
        thread.openThread(kPlainRoom, QStringLiteral("$rootB"));
        QCOMPARE(thread.text(), QStringLiteral("thread B text"));
    }

    // Editing replaces the composer text with the edited body; leaving edit
    // mode -- cancel or a sent edit -- must give the conversation its own
    // draft back rather than an empty composer (and never retire that draft).
    void leavingEditModeRestoresThePreEditDraft()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);
        MessageComposer composer;
        composer.setClient(&client);
        composer.setDraftStore(&store);

        composer.setRoomId(kPlainRoom);
        // Typed just now: the debounce has not fired yet.
        composer.setText(QStringLiteral("half-written reply"));
        composer.beginEdit(QStringLiteral("$edited"),
                           QStringLiteral("old message body"));
        QCOMPARE(composer.text(), QStringLiteral("old message body"));
        composer.cancelReplyOrEdit();
        QCOMPARE(composer.text(), QStringLiteral("half-written reply"));

        composer.beginEdit(QStringLiteral("$edited"),
                           QStringLiteral("old message body"));
        composer.setText(QStringLiteral("new message body"));
        composer.send();
        QVERIFY(!composer.isEditing());
        QCOMPARE(composer.text(), QStringLiteral("half-written reply"));
        QCOMPARE(store.load(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("half-written reply"));
    }

    void accountsAreIsolated()
    {
        DraftFakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        MockMatrixClient client;
        QVERIFY(login(client));
        DraftStore store;
        store.setSettings(&settings);
        store.setClient(&client);

        store.save(kPlainRoom, kPlainRoom,
                   textDraft(QStringLiteral("alice draft")));
        QCOMPARE(store.load(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("alice draft"));

        // Another account does not see it, and its writes do not leak back.
        activateAccount(settings, QStringLiteral("@second:mock.local"));
        store.clearMemoryDrafts();
        QVERIFY(store.load(kPlainRoom).isEmpty());
        store.save(kPlainRoom, kPlainRoom,
                   textDraft(QStringLiteral("second draft")));
        settings.setActiveAccountUserId(QStringLiteral("@alice:mock.local"));
        QCOMPARE(store.load(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("alice draft"));
    }
};

QTEST_MAIN(DraftStoreTest)
#include "DraftStoreTest.moc"
