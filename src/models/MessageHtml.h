#pragma once

#include <QList>
#include <QString>

#include <functional>

// v0.7.1: sanitizer for Matrix `org.matrix.custom.html` formatted bodies.
//
// Incoming formatted message bodies are untrusted HTML. This turns them into
// a small, safe Qt RichText subset that the timeline can render:
//   * only an allowlist of inline/block formatting tags survives; every
//     other tag is dropped (its text content is kept);
//   * <script>/<style>/<iframe>/<svg>/<mx-reply>/... are dropped WITH their
//     content, so nothing from them is rendered;
//   * all attributes are stripped, EXCEPT a validated href on <a>: only
//     http(s) links survive as external links, and matrix.to user links are
//     rewritten to an internal "mention:<user-id>" link whose visible text is
//     the resolved display name (never a bare MXID). Every other scheme
//     (javascript:, data:, …) is dropped and the link renders as plain text;
//   * <img> is dropped entirely, so a formatted body can never make the
//     client fetch a remote/tracking image.
//
// The result contains no attributes that could load remote content or carry
// active behaviour; combined with the timeline's onLinkActivated (which only
// opens validated http(s) URLs and routes mention: links to a profile), it
// renders untrusted content without executing anything. Fail-closed: unknown
// or malformed markup is dropped, not passed through.
namespace MessageHtml {

// Theme ink for mention chips. Colors arrive as validated #rrggbb/#aarrggbb
// strings (the model validates through QColor before storing); empty means
// "no chip styling" and mentions render as plain internal links. Qt rich
// text cannot round corners, so the chip is a soft rectangular highlight.
struct MentionStyle {
    QString accentColor;   // chip ink
    QString softColor;     // chip surface
    QString codeBackground; // subtle surface behind inline `code`/```blocks```
                            // (empty = leave code unstyled)
};

// resolveDisplayName maps a Matrix user id to a room display name (empty or
// the id itself when unknown — the sanitizer falls back to the localpart).
// ownUserId, when it matches a mention target, marks a self-mention (bold).
QString sanitize(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle = {});

// v0.7.4: fenced code blocks are not rich text.
//
// Qt's rich-text engine treats <pre> as PREFORMATTED and does not wrap it, so
// a single long terminal line painted a TextEdit far past its own width and
// escaped the timeline entirely (the delegate root is deliberately clip:false
// so the hover action bar can overhang). A code block is therefore not
// something to style better inside the one rich-text item — it is a different
// KIND of content that needs its own renderer, with horizontal scrolling of
// its own. segments() is the split that makes that possible.
enum class SegmentKind {
    RichText,   // sanitized Qt-RichText subset, rendered as today
    CodeBlock,  // plain text, rendered by qml/CodeBlock.qml
};

struct Segment {
    SegmentKind kind = SegmentKind::RichText;
    // RichText: exactly what sanitize() produces for that span — inline
    //           <code> that is NOT inside a <pre> stays inline in here and
    //           keeps its codeBackground styling.
    // CodeBlock: PLAIN text, already entity-decoded, newline separated, never
    //           html. The UI renders it with Text.PlainText, so "&lt;b&gt;"
    //           arrives as the literal characters and can never become markup.
    QString text;
    // CodeBlock only. Empty unless the source carried a class of the form
    // `language-xxx` / `lang-xxx` where xxx matches ^[A-Za-z0-9+#._-]{1,24}$.
    // Anything else -> empty: a class attribute is attacker-chosen text and
    // only this validated token ever leaves the parser.
    QString language;
};

// Splits a formatted body into ordered segments. A body with no code block
// returns exactly ONE RichText segment whose text IS sanitize()'s output —
// that equality is by construction (the fast path calls sanitize on the
// untouched input), so the ordinary message keeps its existing rendering and
// its existing cost.
QList<Segment> segments(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle = {});

} // namespace MessageHtml
