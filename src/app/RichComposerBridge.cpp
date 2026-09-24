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
        // Empty text with queued attachments is a real send.
        if (m_composer->hasAttachments())
            m_composer->send();
        return;
    }
    // Commands are checked before formatting, so a formatted "/spoiler" or
    // "/kick" is never published as text. Formatting is dropped.
    if (plain.startsWith(QLatin1Char('/')) && !plain.startsWith(QLatin1String("//"))) {
        m_composer->setText(plain);
        m_composer->send();
        return;
    }
    if (composed.html.isEmpty()) {
        // "//" escapes a slash, as in markdown mode. Everything else is sent
        // verbatim, never re-read as markdown.
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
    // Before the formatting test; see sendDocument.
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
