#pragma once

#include <QColor>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QVariantList>
#include <qqmlintegration.h>

// Composer mention styling. Composers keep mentions as {userId, range} refs
// over plain text; this highlighter inks those ranges in the editable
// TextArea without changing text, offsets or send semantics. Qt Quick text
// never calls drawObject, so character formats are the reliable mechanism.
//
// No background fill: a character-format background is a square,
// full-line-height slab that reads as a box (see MessageHtml::MentionStyle),
// and neither QTextCharFormat nor Qt rich text can round corners. Ink plus
// DemiBold marks the token instead.
//
// Ranges ([{start, length}, ...]) come from the owning composer, which
// re-anchors them on every edit.
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
    // Emoji face for typed text. Qt's per-character fallback varies by version
    // (6.8 can choose a monochrome font), so emoji must be named. The composer
    // is mixed text, so the face is applied per range through this highlighter
    // rather than on the whole TextArea.
    Q_PROPERTY(QString emojiFontFamily READ emojiFontFamily
                   WRITE setEmojiFontFamily NOTIFY styleChanged)

public:
    explicit MentionHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_quickDocument; }
    void setDocument(QQuickTextDocument *document);

    QVariantList ranges() const { return m_ranges; }
    void setRanges(const QVariantList &ranges);

    QColor accentColor() const { return m_accent; }
    void setAccentColor(const QColor &color);
    QString emojiFontFamily() const { return m_emojiFamily; }
    void setEmojiFontFamily(const QString &family);

Q_SIGNALS:
    void documentChanged();
    void rangesChanged();
    void styleChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    QQuickTextDocument *m_quickDocument = nullptr;
    QVariantList m_ranges;
    QString m_emojiFamily;
    QColor m_accent;
};
