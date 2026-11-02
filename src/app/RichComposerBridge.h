#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class MessageComposer;
class QQuickTextDocument;
class QTextDocument;
class ThreadController;

// QML-facing half of rich-text composing. Keeps QQuickTextDocument out of
// MessageComposer (whose tests link only Qt6::Core/Gui), delegates text-model
// work to RichComposition, and sends through MessageComposer::sendPrepared.
//
// Markdown mode conversions are draft-only; wire bodies always come from
// RichComposition::compose.
class RichComposerBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MessageComposer *composer READ composer WRITE setComposer
                   NOTIFY composerChanged)

public:
    explicit RichComposerBridge(QObject *parent = nullptr);

    MessageComposer *composer() const { return m_composer; }
    void setComposer(MessageComposer *composer);
    void setThread(ThreadController *thread) { m_thread = thread; }

    Q_INVOKABLE void sendDocument(QQuickTextDocument *document);
    Q_INVOKABLE void sendDocumentToThread(QQuickTextDocument *document);
    Q_INVOKABLE QVariantMap composeDocument(QQuickTextDocument *document) const;

    Q_INVOKABLE void toggleFormat(QQuickTextDocument *document,
                                  int selectionStart, int selectionEnd,
                                  const QString &format,
                                  const QString &argument = QString());
    Q_INVOKABLE QVariantMap formatState(QQuickTextDocument *document,
                                        int selectionStart, int selectionEnd);

    Q_INVOKABLE void loadMarkdown(QQuickTextDocument *document,
                                  const QString &markdown);
    Q_INVOKABLE void loadMarkdownLines(QQuickTextDocument *document,
                                       const QString &markdown);
    Q_INVOKABLE QString toMarkdown(QQuickTextDocument *document) const;
    Q_INVOKABLE QString markdownForPlainBox(QQuickTextDocument *document) const;

    Q_INVOKABLE bool isSafeLinkTarget(const QString &url) const;

    Q_INVOKABLE QVariantList spellSkipRanges(QQuickTextDocument *document) const;
    Q_INVOKABLE bool documentIsBlank(QQuickTextDocument *document) const;
    Q_INVOKABLE void replaceRange(QQuickTextDocument *document, int start, int length,
                                  const QString &replacement);

Q_SIGNALS:
    void composerChanged();

private:
    static QTextDocument *unwrap(QQuickTextDocument *document);
    MessageComposer *m_composer = nullptr;
    ThreadController *m_thread = nullptr;
};
