#pragma once

#include <QChar>
#include <QList>
#include <QString>

// How a member-chosen display name is shown where it stands for a PERSON in a
// sentence the user trusts: the incoming-call card, and the actor of a
// room-state row ("Alice changed the power levels."). A display name is room
// state any member writes, so it must not be able to impersonate an address
// or hide characters.
//
// Header-only on purpose: TimelineModel uses it, and several test targets
// compile TimelineModel.cpp without any extra source.
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
    // Default-ignorable code points that are not Cf also render as nothing:
    // the combining grapheme joiner, the Hangul fillers, the Khmer inherent
    // vowels, the Mongolian selectors, the variation selectors, and U+2065,
    // which is unassigned (so not Cf) yet hidden by text shaping.
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
        // Never leave half of a surrogate pair at the cut.
        if (name.back().isHighSurrogate())
            name.chop(1);
    }
    return name;
}

// A sender-chosen FILE name with its format characters dropped and nothing
// else touched (no flattening, no length cap: a file name keeps its spaces and
// its suffix). The bidirectional controls (U+061C, U+200E/F, U+202A-202E,
// U+2066-2069) would let "r<RLO>txt.exe" read as "rexe.txt" in a chip, a card
// or a file manager, and every other Cf (zero-width space, word joiner,
// invisible operators, BOM, tag characters) hides characters the same way.
// ZWJ and ZWNJ are the exception: they carry emoji sequences and Persian or
// Indic shaping and cannot reorder anything, so a "family" emoji in a name
// survives. Variation selectors are kept for the same reason.
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

// A remote ROOM name (or a display name shown in a row of its own: a sender
// header, a member row, an inviter) with the EXPLICIT directional formatting
// characters removed: the embeddings and overrides LRE, RLE, PDF, LRO, RLO
// (U+202A-202E) and the isolates LRI, RLI, FSI, PDI (U+2066-2069). Nothing
// else is touched, so emoji sequences, ZWNJ shaping and the implicit marks
// (LRM, RLM, ALM) that a correctly written mixed-script name needs survive.
//
// Stripped, not isolated: wrapping the name in FSI..PDI keeps an override from
// leaking OUT into the sentence around it, but an RLO inside the isolate still
// reverses the name's own tail ("Room<RLO>edirrevo-ltr" still reads
// "Roomrtl-overide"), which is the spoof (break-it s03). matrix-js-sdk drops
// direction overrides from names for the same reason. Returns the input
// itself when it has none of them: this runs for every room row on every
// room-list update.
inline QString withoutDirectionalControls(const QString &text)
{
    const auto directional = [](char16_t u) {
        return (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069);
    };
    // All nine are in the BMP and none is a surrogate, so a UTF-16 walk is
    // exact.
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

// The localpart of a user id, as the app shows an unnamed user.
inline QString localpart(const QString &userId)
{
    return userId.size() > 1 && userId.startsWith(QLatin1Char('@'))
        ? userId.mid(1).section(QLatin1Char(':'), 0, 0)
        : userId;
}

// The name to show for `userId`: the sanitized display name, or the
// localpart when there is none (or the "name" is the id itself). A name that
// looks like an address (NFKC-folded, so a fullwidth "＠alice：example.org"
// counts too), or one another member also uses (`ambiguous`), carries the
// localpart: "Name (localpart)".
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

} // namespace matrix::presentable_name
