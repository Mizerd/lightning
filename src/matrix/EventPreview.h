#pragma once

#include <QString>

struct TimelineEvent;

// One-line event summaries for side surfaces (room list, DM list, desktop
// notifications). Bodies are free-form (newlines, markdown, MSC3381 poll
// fallbacks) and must never define side-surface geometry, so every consumer
// goes through this choke point instead of forwarding `body`.
namespace matrix::preview {

// Semantic one-line summary by event type: "Poll: <question>",
// "Voice message", "File: <name>", "Sticker", "Unable to decrypt",
// "Message removed", media labels, else the normalized body.
QString oneLineSummary(const TimelineEvent &event);

// Presentation normalization, for summaries and plain-text previews (the
// Rust latest-event path):
//   - matrix.to markdown links reduce to their label ("[@x](…)" -> "@x"),
//   - newlines and line/paragraph separators become spaces,
//   - whitespace runs collapse,
//   - the result is bounded with a trailing ellipsis.
// A reply quote elides to its available width in the timeline, so its cap
// only keeps a pathological body out of the model.
inline constexpr int kReplyPreviewMaxChars = 320;

QString normalizePreviewText(const QString &text, int maxChars = 120);

} // namespace matrix::preview
