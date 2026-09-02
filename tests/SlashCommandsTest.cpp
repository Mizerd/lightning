// v0.9 slash commands: the parser and registry contract. Everything here is
// pure (no composer, no client), which is what lets the corner cases —
// escapes, emoticons, non-letter words — be pinned exactly.

#include "models/SlashCommands.h"

#include <QtTest/QtTest>

class SlashCommandsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theRegistryCarriesEveryRequiredCommand()
    {
        // The task list this feature answers to, verbatim. A command
        // silently dropped from the registry must fail here by name.
        const QStringList required = {
            QStringLiteral("me"),      QStringLiteral("join"),
            QStringLiteral("invite"),  QStringLiteral("kick"),
            QStringLiteral("ban"),     QStringLiteral("unban"),
            QStringLiteral("topic"),   QStringLiteral("nick"),
            QStringLiteral("roomname"), QStringLiteral("shrug"),
            QStringLiteral("spoiler"), QStringLiteral("markdown"),
            QStringLiteral("clear"),
        };
        for (const QString &name : required)
            QVERIFY2(SlashCommands::find(name) != nullptr,
                     qPrintable(QStringLiteral("missing /%1").arg(name)));
    }

    void ordinaryTextIsNeverACommand()
    {
        QCOMPARE(SlashCommands::parse(QStringLiteral("hello /me")).kind,
                 SlashCommands::Parse::NotACommand);
        QCOMPARE(SlashCommands::parse(QStringLiteral("look at /tmp/x")).kind,
                 SlashCommands::Parse::NotACommand);
        QCOMPARE(SlashCommands::parse(QString()).kind,
                 SlashCommands::Parse::NotACommand);
    }

    void aBareOrNonLetterSlashIsText()
    {
        // "/", "/ x", "/1", an emoticon: text, never a command and never an
        // "unknown command" error.
        QCOMPARE(SlashCommands::parse(QStringLiteral("/")).kind,
                 SlashCommands::Parse::NotACommand);
        QCOMPARE(SlashCommands::parse(QStringLiteral("/ me")).kind,
                 SlashCommands::Parse::NotACommand);
        QCOMPARE(SlashCommands::parse(QStringLiteral("/1st")).kind,
                 SlashCommands::Parse::NotACommand);
        QCOMPARE(SlashCommands::parse(QStringLiteral("/¯\\_(ツ)_/¯")).kind,
                 SlashCommands::Parse::NotACommand);
        // Digits INSIDE the word break it too — "/me2" is text, not a typo.
        QCOMPARE(SlashCommands::parse(QStringLiteral("/me2 hi")).kind,
                 SlashCommands::Parse::NotACommand);
    }

    void doubleSlashEscapesToALiteralSingleSlash()
    {
        const auto p = SlashCommands::parse(QStringLiteral("//shrug it off"));
        QCOMPARE(p.kind, SlashCommands::Parse::Escaped);
        QCOMPARE(p.literalText, QStringLiteral("/shrug it off"));
    }

    void knownCommandSplitsNameAndArgs()
    {
        const auto p = SlashCommands::parse(
            QStringLiteral("/kick @bob:example.org being rude"));
        QCOMPARE(p.kind, SlashCommands::Parse::Known);
        QCOMPARE(p.name, QStringLiteral("kick"));
        QCOMPARE(p.args, QStringLiteral("@bob:example.org being rude"));
        // Case-insensitive name, args untouched.
        const auto upper = SlashCommands::parse(QStringLiteral("/ME waves"));
        QCOMPARE(upper.kind, SlashCommands::Parse::Known);
        QCOMPARE(upper.name, QStringLiteral("me"));
        QCOMPARE(upper.args, QStringLiteral("waves"));
    }

    void unknownCommandIsReportedNotSwallowed()
    {
        const auto p = SlashCommands::parse(QStringLiteral("/frobnicate x"));
        QCOMPARE(p.kind, SlashCommands::Parse::Unknown);
        QCOMPARE(p.name, QStringLiteral("frobnicate"));
    }

    void completionsMatchThePrefixBeingTyped()
    {
        const auto both = SlashCommands::completions(QStringLiteral("/"));
        QCOMPARE(both.size(), SlashCommands::registry().size());
        const auto m = SlashCommands::completions(QStringLiteral("/m"));
        QStringList names;
        for (const auto &c : m)
            names.append(c.name);
        QVERIFY(names.contains(QStringLiteral("me")));
        QVERIFY(names.contains(QStringLiteral("markdown")));
        QVERIFY(!names.contains(QStringLiteral("kick")));
        // Once whitespace follows, the user is on the arguments — nothing
        // is offered.
        QVERIFY(SlashCommands::completions(QStringLiteral("/me ")).isEmpty());
        QVERIFY(SlashCommands::completions(QStringLiteral("//m")).isEmpty());
        QVERIFY(SlashCommands::completions(QStringLiteral("m")).isEmpty());
    }

    void requiredArgsAreDeclaredForTheArgumentTakingCommands()
    {
        // The composer refuses these without arguments; the flag is the
        // contract it reads.
        for (const char *name : { "me", "spoiler", "join", "invite", "kick",
                                  "ban", "unban", "topic", "roomname",
                                  "nick" })
            QVERIFY2(SlashCommands::find(QLatin1String(name))->requiresArgs,
                     name);
        QVERIFY(!SlashCommands::find(QStringLiteral("shrug"))->requiresArgs);
        QVERIFY(!SlashCommands::find(QStringLiteral("clear"))->requiresArgs);
        QVERIFY(!SlashCommands::find(QStringLiteral("markdown"))->requiresArgs);
    }

    // ── execute(): the shared semantics, driven with lambdas ──────────

    void executeRoutesEachVerbToItsAction()
    {
        QStringList log;
        SlashCommands::Actions a;
        a.send = [&](const QString &b, const QStringList &ids,
                     const QVariantMap &spec) {
            log << QStringLiteral("send|%1|%2|%3")
                       .arg(b, ids.join(QLatin1Char(',')),
                            spec.value(QStringLiteral("msgtype")).toString()
                                + QLatin1Char('/')
                                + spec.value(QStringLiteral("format")).toString());
        };
        a.join = [&](const QString &t) { log << QStringLiteral("join|") + t; };
        a.invite = [&](const QString &u) { log << QStringLiteral("invite|") + u; };
        a.kick = [&](const QString &u, const QString &r) {
            log << QStringLiteral("kick|%1|%2").arg(u, r);
        };
        a.ban = [&](const QString &u, const QString &r) {
            log << QStringLiteral("ban|%1|%2").arg(u, r);
        };
        a.unban = [&](const QString &u, const QString &r) {
            log << QStringLiteral("unban|%1|%2").arg(u, r);
        };
        a.setTopic = [&](const QString &t) { log << QStringLiteral("topic|") + t; };
        a.setRoomName = [&](const QString &n) { log << QStringLiteral("name|") + n; };
        a.setDisplayName = [&](const QString &n) { log << QStringLiteral("nick|") + n; };
        a.toggleComposerMode = [&] { log << QStringLiteral("mode"); };
        a.clearComposer = [&] { log << QStringLiteral("clear"); };

        const auto run = [&](const QString &text) {
            return SlashCommands::execute(SlashCommands::parse(text), {}, a);
        };
        QVERIFY(run(QStringLiteral("/me waves")).retireDraft);
        QCOMPARE(log.last(), QStringLiteral("send|waves||emote/"));
        run(QStringLiteral("/shrug"));
        QCOMPARE(log.last(), QStringLiteral("send|¯\\_(ツ)_/¯||/plain"));
        run(QStringLiteral("/kick @bob:x rude"));
        QCOMPARE(log.last(), QStringLiteral("kick|@bob:x|rude"));
        run(QStringLiteral("/unban @bob:x"));
        QCOMPARE(log.last(), QStringLiteral("unban|@bob:x|"));
        run(QStringLiteral("/invite @c:x"));
        QCOMPARE(log.last(), QStringLiteral("invite|@c:x"));
        run(QStringLiteral("/join #r:x extra"));
        QCOMPARE(log.last(), QStringLiteral("join|#r:x"));
        run(QStringLiteral("/topic hello"));
        QCOMPARE(log.last(), QStringLiteral("topic|hello"));
        run(QStringLiteral("/roomname The Bridge"));
        QCOMPARE(log.last(), QStringLiteral("name|The Bridge"));
        run(QStringLiteral("/nick New Name"));
        QCOMPARE(log.last(), QStringLiteral("nick|New Name"));
        QVERIFY(!run(QStringLiteral("/markdown")).retireDraft);
        QCOMPARE(log.last(), QStringLiteral("mode"));
        QVERIFY(!run(QStringLiteral("/clear")).retireDraft);
        QCOMPARE(log.last(), QStringLiteral("clear"));
    }

    void executeRefusesWhatTheHostDoesNotOffer()
    {
        // A host that supplies no moderation lane gets an honest refusal,
        // never a silent no-op — and nothing else runs.
        SlashCommands::Actions sendOnly;
        int sends = 0;
        sendOnly.send = [&](const QString &, const QStringList &,
                            const QVariantMap &) { ++sends; };
        const auto out = SlashCommands::execute(
            SlashCommands::parse(QStringLiteral("/kick @bob:x")), {}, sendOnly);
        QVERIFY(!out.error.isEmpty());
        QVERIFY(!out.retireDraft);
        QCOMPARE(sends, 0);
    }

    void executeValidatesArgumentsBeforeActing()
    {
        SlashCommands::Actions a;
        int acted = 0;
        a.kick = [&](const QString &, const QString &) { ++acted; };
        a.join = [&](const QString &) { ++acted; };
        a.send = [&](const QString &, const QStringList &, const QVariantMap &) {
            ++acted;
        };
        QVERIFY(!SlashCommands::execute(
                     SlashCommands::parse(QStringLiteral("/kick bob")), {}, a)
                     .error.isEmpty());
        QVERIFY(!SlashCommands::execute(
                     SlashCommands::parse(QStringLiteral("/join somewhere")), {}, a)
                     .error.isEmpty());
        QVERIFY(!SlashCommands::execute(
                     SlashCommands::parse(QStringLiteral("/me")), {}, a)
                     .error.isEmpty());
        QCOMPARE(acted, 0);
    }

    void userIdTokenAcceptsMentionLinksAndBareIds()
    {
        QString rest = QStringLiteral("@bob:example.org being rude");
        QCOMPARE(SlashCommands::takeUserIdToken(rest),
                 QStringLiteral("@bob:example.org"));
        QCOMPARE(rest, QStringLiteral("being rude"));
        rest = QStringLiteral(
            "[Bob Smith](https://matrix.to/#/%40bob%3Aexample.org) spam");
        QCOMPARE(SlashCommands::takeUserIdToken(rest),
                 QStringLiteral("@bob:example.org"));
        QCOMPARE(rest, QStringLiteral("spam"));
    }

    void moderationCommandsCarryTheirPermissionKeys()
    {
        QCOMPARE(SlashCommands::find(QStringLiteral("kick"))->permissionKey,
                 QStringLiteral("kick"));
        QCOMPARE(SlashCommands::find(QStringLiteral("ban"))->permissionKey,
                 QStringLiteral("ban"));
        QCOMPARE(SlashCommands::find(QStringLiteral("unban"))->permissionKey,
                 QStringLiteral("unban"));
        QCOMPARE(SlashCommands::find(QStringLiteral("invite"))->permissionKey,
                 QStringLiteral("invite"));
        QCOMPARE(SlashCommands::find(QStringLiteral("topic"))->permissionKey,
                 QStringLiteral("topic"));
        QCOMPARE(SlashCommands::find(QStringLiteral("roomname"))->permissionKey,
                 QStringLiteral("roomname"));
        // Sends and local conveniences need no power level.
        QVERIFY(SlashCommands::find(QStringLiteral("me"))->permissionKey.isEmpty());
        QVERIFY(SlashCommands::find(QStringLiteral("clear"))->permissionKey.isEmpty());
    }
};

QTEST_APPLESS_MAIN(SlashCommandsTest)
#include "SlashCommandsTest.moc"
