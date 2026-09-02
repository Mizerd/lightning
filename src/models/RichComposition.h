#pragma once

#include <QTextDocument>
#include <QVariantList>

#include <QString>
#include <QStringList>
#include <QVariantMap>

class QTextDocument;

// v0.9 rich composer: the QTextDocument -> Matrix serializer and the
// formatting operations behind the rich-mode toolbar.
//
// THE document is the canonical representation. Both wire bodies are
// derived from it in one pass — the Matrix-subset HTML for formatted_body
// and the readable plain fallback for body — so the two cannot diverge,
// which is the requirement naive tag-stripping cannot meet.
//
// SECURITY MODEL, in order:
//   1. Pasted rich content lands in a QTextDocument, which stores
//      FORMATTING, not markup — scripts, event handlers and iframes do not
//      exist in its model at all.
//   2. This serializer is a WHITELIST EMITTER: it walks the document's own
//      structure and can only ever produce the tags written in this file.
//      Nothing in the input can make it emit a tag it does not know.
//   3. Link targets are scheme-validated here (http/https/mailto/matrix/
//      matrix.to); anything else serializes as plain text.
//   4. The Rust boundary strict-sanitizes the HTML again (ruma) — belt and
//      braces, so even a regression here cannot reach the wire.
//
// Version note: this walks QTextDocument's block/fragment model directly
// instead of using Qt's toHtml()/toMarkdown(), because those emitters have
// version-dependent output (the dev shell is Qt 6.11, the packaged fleet
// 6.8) and toHtml() is not remotely Matrix-safe. Qt's setMarkdown()/
// toMarkdown() ARE used for draft-only mode switching, where a cosmetic
// difference between Qt versions cannot reach the protocol.
namespace RichComposition {

// v0.9 spell checking in rich mode. Ranges of the document's plain text
// (`QTextDocument::toRawText` positions == cursor positions) that are NOT
// natural language and must never be underlined: fenced code blocks, inline
// code fragments (fixed pitch) and mention pills (anchors to a user). Link
// anchor TEXT is checked — the destination is not in the text at all.
QVariantList spellSkipRanges(const QTextDocument &document);
// Replaces exactly [start, start+length) with `replacement`, keeping the
// character format the range started with, so a suggestion applied inside a
// bold or linked word stays bold or linked. One undo step.
void replaceRange(QTextDocument *document, int start, int length,
                  const QString &replacement);

struct Composed {
    // The plain m.text fallback: list markers ("- ", "1. "), "> " quote
    // prefixes and newlines, readable in any client.
    QString plainBody;
    // Matrix-subset HTML, or EMPTY when the document carries no formatting
    // at all — an unformatted message stays a plain m.text event, exactly
    // like the markdown path's behaviour for plain text.
    QString html;
    // MXIDs of matrix.to user links found in the document (mention anchors
    // the popup inserted), deduped in first-appearance order — the
    // m.mentions input.
    QStringList mentionUserIds;
};

Composed compose(const QTextDocument &document);

// Toolbar operations over the selection (or the caret's word/position when
// the selection is empty): "bold" | "italic" | "underline" | "strike" |
// "code" | "quote" | "list" | "orderedlist" | "link". Selection offsets are
// QTextCursor positions. "link" takes the target URL through `argument`
// (validated; an unsafe scheme is refused and nothing changes).
void toggleFormat(QTextDocument *document, int selectionStart,
                  int selectionEnd, const QString &format,
                  const QString &argument = QString());

// Active-state flags for the toolbar chips at the given selection, same
// keys as toggleFormat.
QVariantMap formatState(const QTextDocument &document, int selectionStart,
                        int selectionEnd);

// True when the given URL is a link target this serializer will emit.
bool isSafeLinkTarget(const QString &url);

} // namespace RichComposition
