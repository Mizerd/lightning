#pragma once

#include <QColor>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QVariantList>
#include <qqmlintegration.h>

// Composer mention chips. The composers keep mentions as semantic
// {userId, range} refs over plain text ("@Alice") — this highlighter gives
// those ranges the inline chip treatment (accent ink on an accent-soft
// surface) directly in the editable TextArea, without changing the document
// text, offsets, or any send-path semantics. QML cannot render custom
// QTextObjectInterface inline objects (the scene-graph text engine never
// calls drawObject), so character formats are the robust native mechanism;
// the failure mode is simply an unstyled mention.
//
// Ranges arrive as [{start, length}, ...] from the owning composer
// (MessageComposer / ThreadController); they are re-anchored there on every
// edit, so this class never guesses at text positions.
class MentionHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged)
    Q_PROPERTY(QVariantList ranges READ ranges WRITE setRanges
                   NOTIFY rangesChanged)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor
                   NOTIFY styleChanged)
    Q_PROPERTY(QColor softColor READ softColor WRITE setSoftColor
                   NOTIFY styleChanged)

public:
    explicit MentionHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_quickDocument; }
    void setDocument(QQuickTextDocument *document);

    QVariantList ranges() const { return m_ranges; }
    void setRanges(const QVariantList &ranges);

    QColor accentColor() const { return m_accent; }
    void setAccentColor(const QColor &color);
    QColor softColor() const { return m_soft; }
    void setSoftColor(const QColor &color);

Q_SIGNALS:
    void documentChanged();
    void rangesChanged();
    void styleChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    QQuickTextDocument *m_quickDocument = nullptr;
    QVariantList m_ranges;
    QColor m_accent;
    QColor m_soft;
};
