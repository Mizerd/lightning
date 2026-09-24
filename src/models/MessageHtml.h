#pragma once

#include <QList>
#include <QString>

#include <functional>

// Sanitizer for Matrix `org.matrix.custom.html` formatted bodies.
//
// Formatted bodies are untrusted HTML. This reduces them to a small, safe Qt
// RichText subset:
//   * only allowlisted formatting tags survive; other tags are dropped and
//     their text kept;
//   * <script>/<style>/<iframe>/<svg>/<mx-reply>/... are dropped with their
//     content;
//   * attributes are stripped, except a validated href on <a>: http(s) links
//     survive, matrix.to user links become internal "mention:<user-id>" links
//     showing the resolved display name, and any other scheme (javascript:,
//     data:, …) is dropped, leaving plain text;
//   * <img> is dropped except MSC2545 custom emoji addressed by `mxc:`, so a
//     body can never make the client fetch a remote/tracking image.
//
// Nothing emitted can load remote content or carry active behaviour, and
// onLinkActivated only opens validated http(s) URLs. Fail-closed: unknown or
// malformed markup is dropped.
namespace MessageHtml {

// Theme ink for mentions and links in a message body: validated opaque
// #rrggbb strings (see TimelineModel::setMentionStyle); empty means Qt's
// default link appearance.
//
// A mention is ink, not a box. Qt's rich-text engine does not honour
// `border-radius` or `padding` on inline runs, and `background-color` paints
// a square full-line-height slab that reads as a selection or search hit.
// `color`, `font-weight` and `text-decoration` are honoured, so mentions use
// ink plus weight. Do not re-add `background-color` expecting a pill.
//
// The accent is reserved for a mention of the reader; everyone else and every
// external URL gets the link ink.
struct MentionStyle {
    // Ink for a mention of the local user; also the fallback for linkColor.
    QString accentColor;
    // Ink for other mentions and validated external links. Without it Qt uses
    // #0000ff, nearly invisible on dark themes.
    QString linkColor;
    QString codeBackground; // subtle surface behind inline `code`/```blocks```
                            // (empty = unstyled). Also the spoiler cover: a
                            // covered spoiler paints background and text in
                            // this ink. Callers rendering untrusted bodies must
                            // supply it, or covered spoiler text is not hidden;
                            // TimelineModel always does.
};

// resolveDisplayName maps a user id to a room display name (empty or the id
// itself when unknown; the sanitizer then falls back to the localpart).
// ownUserId marks a self-mention (bold).
//
// Spoilers: <span data-mx-spoiler> becomes a click-to-reveal run wrapped in
// an internal <a href="spoiler:toggle">. While `revealSpoilers` is false it is
// covered by a codeBackground slab (background and text in one ink); revealed
// keeps the slab as background only. The reason value is ignored.
QString sanitize(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle = {},
    bool revealSpoilers = false);

// Code blocks are not rich text. Qt does not wrap <pre>, so a long line would
// overflow the timeline (the delegate root is clip:false for the hover bar).
// Code blocks therefore get their own renderer with horizontal scrolling, and
// segments() splits them out.
enum class SegmentKind {
    RichText,   // sanitized Qt-RichText subset
    CodeBlock,  // plain text, rendered by qml/CodeBlock.qml
};

struct Segment {
    SegmentKind kind = SegmentKind::RichText;
    // RichText: exactly what sanitize() produces for that span; inline <code>
    //           outside a <pre> stays here with its codeBackground styling.
    // CodeBlock: plain text, entity-decoded, newline separated, never html.
    //           Rendered with Text.PlainText, so "&lt;b&gt;" stays literal.
    QString text;
    // CodeBlock only. Empty unless the source class was `language-xxx` /
    // `lang-xxx` with xxx matching ^[A-Za-z0-9+#._-]{1,24}$. The class
    // attribute is attacker-chosen; only this validated token leaves the
    // parser.
    QString language;
};

// Styles a literal "@room" inside already-safe rich text (sanitize() output
// or the linkified plain body). A whole-room mention has no link to style, so
// this is what makes it look like one.
//
// The caller must:
//   * call it only when the event's m.mentions.room is true, or anyone could
//     fake a broadcast ping with plain text;
//   * pass already-safe rich text; this never escapes or parses attributes.
//
// Emits a <span>, not an anchor: there is no profile behind @room.
// Substitution happens in text runs only: tags are copied verbatim, entities
// are atomic, matches need word boundaries ("@roomba" is not one), and
// <code>/<pre> contents are left alone.
QString markRoomMention(const QString &safeHtml, const QString &color);

/// Turn the `mxc:` sources of sanitize()'s inline custom emoji into sources
/// the caller's media layer can render.
///
/// Separate from sanitize() because its output also feeds
/// MessageComposer::beginEdit, and a resolved `image://` source must never end
/// up in an outgoing formatted_body.
///
/// `resolve` returns empty for uncached media and starts a fetch, so callers
/// re-read when it arrives; meanwhile the emoji shows its `alt` text.
QString resolveInlineImages(
    const QString &safeHtml,
    const std::function<QString(const QString &mxcUri)> &resolve);

// Enlarges inline emoji inside already-safe rich text (sanitize() output or
// the linkified plain body) so they read as pictures.
//
// Uses the `x-large` CSS keyword: Qt's CSS ignores `em` and `%`, and an
// absolute px size would not follow the body size, which varies by surface and
// text-size setting. Keywords map to QTextFormat::FontSizeAdjustment (ladder
// 0.7/0.8/1.0/1.2/1.5/2.0/2.4); `x-large` is the relative 1.5 step. With the
// bundled fonts it adds 0-2 px of line height.
//
// Suppressed for an emoji-only body of 1-3 sequences, which the delegate
// already renders large (bodyLabel.bigEmoji). That test is deliberately broader
// than the catalogue's, so disagreement can only suppress.
//
// Everything emitted is a constant of ours. Parses no attributes, copies tags
// through, treats entities as atomic and leaves <code>/<pre> alone.
QString markEmoji(const QString &safeHtml);

// Splits a formatted body into ordered segments. A body without a code block
// yields exactly one RichText segment equal to sanitize()'s output (the fast
// path calls sanitize on the untouched input).
QList<Segment> segments(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle = {},
    bool revealSpoilers = false);

} // namespace MessageHtml
