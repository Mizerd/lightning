#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

// Compiled-in endpoint policy for updates. Two roles; merging them is a
// security regression:
//
//   * Metadata (manifest, signature, release_notes_url) is trusted only
//     because the Ed25519 signature verifies against the compiled-in key, and
//     only strictly newer versions are ever offered. The PRIMARY source is the
//     fixed GitHub release slot and the canonical GitLab registry is the
//     single FALLBACK, so a private or unreachable GitLab costs nothing.
//     Neither host decides anything: the signature does.
//   * Artifact bytes (and their redirect hops) may come from the compiled-in
//     mirror (tried first) or the canonical host (the fallback), and only
//     supply bytes the signed manifest already named, sized and hashed.
//
// Every manifest URL and redirect Location is re-validated here (https, port
// 443, no userinfo, exact host match), so neither a hostile manifest nor a
// compromised host can redirect downloads or change the installed version. No
// tokens, user-derived parameters, GitHub API or page scraping, and no
// "latest" URL GitHub could choose: a fixed release tag whose assets the
// pipeline replaces, plus the fixed public Generic Package Registry path.
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

// Canonical host only: the GitLab copy of the manifest pair, the manifest's
// artifact `url` entries and release_notes_url.
bool isAllowedManifestUrl(const QUrl &url);

// Mirror hosts only: the GitHub manifest pair and its redirect hops. The
// Ed25519 signature is verified the same way and only strictly newer versions
// are offered, so the slot can never cause an unsigned install or a downgrade.
// ACCEPTED TRADE-OFF: the fallback runs only on a transport failure or a failed
// signature. An OLD but validly signed manifest in the slot makes the client
// report "up to date" without asking GitLab, so whoever controls the slot
// (the GitHub account, a leaked mirror token, or a failed pipeline write) can
// freeze clients, or pin them to any signed release newer than the one they
// run. This is inherent once GitHub is the only public source; before the
// change, GitLab's rollback-guarded "latest" held that control.
bool isAllowedFallbackManifestUrl(const QUrl &url);

// The order the updater reads the manifest pair in. Primary is the GitHub slot
// when one is compiled in, otherwise the canonical copy. Fallback is the
// canonical copy when the primary is the mirror, otherwise empty. Each is
// tried at most once per check.
QUrl primaryManifestUrl();
QUrl primaryManifestSignatureUrl();
QUrl fallbackManifestUrl();
QUrl fallbackManifestSignatureUrl();

// The release page the user is sent to: a fixed tag URL on the mirror host
// derived from the (already signature-verified, semver-validated) version, so
// the link does not depend on the manifest's release_notes_url, which must keep
// naming the canonical host for deployed clients. Empty for a version with
// characters outside [0-9A-Za-z.-] or when the base is not on a mirror host.
QUrl releasePageUrlForVersion(const QString &version);

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
