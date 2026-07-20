#pragma once

#include <QString>
#include <QVariantMap>

// Markdown editing operations behind the composer's formatting toolbar.
// These are pure text transformations over (text, selection): the toolbar
// wraps or unwraps the standard markdown syntax the Rust send path parses
// (bold **, italic _, strike ~~, inline code `, [label](url) links, "- "
// lists, "> " quotes). No Matrix protocol logic, no HTML generation — the
// SDK converts markdown at send time.
namespace MarkdownFormat {

struct Result {
    QString text;
    int selectionStart = 0;
    int selectionEnd = 0;
};

// format: "bold" | "italic" | "strike" | "code" | "link" | "list" | "quote".
// Unknown formats return the input unchanged.
Result toggle(const QString &format, const QString &text,
              int selectionStart, int selectionEnd);

// Active-state flags for the same format keys, resolved against the current
// selection (used for the toolbar's accent-chip state).
QVariantMap state(const QString &text, int selectionStart, int selectionEnd);

} // namespace MarkdownFormat
