
#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");
const QString kActor = QStringLiteral("@bob:example.org");

TimelineEvent makeProfileChange(const QString &eventId,
                                const QString &kind,
                                const QString &oldName,
                                const QString &newName,
                                bool avatarChanged = false)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.itemId = QStringLiteral("uid-") + eventId;
    e.roomId = kRoom;
    e.sender = kActor;
    e.type = TimelineEvent::StateChange;
    e.stateKind = QStringLiteral("member_profile");
    e.stateTarget = kActor;
    e.body = QString();
    e.profileNameChange = kind;
    e.profileNameOld = oldName;
    e.profileNameNew = newName;
    e.profileAvatarChanged = avatarChanged;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    return e;
}

class FakeClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit FakeClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QList<TimelineEvent> mirror;
    QHash<QString, QString> displayNames;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    {
        return QStringLiteral("@me:example.org");
    }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &roomId) const override
    {
        return roomId == kRoom ? mirror : QList<TimelineEvent>{};
    }
    QString displayNameFor(const QString &, const QString &userId) const override
    {
        return displayNames.value(userId, userId);
    }
    QString avatarMxcFor(const QString &, const QString &) const override
    {
        return {};
    }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    {
        return {};
    }
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

}

class ProfileActivityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void changedNamesTheOldAndTheNew();
    void setNamesOnlyTheNew();
    void clearedNeverPrintsEmptyQuotes();
    void avatarOnlyStatesOnlyTheAvatar();
    void nameAndAvatarCombineInOneSentence();
    void nothingKnownFallsBackToUpdatedTheirProfile();
    void claimedNameChangeWithoutNamesDegradesHonestly();
    void unicodeNamesRoundTripIntact();
    void longNamesAreCarriedVerbatim();
    void markupInANameIsNeverRichText();
    void modelRowsPhraseThemselvesFromTypedFields();
    void actorIsTheResolvedNameNeverABareMxid();
    void backendPhrasedRowsKeepTheirOwnSentence();

private:
    FakeClient *m_client = nullptr;
    TimelineModel *m_model = nullptr;
};

void ProfileActivityTest::init()
{
    m_client = new FakeClient(this);
    m_model = new TimelineModel(this);
    m_model->setClient(m_client);
}

void ProfileActivityTest::cleanup()
{
    delete m_model;
    delete m_client;
    m_model = nullptr;
    m_client = nullptr;
}

void ProfileActivityTest::changedNamesTheOldAndTheNew()
{
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("changed"),
        QStringLiteral("Bob"), QStringLiteral("Bobby"));
    const QString sentence =
        TimelineModel::profileChangeDescription(e, QStringLiteral("Bob"));

    QCOMPARE(sentence,
             QString::fromUtf8("Bob changed their display name from “Bob”"
                               " to “Bobby”."));
}

void ProfileActivityTest::setNamesOnlyTheNew()
{
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("set"), QString(),
        QStringLiteral("Bobby"));
    QCOMPARE(TimelineModel::profileChangeDescription(e, QStringLiteral("bob")),
             QString::fromUtf8("bob set their display name to “Bobby”."));
}

void ProfileActivityTest::clearedNeverPrintsEmptyQuotes()
{
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("cleared"),
        QStringLiteral("Bobby"), QString());
    const QString sentence =
        TimelineModel::profileChangeDescription(e, QStringLiteral("Bobby"));

    QCOMPARE(sentence, QStringLiteral("Bobby cleared their display name."));
    QVERIFY(!sentence.contains(QString::fromUtf8("“”")));
}

void ProfileActivityTest::avatarOnlyStatesOnlyTheAvatar()
{
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QString(), QString(), QString(), true);
    const QString sentence =
        TimelineModel::profileChangeDescription(e, QStringLiteral("Bob"));

    QCOMPARE(sentence, QStringLiteral("Bob changed their avatar."));
    QVERIFY(!sentence.contains(QStringLiteral("display name")));
}

void ProfileActivityTest::nameAndAvatarCombineInOneSentence()
{
    const TimelineEvent changed = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("changed"),
        QStringLiteral("Bob"), QStringLiteral("Bobby"), true);
    QCOMPARE(TimelineModel::profileChangeDescription(changed,
                                                     QStringLiteral("Bobby")),
             QString::fromUtf8("Bobby changed their display name from "
                               "“Bob” to “Bobby” and "
                               "changed their avatar."));

    const TimelineEvent set = makeProfileChange(
        QStringLiteral("$p1"), QStringLiteral("set"), QString(),
        QStringLiteral("Bobby"), true);
    QCOMPARE(TimelineModel::profileChangeDescription(set,
                                                     QStringLiteral("Bobby")),
             QString::fromUtf8("Bobby set their display name to "
                               "“Bobby” and changed their avatar."));

    const TimelineEvent cleared = makeProfileChange(
        QStringLiteral("$p2"), QStringLiteral("cleared"),
        QStringLiteral("Bobby"), QString(), true);
    QCOMPARE(TimelineModel::profileChangeDescription(cleared,
                                                     QStringLiteral("bob")),
             QStringLiteral("bob cleared their display name and changed "
                            "their avatar."));
}

void ProfileActivityTest::nothingKnownFallsBackToUpdatedTheirProfile()
{
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QString(), QString(), QString(), false);
    QCOMPARE(TimelineModel::profileChangeDescription(e, QStringLiteral("Bob")),
             QStringLiteral("Bob updated their profile."));
}

