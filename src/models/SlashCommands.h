#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace SlashCommands {

struct Command {
    QString name;
    QString argsHint;
    QString description;
    bool requiresArgs = false;
    // Key into the permissions map QML supplies (RoomInfoController's can*
    // booleans); empty means always available. A missing key counts as allowed:
    // the server enforces, and hiding a command on unknown data would mislead.
    QString permissionKey;
};

const QList<Command> &registry();

const Command *find(const QString &name);

struct Parse {
    enum Kind {
        NotACommand,
        Escaped,
        Known,
        Unknown,
    };
    Kind kind = NotACommand;
    QString name;
    QString args;
    QString literalText;
};

Parse parse(const QString &text);

QList<Command> completions(const QString &text);

struct Actions {
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
    bool retireDraft = false;
};

QString takeUserIdToken(QString &rest);

Outcome execute(const Parse &parsed, const QStringList &mentionIds,
                const Actions &actions);

}
