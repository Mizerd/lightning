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
// gitlab.com is a last-resort byte source, reached only after the other sources
// fail to answer. Its host, project and paths are constants here: nothing in
// a manifest or a response can name it, and it is excluded from
// isAllowedArtifactUrl so no other transfer can be redirected onto it.
//
// Every manifest URL and redirect Location is re-validated here (https, port
// 443, no userinfo, exact host match), so neither a hostile manifest nor a
// compromised host can redirect downloads or change the installed version. No
// tokens, user-derived parameters, GitHub API or page scraping, and no
// "latest" URL GitHub could choose: a fixed release tag whose assets the
// pipeline replaces, plus the fixed public Generic Package Registry path.
namespace lightning::update {

QString canonicalUpdateHost();

QStringList mirrorArtifactHosts();

QStringList allowedUpdateHosts();

bool isAllowedManifestUrl(const QUrl &url);

bool isAllowedFallbackManifestUrl(const QUrl &url);

QUrl primaryManifestUrl();
QUrl primaryManifestSignatureUrl();
QUrl fallbackManifestUrl();
QUrl fallbackManifestSignatureUrl();

QUrl releasePageUrlForVersion(const QString &version);

QUrl mirrorLatestManifestUrl();
QUrl mirrorLatestManifestSignatureUrl();

bool isAllowedArtifactUrl(const QUrl &url);

QString gitlabComHost();
bool isAllowedGitlabComUrl(const QUrl &url);
QUrl gitlabComManifestUrl();
QUrl gitlabComManifestSignatureUrl();
QUrl gitlabComArtifactUrl(const QString &version, const QString &filename);

QUrl latestManifestUrl();
QUrl latestManifestSignatureUrl();
QUrl manifestUrlForVersion(const QString &version);
QUrl manifestSignatureUrlForVersion(const QString &version);

}
