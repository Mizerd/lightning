#pragma once

#include <QColor>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QVariantList>
#include <qqmlintegration.h>

// Composer mention styling. The composers keep mentions as semantic
// {userId, range} refs over plain text ("@Alice") — this highlighter inks
// those ranges directly in the editable TextArea, without changing the
// document text, offsets, or any send-path semantics. QML cannot render
// custom QTextObjectInterface inline objects (the scene-graph text engine
// never calls drawObject), so character formats are the robust native
// mechanism; the failure mode is simply an unstyled mention.
//
// It used to also fill the range's background. That fill is gone, and the
// reason is the same one recorded in MessageHtml::MentionStyle: a character
// format's background is an unroundable, full-line-height square slab, so it
// reads as a box drawn around the name — the composer being where the user
// meets it FIRST, the instant they type "@". Ink plus DemiBold marks the
// token just as clearly and cannot read as an error state. Neither
// QTextCharFormat nor Qt's rich text can round a corner, so do not try to
// restore an Element-style pill through either of them.
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
    // Vestigial: the former chip surface. Retained because removing a
    // property a .qml file still assigns is a hard component-load error, and
    // the composers are not this class's to edit. Assigning it is harmless
    // and changes nothing.
    Q_PROPERTY(QColor softColor READ softColor WRITE setSoftColor
                   NOTIFY styleChanged)
    // THE EMOJI FACE FOR TYPED TEXT, and the composer is why it has to be here.
    //
    // Qt's automatic per-character fallback is version-dependent (Qt 6.8 picks
    // a MONOCHROME font that claims the codepoint where 6.11 picks the colour
    // one), so emoji have to be NAMED. On a single-purpose label that is just
    // `font.family`, but the composer is MIXED text -- naming the emoji face on
    // the whole TextArea would render every letter in it. A QSyntaxHighlighter
    // is already attached to this exact document for mentions, and a
    // QTextCharFormat CAN carry font families per range, so the emoji runs get
    // the face and the words keep theirs.
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
    QColor softColor() const { return m_soft; }
    void setSoftColor(const QColor &color);
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
    QColor m_soft;
};
