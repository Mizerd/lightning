#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

// Lightning secure update system — the compiled-in endpoint policy.
//
// There are TWO roles here and collapsing them is a security regression:
//
//   * METADATA (the manifest, its detached signature, release_notes_url) may
//     come from the canonical release host and NOTHING else. GitLab is the
//     release authority: it decides what Lightning may install.
//   * ARTIFACT BYTES (a package, and every redirect hop taken while fetching
//     one) may additionally come from a compiled-in bandwidth MIRROR. A
//     mirror is only a faster place to get bytes the signed manifest has
//     already named, sized and hashed; nothing a mirror returns is ever an
//     input to a decision.
//
// The manifest cannot introduce a host: every URL taken from a manifest and
// every redirect Location is re-validated here, so a signed-but-hostile
// manifest still cannot point downloads at a third party, and a compromised
// mirror cannot change which version is installed.
//
// No token, no query parameter derived from the user, no GitHub API, no HTML
// scraping, no release-page parsing — a fixed, public, unauthenticated
// Generic Package Registry path plus, optionally, an immutable mirror asset
// URL that the signed manifest itself supplies.
namespace lightning::update {

// Canonical release host. Overridable at build time only.
QString canonicalUpdateHost();

// Bandwidth mirror hosts for ARTIFACT BYTES only, from the build-time
// LIGHTNING_UPDATE_MIRROR_HOSTS list (';' separated, ',' also accepted
// because a bare ';' in a CMake compile definition splits into several -D
// flags). Lowercase, de-duplicated, never containing the canonical host, and
// entries that are not bare hosts are dropped rather than half-parsed.
QStringList mirrorArtifactHosts();

// Every host this build will talk to, canonical first (lowercase, exact
// match — never a suffix match, which would accept
// "evil-gitlab.smetonis.net"). Diagnostics only: code uses the two role
// predicates below.
QStringList allowedUpdateHosts();

// Canonical host ONLY. The manifest, its signature and release_notes_url.
// This is what the client asks FIRST and what it uses in every normal check.
bool isAllowedManifestUrl(const QUrl &url);

// v0.7.3 availability fallback. A mirrored copy of the manifest pair, used
// ONLY after the canonical host failed to answer, so that an outage on the
// release server stops updates being DISCOVERED rather than merely slowing
// them down. This deliberately relaxes the canonical-only metadata rule
// above, and the reasoning for why it is safe is worth keeping explicit:
//
//   * the Ed25519 signature is verified identically whichever host answered,
//     so a mirror cannot forge or alter a manifest;
//   * the client installs only a version strictly NEWER than the installed
//     one, so a mirror serving a stale (but validly signed) manifest offers
//     no update at all rather than a downgrade;
//   * the canonical host is always tried first, so in normal operation
//     GitLab still decides what exists.
//
// What a hostile mirror CAN do here is withhold a new version from clients
// that cannot reach GitLab — which is precisely the outage this exists to
// soften, so it trades nothing that was not already lost.
bool isAllowedFallbackManifestUrl(const QUrl &url);

// Empty when no mirror base is compiled in, which disables the fallback and
// restores canonical-only metadata byte for byte.
QUrl mirrorLatestManifestUrl();
QUrl mirrorLatestManifestSignatureUrl();

// Canonical host OR a mirror host. Artifact bytes and the redirect hops of
// an artifact download, and nothing else.
bool isAllowedArtifactUrl(const QUrl &url);

// https://<host>/api/v4/projects/<id>/packages/generic/lightning-update/latest/...
QUrl latestManifestUrl();
QUrl latestManifestSignatureUrl();
// Immutable per-release copies.
QUrl manifestUrlForVersion(const QString &version);
QUrl manifestSignatureUrlForVersion(const QString &version);

} // namespace lightning::update
