#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

// Composer slash commands: registry and parser, pure and QObject-free so
// every rule is unit-testable.
//
// Separate from the Ctrl+K command palette, which holds argument-free app
// actions. A slash command is a typed, argument-carrying, room-scoped verb;
// execution reuses the existing MatrixClient moderation and send APIs.
//
// Parse rules (Element's observable behaviour):
//   * only a message beginning with "/" can be a command;
//   * "//rest" is the escape and sends "/rest" as literal text;
//   * "/word args…" splits on the first whitespace; the name is
//     case-insensitive;
//   * "/" followed by anything but a letter ("/ ", "/1", an emoticon) is
//     ordinary text, never a command or an error.
namespace SlashCommands {

struct Command {
    QString name;        // without the slash, lowercase ("me", "join", …)
    QString argsHint;    // "<message>", "[reason]" …; empty = no arguments
    QString description; // one line for the completion popup
    bool requiresArgs = false;
    // Key into the permissions map QML supplies (RoomInfoController's can*
    // booleans); empty means always available. A missing key counts as allowed:
    // the server enforces, and hiding a command on unknown data would mislead.
    QString permissionKey;
};

// The full registry, in the order the completion popup shows.
const QList<Command> &registry();

// nullptr when no such command exists.
const Command *find(const QString &name);

struct Parse {
    enum Kind {
        NotACommand, // ordinary message text (includes "/", "/ x", "/1x")
        Escaped,     // "//…" — send literalText as an ordinary message
        Known,       // a registered command; name + args filled
        Unknown,     // "/word" that matches nothing; name filled
    };
    Kind kind = NotACommand;
    QString name;
    QString args;        // trimmed remainder after the command word
    QString literalText; // Escaped only: the text with one slash removed
};

Parse parse(const QString &text);

// Commands whose name starts with the partial word the user has typed so
// far. Only meaningful while the text is "/word-in-progress" with no
// whitespace yet; empty otherwise (a completed command word offers nothing).
QList<Command> completions(const QString &text);

// ---- Execution, shared by every composer.
//
// The room composer and the thread composer own different send lanes, so
// execution is expressed against callbacks: command semantics live here once
// and each host supplies how content goes out. A callback left empty makes
// its command refuse with an error rather than silently doing nothing.
struct Actions {
    // Send composed content through the host's context. `spec` is the body spec
    // ({format, html, msgtype}); empty means markdown.
    std::function<void(const QString &body, const QStringList &mentionIds,
                       const QVariantMap &spec)> send;
    std::function<void(const QString &target)> join;
    std::function<void(const QString &userId)> invite;
    std::function<void(const QString &userId, const QString &reason)> kick;
    std::function<void(const QString &userId, const QString &reason)> ban;
    std::function<void(const QString &userId, const QString &reason)> unban;
    std::function<void(const QString &topic)> setTopic;
    std::function<void(const QString &name)> setRoomName;
    std::function<void(const QString &name)> setDisplayName;
    std::function<void()> toggleComposerMode;
    std::function<void()> clearComposer;
};

struct Outcome {
    // Non-empty = the command was refused (missing/invalid arguments, or an
    // action this host does not offer) and NOTHING happened; the host keeps
    // the draft and shows the text.
    QString error;
    // The command ran; the host should retire its draft the way a send does
    // (false only for /markdown, which keeps the text, and /clear, which
    // cleared it itself).
    bool retireDraft = false;
};

// The parsed text's MXID-bearing first token, accepting a bare
// "@user:server" or the matrix.to markdown link mention expansion produces;
// strips it from `rest`. Exposed for tests.
QString takeUserIdToken(QString &rest);

Outcome execute(const Parse &parsed, const QStringList &mentionIds,
                const Actions &actions);

} // namespace SlashCommands
