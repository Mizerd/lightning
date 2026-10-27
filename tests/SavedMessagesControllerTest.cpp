#include "app/SavedMessagesController.h"
#include "matrix/MatrixClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

class SavedFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    QString user = QStringLiteral("@one:example.org");
    QVariantMap lastWrite;
    QString lastType;
    QString requestedRoom;
    QString requestedEvent;
    int reads = 0;
    bool supportsAccountPrefs() const override { return true; }
    bool isLoggedIn() const override { return !user.isEmpty(); }
    QString currentUserId() const override { return user; }
    QString homeserverUrl() const override { return {}; }
    void readAccountPref(const QString &type) override { lastType = type; ++reads; }
    void writeAccountPref(const QString &type, const QVariantMap &content) override
    { lastType = type; lastWrite = content; }
    void requestEventSource(const QString &roomId, const QString &eventId) override
    { requestedRoom = roomId; requestedEvent = eventId; }
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { user.clear(); Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    RoomInfo roomInfo(const QString &roomId) const override
    { RoomInfo r; r.id = roomId; r.name = QStringLiteral("Room"); return r; }
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
};

class SavedMessagesControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void onlyReferencesAreWrittenAndAccountsAreIsolated()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        QCOMPARE(client.lastType, SavedMessagesController::eventType());
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QVERIFY(saved.loaded());
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$reply"),
                   QStringLiteral("$root"));
        QCOMPARE(client.lastWrite.value(QStringLiteral("version")).toInt(), 1);
        const QVariantList refs = client.lastWrite.value(QStringLiteral("entries")).toList();
        QCOMPARE(refs.size(), 1);
        const QVariantMap ref = refs.first().toMap();
        QCOMPARE(ref.size(), 4);
        QCOMPARE(ref.value(QStringLiteral("room_id")).toString(),
                 QStringLiteral("!room:example.org"));
        QCOMPARE(ref.value(QStringLiteral("event_id")).toString(),
                 QStringLiteral("$reply"));
        QCOMPARE(ref.value(QStringLiteral("thread_root_id")).toString(),
                 QStringLiteral("$root"));
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(client.lastWrite))
                    .toJson().contains("secret body"));
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QSignalSpy opened(&saved, &SavedMessagesController::openRequested);
        saved.open(QStringLiteral("!room:example.org"), QStringLiteral("$reply"));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().at(2).toString(), QStringLiteral("$root"));
        client.logout();
        QCOMPARE(saved.count(), 0);
        QVERIFY(!saved.loaded());
        client.user = QStringLiteral("@two:example.org");
        Q_EMIT client.loginSucceeded(client.user);
        QCOMPARE(saved.count(), 0);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QVERIFY(!saved.isSaved(QStringLiteral("!room:example.org"),
                               QStringLiteral("$reply")));
    }

    void removalUndoAndRedactionNeverRestoreCachedText()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$event"), {});
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        saved.resolveVisible();
        QCOMPARE(client.requestedEvent, QStringLiteral("$event"));
        Q_EMIT client.eventSourceReceived(QStringLiteral("!room:example.org"),
                                          QStringLiteral("$event"), true,
            QStringLiteral("{\"type\":\"m.room.message\",\"sender\":\"@bob:example.org\",\"content\":{\"msgtype\":\"m.text\",\"body\":\"secret body\"}}"), {});
        QCOMPARE(saved.entries().first().toMap().value(QStringLiteral("preview"))
                     .toString(), QStringLiteral("secret body"));
        Q_EMIT client.eventRedacted(QStringLiteral("!room:example.org"),
                                    QStringLiteral("$event"));
        const QVariantMap redacted = saved.entries().first().toMap();
        QVERIFY(!redacted.value(QStringLiteral("available")).toBool());
        QVERIFY(redacted.value(QStringLiteral("preview")).toString().isEmpty());
        saved.remove(QStringLiteral("!room:example.org"), QStringLiteral("$event"));
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QCOMPARE(saved.count(), 0);
        saved.undoRemove();
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QCOMPARE(saved.count(), 1);
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(client.lastWrite))
                    .toJson().contains("secret body"));
        QVERIFY(saved.entries().first().toMap().value(QStringLiteral("preview"))
                    .toString().isEmpty());
    }

    void failedWriteRestoresThePriorList()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$event"), {});
        QCOMPARE(saved.count(), 1);
        Q_EMIT client.accountPrefWritten(client.lastType, false,
                                          QStringLiteral("timeout"));
        QCOMPARE(saved.count(), 0);
        QVERIFY(!saved.busy());
        QVERIFY(!saved.error().isEmpty());
    }

    void redactionWinsOverAnInFlightResolution()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$event"), {});
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        saved.resolveVisible();
        Q_EMIT client.eventRedacted(QStringLiteral("!room:example.org"),
                                    QStringLiteral("$event"));
        Q_EMIT client.eventSourceReceived(QStringLiteral("!room:example.org"),
                                          QStringLiteral("$event"), true,
            QStringLiteral("{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\",\"body\":\"stale secret\"}}"), {});
        const QVariantMap ref = saved.entries().first().toMap();
        QVERIFY(ref.value(QStringLiteral("resolved")).toBool());
        QVERIFY(!ref.value(QStringLiteral("available")).toBool());
        QVERIFY(ref.value(QStringLiteral("preview")).toString().isEmpty());
    }

    void malformedAccountDataCannotBecomeABookmark()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, true, QVariantMap{
            { QStringLiteral("version"), 1 },
            { QStringLiteral("entries"), QVariantList{QVariantMap{
                { QStringLiteral("room_id"), QStringLiteral("!r:example.org\x1f" "thread") },
                { QStringLiteral("event_id"), QStringLiteral("$e") },
                { QStringLiteral("thread_root_id"), QStringLiteral("$root") },
                { QStringLiteral("saved_at_ms"), 42 },
            }} },
        });
        QVERIFY(!saved.loaded());
        QCOMPARE(saved.count(), 0);
        QVERIFY(!saved.error().isEmpty());
    }
};

QTEST_GUILESS_MAIN(SavedMessagesControllerTest)
#include "SavedMessagesControllerTest.moc"
