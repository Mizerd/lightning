// v0.9 slash commands: what the COMPOSER does with a parsed command — the
// half SlashCommandsTest (pure parser) cannot see. A recording client pins
// which backend verb ran, with which arguments and which body spec, and the
// draft-preservation contract: a refusal never destroys what the user typed.

#include "matrix/MatrixClient.h"
#include "models/MessageComposer.h"

#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");

struct SendRecord {
    QString kind; // "text" | "reply" | "thread" | "edit"
    QString roomId;
    QString target; // reply-to / edit target / thread root
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
    QStringList calls; // moderation/admin verbs, formatted for assertion

    // ---- interface stubs (the TimelineModelDiffTest double pattern).
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
    // Recorded rather than discarded: the typing-privacy gate can only be
    // proven by what the CLIENT was asked to send, and a stub that throws
    // the call away can prove neither that it happened nor that it did not.
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

    // ---- the recorded surfaces. The base 2-arg lanes trap accidental
    // fallbacks; the spec overloads are what the composer must reach.
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
    quint64 joinRoomByIdOrAlias(const QString &target, const QStringList &) override
    {
        calls.append(QStringLiteral("join|%1").arg(target));
        return 1;
    }
};

} // namespace

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

    // ── Typing privacy (Settings -> Privacy -> Reading and typing) ──────
    //
    // Typing notices are the highest-frequency disclosure a chat client
    // makes. The setting has to stop them at the source, and it has to deal
    // with being switched off MID-NOTICE.

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

    // Turning it off WHILE a notice is live must retract it now. The server
    // would eventually time the notice out on its own, but "you will stop
    // appearing to type within thirty seconds" is not what the switch says.
    void turningItOffMidNoticeSendsTheStopImmediately()
    {
        m_composer->setText(QStringLiteral("hello"));
        QVERIFY(m_client->typingCalls.contains(true));
        m_client->typingCalls.clear();

        m_composer->setTypingNotificationsEnabled(false);
        QCOMPARE(m_client->typingCalls.size(), 1);
        QCOMPARE(m_client->typingCalls.first(), false);
    }

    // And turning it back on with text already in the box resumes, rather
    // than waiting for the next keystroke.
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
        // A successful command clears the composer like a send.
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
        // Any edit clears the refusal.
        m_composer->setText(QStringLiteral("/me x"));
        QVERIFY(m_composer->commandError().isEmpty());
    }

    void unknownCommandRefusesAndSendAnywayPostsItLiterally()
    {
        m_composer->setText(QStringLiteral("/frobnicate hard"));
        m_composer->send();
        QVERIFY(m_client->sends.isEmpty());
        QVERIFY(m_composer->commandError()
                    .contains(QStringLiteral("/frobnicate")));
        QCOMPARE(m_composer->text(), QStringLiteral("/frobnicate hard"));

        m_composer->sendBypassingCommands();
        QCOMPARE(m_client->sends.size(), 1);
        QCOMPARE(m_client->sends.first().body,
                 QStringLiteral("/frobnicate hard"));
        QVERIFY(m_client->sends.first().spec.isEmpty());
        QCOMPARE(m_composer->text(), QString());
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
        // The spoiler TEXT is escaped — typed angle brackets are content,
        // not markup, and cannot smuggle tags into the formatted body.
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

        // A join target that is neither an alias nor a room id refuses.
        m_composer->setText(QStringLiteral("/join somewhere"));
        m_composer->send();
        QVERIFY(!m_composer->commandError().isEmpty());
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
        // A key the map does not carry counts as allowed.
        m_composer->setText(QStringLiteral("/ban"));
        comps = m_composer->commandCompletions();
        QCOMPARE(comps.size(), 1);
        QVERIFY(comps.first().toMap().value(QStringLiteral("enabled")).toBool());
    }

private:
    RecordingClient *m_client = nullptr;
    MessageComposer *m_composer = nullptr;
};

QTEST_GUILESS_MAIN(SlashCommandComposerTest)
#include "SlashCommandComposerTest.moc"
