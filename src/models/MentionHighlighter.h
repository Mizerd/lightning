#pragma once

#include <QColor>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QVariantList>
#include <qqmlintegration.h>

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
