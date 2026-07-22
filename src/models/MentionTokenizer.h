#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// v0.7 outgoing @-mentions — pure, presentation-agnostic helpers shared by the
// room composer (MessageComposer) and the thread composer (ThreadController).
//
// Nothing here talks to Matrix, the network, or member data: it only detects
// the active @-token at the cursor, keeps a list of inserted mention ranges in
// sync with the visible editor text across single edits, and expands those
// ranges into matrix.to markdown links at send time. Never logs message text.
namespace mention {

// One inserted mention. `displayText` is the exact human-readable slice in the
// editor (for example "@Alice", WITHOUT the trailing space); `start`/`length`
// address that slice inside the composer text. A ref whose slice no longer
// matches the editor text is dropped (fail-closed) rather than mis-sent.
struct MentionRef {
    QString userId;      // full MXID, e.g. "@alice:example.org"
    QString displayText; // "@Alice"
    int start = 0;
    int length = 0;
};

// The active @-token at the cursor, if any. `start` is the index of the '@';
// `query` is the text between '@' and the cursor.
struct Token {
    bool active = false;
    int start = 0;
    QString query;
};

// Detect an active @-mention token whose '@' is triggered at the start of the
// line / after whitespace / after '(' or '>', is not part of a user@host
// address, and does not sit inside inline code or a fenced code block. The
// query accepts letters, digits, '.', '_', '=', '/', '+', '-' and spaces, is
// capped at 40 characters, and must run to the cursor.
Token activeToken(const QString &text, int cursorPos);

// Result of inserting a chosen suggestion over the active token.
struct InsertResult {
    QString text;        // the new composer text
    int cursorPos = 0;   // cursor placed after the inserted "@Name " space
    MentionRef ref;      // the recorded range covering "@Name"
};

// Replace text[tokenStart, cursorPos) with "@<displayName> " (a trailing space
// so the next word starts cleanly) and return the new text, the new cursor
// position, and the ref covering "@<displayName>".
InsertResult buildInsertion(const QString &text, int tokenStart, int cursorPos,
                            const QString &userId, const QString &displayName);

// Reconcile mention refs across a single edit (one contiguous change computed
// from the common prefix/suffix of old vs new): refs fully before the change
// are kept, refs fully after are shifted, refs overlapping the change are
// dropped, and any surviving ref whose slice no longer matches its displayText
// is dropped as well.
QList<MentionRef> shiftRefs(const QList<MentionRef> &refs,
                            const QString &oldText, const QString &newText);

// Send-time expansion.
struct Expansion {
    QString body;            // markdown body with matrix.to mention links
    QStringList userIds;     // deduped MXIDs in first-appearance order
};

// Rewrite each valid ref range into a markdown link
// [DisplayText](https://matrix.to/#/<mxid>) (']' and '\\' escaped in the
// label, the MXID percent-encoded) and collect the deduped, ordered user ids.
// Text with no refs is returned unchanged, so the readable body is preserved.
Expansion expand(const QString &text, const QList<MentionRef> &refs);

// Reverse of expand(), for the Edit flow: parse a raw plain body containing
// [label](https://matrix.to/#/<mxid>) user links back into the human-readable
// display text plus recovered refs, so editing shows "@Alice" instead of raw
// markdown and a resend keeps m.mentions. Room/event matrix.to links and all
// other markdown pass through untouched.
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

} // namespace mention
