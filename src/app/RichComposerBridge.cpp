#include "app/RichComposerBridge.h"

#include "models/MessageComposer.h"
#include "models/RichComposition.h"
#include "threads/ThreadController.h"

#include <QQuickTextDocument>
#include <QTextDocument>

RichComposerBridge::RichComposerBridge(QObject *parent)
    : QObject(parent)
{
}

void RichComposerBridge::setComposer(MessageComposer *composer)
{
    if (m_composer == composer)
        return;
    m_composer = composer;
    Q_EMIT composerChanged();
}

QTextDocument *RichComposerBridge::unwrap(QQuickTextDocument *document)
{
    return document ? document->textDocument() : nullptr;
}

void RichComposerBridge::sendDocument(QQuickTextDocument *document)
{
    QTextDocument *doc = unwrap(document);
    if (!doc || !m_composer)
        return;
    const RichComposition::Composed composed = RichComposition::compose(*doc);
    const QString plain = composed.plainBody.trimmed();
    if (plain.isEmpty() && composed.html.isEmpty()) {
        // An empty box with queued attachments is a REAL send, and the Send
        // button is enabled for it. Returning here dropped the whole thing
        // silently: the click was accepted and nothing left the client. The
        // markdown lane has always dispatched attachments before its own
        // empty-body check; this is that path.
        if (m_composer->hasAttachments())
            m_composer->send();
        return;
    }
    // A COMMAND IS A COMMAND WHATEVER IT IS WEARING, and this test must come
    // before the formatting one. It used to sit inside the unformatted
    // branch, so bolding one word of "/spoiler the ending is X" published the
    // sentence to the room as ordinary formatted text — the exact opposite of
    // what the user asked for, and the same shape turned "/kick" into a
    // public message and skipped the unknown-command refusal entirely.
    // Formatting is dropped rather than honoured: a command's argument has no
    // formatted form, and refusing to run it would be worse.
    if (plain.startsWith(QLatin1Char('/')) && !plain.startsWith(QLatin1String("//"))) {
        m_composer->setText(plain);
        m_composer->send();
        return;
    }
    if (composed.html.isEmpty()) {
        // No formatting. "//" is the escape, as in markdown mode. Anything
        // else travels the PLAIN lane verbatim — a WYSIWYG editor showing
        // "*not bold*" must send exactly that, never re-read as markdown.
        const QString verbatim = plain.startsWith(QLatin1String("//")) ? plain.mid(1)
                                                                          : plain;
        m_composer->sendPrepared(verbatim, QString(), composed.mentionUserIds);
        return;
    }
    m_composer->sendPrepared(plain, composed.html, composed.mentionUserIds);
}

void RichComposerBridge::sendDocumentToThread(QQuickTextDocument *document)
{
    QTextDocument *doc = unwrap(document);
    if (!doc || !m_thread)
        return;
    const RichComposition::Composed composed = RichComposition::compose(*doc);
    const QString plain = composed.plainBody.trimmed();
    if (plain.isEmpty() && composed.html.isEmpty()) {
        // Attachments with no text are a real thread send; see sendDocument.
        if (m_thread->hasAttachments())
            m_thread->sendText(QString());
        return;
    }
    // Before the formatting test, for the reason given in sendDocument: a
    // formatted "/spoiler" must not be published as ordinary text.
    if (plain.startsWith(QLatin1Char('/')) && !plain.startsWith(QLatin1String("//"))) {
        m_thread->setText(plain);
        m_thread->sendText(plain);
        return;
    }
    if (composed.html.isEmpty()) {
        // Unformatted: "//" escapes, everything else travels the PLAIN lane
        // verbatim.
        const QString verbatim = plain.startsWith(QLatin1String("//")) ? plain.mid(1)
                                                                          : plain;
        m_thread->sendPrepared(verbatim, QString(), composed.mentionUserIds);
        return;
    }
    m_thread->sendPrepared(plain, composed.html, composed.mentionUserIds);
}

QVariantMap RichComposerBridge::composeDocument(QQuickTextDocument *document) const
{
    QTextDocument *doc = unwrap(document);
    if (!doc)
        return {};
    const RichComposition::Composed composed = RichComposition::compose(*doc);
    return QVariantMap{
        { QStringLiteral("body"), composed.plainBody.trimmed() },
        { QStringLiteral("html"), composed.html },
        { QStringLiteral("mentionIds"), composed.mentionUserIds },
    };
}

void RichComposerBridge::toggleFormat(QQuickTextDocument *document,
                                      int selectionStart, int selectionEnd,
                                      const QString &format,
                                      const QString &argument)
{
    if (QTextDocument *doc = unwrap(document))
        RichComposition::toggleFormat(doc, selectionStart, selectionEnd,
                                      format, argument);
}

QVariantMap RichComposerBridge::formatState(QQuickTextDocument *document,
                                            int selectionStart,
                                            int selectionEnd)
{
    if (QTextDocument *doc = unwrap(document))
        return RichComposition::formatState(*doc, selectionStart,
                                            selectionEnd);
    return {};
}

void RichComposerBridge::loadMarkdown(QQuickTextDocument *document,
                                      const QString &markdown)
{
    if (QTextDocument *doc = unwrap(document))
        doc->setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
}

QString RichComposerBridge::toMarkdown(QQuickTextDocument *document) const
{
    if (QTextDocument *doc = unwrap(document))
        return doc->toMarkdown(QTextDocument::MarkdownDialectGitHub);
    return {};
}

bool RichComposerBridge::isSafeLinkTarget(const QString &url) const
{
    return RichComposition::isSafeLinkTarget(url);
}

QVariantList RichComposerBridge::spellSkipRanges(QQuickTextDocument *document) const
{
    if (QTextDocument *doc = unwrap(document))
        return RichComposition::spellSkipRanges(*doc);
    return {};
}

void RichComposerBridge::replaceRange(QQuickTextDocument *document, int start,
                                      int length, const QString &replacement)
{
    if (QTextDocument *doc = unwrap(document))
        RichComposition::replaceRange(doc, start, length, replacement);
}

bool RichComposerBridge::documentIsBlank(QQuickTextDocument *document) const
{
    if (QTextDocument *doc = unwrap(document))
        return RichComposition::documentIsBlank(*doc);
    return true;
}
