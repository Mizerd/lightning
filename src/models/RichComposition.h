#pragma once

#include <QTextDocument>
#include <QVariantList>

#include <QString>
#include <QStringList>
#include <QVariantMap>

class QTextDocument;

// Rich composer: the QTextDocument -> Matrix serializer and the formatting
// operations behind the rich-mode toolbar.
//
// The document is canonical. Both wire bodies (Matrix-subset HTML for
// formatted_body and the plain fallback for body) are derived from it in one
// pass, so they cannot diverge.
//
// Security, in order:
//   1. Pasted rich content lands in a QTextDocument, which stores formatting,
//      not markup: scripts, handlers and iframes do not exist in its model.
//   2. This serializer is a whitelist emitter over the document structure;
//      it can only produce the tags written in this file.
//   3. Link targets are scheme-validated (http/https/mailto/matrix/
//      matrix.to); anything else serializes as plain text.
//   4. Rust sanitizes the HTML again (ruma) before it reaches the wire.
//
// The document model is walked directly rather than via toHtml()/
// toMarkdown(): their output varies across Qt versions and toHtml() is not
// Matrix-safe. setMarkdown() is used only for draft mode switching and the edit
// seed.
namespace RichComposition {

// The composer's markdown <-> document mirror. A line of the draft is a line:
// Qt's setMarkdown() turns a lone newline into a space (CommonMark's soft
// break), which flattened a four-line draft on a mode switch, and its
// toMarkdown() folds at 80 columns, which that newline rule cannot tell from a
// line.
void loadMarkdown(QTextDocument *document, const QString &markdown);
QString toMarkdown(const QTextDocument &document);

// Markdown for the message box when editing a message whose body does not
// carry its own formatting (a rich-mode send: body "hi", html "<b>hi</b>").
// Empty when the plain body is already the right seed: nothing is formatted,
// or the body is the sender's markdown source and differs from the html's text.
// `sanitizedHtml` is MessageHtml::sanitize() output.
QString markdownFromSanitizedHtml(const QString &sanitizedHtml,
                                  const QString &plainBody);

// Spell checking in rich mode: ranges of the document's plain text
// (`QTextDocument::toRawText` positions == cursor positions) that must never
// be underlined: fenced code blocks, inline code (fixed pitch) and mention
// pills. Link anchor text is checked; the destination is not in the text.
QVariantList spellSkipRanges(const QTextDocument &document);
// True when the document holds nothing visible. Not the same as "no
// characters": an empty list item, quote or code block still draws (Qt
// paints "1." for an empty ordered item), while TextEdit's `length` counts
// characters only, so a placeholder bound to it would draw under the marker.
bool documentIsBlank(const QTextDocument &document);
// Replaces exactly [start, start+length) with `replacement`, keeping the
// character format the range started with, so a suggestion applied inside a
// bold or linked word stays bold or linked. One undo step.
void replaceRange(QTextDocument *document, int start, int length,
                  const QString &replacement);

struct Composed {
    // The plain m.text fallback: list markers ("- ", "1. "), "> " quote
    // prefixes and newlines, readable in any client.
    QString plainBody;
    // Matrix-subset HTML, or empty when the document carries no formatting, so
    // an unformatted message stays a plain m.text event.
    QString html;
    // MXIDs of matrix.to user links (inserted mention anchors), deduplicated in
    // first-appearance order; the m.mentions input.
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
