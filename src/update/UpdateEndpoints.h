#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

// Compiled-in endpoint policy for updates. Two roles; merging them is a
// security regression:
//
//   * Metadata (manifest, signature, release_notes_url) comes from the
//     canonical release host only. GitLab decides what may be installed.
//   * Artifact bytes (and their redirect hops) may also come from a
//     compiled-in mirror, which only supplies bytes the signed manifest
//     already named, sized and hashed.
//
// Every manifest URL and redirect Location is re-validated here, so neither a
// hostile manifest nor a compromised mirror can redirect downloads or change
// the installed version. No tokens, user-derived parameters, GitHub API or
// page scraping: a fixed public Generic Package Registry path, plus an
// optional immutable mirror URL from the signed manifest.
namespace lightning::update {

// Canonical release host. Overridable at build time only.
QString canonicalUpdateHost();

// Artifact mirror hosts from LIGHTNING_UPDATE_MIRROR_HOSTS (';' or ','
// separated; a bare ';' would split a CMake -D flag). Lowercase, de-duplicated,
// never the canonical host; malformed entries are dropped.
QStringList mirrorArtifactHosts();

// Every host this build contacts, canonical first. Exact matches only, never a
// suffix match ("evil-gitlab.smetonis.net"). Diagnostics only.
QStringList allowedUpdateHosts();

// Canonical host only: the manifest, its signature and release_notes_url.
// Always tried first.
bool isAllowedManifestUrl(const QUrl &url);

// Availability fallback: a mirrored manifest pair, read only after the
// canonical host failed. Relaxing the canonical-only rule is safe because the
// Ed25519 signature is verified the same way, only strictly newer versions are
// offered (a stale manifest offers nothing), and the canonical host is always
// tried first. A hostile mirror can at most withhold an update from clients
// that cannot reach GitLab anyway.
bool isAllowedFallbackManifestUrl(const QUrl &url);

// Empty when no mirror base is compiled in, disabling the fallback.
QUrl mirrorLatestManifestUrl();
QUrl mirrorLatestManifestSignatureUrl();

// Canonical or mirror host: artifact bytes and their redirect hops only.
bool isAllowedArtifactUrl(const QUrl &url);

// https://<host>/api/v4/projects/<id>/packages/generic/lightning-update/latest/...
QUrl latestManifestUrl();
QUrl latestManifestSignatureUrl();
// Immutable per-release copies.
QUrl manifestUrlForVersion(const QString &version);
QUrl manifestSignatureUrlForVersion(const QString &version);

} // namespace lightning::update
