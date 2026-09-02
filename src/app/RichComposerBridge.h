#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class MessageComposer;
class QQuickTextDocument;
class QTextDocument;
class ThreadController;

// v0.9 rich composer: the QML-facing half of rich-text composing.
//
// This class exists so QQuickTextDocument (a QtQuick type) stays OUT of
// MessageComposer — several standalone test targets compile the composer
// against Qt6::Core/Gui only, and the composer's own responsibilities
// (context, drafts, commands, mentions) are editor-agnostic. The bridge
// unwraps the QML document, delegates every text-model operation to
// RichComposition (pure, unit-tested), and hands finished (plainBody, html,
// mentionIds) triples to MessageComposer::sendPrepared, which owns the
// thread/reply/edit routing exactly as for markdown sends.
//
// Mode-switch conversions (Qt's own setMarkdown/toMarkdown) live here too:
// they are DRAFT-ONLY — cosmetic differences between Qt versions cannot
// reach the protocol, because the wire bodies always come from
// RichComposition::compose over the live document.
class RichComposerBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MessageComposer *composer READ composer WRITE setComposer
                   NOTIFY composerChanged)

public:
    explicit RichComposerBridge(QObject *parent = nullptr);

    MessageComposer *composer() const { return m_composer; }
    void setComposer(MessageComposer *composer);
    // The thread panel's composer, which has its own send lane
    // (ThreadController::sendPrepared / sendText).
    void setThread(ThreadController *thread) { m_thread = thread; }

    // Compose the document into (plainBody, html, mentionIds) and send it
    // through the composer's current context. A document whose plain text
    // begins with "/" and which carries NO formatting is routed through the
    // composer's ordinary send instead, so slash commands work identically
    // in rich mode.
    Q_INVOKABLE void sendDocument(QQuickTextDocument *document);
    // Same, through the thread panel's composer.
    Q_INVOKABLE void sendDocumentToThread(QQuickTextDocument *document);
    // v0.9 scheduled send: the document composed WITHOUT sending —
    // {body, html, mentionIds}; html empty when the document has no
    // formatting.
    Q_INVOKABLE QVariantMap composeDocument(QQuickTextDocument *document) const;

    // Toolbar operations; see RichComposition::toggleFormat/formatState.
    Q_INVOKABLE void toggleFormat(QQuickTextDocument *document,
                                  int selectionStart, int selectionEnd,
                                  const QString &format,
                                  const QString &argument = QString());
    Q_INVOKABLE QVariantMap formatState(QQuickTextDocument *document,
                                        int selectionStart, int selectionEnd);

    // Draft-only mode conversions.
    Q_INVOKABLE void loadMarkdown(QQuickTextDocument *document,
                                  const QString &markdown);
    Q_INVOKABLE QString toMarkdown(QQuickTextDocument *document) const;

    Q_INVOKABLE bool isSafeLinkTarget(const QString &url) const;

    // v0.9 spell checking in rich mode; see RichComposition::spellSkipRanges
    // and ::replaceRange.
    Q_INVOKABLE QVariantList spellSkipRanges(QQuickTextDocument *document) const;
    Q_INVOKABLE void replaceRange(QQuickTextDocument *document, int start, int length,
                                  const QString &replacement);

Q_SIGNALS:
    void composerChanged();

private:
    static QTextDocument *unwrap(QQuickTextDocument *document);
    MessageComposer *m_composer = nullptr;
    ThreadController *m_thread = nullptr;
};
