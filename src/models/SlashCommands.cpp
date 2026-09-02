#include "models/SlashCommands.h"

#include "models/MentionTokenizer.h"

#include <QCoreApplication>
#include <algorithm>
#include <QRegularExpression>
#include <QUrl>

namespace SlashCommands {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("SlashCommands", text);
}

} // namespace

const QList<Command> &registry()
{
    // Built lazily so the descriptions pick up an installed translator.
    // The order here is the completion popup's order: sends first, then
    // room administration, then local conveniences.
    static const QList<Command> commands = {
        { QStringLiteral("me"), QStringLiteral("<message>"),
          tr("Send an action message"), true, {} },
        { QStringLiteral("shrug"), QStringLiteral("[message]"),
          tr("Append ¯\\_(ツ)_/¯ to a message"), false, {} },
        { QStringLiteral("spoiler"), QStringLiteral("<message>"),
          tr("Send the message as a click-to-reveal spoiler"), true, {} },
        { QStringLiteral("join"), QStringLiteral("<room address or id>"),
          tr("Join a room"), true, {} },
        { QStringLiteral("invite"), QStringLiteral("<user id>"),
          tr("Invite a user to this room"), true,
          QStringLiteral("invite") },
        { QStringLiteral("kick"), QStringLiteral("<user id> [reason]"),
          tr("Remove a user from this room"), true, QStringLiteral("kick") },
        { QStringLiteral("ban"), QStringLiteral("<user id> [reason]"),
          tr("Ban a user from this room"), true, QStringLiteral("ban") },
        { QStringLiteral("unban"), QStringLiteral("<user id>"),
          tr("Unban a user from this room"), true, QStringLiteral("unban") },
        { QStringLiteral("topic"), QStringLiteral("<topic>"),
          tr("Change this room's topic"), true, QStringLiteral("topic") },
        { QStringLiteral("roomname"), QStringLiteral("<name>"),
          tr("Change this room's name"), true, QStringLiteral("roomname") },
        { QStringLiteral("nick"), QStringLiteral("<display name>"),
          tr("Change your display name"), true, {} },
        { QStringLiteral("markdown"), QString(),
          tr("Switch between Markdown and rich-text composing"), false, {} },
        { QStringLiteral("clear"), QString(),
          tr("Clear the message box and queued attachments"), false, {} },
    };
    return commands;
}

const Command *find(const QString &name)
{
    const QString lowered = name.toLower();
    for (const Command &command : registry()) {
        if (command.name == lowered)
            return &command;
    }
    return nullptr;
}

Parse parse(const QString &text)
{
    Parse result;
    if (!text.startsWith(QLatin1Char('/')))
        return result;
    if (text.startsWith(QStringLiteral("//"))) {
        result.kind = Parse::Escaped;
        result.literalText = text.mid(1);
        return result;
    }
    // "/", "/ x", "/1x", "/¯\_(ツ)_/¯": ordinary text. Only a letter can
    // begin a command word — this is what keeps a slash-leading emoticon or
    // path from becoming a surprise error.
    if (text.size() < 2 || !text.at(1).isLetter())
        return result;

    int wordEnd = 1;
    while (wordEnd < text.size() && !text.at(wordEnd).isSpace())
        ++wordEnd;
    const QString word = text.mid(1, wordEnd - 1);
    // A command word is letters only; "/me2" or "/me…" is text, not a typo
    // of /me — refusing to guess keeps false positives near zero.
    for (const QChar &c : word) {
        if (!c.isLetter())
            return result;
    }
    result.name = word.toLower();
    result.args = text.mid(wordEnd).trimmed();
    result.kind = find(result.name) ? Parse::Known : Parse::Unknown;
    return result;
}

