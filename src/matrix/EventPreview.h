#pragma once

#include <QString>

struct TimelineEvent;

// One-line semantic event summaries for side surfaces: the room list, DM
// list, and desktop notifications. Message bodies are free-form (newlines,
// markdown, MSC3381 poll fallbacks with one line per answer) and must never
// define side-surface geometry; every consumer goes through this single
// normalizing choke point instead of forwarding `body` verbatim.
namespace matrix::preview {

// Semantic one-line summary by event type: "Poll: <question>",
// "Voice message", "File: <name>", "Sticker", "Unable to decrypt",
// "Message removed", media labels, else the normalized body.
QString oneLineSummary(const TimelineEvent &event);

// Presentation normalization only. Applied to every summary and usable on
// preview strings that arrive as plain text (the Rust latest-event path):
//   - matrix.to markdown links reduce to their label ("[@x](…)" -> "@x"),
//   - newlines / line+paragraph separators become spaces,
//   - whitespace runs collapse,
//   - the result is bounded with a trailing ellipsis.
// A reply quote is not a room-list row: it sits in a timeline that can be
// 1500 px wide and its label elides to the width actually available, so the
// character cap only has to keep a pathological body out of the model.
inline constexpr int kReplyPreviewMaxChars = 320;

QString normalizePreviewText(const QString &text, int maxChars = 120);

} // namespace matrix::preview
