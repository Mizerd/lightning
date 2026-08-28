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

void MentionHighlighter::setSoftColor(const QColor &color)
{
    if (m_soft == color)
        return;
    m_soft = color;
    Q_EMIT styleChanged();
    rehighlight();
}


// WHAT COUNTS AS AN EMOJI HERE, and why this is a codepoint test rather than a
// catalogue lookup. The catalogue lives in EmojiCatalog, which is linked
// against Qt6::Core alone by its own test target and knows nothing about fonts;
// pulling it in for a font decision would couple two unrelated things. The
// question is narrow — "would Qt's fallback pick the wrong face here" — and
// that is true of the pictographic blocks and nothing else. Variation
// selectors and ZWJ are INCLUDED so a sequence keeps ONE face across its
// joiners: a run that changed family mid-sequence would break the ligature the
// colour font provides.
static bool isEmojiCodepoint(char32_t cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF)   // pictographs, symbols, faces
        || (cp >= 0x2600 && cp <= 0x27BF)     // misc symbols and dingbats
        || cp == 0x200D                       // zero-width joiner
        || cp == 0xFE0F                       // emoji presentation selector
        || cp == 0x20E3                       // combining enclosing keycap
        || (cp >= 0x2B00 && cp <= 0x2BFF);    // additional symbols and arrows
}

void MentionHighlighter::highlightBlock(const QString &text)
{
    // THE EMOJI PASS RUNS FIRST, AND DELIBERATELY NOT BEHIND THE RETURN BELOW.
    // A composer with no mention in it still types emoji; gating this on
    // m_ranges would skip them in exactly the common case.
    if (!m_emojiFamily.isEmpty()) {
        QTextCharFormat emojiFormat;
        // BOTH APIs, deliberately. setFontFamilies() is the Qt 6 list form and
        // is what a QTextDocument round-trips, but QQuickTextEdit's layout
        // consults the SINGULAR FontFamily property when it resolves a run's
        // face — setting only the list left the composer rendering emoji in
        // the default face while the picker (a plain font.family binding) was
        // already correct, which is exactly how this was reported: "good in
        // catalog, bad in the text box".
        emojiFormat.setFontFamilies({ m_emojiFamily });
        emojiFormat.setFontFamily(m_emojiFamily);
        // Walk UTF-16 indices, because setFormat() takes them and an astral
        // emoji is a surrogate PAIR — formatting by codepoint index would
        // drift by one for every emoji already passed.
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

    // The soft colour is no longer consulted at all — it used to gate this
    // early return as well, so a theme that pushed only an ink silently lost
    // mention styling in the composer.
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