QString takeUserIdToken(QString &rest)
{
    rest = rest.trimmed();
    if (rest.isEmpty())
        return {};
    // Markdown mention link first — the label may contain spaces, so a
    // whitespace split cannot find its end.
    // The expansion percent-encodes the MXID ("%40bob%3Aexample.org"), so
    // the id is decoded AFTER capture and checked for its '@' then.
    static const QRegularExpression mentionLink(QStringLiteral(
        "^\\[[^\\]]*\\]\\(https://matrix\\.to/#/([^)?]+)[^)]*\\)"));
    const QRegularExpressionMatch link = mentionLink.match(rest);
    if (link.hasMatch()) {
        const QString decoded =
            QUrl::fromPercentEncoding(link.captured(1).toUtf8());
        if (decoded.startsWith(QLatin1Char('@'))) {
            rest = rest.mid(link.capturedLength()).trimmed();
            return decoded;
        }
    }
    const int space = rest.indexOf(QLatin1Char(' '));
    const QString token = space < 0 ? rest : rest.left(space);
    rest = space < 0 ? QString() : rest.mid(space + 1).trimmed();
    return token;
}

namespace {

// Structural MXID shape only — the server owns real validation.
bool looksLikeUserId(const QString &id)
{
    return id.size() >= 4 && id.startsWith(QLatin1Char('@'))
        && id.contains(QLatin1Char(':'));
}

Outcome refuse(const QString &error)
{
    Outcome o;
    o.error = error;
    return o;
}

Outcome done(bool retireDraft = true)
{
    Outcome o;
    o.retireDraft = retireDraft;
    return o;
}

} // namespace

