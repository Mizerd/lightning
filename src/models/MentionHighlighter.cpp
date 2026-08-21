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

void MentionHighlighter::highlightBlock(const QString &text)
{
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