void ProfileActivityTest::claimedNameChangeWithoutNamesDegradesHonestly()
{
    for (const QString &kind : { QStringLiteral("changed"),
                                 QStringLiteral("set") }) {
        const TimelineEvent missingNew =
            makeProfileChange(QStringLiteral("$p0"), kind,
                              QStringLiteral("Bob"), QString());
        const QString sentence = TimelineModel::profileChangeDescription(
            missingNew, QStringLiteral("Bob"));
        QCOMPARE(sentence, QStringLiteral("Bob updated their profile."));
        QVERIFY(!sentence.contains(QString::fromUtf8("“”")));
    }

    const TimelineEvent missingOld =
        makeProfileChange(QStringLiteral("$p1"), QStringLiteral("changed"),
                          QString(), QStringLiteral("Bobby"));
    QCOMPARE(TimelineModel::profileChangeDescription(missingOld,
                                                     QStringLiteral("Bob")),
             QStringLiteral("Bob updated their profile."));

    const TimelineEvent blank =
        makeProfileChange(QStringLiteral("$p2"), QStringLiteral("set"),
                          QString(), QStringLiteral("   "));
    QCOMPARE(TimelineModel::profileChangeDescription(blank,
                                                     QStringLiteral("Bob")),
             QStringLiteral("Bob updated their profile."));
}

void ProfileActivityTest::unicodeNamesRoundTripIntact()
{
    const QString oldName =
        QString::fromUtf8("\U0001F469\u200D\U0001F4BB Ada");
    const QString newName = QString::fromUtf8("Ada\u0301 \u10D0\u10D1 \u2764");
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("changed"), oldName, newName);

    const QString sentence =
        TimelineModel::profileChangeDescription(e, oldName);
    QVERIFY(sentence.contains(oldName));
    QVERIFY(sentence.contains(newName));
}

void ProfileActivityTest::longNamesAreCarriedVerbatim()
{
    const QString longName = QString(255, QLatin1Char('n'));
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("set"), QString(), longName);
    const QString sentence =
        TimelineModel::profileChangeDescription(e, QStringLiteral("Bob"));

    QVERIFY(sentence.contains(longName));
    QCOMPARE(sentence.count(QLatin1Char('n')), longName.size() + 1);
}

void ProfileActivityTest::markupInANameIsNeverRichText()
{
    const QString hostile =
        QStringLiteral("<b>bold</b><img src=x onerror=alert(1)>");
    const TimelineEvent e = makeProfileChange(
        QStringLiteral("$p0"), QStringLiteral("set"), QString(), hostile);

    const QString sentence =
        TimelineModel::profileChangeDescription(e, QStringLiteral("Bob"));
    QVERIFY(sentence.contains(hostile));
    QVERIFY(!sentence.contains(QStringLiteral("&lt;")));

    m_client->displayNames.insert(kActor, QStringLiteral("Bob"));
    m_client->mirror = { e };
    m_model->setRoomId(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::FormattedBodyRole).toString(),
             QString());
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::BodyRole).toString(),
             sentence);
}

void ProfileActivityTest::modelRowsPhraseThemselvesFromTypedFields()
{
    m_client->displayNames.insert(kActor, QStringLiteral("Bob"));
    m_client->mirror = {
        makeProfileChange(QStringLiteral("$p0"), QStringLiteral("changed"),
                          QStringLiteral("Bob"), QStringLiteral("Bobby")),
    };
    m_model->setRoomId(kRoom);

    const QString expected = QString::fromUtf8(
        "Bob changed their display name from “Bob” to "
        "“Bobby”.");
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::BodyRole).toString(),
             expected);

    const QVariantList entries =
        m_model->data(m_model->index(0),
                      TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).toMap().value(QStringLiteral("description")).toString(),
             expected);
    // The typed target still crosses untouched — the sentence is presentation,
    // not a replacement for the row's identity fields.
    QCOMPARE(entries.at(0).toMap()
                 .value(QStringLiteral("affectedMemberDisplayName")).toString(),
             kActor);
}

void ProfileActivityTest::actorIsTheResolvedNameNeverABareMxid()
{
    m_client->mirror = {
        makeProfileChange(QStringLiteral("$p0"), QStringLiteral("cleared"),
                          QStringLiteral("Bobby"), QString()),
    };
    m_model->setRoomId(kRoom);
    const QString unresolved =
        m_model->data(m_model->index(0), TimelineModel::BodyRole).toString();
    QCOMPARE(unresolved, QStringLiteral("bob cleared their display name."));
    QVERIFY(!unresolved.contains(kActor));

    m_client->displayNames.insert(kActor, QStringLiteral("Bobby B"));
    Q_EMIT m_client->membersChanged(kRoom);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::BodyRole).toString(),
             QStringLiteral("Bobby B cleared their display name."));
}

void ProfileActivityTest::backendPhrasedRowsKeepTheirOwnSentence()
{
    // The mock and HTTP backends still build the sentence themselves and
    // carry NO typed fields. Overriding their body with the "we know
    // nothing" fallback would discard information, not translate it.
    TimelineEvent legacy = makeProfileChange(QStringLiteral("$p0"), QString(),
                                             QString(), QString(), false);
    legacy.body = QStringLiteral("Grace changed their display name.");
    m_client->mirror = { legacy };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::BodyRole).toString(),
             QStringLiteral("Grace changed their display name."));

    TimelineEvent typed = makeProfileChange(
        QStringLiteral("$p1"), QStringLiteral("cleared"),
        QStringLiteral("Grace"), QString());
    typed.body = QStringLiteral("stale bridge sentence");
    m_client->mirror.append(typed);
    Q_EMIT m_client->eventAppended(kRoom, typed);
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::BodyRole).toString(),
             QStringLiteral("bob cleared their display name."));
}

QTEST_GUILESS_MAIN(ProfileActivityTest)
#include "ProfileActivityTest.moc"
