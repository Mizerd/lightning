#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

// Lightning secure update system — the compiled-in endpoint policy.
//
// Lightning contacts EXACTLY one host family, defined here at build time.
// The manifest cannot introduce a host: every URL taken from a manifest and
// every redirect Location is re-validated against this allowlist, so a
// signed-but-hostile manifest still cannot point downloads at a third party.
//
// No token, no query parameter derived from the user, no GitHub, no HTML
// scraping — a fixed, public, unauthenticated Generic Package Registry path.
namespace lightning::update {

// Canonical release host. Overridable at build time only.
QString canonicalUpdateHost();

// Every host this build will talk to (lowercase, exact match — never a
// suffix match, which would accept "evil-gitlab.smetonis.net").
QStringList allowedUpdateHosts();

// Scheme is https, host is on the allowlist, no userinfo, no non-standard
// port. Anything else is rejected without a diagnostic that leaks the URL.
bool isAllowedUpdateUrl(const QUrl &url);

// https://<host>/api/v4/projects/<id>/packages/generic/lightning-update/latest/...
QUrl latestManifestUrl();
QUrl latestManifestSignatureUrl();
// Immutable per-release copies.
QUrl manifestUrlForVersion(const QString &version);
QUrl manifestSignatureUrlForVersion(const QString &version);

} // namespace lightning::update
