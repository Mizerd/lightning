// Room-state and membership sentences are worded in C++ in the UI language
// (2026-10-18 live runs: "joined the room", "changed the room name" stayed
// English in a German UI because rust/src/timeline.rs wrote the sentence).
// The bridge now sends a closed name (`state_sentence`) beside its English
// body; this covers the ingest, the timeline row, the re-wording after a
// language switch, the room-list preview kind, and that the C++ table and the
// Rust one name the same sentences with the same English.

#include "matrix/EventPreview.h"
#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/RustTimelineIngest.h"
#include "models/RoomExport.h"
#include "models/RoomExportHtml.h"
#include "models/TimelineModel.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTranslator>
#include <QtTest/QtTest>

using matrix::preview::Kind;

namespace {

// A name as the timeline inserts it: in a first-strong isolate.
QString iso(const QString &name)
{
    return QChar(0x2068) + name + QChar(0x2069);
}

const QString kRoom = QStringLiteral("!room:example.org");

// German for the EventPreview context, nothing else.
struct German : QTranslator {
    QString translate(const char *ctx, const char *src, const char *, int) const override
    {
        if (QByteArray(ctx) != "EventPreview")
            return {};
        const QByteArray s(src);
        if (s == "%1 joined the room.") return QStringLiteral("%1 ist dem Raum beigetreten.");
        if (s == "%1 invited %2.") return QStringLiteral("%1 hat %2 eingeladen.");
        if (s == "%1 changed the room name.") return QStringLiteral("%1 hat den Raumnamen geändert.");
        if (s == "Encryption was enabled.") return QStringLiteral("Verschlüsselung wurde aktiviert.");
        return {};
    }
    bool isEmpty() const override { return false; }
};

struct Installed {
    explicit Installed(QTranslator *t) : t(t) { QCoreApplication::installTranslator(t); }
    ~Installed() { QCoreApplication::removeTranslator(t); }
    QTranslator *t;
};

// A row as rust/src/timeline.rs sends it.
QJsonObject stateItem(const QString &eventId, const QString &stateKind,
                      const QString &sentence, const QString &target,
                      const QString &englishBody)
{
    return QJsonObject{
        { QStringLiteral("kind"), QStringLiteral("event") },
        { QStringLiteral("item_id"), QStringLiteral("uid-") + eventId },
        { QStringLiteral("event_id"), eventId },
        { QStringLiteral("sender"), QStringLiteral("@alice:example.org") },
        { QStringLiteral("sender_display_name"), QStringLiteral("Alice") },
        { QStringLiteral("msgtype"), QStringLiteral("state") },
        { QStringLiteral("state_kind"), stateKind },
        { QStringLiteral("state_sentence"), sentence },
        { QStringLiteral("state_target"), target },
        { QStringLiteral("body"), englishBody },
        { QStringLiteral("timestamp_ms"), 1700000000000.0 },
    };
}

class FakeClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit FakeClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QList<TimelineEvent> mirror;

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
    QList<TimelineEvent> timeline(const QString &roomId) const override
    {
        return roomId == kRoom ? mirror : QList<TimelineEvent>{};
    }
    QString displayNameFor(const QString &, const QString &userId) const override { return userId; }
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

} // namespace

class StateSentenceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The closed name crosses; one this build does not know does not.
    void theIngestKeepsOnlyAKnownSentence()
    {
        const TimelineEvent joined = matrix::rust_timeline::eventFromItemJson(
            stateItem(QStringLiteral("$j"), QStringLiteral("membership"),
                      QStringLiteral("member_joined"), QStringLiteral("Bob"),
                      QStringLiteral("Bob joined the room.")),
            kRoom);
        QCOMPARE(joined.stateSentence, QStringLiteral("member_joined"));
        const TimelineEvent unknown = matrix::rust_timeline::eventFromItemJson(
            stateItem(QStringLiteral("$u"), QStringLiteral("m.room.name"),
                      QStringLiteral("hologram_projected"), {},
                      QStringLiteral("@alice:example.org changed the room name.")),
            kRoom);
        QVERIFY(unknown.stateSentence.isEmpty());
    }

    // The timeline row says it in the UI language, with the sender as
    // presented and the target member. Old code: the bridge's English.
    void aStateRowIsWordedInTheUiLanguage()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        client.mirror = {
            matrix::rust_timeline::eventFromItemJson(
                stateItem(QStringLiteral("$j"), QStringLiteral("membership"),
                          QStringLiteral("member_joined"), QStringLiteral("Bob"),
                          QStringLiteral("Bob joined the room.")), kRoom),
            matrix::rust_timeline::eventFromItemJson(
                stateItem(QStringLiteral("$i"), QStringLiteral("membership"),
                          QStringLiteral("member_invited"), QStringLiteral("Bob"),
                          QStringLiteral("@alice:example.org invited Bob.")), kRoom),
            matrix::rust_timeline::eventFromItemJson(
                stateItem(QStringLiteral("$e"), QStringLiteral("m.room.encryption"),
                          QStringLiteral("encryption_enabled"), {},
                          QStringLiteral("Encryption was enabled.")), kRoom),
        };
        German german;
        Installed on(&german);
        model.setRoomId(kRoom);
        QStringList bodies;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QString body =
                model.data(model.index(row), TimelineModel::BodyRole).toString();
            if (!body.isEmpty())
                bodies << body;
        }
        QVERIFY2(bodies.contains(iso(QStringLiteral("Bob")) + QStringLiteral(" ist dem Raum beigetreten.")),
                 qPrintable(bodies.join(QLatin1String(" | "))));
        QVERIFY2(bodies.contains(iso(QStringLiteral("Alice")) + QStringLiteral(" hat ") + iso(QStringLiteral("Bob")) + QStringLiteral(" eingeladen.")),
                 qPrintable(bodies.join(QLatin1String(" | "))));
        QVERIFY2(bodies.contains(QStringLiteral("Verschlüsselung wurde aktiviert.")),
                 qPrintable(bodies.join(QLatin1String(" | "))));
    }

    // A switch re-reads the words: one dataChanged over the model carrying the
    // body and the group entries.
    void retranslateAnnouncesTheWords()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        client.mirror = { matrix::rust_timeline::eventFromItemJson(
            stateItem(QStringLiteral("$j"), QStringLiteral("membership"),
                      QStringLiteral("member_joined"), QStringLiteral("Bob"),
                      QStringLiteral("Bob joined the room.")), kRoom) };
        model.setRoomId(kRoom);
        QVERIFY(model.rowCount() > 0);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.retranslate();
        QCOMPARE(changed.count(), 1);
        const auto roles = changed.first().at(2).value<QList<int>>();
        QVERIFY(roles.contains(TimelineModel::BodyRole));
        QVERIFY(roles.contains(TimelineModel::StateGroupEntriesRole));
        QCOMPARE(changed.first().at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(changed.first().at(1).value<QModelIndex>().row(), model.rowCount() - 1);
    }

    // The room list keeps the sentence as a kind, so it too follows a switch.
    void thePreviewIsAKindThatFollowsTheLanguage()
    {
        const TimelineEvent invited = matrix::rust_timeline::eventFromItemJson(
            stateItem(QStringLiteral("$i"), QStringLiteral("membership"),
                      QStringLiteral("member_invited"), QStringLiteral("Bob"),
                      QStringLiteral("@alice:example.org invited Bob.")), kRoom);
        const auto token = matrix::preview::summaryToken(invited);
        QCOMPARE(token.kind, Kind::StateSentence);
        RoomInfo room;
        room.setPreview(token.kind, token.arg);
        QCOMPARE(room.lastMessagePreview, iso(QStringLiteral("Alice")) + QStringLiteral(" invited ") + iso(QStringLiteral("Bob")) + QStringLiteral("."));
        German german;
        Installed on(&german);
        QVERIFY(room.retranslatePreview());
        QCOMPARE(room.lastMessagePreview, iso(QStringLiteral("Alice")) + QStringLiteral(" hat ") + iso(QStringLiteral("Bob")) + QStringLiteral(" eingeladen."));
    }

    // A name carrying the field separator cannot move the other fields.
    void aNameCannotShiftTheFields()
    {
        const QString arg = matrix::preview::packStateSentence(
            QStringLiteral("member_invited"),
            QStringLiteral("Al") + QChar(0x1f) + QStringLiteral("member_banned"),
            QStringLiteral("Bob"));
        QCOMPARE(matrix::preview::render(Kind::StateSentence, arg),
                 iso(QStringLiteral("Almember_banned")) + QStringLiteral(" invited ")
                     + iso(QStringLiteral("Bob")) + QStringLiteral("."));
    }

    // Review: never an empty row. A name this build does not know (a newer
    // build's, from the cache) shows the bridge's English, else the generic
    // sentence. Old code: an empty preview.
    void anUnknownSentenceFallsBackToWords()
    {
        const QString withEnglish = matrix::preview::packStateSentence(
            QStringLiteral("room_hologram_projected"), QStringLiteral("Alice"), {},
            QStringLiteral("Alice projected a hologram."));
        QCOMPARE(matrix::preview::render(Kind::StateSentence, withEnglish),
                 QStringLiteral("Alice projected a hologram."));
        const QString bare = matrix::preview::packStateSentence(
            QStringLiteral("room_hologram_projected"), QStringLiteral("Alice"), {});
        QCOMPARE(matrix::preview::render(Kind::StateSentence, bare),
                 iso(QStringLiteral("Alice")) + QStringLiteral(" updated room settings."));
    }

    // Review: a right-to-left name must not reorder the sentence. Each name
    // is a first-strong isolate inside it, and the timeline row carries them.
    void namesAreIsolatedInTheSentence()
    {
        const QString hebrew = QString::fromUtf8("\u05D3\u05E0\u05D4"); // "Dana"
        const QString text = matrix::preview::stateSentence(
            u"member_invited", hebrew, QStringLiteral("Bob"));
        QCOMPARE(text, iso(hebrew) + QStringLiteral(" invited ") + iso(QStringLiteral("Bob"))
                           + QStringLiteral("."));
        // Without isolation (the export), the plain words.
        QCOMPARE(matrix::preview::stateSentence(u"member_invited", hebrew,
                                                QStringLiteral("Bob"), false),
                 hebrew + QStringLiteral(" invited Bob."));
        // A name that brings its own isolates cannot close ours early: the
        // sanitizer strips them before the sentence is built.
        const QString sneaky = QChar(0x2069) + QStringLiteral("x") + QChar(0x2066);
        QVERIFY(!matrix::presentable_name::sanitized(sneaky).contains(QChar(0x2069)));
    }

    // Review: the export writes the sentence in the exporting user's app
    // language, without isolates. Old code: the bridge's English body.
    void theExportWritesTheSentenceInTheAppLanguage()
    {
        TimelineEvent joined = matrix::rust_timeline::eventFromItemJson(
            stateItem(QStringLiteral("$j"), QStringLiteral("membership"),
                      QStringLiteral("member_joined"), QStringLiteral("Bob"),
                      QStringLiteral("Bob joined the room.")), kRoom);
        TimelineEvent unnamed = matrix::rust_timeline::eventFromItemJson(
            stateItem(QStringLiteral("$x"), QStringLiteral("m.room.name"),
                      QString(), {}, QStringLiteral("@alice:example.org changed the room name.")),
            kRoom);
        roomexport::Options options;
        options.roomName = QStringLiteral("Room");
        German german;
        Installed on(&german);
        const QString text = roomexport::renderPlainText({ joined, unnamed }, options);
        QVERIFY2(text.contains(QStringLiteral("Bob ist dem Raum beigetreten.")), qPrintable(text));
        QVERIFY2(!text.contains(QChar(0x2068)), "isolates in an exported text file");
        // A row the bridge did not name keeps its body, as before.
        QVERIFY2(text.contains(QStringLiteral("@alice:example.org changed the room name.")),
                 qPrintable(text));
        const QString html = QString::fromUtf8(roomexport::renderHtml({ joined }, options));
        QVERIFY2(html.contains(QStringLiteral("Bob ist dem Raum beigetreten.")), "html export");
        const QString json = roomexport::renderJson({ joined }, options);
        QVERIFY2(json.contains(QStringLiteral("Bob ist dem Raum beigetreten.")), qPrintable(json));
    }

    // The same table as rust/src/timeline.rs (`state_sentence`,
    // `membership_sentence`, `failed_state_sentence`, and the English in
    // `state_sentence_english`, which its tests pin): every name the bridge can
    // send is worded here, and in English exactly as the bridge's body.
    void everySentenceTheBridgeSendsIsWordedHere()
    {
        const QString a = QStringLiteral("@a:b.c");
        const QString b = QStringLiteral("Bob");
        const QList<QPair<QString, QString>> table = {
            { QStringLiteral("room_created"), a + QStringLiteral(" created the room.") },
            { QStringLiteral("room_name_changed"), a + QStringLiteral(" changed the room name.") },
            { QStringLiteral("room_topic_changed"), a + QStringLiteral(" changed the room topic.") },
            { QStringLiteral("room_avatar_changed"), a + QStringLiteral(" changed the room avatar.") },
            { QStringLiteral("encryption_enabled"), QStringLiteral("Encryption was enabled.") },
            { QStringLiteral("room_upgraded"), a + QStringLiteral(" upgraded this room.") },
            { QStringLiteral("call_started"), a + QStringLiteral(" started a call.") },
            { QStringLiteral("video_call_started"), a + QStringLiteral(" started a video call.") },
            { QStringLiteral("policy_rule_updated"), a + QStringLiteral(" updated a moderation rule.") },
            { QStringLiteral("settings_updated"), a + QStringLiteral(" updated room settings.") },
            { QStringLiteral("space_parent_removed"), a + QStringLiteral(" removed this room from a space.") },
            { QStringLiteral("policy_rule_removed"), a + QStringLiteral(" removed a moderation rule.") },
            { QStringLiteral("member_joined"), b + QStringLiteral(" joined the room.") },
            { QStringLiteral("member_left"), b + QStringLiteral(" left the room.") },
            { QStringLiteral("member_invited"), a + QStringLiteral(" invited Bob.") },
            { QStringLiteral("invitation_accepted"), b + QStringLiteral(" accepted the invitation.") },
            { QStringLiteral("member_kicked"), a + QStringLiteral(" removed Bob from the room.") },
            { QStringLiteral("member_banned"), a + QStringLiteral(" banned Bob.") },
            { QStringLiteral("member_unbanned"), a + QStringLiteral(" unbanned Bob.") },
            { QStringLiteral("invitation_rejected"), b + QStringLiteral(" rejected the invitation.") },
            { QStringLiteral("invitation_revoked"), a + QStringLiteral(" revoked Bob's invitation.") },
            { QStringLiteral("membership_changed"), QStringLiteral("Membership for Bob changed.") },
        };
        QCOMPARE(table.size(), 22);
        // The lookup set and the renderer name the same sentences.
        QCOMPARE(int(matrix::preview::stateSentenceNames().size()), int(table.size()));
        for (const auto &[sentence, english] : table) {
            QVERIFY2(matrix::preview::isStateSentence(sentence), qPrintable(sentence));
            QCOMPARE(matrix::preview::stateSentence(sentence, a, b, /*isolate=*/false),
                     english);
        }
        QVERIFY(!matrix::preview::isStateSentence(QStringLiteral("")));
        QVERIFY(!matrix::preview::isStateSentence(QStringLiteral("m.room.name")));
    }
};

QTEST_GUILESS_MAIN(StateSentenceTest)
#include "StateSentenceTest.moc"
