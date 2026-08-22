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

// Theme ink for mentions and links inside a message body. Colors arrive as
// validated OPAQUE #rrggbb strings (TimelineModel::setMentionStyle validates
// them); empty means "no styling" and the anchor falls back to Qt's default
// link appearance.
//
// A mention is INK, not a box — measured, not assumed. Qt 6.11's rich-text
// engine was probed directly (QTextDocument::setHtml + a rendered QImage):
//   * `border-radius` and `padding` on an inline run are NOT honoured at all
//     — QTextCharFormat has no such properties, so Element's rounded pill is
//     unreachable in this renderer. The header used to say "cannot round
//     corners" and then shipped the box anyway;
//   * `background-color` paints a SQUARE slab spanning the full line height
//     (18 px on an 18 px line) and stays full height even when the run
//     carries a smaller `font-size` — so the fill can be neither rounded nor
//     shrunk toward the glyphs;
//   * every candidate fill colour was rendered against the Storm timeline
//     ground and they all read as a selection highlight or a search hit,
//     which is exactly the "red box around the tag" the user reported (the
//     14% bolt wash that shipped composites to a warm brown on navy);
//   * `color`, `font-weight` and `text-decoration` ARE honoured.
// So the fill is gone and the mention carries ink plus weight instead, which
// is what Slack/Discord degrade to and what reads as a name rather than an
// error state. Do not re-add `background-color` here expecting a pill.
//
// The two inks are a semantic split, not decoration: the accent is spent on
// the ONE mention that concerns the reader (their own), everybody else — and
// every external URL — gets the theme's link ink.
struct MentionStyle {
    // Ink for a mention of the local user. Also the fallback for linkColor,
    // so a theme that pushes only this still renders legibly.
    QString accentColor;
    // Ink for a mention of anybody else and for validated external links.
    // Without it Qt paints http links in its built-in #0000ff, which is very
    // nearly invisible on a dark timeline ground.
    QString linkColor;
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

// Styles the literal "@room" inside ALREADY-SAFE rich text (either
// sanitize()'s output or the linkified plain body).
//
// A whole-room mention has no matrix.to link to hang a pill on — there is no
// such link for "everyone here", and inventing one would put a dead URL in
// every @room message — so it arrives as ordinary body text and renders as
// ordinary body text. This is what makes it look like a mention.
//
// Two rules the caller must respect:
//   * call it ONLY for an event whose m.mentions.room is actually true.
//     Styling any body that happens to contain "@room" would let anyone
//     paint a convincing broadcast ping out of plain text;
//   * pass rich text that is already safe. This never escapes anything and
//     never parses attributes; it copies markup through untouched.
//
// It emits a <span>, NOT an anchor: there is no profile behind @room, and an
// <a href="mention:@room"> would invite a click that can only fail.
//
// Substitution happens in text runs only. Tags are copied verbatim, entities
// are atomic (so "&amp;" is never split), a match must stand on word
// boundaries ("@roomba" is not a mention), and anything inside <code>/<pre>
// is left alone — a literal @room in a code sample is a string, not a ping.
QString markRoomMention(const QString &safeHtml, const QString &color);

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
