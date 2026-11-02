#pragma once

#include <QChar>
#include <QList>
#include <QString>

namespace matrix::presentable_name {

// A display name as shown: format characters (bidi controls, zero-width
// characters, BOM) and other default-ignorables dropped, control characters
// and line breaks flattened, at most 64 characters. Ambiguity is judged on
// this form, the one the user sees.
//
// Format characters (Unicode Cf) are dropped: the bidirectional controls that
// could reorder the sentence, and the zero-width space, joiners, word joiner,
// invisible operators and BOM that would make "Alice" and "Al<ZWSP>ice"
// different names that look the same. Walked by code point, so a Cf outside
// the BMP (tag characters) is caught too. The accepted cost: emoji joined by
// ZWJ fall apart, variation selectors go (emoji fall back to text
// presentation, CJK glyph variants and Mongolian letter forms change), and
// ZWNJ goes, which can change how a Persian or Indic name is shaped.
inline QString sanitized(const QString &displayName)
{
    const QList<uint> points = displayName.left(256).toUcs4();
    QString name;
    name.reserve(points.size());
    const auto ignorable = [](uint c) {
        return c == 0x034F || c == 0x115F || c == 0x1160 || c == 0x17B4
            || c == 0x17B5 || (c >= 0x180B && c <= 0x180F) || c == 0x3164
            || c == 0x2065 || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFFA0
            || (c >= 0xFFF0 && c <= 0xFFF8) || (c >= 0xE0000 && c <= 0xE0FFF);
    };
    for (const uint point : points) {
        const auto category = QChar::category(char32_t(point));
        if (category == QChar::Other_Format || ignorable(point))
            continue;
        if (category == QChar::Other_Control) {
            name.append(QChar(u' '));
            continue;
        }
        const char32_t unit = char32_t(point);
        name.append(QString::fromUcs4(&unit, 1));
    }
    name = name.simplified();
    if (name.size() > 64) {
        name.truncate(64);
        if (name.back().isHighSurrogate())
            name.chop(1);
    }
    return name;
}

inline QString withoutFormatCharacters(const QString &fileName)
{
    const QList<uint> points = fileName.toUcs4();
    QString out;
    out.reserve(fileName.size());
    for (const uint point : points) {
        if (point != 0x200C && point != 0x200D
            && QChar::category(char32_t(point)) == QChar::Other_Format)
            continue;
        const char32_t unit = char32_t(point);
        out.append(QString::fromUcs4(&unit, 1));
    }
    return out;
}

inline QString withoutDirectionalControls(const QString &text)
{
    const auto directional = [](char16_t u) {
        return (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069);
    };
    qsizetype first = -1;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (directional(text.at(i).unicode())) {
            first = i;
            break;
        }
    }
    if (first < 0)
        return text;
    QString out = text.left(first);
    out.reserve(text.size());
    for (qsizetype i = first + 1; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (!directional(c.unicode()))
            out.append(c);
    }
    return out;
}

inline QString localpart(const QString &userId)
{
    return userId.size() > 1 && userId.startsWith(QLatin1Char('@'))
        ? userId.mid(1).section(QLatin1Char(':'), 0, 0)
        : userId;
}

inline QString presentable(const QString &userId, const QString &displayName,
                           bool ambiguous = false)
{
    const QString name = sanitized(displayName);
    const QString local = localpart(userId);
    if (name.isEmpty() || name == userId)
        return local;
    const QString folded = name.normalized(QString::NormalizationForm_KC);
    const bool looksLikeMxid = folded.startsWith(QLatin1Char('@'))
        && folded.contains(QLatin1Char(':'));
    if ((ambiguous || looksLikeMxid) && name != local)
        return QStringLiteral("%1 (%2)").arg(name, local);
    return name;
}

}
