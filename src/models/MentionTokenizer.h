#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace mention {

struct MentionRef {
    QString userId;
    QString displayText;
    int start = 0;
    int length = 0;
};

struct Token {
    bool active = false;
    int start = 0;
    QString query;
};

Token activeToken(const QString &text, int cursorPos);

struct InsertResult {
    QString text;
    int cursorPos = 0;
    MentionRef ref;
};

InsertResult buildInsertion(const QString &text, int tokenStart, int cursorPos,
                            const QString &userId, const QString &displayName);

QList<MentionRef> shiftRefs(const QList<MentionRef> &refs,
                            const QString &oldText, const QString &newText);

struct Expansion {
    QString body;
    QStringList userIds;
};

Expansion expand(const QString &text, const QList<MentionRef> &refs);

struct Recovery {
    QString text;
    QList<MentionRef> refs;
};
Recovery recoverFromBody(const QString &body);

// Recovery for display-text plain bodies (sends after the plain-body
// reduction): the body carries "@Alice hi" with no markdown, and the
// mention identities live only in the sanitized formatted body's
// "mention:<mxid>" anchors. Each anchor's label is matched against the
// next unconsumed occurrence in the plain body; an unmatched anchor is
// dropped fail-closed (the mention degrades to plain text, never a wrong
// range).
QList<MentionRef> refsFromSanitizedHtml(const QString &plainBody,
                                        const QString &sanitizedHtml);

}
