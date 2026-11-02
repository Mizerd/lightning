
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
}

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
        settings.setRoomDraft(kPlainRoom, textDraft(QStringLiteral("lost")));
        QVERIFY(settings.roomDraft(kPlainRoom).isEmpty());

        activateAccount(settings, QStringLiteral("@alice:mock.local"));
        settings.setRoomDraft(kPlainRoom, textDraft(QStringLiteral("kept")));
        QCOMPARE(settings.roomDraft(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("kept"));
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

        store.save(kPlainRoom, kPlainRoom, textDraft(QStringLiteral("plain")));
        store.clearMemoryDrafts();
        QCOMPARE(store.load(kPlainRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("plain"));

        store.save(kEncryptedRoom, kEncryptedRoom,
                   textDraft(QStringLiteral("secret sentence")));
        QCOMPARE(store.load(kEncryptedRoom)
                     .value(QStringLiteral("text"))
                     .toString(),
                 QStringLiteral("secret sentence"));
        QVERIFY(settings.roomDraft(kEncryptedRoom).isEmpty());
        store.clearMemoryDrafts();
        QVERIFY(store.load(kEncryptedRoom).isEmpty());

        const QString unknown = QStringLiteral("!nowhere:mock.local");
        store.save(unknown, unknown, textDraft(QStringLiteral("limbo")));
        QVERIFY(settings.roomDraft(unknown).isEmpty());
        store.clearMemoryDrafts();
        QVERIFY(store.load(unknown).isEmpty());

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
        composer.setRoomId(QString());
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("half-written thought"));
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
        QCOMPARE(composer.replyingToMediaKey(), QStringLiteral("$target"));

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

        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        thread.setText(QStringLiteral("thread one draft"));
        thread.openThread(kPlainRoom, QStringLiteral("$root2"));
        QVERIFY(thread.text().isEmpty());
        thread.setText(QStringLiteral("thread two draft"));
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        QCOMPARE(thread.text(), QStringLiteral("thread one draft"));
        thread.close();
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        QCOMPARE(thread.text(), QStringLiteral("thread one draft"));
        thread.sendText(thread.text());
        QVERIFY(thread.text().isEmpty());
        thread.close();
        thread.openThread(kPlainRoom, QStringLiteral("$root1"));
        QVERIFY(thread.text().isEmpty());
    }

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

        composer.setThreadTargetRootId(QStringLiteral("$rootB"));
        composer.setRoomId(kEncryptedRoom);
        QVERIFY(composer.threadTargetRootId().isEmpty());
        composer.setRoomId(kPlainRoom);
        QCOMPARE(composer.text(), QStringLiteral("room text"));

        ThreadController thread;
        thread.setClient(&client);
        thread.setDraftStore(&store);
        thread.openThread(kPlainRoom, QStringLiteral("$rootB"));
        QCOMPARE(thread.text(), QStringLiteral("thread B text"));
    }

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