Outcome execute(const Parse &parsed, const QStringList &mentionIds,
                const Actions &actions)
{
    const Command *command = find(parsed.name);
    if (!command)
        return refuse(tr("Unknown command."));
    if (command->requiresArgs && parsed.args.isEmpty()) {
        return refuse(tr("/%1 needs arguments: /%1 %2")
                          .arg(command->name, command->argsHint));
    }
    const QString unavailable =
        tr("/%1 is not available from here.").arg(command->name);

    const QString &name = command->name;
    if (name == QLatin1String("me")) {
        if (!actions.send)
            return refuse(unavailable);
        actions.send(parsed.args, mentionIds,
                     { { QStringLiteral("msgtype"), QStringLiteral("emote") } });
        return done();
    }
    if (name == QLatin1String("shrug")) {
        if (!actions.send)
            return refuse(unavailable);
        const QString shrug = QStringLiteral("¯\\_(ツ)_/¯");
        if (mentionIds.isEmpty()) {
            // The plain lane sends the shrug verbatim — markdown would eat
            // the escaped underscore.
            const QString body = parsed.args.isEmpty()
                ? shrug
                : parsed.args + QLatin1Char(' ') + shrug;
            actions.send(body, mentionIds,
                         { { QStringLiteral("format"), QStringLiteral("plain") } });
        } else {
            // With mentions the matrix.to links need the markdown lane, so
            // the shrug is markdown-escaped to survive the parse.
            const QString escaped = QStringLiteral("¯\\\\\\_(ツ)\\_/¯");
            actions.send(parsed.args + QLatin1Char(' ') + escaped, mentionIds,
                         QVariantMap());
        }
        return done();
    }
    if (name == QLatin1String("spoiler")) {
        if (!actions.send)
            return refuse(unavailable);
        // Matrix spoiler (spec §11.36): a data-mx-spoiler span in the
        // formatted body; the plain body carries the text, matching
        // Element. The content is HTML-escaped here and strict-sanitized
        // again in Rust.
        // A mention pill in the argument is markdown link syntax in
        // `parsed.args` (the composer's send-time expansion); the spoiler
        // is a formatted send, so the pill becomes a real matrix.to anchor
        // in the HTML and its display text in the plain body.
        const mention::Recovery recovered = mention::recoverFromBody(parsed.args);
        QString inner;
        int pos = 0;
        QList<mention::MentionRef> refs = recovered.refs;
        std::sort(refs.begin(), refs.end(),
                  [](const mention::MentionRef &a, const mention::MentionRef &b) {
                      return a.start < b.start;
                  });
        for (const mention::MentionRef &ref : refs) {
            if (ref.start < pos)
                continue;
            inner += recovered.text.mid(pos, ref.start - pos).toHtmlEscaped();
            inner += QStringLiteral("<a href=\"https://matrix.to/#/")
                + QString::fromLatin1(QUrl::toPercentEncoding(ref.userId))
                + QStringLiteral("\">") + ref.displayText.toHtmlEscaped()
                + QStringLiteral("</a>");
            pos = ref.start + ref.length;
        }
        inner += recovered.text.mid(pos).toHtmlEscaped();
        const QString html = QStringLiteral("<span data-mx-spoiler>") + inner
            + QStringLiteral("</span>");
        actions.send(recovered.text, mentionIds,
                     { { QStringLiteral("format"), QStringLiteral("html") },
                       { QStringLiteral("html"), html } });
        return done();
    }
    if (name == QLatin1String("join")) {
        if (!actions.join)
            return refuse(unavailable);
        const QString target = parsed.args.section(QLatin1Char(' '), 0, 0);
        if (!target.startsWith(QLatin1Char('#'))
            && !target.startsWith(QLatin1Char('!'))) {
            return refuse(
                tr("/join needs a room address (#room:server) or room id."));
        }
        actions.join(target);
        return done();
    }
    if (name == QLatin1String("invite") || name == QLatin1String("kick")
        || name == QLatin1String("ban") || name == QLatin1String("unban")) {
        QString rest = parsed.args;
        const QString userId = takeUserIdToken(rest);
        if (!looksLikeUserId(userId))
            return refuse(tr("/%1 needs a user id (@user:server).").arg(name));
        if (name == QLatin1String("invite")) {
            if (!actions.invite)
                return refuse(unavailable);
            actions.invite(userId);
        } else if (name == QLatin1String("kick")) {
            if (!actions.kick)
                return refuse(unavailable);
            actions.kick(userId, rest);
        } else if (name == QLatin1String("ban")) {
            if (!actions.ban)
                return refuse(unavailable);
            actions.ban(userId, rest);
        } else {
            if (!actions.unban)
                return refuse(unavailable);
            actions.unban(userId, rest);
        }
        return done();
    }
    if (name == QLatin1String("topic")) {
        if (!actions.setTopic)
            return refuse(unavailable);
        // Display text, never pill markdown, in a topic.
        actions.setTopic(mention::recoverFromBody(parsed.args).text);
        return done();
    }
    if (name == QLatin1String("roomname")) {
        if (!actions.setRoomName)
            return refuse(unavailable);
        actions.setRoomName(mention::recoverFromBody(parsed.args).text);
        return done();
    }
    if (name == QLatin1String("nick")) {
        if (!actions.setDisplayName)
            return refuse(unavailable);
        actions.setDisplayName(mention::recoverFromBody(parsed.args).text);
        return done();
    }
    if (name == QLatin1String("markdown")) {
        if (!actions.toggleComposerMode)
            return refuse(unavailable);
        actions.toggleComposerMode();
        // The host removes the command text itself but keeps no draft to
        // retire — there is none, the command was the whole message.
        return done(false);
    }
    if (name == QLatin1String("clear")) {
        if (!actions.clearComposer)
            return refuse(unavailable);
        actions.clearComposer();
        return done(false);
    }
    return refuse(tr("Unknown command."));
}

QList<Command> completions(const QString &text)
{
    QList<Command> matches;
    if (!text.startsWith(QLatin1Char('/')) || text.startsWith(QStringLiteral("//")))
        return matches;
    // Completion is offered only while the command WORD is being typed;
    // once whitespace follows, the user is on the arguments.
    const QString rest = text.mid(1);
    for (const QChar &c : rest) {
        if (c.isSpace())
            return matches;
        if (!c.isLetter())
            return matches;
    }
    const QString prefix = rest.toLower();
    for (const Command &command : registry()) {
        if (command.name.startsWith(prefix))
            matches.append(command);
    }
    return matches;
}

} // namespace SlashCommands
