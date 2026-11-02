
#include "matrix/MatrixClient.h"
#include "models/MessageComposer.h"

#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");

struct SendRecord {
    QString kind;
    QString roomId;
    QString target;
    QString body;
    QStringList mentionIds;
    QVariantMap spec;
};

class RecordingClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit RecordingClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QList<SendRecord> sends;
    QStringList calls;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &userId) const override
    { return userId; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    QList<bool> typingCalls;
    void sendTyping(const QString &, bool isTyping, int) override
    {
        typingCalls.append(isTyping);
    }
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    void sendTextMessage(const QString &roomId, const QString &body) override
    {
        sends.append({ QStringLiteral("text"), roomId, {}, body, {}, {} });
    }
    void sendReply(const QString &roomId, const QString &target,
                   const QString &body) override
    {
        sends.append({ QStringLiteral("reply"), roomId, target, body, {}, {} });
    }
    void editMessage(const QString &roomId, const QString &target,
                     const QString &body) override
    {
        sends.append({ QStringLiteral("edit"), roomId, target, body, {}, {} });
    }
    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &mentionIds,
                         const QVariantMap &spec) override
    {
        sends.append({ QStringLiteral("text"), roomId, {}, body, mentionIds,
                       spec });
    }
    void sendReply(const QString &roomId, const QString &target,
                   const QString &body, const QStringList &mentionIds,
                   const QVariantMap &spec) override
    {
        sends.append({ QStringLiteral("reply"), roomId, target, body,
                       mentionIds, spec });
    }
    void sendThreadReplyTo(const QString &roomId, const QString &root,
                           const QString &inReplyTo, const QString &body,
                           const QStringList &mentionIds,
                           const QVariantMap &spec) override
    {
        Q_UNUSED(inReplyTo);
        sends.append({ QStringLiteral("thread"), roomId, root, body,
                       mentionIds, spec });
    }
    void editMessage(const QString &roomId, const QString &target,
                     const QString &body, const QStringList &mentionIds,
                     const QVariantMap &spec) override
    {
        sends.append({ QStringLiteral("edit"), roomId, target, body,
                       mentionIds, spec });
    }

    quint64 kickUser(const QString &roomId, const QString &userId,
                     const QString &reason) override
    {
        calls.append(QStringLiteral("kick|%1|%2|%3").arg(roomId, userId, reason));
        return 1;
    }
    quint64 banUser(const QString &roomId, const QString &userId,
                    const QString &reason) override
    {
        calls.append(QStringLiteral("ban|%1|%2|%3").arg(roomId, userId, reason));
        return 1;
    }
    quint64 unbanUser(const QString &roomId, const QString &userId,
                      const QString &reason) override
    {
        calls.append(QStringLiteral("unban|%1|%2|%3").arg(roomId, userId, reason));
        return 1;
    }
    quint64 inviteUsers(const QString &roomId, const QStringList &userIds) override
    {
        calls.append(QStringLiteral("invite|%1|%2")
                         .arg(roomId, userIds.join(QLatin1Char(','))));
        return 1;
    }
    quint64 setRoomTopic(const QString &roomId, const QString &topic) override
    {
        calls.append(QStringLiteral("topic|%1|%2").arg(roomId, topic));
        return 1;
    }
    quint64 setRoomName(const QString &roomId, const QString &name) override
    {
        calls.append(QStringLiteral("roomname|%1|%2").arg(roomId, name));
        return 1;
    }
    quint64 joinOp = 1;
    quint64 joinRoomByIdOrAlias(const QString &target, const QStringList &) override
    {
        calls.append(QStringLiteral("join|%1").arg(target));
        return joinOp;
    }
};

}

class SlashCommandComposerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_client = new RecordingClient(this);
        m_composer = new MessageComposer(this);
        m_composer->setClient(m_client);
        m_composer->setRoomId(kRoom);
    }
    void cleanup()
    {
        delete m_composer;
        m_composer = nullptr;
        delete m_client;
        m_client = nullptr;
    }

    void typingNoticesAreSentByDefault()
    {
        m_composer->setText(QStringLiteral("hello"));
        QVERIFY2(m_client->typingCalls.contains(true),
                 "the default must be unchanged: notices are sent");
    }

    void switchingTypingOffStopsTheNoticesAtTheSource()
    {
        m_composer->setTypingNotificationsEnabled(false);
        m_client->typingCalls.clear();
        m_composer->setText(QStringLiteral("hello"));
        QVERIFY2(!m_client->typingCalls.contains(true),
                 "no typing notice may leave the device while it is off");
    }

    void turningItOffMidNoticeSendsTheStopImmediately()
    {
        m_composer->setText(QStringLiteral("hello"));
        QVERIFY(m_client->typingCalls.contains(true));
        m_client->typingCalls.clear();

        m_composer->setTypingNotificationsEnabled(false);
        QCOMPARE(m_client->typingCalls.size(), 1);
        QCOMPARE(m_client->typingCalls.first(), false);
    }

    void turningItBackOnResumesWithTextAlreadyPresent()
    {
        m_composer->setTypingNotificationsEnabled(false);
        m_composer->setText(QStringLiteral("hello"));
        m_client->typingCalls.clear();

        m_composer->setTypingNotificationsEnabled(true);
        QVERIFY(m_client->typingCalls.contains(true));
    }

    void meSendsAnEmoteThroughTheCurrentContext()
    {
        m_composer->setText(QStringLiteral("/me waves"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        const SendRecord &s = m_client->sends.first();
        QCOMPARE(s.kind, QStringLiteral("text"));
        QCOMPARE(s.body, QStringLiteral("waves"));
        QCOMPARE(s.spec.value(QStringLiteral("msgtype")).toString(),
                 QStringLiteral("emote"));
        QCOMPARE(m_composer->text(), QString());
    }

    void meInsideAThreadStaysInTheThread()
    {
        m_composer->beginThreadReply(QStringLiteral("$root"),
                                     QStringLiteral("preview"));
        m_composer->setText(QStringLiteral("/me waves"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().kind, QStringLiteral("thread"));
        QCOMPARE(m_client->sends.first().target, QStringLiteral("$root"));
        QCOMPARE(m_client->sends.first().spec
                     .value(QStringLiteral("msgtype")).toString(),
                 QStringLiteral("emote"));
    }

    void missingArgumentsRefuseWithoutDestroyingTheDraft()
    {
        m_composer->setText(QStringLiteral("/me"));
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());
        QVERIFY(!m_composer->commandError().isEmpty());
        QCOMPARE(m_composer->text(), QStringLiteral("/me"));
        m_composer->setText(QStringLiteral("/me x"));
        QVERIFY(m_composer->commandError().isEmpty());
    }

    void unknownCommandIsSentAsTextWithItsSlashIntact()
    {
        m_composer->setText(QStringLiteral("/frobnicate hard"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().body,
                 QStringLiteral("/frobnicate hard"));
        QVERIFY2(m_composer->commandError().isEmpty(),
                 "an unknown command must not raise a refusal any more");
        QCOMPARE(m_composer->text(), QString());
    }

    void aKnownCommandWithMissingArgumentsStillRefuses()
    {
        m_composer->setText(QStringLiteral("/me"));
        m_composer->send();
        QVERIFY2(m_client->sends.isEmpty(),
                 "a known command missing its argument was sent as text");
        QVERIFY(!m_composer->commandError().isEmpty());
        QCOMPARE(m_composer->text(), QStringLiteral("/me"));
    }

    void doubleSlashSendsTheLiteralSingleSlashText()
    {
        m_composer->setText(QStringLiteral("//shrug it off"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().body,
                 QStringLiteral("/shrug it off"));
        QVERIFY(m_client->sends.first().spec.isEmpty());
    }

    void shrugSendsThePlainKaomojiUnmangled()
    {
        m_composer->setText(QStringLiteral("/shrug oh well"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        const SendRecord &s = m_client->sends.first();
        QCOMPARE(s.body, QStringLiteral("oh well ¯\\_(ツ)_/¯"));
        QCOMPARE(s.spec.value(QStringLiteral("format")).toString(),
                 QStringLiteral("plain"));
    }

    void spoilerSendsAnEscapedDataMxSpoilerSpan()
    {
        m_composer->setText(QStringLiteral("/spoiler ends <b>badly</b>"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        const SendRecord &s = m_client->sends.first();
        QCOMPARE(s.body, QStringLiteral("ends <b>badly</b>"));
        QCOMPARE(s.spec.value(QStringLiteral("format")).toString(),
                 QStringLiteral("html"));
        QCOMPARE(s.spec.value(QStringLiteral("html")).toString(),
                 QStringLiteral("<span data-mx-spoiler>"
                                "ends &lt;b&gt;badly&lt;/b&gt;</span>"));
    }

    void moderationCommandsRouteWithReasonAndSendNothing()
    {
        m_composer->setText(
            QStringLiteral("/kick @bob:example.org being rude"));
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());
        QCOMPARE(m_client->calls,
                 QStringList{ QStringLiteral(
                     "kick|!room:example.org|@bob:example.org|being rude") });
        QCOMPARE(m_composer->text(), QString());

        m_composer->setText(QStringLiteral("/ban @bob:example.org"));
        m_composer->send();
        QCOMPARE(m_client->calls.last(),
                 QStringLiteral("ban|!room:example.org|@bob:example.org|"));

        m_composer->setText(QStringLiteral("/invite @carol:example.org"));
        m_composer->send();
        QCOMPARE(m_client->calls.last(),
                 QStringLiteral("invite|!room:example.org|@carol:example.org"));
    }

    void aBadUserIdRefusesInsteadOfCallingTheServer()
    {
        m_composer->setText(QStringLiteral("/kick bob"));
        m_composer->send();
        QVERIFY(m_client->calls.isEmpty());
        QVERIFY(!m_composer->commandError().isEmpty());
        QCOMPARE(m_composer->text(), QStringLiteral("/kick bob"));
    }

    void topicRoomnameAndJoinRouteToTheirVerbs()
    {
        m_composer->setText(QStringLiteral("/topic release day"));
        m_composer->send();
        QCOMPARE(m_client->calls.last(),
                 QStringLiteral("topic|!room:example.org|release day"));

        m_composer->setText(QStringLiteral("/roomname The Bridge"));
        m_composer->send();
        QCOMPARE(m_client->calls.last(),
                 QStringLiteral("roomname|!room:example.org|The Bridge"));

        m_composer->setText(QStringLiteral("/join #matrix:example.org"));
        m_composer->send();
        QCOMPARE(m_client->calls.last(),
                 QStringLiteral("join|#matrix:example.org"));

        m_composer->setText(QStringLiteral("/join somewhere"));
        m_composer->send();
        QVERIFY(!m_composer->commandError().isEmpty());
    }

    void aFailedSlashJoinIsReportedNotSwallowed()
    {
        m_client->joinOp = 41;
        m_composer->setText(QStringLiteral("/join #gone:example.org"));
        m_composer->send();
        QVERIFY(m_composer->commandError().isEmpty());

        Q_EMIT m_client->roomJoinFinished(7, false, QString(),
                                          QStringLiteral("not_found"));
        QVERIFY(m_composer->commandError().isEmpty());

        Q_EMIT m_client->roomJoinFinished(41, false, QString(),
                                          QStringLiteral("forbidden"));
        QVERIFY(m_composer->commandError().contains(
            QStringLiteral("invite-only")));

        m_composer->setText(QStringLiteral("/join #ok:example.org"));
        m_composer->send();
        m_composer->setText(QString());
        Q_EMIT m_client->roomJoinFinished(41, true, QStringLiteral("!ok:x"),
                                          QString());
        QVERIFY(m_composer->commandError().isEmpty());

        m_client->joinOp = 0;
        m_composer->setText(QStringLiteral("/join #gone:example.org"));
        m_composer->send();
        QVERIFY(m_composer->commandError().contains(
            QStringLiteral("Could not reach the server")));
    }

    void nickAsksTheOwnerInsteadOfActingItself()
    {
        QSignalSpy asked(m_composer,
                         &MessageComposer::displayNameChangeRequested);
        m_composer->setText(QStringLiteral("/nick Lightning Fan"));
        m_composer->send();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().first().toString(),
                 QStringLiteral("Lightning Fan"));
        QVERIFY(m_client->sends.isEmpty());
    }

    void markdownTogglesTheModeAndKeepsNothingToSend()
    {
        QSignalSpy asked(m_composer,
                         &MessageComposer::composerModeToggleRequested);
        m_composer->setText(QStringLiteral("/markdown"));
        m_composer->send();
        QCOMPARE(asked.size(), 1);
        QVERIFY(m_client->sends.isEmpty());
        QCOMPARE(m_composer->text(), QString());
    }

    void clearEmptiesTheComposerAndTouchesNoServer()
    {
        m_composer->setText(QStringLiteral("/clear"));
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());
        QVERIFY(m_client->calls.isEmpty());
        QCOMPARE(m_composer->text(), QString());
    }

    void anEditBeginningWithASlashIsTextBeingEditedNotACommand()
    {
        m_composer->beginEdit(QStringLiteral("$target"),
                              QStringLiteral("old body"));
        m_composer->setText(QStringLiteral("/me is edited text"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().kind, QStringLiteral("edit"));
        QCOMPARE(m_client->sends.first().body,
                 QStringLiteral("/me is edited text"));
        QVERIFY(m_client->calls.isEmpty());
    }

    void completionsFollowTheTypedPrefixAndThePermissionMap()
    {
        m_composer->setText(QStringLiteral("/k"));
        QVariantList comps = m_composer->commandCompletions();
        QCOMPARE(comps.size(), 1);
        QCOMPARE(comps.first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("kick"));
        QVERIFY(comps.first().toMap().value(QStringLiteral("enabled")).toBool());

        m_composer->setCommandPermissions(
            { { QStringLiteral("kick"), false } });
        comps = m_composer->commandCompletions();
        QCOMPARE(comps.first().toMap().value(QStringLiteral("enabled")).toBool(),
                 false);
        m_composer->setText(QStringLiteral("/ban"));
        comps = m_composer->commandCompletions();
        QCOMPARE(comps.size(), 1);
        QVERIFY(comps.first().toMap().value(QStringLiteral("enabled")).toBool());
    }

    void anOversizeMessageIsRefusedAndKeepsItsText()
    {
        const QString big(70000, QLatin1Char('x'));
        m_composer->setText(big);
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());
        QCOMPARE(m_composer->text(), big);
        QVERIFY(m_composer->commandError().contains(
            QStringLiteral("too long")));

        const QString wide(25000, QChar(0x2603));
        m_composer->setText(wide);
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());

        m_composer->setText(QStringLiteral("hello"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
    }

    void anExactOptionalArgumentCommandIsMarkedSendable()
    {
        m_composer->setText(QStringLiteral("/shrug"));
        QVariantMap shrug = m_composer->commandCompletions().first().toMap();
        QVERIFY(shrug.value(QStringLiteral("exact")).toBool());
        QCOMPARE(shrug.value(QStringLiteral("requiresArgs")).toBool(), false);

        m_composer->setText(QStringLiteral("/me"));
        QVariantMap me = m_composer->commandCompletions().first().toMap();
        QVERIFY(me.value(QStringLiteral("exact")).toBool());
        QVERIFY(me.value(QStringLiteral("requiresArgs")).toBool());

        m_composer->setText(QStringLiteral("/shru"));
        QCOMPARE(m_composer->commandCompletions().first().toMap()
                     .value(QStringLiteral("exact")).toBool(), false);
    }

    // An edit goes to the timeline that holds the event, not the composer's
    // own room: the live room timeline hides threaded events, so a thread
    // reply's edit must carry the thread's timeline id.
    void anEditIsSentToTheTimelineThatHoldsTheEvent()
    {
        const QString composite =
            MatrixClient::threadTimelineId(kRoom, QStringLiteral("$root"));
        m_composer->beginEdit(QStringLiteral("$reply"),
                              QStringLiteral("before"), QString(), composite);
        m_composer->setText(QStringLiteral("after"));
        m_composer->send();
        QVERIFY(!m_client->sends.isEmpty());
        const auto edit = m_client->sends.last();
        QCOMPARE(edit.kind, QStringLiteral("edit"));
        QCOMPARE(edit.roomId, composite);

        m_client->sends.clear();
        m_composer->beginEdit(QStringLiteral("$plain"),
                              QStringLiteral("before"));
        m_composer->setText(QStringLiteral("after"));
        m_composer->send();
        QVERIFY(!m_client->sends.isEmpty());
        QCOMPARE(m_client->sends.last().roomId, kRoom);
    }

    void anUnchangedEditSendsNothingAndLeavesEditMode()
    {
        m_composer->beginEdit(QStringLiteral("$same"),
                              QStringLiteral("unchanged"));
        QVERIFY(m_composer->isEditing());
        QCOMPARE(m_composer->editOriginalText(), QStringLiteral("unchanged"));
        m_composer->send();
        QVERIFY2(m_client->sends.isEmpty(),
                 "an unchanged edit was sent to the server");
        QVERIFY(!m_composer->isEditing());
        QCOMPARE(m_composer->text(), QString());

        m_composer->beginEdit(QStringLiteral("$changed"),
                              QStringLiteral("unchanged"));
        m_composer->setText(QStringLiteral("changed"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().kind, QStringLiteral("edit"));
    }

    void anUnchangedRichEditSendsNothing()
    {
        m_composer->beginEdit(QStringLiteral("$rich"),
                              QStringLiteral("same body"));
        m_composer->sendPrepared(QStringLiteral("same body"), QString(),
                                 QStringList());
        QVERIFY2(m_client->sends.isEmpty(),
                 "an unchanged rich edit was sent to the server");
        QVERIFY(!m_composer->isEditing());

        m_composer->beginEdit(QStringLiteral("$rich2"),
                              QStringLiteral("same body"));
        m_composer->setText(QStringLiteral("**same body**"));
        m_composer->sendPrepared(QStringLiteral("same body"),
                                 QStringLiteral("<b>same body</b>"),
                                 QStringList());
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().kind, QStringLiteral("edit"));

        m_client->sends.clear();
        m_composer->beginEdit(QStringLiteral("$rich3"),
                              QStringLiteral("same body"),
                              QStringLiteral("<b>same body</b>"));
        m_composer->setText(QStringLiteral("same body"));
        m_composer->sendPrepared(QStringLiteral("same body"), QString(),
                                 QStringList());
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().kind, QStringLiteral("edit"));
    }

    void theEditBannerQuotesWhatTheMessageReadsAs()
    {
        m_composer->beginEdit(QStringLiteral("$fmt"),
                              QStringLiteral("**line one\nline two**"),
                              QStringLiteral("<strong>line one<br/>line two</strong>"));
        QCOMPARE(m_composer->editOriginalText(),
                 QStringLiteral("line one\nline two"));
        m_composer->cancelReplyOrEdit();
        QCOMPARE(m_composer->editOriginalText(), QString());
    }

    void anUnchangedSaveOfAFormattedOriginalSendsNothing_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("seed");
        QTest::newRow("seedable") << QStringLiteral("<b>bold</b>")
                                  << QStringLiteral("**bold**");
        QTest::newRow("plain seed") << QStringLiteral("<ul><li><b>bold</b></li></ul>")
                                    << QStringLiteral("bold");
    }
    void anUnchangedSaveOfAFormattedOriginalSendsNothing()
    {
        QFETCH(QString, html);
        QFETCH(QString, seed);
        m_composer->beginEdit(QStringLiteral("$fmt"), QStringLiteral("bold"), html);
        QCOMPARE(m_composer->text(), seed);
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());
        QVERIFY(!m_composer->isEditing());

        m_composer->beginEdit(QStringLiteral("$fmt"), QStringLiteral("bold"), html);
        m_composer->sendPrepared(QStringLiteral("bold"), QString(), QStringList());
        QVERIFY(m_client->sends.isEmpty());

        m_composer->beginEdit(QStringLiteral("$fmt"), QStringLiteral("bold"), html);
        m_composer->setText(QStringLiteral("bold!"));
        m_composer->send();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().kind, QStringLiteral("edit"));
        m_client->sends.clear();
    }

private:
    RecordingClient *m_client = nullptr;
    MessageComposer *m_composer = nullptr;
};

QTEST_GUILESS_MAIN(SlashCommandComposerTest)
#include "SlashCommandComposerTest.moc"
