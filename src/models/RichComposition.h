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

void loadMarkdown(QTextDocument *document, const QString &markdown);
void loadMarkdownLines(QTextDocument *document, const QString &markdown);

QString markdownForPlainBox(const QTextDocument &document);

QString plainTextOfSanitizedHtml(const QString &sanitizedHtml);

QString markdownFromSanitizedHtml(const QString &sanitizedHtml,
                                  const QString &plainBody);

QVariantList spellSkipRanges(const QTextDocument &document);
bool documentIsBlank(const QTextDocument &document);
void replaceRange(QTextDocument *document, int start, int length,
                  const QString &replacement);

struct Composed {
    QString plainBody;
    QString html;
    QStringList mentionUserIds;
};

Composed compose(const QTextDocument &document);

void toggleFormat(QTextDocument *document, int selectionStart,
                  int selectionEnd, const QString &format,
                  const QString &argument = QString());

QVariantMap formatState(const QTextDocument &document, int selectionStart,
                        int selectionEnd);

bool isSafeLinkTarget(const QString &url);

}
