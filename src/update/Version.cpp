#include "update/Version.h"

#include <QLatin1Char>

namespace lightning::update {
namespace {

bool isAsciiDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

bool isIdentifierChar(QChar c)
{
    return isAsciiDigit(c) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || c == QLatin1Char('-');
}

bool isNumericIdentifier(const QString &id)
{
    if (id.isEmpty())
        return false;
    for (const QChar c : id) {
        if (!isAsciiDigit(c))
            return false;
    }
    return true;
}

// A strict numeric core component: digits only, no leading zero unless the
// value is exactly "0", and small enough to be an int.
bool parseNumericCore(const QString &text, int *out)
{
    if (text.isEmpty())
        return false;
    for (const QChar c : text) {
        if (!isAsciiDigit(c))
            return false;
    }
    if (text.size() > 1 && text.at(0) == QLatin1Char('0'))
        return false;
    if (text.size() > 9) // beyond any plausible version component
        return false;
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < 0)
        return false;
    *out = value;
    return true;
}

// Dot-separated identifiers. `numericRulesApply` enforces the semver
// prerelease rule that numeric identifiers carry no leading zeros.
bool parseIdentifiers(const QString &text, bool numericRulesApply, QStringList *out)
{
    if (text.isEmpty())
        return false;
    const QStringList parts = text.split(QLatin1Char('.'));
    for (const QString &part : parts) {
        if (part.isEmpty())
            return false;
        for (const QChar c : part) {
            if (!isIdentifierChar(c))
                return false;
        }
        if (numericRulesApply && isNumericIdentifier(part) && part.size() > 1
            && part.at(0) == QLatin1Char('0')) {
            return false;
        }
    }
    *out = parts;
    return true;
}

// Semver 11.4.1-11.4.4: numeric identifiers compare numerically and always
// sort below alphanumeric ones; a longer identifier list wins when every
// shared identifier is equal.
int comparePrerelease(const QStringList &a, const QStringList &b)
{
    if (a.isEmpty() && b.isEmpty())
        return 0;
    // A version WITH a prerelease sorts below the same version without one.
    if (a.isEmpty())
        return 1;
    if (b.isEmpty())
        return -1;

    const qsizetype shared = qMin(a.size(), b.size());
    for (qsizetype i = 0; i < shared; ++i) {
        const QString &lhs = a.at(i);
        const QString &rhs = b.at(i);
        const bool lhsNumeric = isNumericIdentifier(lhs);
        const bool rhsNumeric = isNumericIdentifier(rhs);
        if (lhsNumeric && rhsNumeric) {
            // Identifiers are validated to be short and leading-zero free.
            const qulonglong lv = lhs.toULongLong();
            const qulonglong rv = rhs.toULongLong();
            if (lv != rv)
                return lv < rv ? -1 : 1;
            continue;
        }
        if (lhsNumeric != rhsNumeric)
            return lhsNumeric ? -1 : 1;
        const int cmp = QString::compare(lhs, rhs, Qt::CaseSensitive);
        if (cmp != 0)
            return cmp < 0 ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

} // namespace

std::optional<Version> Version::parse(const QString &text)
{
    if (text.isEmpty())
        return std::nullopt;
    // No tolerance for whitespace or a "v" prefix: this is machine input.
    if (text != text.trimmed())
        return std::nullopt;

    QString core = text;
    QString build;
    QString prerelease;

    const qsizetype plus = core.indexOf(QLatin1Char('+'));
    if (plus >= 0) {
        build = core.mid(plus + 1);
        core.truncate(plus);
    }
    const qsizetype dash = core.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        prerelease = core.mid(dash + 1);
        core.truncate(dash);
    }

    const QStringList numbers = core.split(QLatin1Char('.'));
    if (numbers.size() != 3)
        return std::nullopt;

    Version version;
    if (!parseNumericCore(numbers.at(0), &version.m_major))
        return std::nullopt;
    if (!parseNumericCore(numbers.at(1), &version.m_minor))
        return std::nullopt;
    if (!parseNumericCore(numbers.at(2), &version.m_patch))
        return std::nullopt;

    if (dash >= 0 && !parseIdentifiers(prerelease, /*numericRulesApply=*/true, &version.m_prerelease))
        return std::nullopt;
    if (plus >= 0 && !parseIdentifiers(build, /*numericRulesApply=*/false, &version.m_build))
        return std::nullopt;

    return version;
}

QString Version::toComparableString() const
{
    QString out = QString::number(m_major) + QLatin1Char('.') + QString::number(m_minor)
        + QLatin1Char('.') + QString::number(m_patch);
    if (!m_prerelease.isEmpty())
        out += QLatin1Char('-') + m_prerelease.join(QLatin1Char('.'));
    return out;
}

QString Version::toString() const
{
    QString out = toComparableString();
    if (!m_build.isEmpty())
        out += QLatin1Char('+') + m_build.join(QLatin1Char('.'));
    return out;
}

int Version::compare(const Version &a, const Version &b)
{
    if (a.m_major != b.m_major)
        return a.m_major < b.m_major ? -1 : 1;
    if (a.m_minor != b.m_minor)
        return a.m_minor < b.m_minor ? -1 : 1;
    if (a.m_patch != b.m_patch)
        return a.m_patch < b.m_patch ? -1 : 1;
    return comparePrerelease(a.m_prerelease, b.m_prerelease);
}

} // namespace lightning::update
