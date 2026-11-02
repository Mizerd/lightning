#include "models/MentionHighlighter.h"

#include <QTextBlock>
#include <QTextCharFormat>

MentionHighlighter::MentionHighlighter(QObject *parent)
    : QSyntaxHighlighter(parent)
{
}

void MentionHighlighter::setDocument(QQuickTextDocument *document)
{
    if (m_quickDocument == document)
        return;
    m_quickDocument = document;
    QSyntaxHighlighter::setDocument(
        document ? document->textDocument() : nullptr);
    Q_EMIT documentChanged();
}

void MentionHighlighter::setRanges(const QVariantList &ranges)
{
    if (m_ranges == ranges)
        return;
    m_ranges = ranges;
    Q_EMIT rangesChanged();
    rehighlight();
}

void MentionHighlighter::setAccentColor(const QColor &color)
{
    if (m_accent == color)
        return;
    m_accent = color;
    Q_EMIT styleChanged();
    rehighlight();
}

static bool isEmojiCodepoint(char32_t cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF)
        || (cp >= 0x2600 && cp <= 0x27BF)
        || cp == 0x200D
        || cp == 0xFE0F
        || cp == 0x20E3
        || (cp >= 0x2B00 && cp <= 0x2BFF);
}

void MentionHighlighter::highlightBlock(const QString &text)
{
    if (!m_emojiFamily.isEmpty()) {
        QTextCharFormat emojiFormat;
        // Set both APIs: setFontFamilies() is what QTextDocument round-trips,
        // but QQuickTextEdit's layout resolves a run's face from the singular
        // FontFamily property.
        emojiFormat.setFontFamilies({ m_emojiFamily });
        emojiFormat.setFontFamily(m_emojiFamily);
        int i = 0;
        while (i < text.length()) {
            const int start = i;
            bool run = false;
            while (i < text.length()) {
                const QChar c = text.at(i);
                char32_t cp = c.unicode();
                int step = 1;
                if (c.isHighSurrogate() && i + 1 < text.length()
                    && text.at(i + 1).isLowSurrogate()) {
                    cp = QChar::surrogateToUcs4(c, text.at(i + 1));
                    step = 2;
                }
                if (!isEmojiCodepoint(cp))
                    break;
                run = true;
                i += step;
            }
            if (run)
                setFormat(start, i - start, emojiFormat);
            else
                ++i;
        }
    }

    if (m_ranges.isEmpty() || !m_accent.isValid())
        return;
    const int blockStart = currentBlock().position();
    const int blockLength = static_cast<int>(text.length());

    QTextCharFormat mention;
    mention.setForeground(m_accent);
    mention.setFontWeight(QFont::DemiBold);

    for (const QVariant &value : m_ranges) {
        const QVariantMap range = value.toMap();
        const int start = range.value(QStringLiteral("start")).toInt();
        const int length = range.value(QStringLiteral("length")).toInt();
        if (length <= 0)
            continue;
        const int localStart = start - blockStart;
        const int localEnd = localStart + length;
        if (localEnd <= 0 || localStart >= blockLength)
            continue;
        const int clampedStart = qMax(0, localStart);
        const int clampedLen = qMin(blockLength, localEnd) - clampedStart;
        if (clampedLen > 0)
            setFormat(clampedStart, clampedLen, mention);
    }
}

void MentionHighlighter::setEmojiFontFamily(const QString &family)
{
    if (m_emojiFamily == family)
        return;
    m_emojiFamily = family;
    Q_EMIT styleChanged();
    rehighlight();
}
