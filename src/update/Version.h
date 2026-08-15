#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Lightning secure update system — semantic version parsing and ordering.
//
// Update decisions are NEVER made with a lexicographic string compare:
// "0.10.0" must sort above "0.9.0", and "0.8.0-rc1" must sort BELOW "0.8.0".
// Parsing is strict — a malformed local or remote version is REPORTED as
// invalid (std::nullopt), never guessed and never treated as "probably
// newer". Callers must surface that as a failed check.
//
// Grammar (semver 2.0.0, strict):
//     MAJOR "." MINOR "." PATCH [ "-" prerelease ] [ "+" build ]
// - MAJOR/MINOR/PATCH are digits with no leading zero (except "0" itself).
// - prerelease/build are dot-separated non-empty identifiers of
//   [0-9A-Za-z-]; numeric prerelease identifiers carry no leading zero.
// - Build metadata is parsed for round-tripping but IGNORED for ordering.
// - No leading "v", no surrounding whitespace: a tag ("v0.8.0") is not a
//   version and is rejected rather than silently repaired.
namespace lightning::update {

class Version
{
public:
    Version() = default;

    // Strict parse. Returns nullopt for anything that is not exactly a
    // semantic version — the caller reports an error instead of guessing.
    static std::optional<Version> parse(const QString &text);

    int majorVersion() const { return m_major; }
    int minorVersion() const { return m_minor; }
    int patchVersion() const { return m_patch; }

    bool isPrerelease() const { return !m_prerelease.isEmpty(); }
    const QStringList &prereleaseIdentifiers() const { return m_prerelease; }
    const QStringList &buildIdentifiers() const { return m_build; }

    // Canonical rendering including prerelease and build metadata.
    QString toString() const;
    // Rendering used for ordering-relevant display (build metadata dropped).
    QString toComparableString() const;

    // -1 / 0 / +1. Build metadata is ignored, per semver.
    static int compare(const Version &a, const Version &b);

    bool operator==(const Version &other) const { return compare(*this, other) == 0; }
    bool operator!=(const Version &other) const { return compare(*this, other) != 0; }
    bool operator<(const Version &other) const { return compare(*this, other) < 0; }
    bool operator<=(const Version &other) const { return compare(*this, other) <= 0; }
    bool operator>(const Version &other) const { return compare(*this, other) > 0; }
    bool operator>=(const Version &other) const { return compare(*this, other) >= 0; }

private:
    int m_major = 0;
    int m_minor = 0;
    int m_patch = 0;
    QStringList m_prerelease;
    QStringList m_build;
};

} // namespace lightning::update
