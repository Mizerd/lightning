#include "update/UpdateEndpoints.h"

#include <QLatin1Char>
#include <QLatin1String>

// Canonical GitLab host and project id for the update feed. Both are
// compile-time constants: nothing at runtime — and certainly nothing from
// the network — can change where Lightning looks for updates.
#ifndef LIGHTNING_UPDATE_HOST
#define LIGHTNING_UPDATE_HOST "gitlab.smetonis.net"
#endif
#ifndef LIGHTNING_UPDATE_PROJECT_ID
// CLAUDE.md §14: project 6 is the Lightning source project whose Generic
// Package Registry holds the published packages. Confirm against the deploy
// repository before a release build.
#define LIGHTNING_UPDATE_PROJECT_ID "6"
#endif
#ifndef LIGHTNING_UPDATE_MIRROR_HOSTS
// Semicolon-separated hosts that may serve ARTIFACT BYTES. A GitHub release
// asset download 302s from github.com to a *.githubusercontent.com object
// host, so both object hosts must be acceptable as redirect targets or every
// mirror download would fail on its first hop.
//
// An EMPTY configured list is not "mirroring off": it means this build trusts
// no mirror, so a manifest that names one is refused rather than silently
// downgraded. Keep this non-empty unless that is genuinely what you want.
#define LIGHTNING_UPDATE_MIRROR_HOSTS                                                              \
    "github.com;objects.githubusercontent.com;release-assets.githubusercontent.com"
#endif

namespace lightning::update {
namespace {

constexpr char kManifestFile[] = "update-manifest-v1.json";
constexpr char kSignatureFile[] = "update-manifest-v1.json.sig";

QString registryBase()
{
    return QStringLiteral("https://%1/api/v4/projects/%2/packages/generic/lightning-update")
        .arg(canonicalUpdateHost(), QString::fromLatin1(LIGHTNING_UPDATE_PROJECT_ID));
}

QUrl documentUrl(const QString &slot, const char *file)
{
    return QUrl(registryBase() + QLatin1Char('/') + slot + QLatin1Char('/')
                + QString::fromLatin1(file));
}

// Transport policy shared by both roles. Only the accepted host set differs.
bool isAllowedUrlOn(const QUrl &url, const QStringList &hosts)
{
    if (!url.isValid() || url.isRelative())
        return false;
    if (url.scheme() != QLatin1String("https"))
        return false;
    // Credentials in an update URL would be both a leak and a redirect
    // trick; there is no legitimate use for them here.
    if (!url.userInfo().isEmpty())
        return false;
    const int port = url.port(443);
    if (port != 443)
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return false;
    return hosts.contains(host);
}

// Parsed once. A malformed entry is DROPPED rather than half-understood: a
// host is a host, so anything carrying a scheme, path, port, credentials,
// wildcard or whitespace is not one.
//
// Both ';' and ',' separate entries. The documented form is semicolons, but a
// semicolon is also CMake's list separator: an unescaped one in a compile
// definition splits the value into several -D flags. Accepting commas gives
// the build an unambiguous alternative, and no valid host contains either.
QStringList parseMirrorHosts()
{
    QStringList hosts;
    QString configured = QString::fromLatin1(LIGHTNING_UPDATE_MIRROR_HOSTS);
    configured.replace(QLatin1Char(','), QLatin1Char(';'));
    const QString canonical = canonicalUpdateHost();
    const QStringList parts = configured.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString host = part.trimmed().toLower();
        if (host.isEmpty())
            continue;
        if (host.contains(QLatin1Char('/')) || host.contains(QLatin1Char(':'))
            || host.contains(QLatin1Char('@')) || host.contains(QLatin1Char('*'))
            || host.contains(QLatin1Char(' '))) {
            continue;
        }
        if (host == canonical)
            continue; // already trusted for everything; not a mirror
        if (!hosts.contains(host))
            hosts.append(host);
    }
    return hosts;
}

} // namespace

QString canonicalUpdateHost()
{
    return QString::fromLatin1(LIGHTNING_UPDATE_HOST).toLower();
}

QStringList mirrorArtifactHosts()
{
    static const QStringList hosts = parseMirrorHosts();
    return hosts;
}

QStringList allowedUpdateHosts()
{
    QStringList hosts{ canonicalUpdateHost() };
    hosts += mirrorArtifactHosts();
    return hosts;
}

bool isAllowedManifestUrl(const QUrl &url)
{
    // Metadata is canonical-only. GitLab decides WHAT may be installed; a
    // mirror only carries bytes that decision already named.
    return isAllowedUrlOn(url, QStringList{ canonicalUpdateHost() });
}

bool isAllowedArtifactUrl(const QUrl &url)
{
    return isAllowedUrlOn(url, allowedUpdateHosts());
}

QUrl latestManifestUrl()
{
    return documentUrl(QStringLiteral("latest"), kManifestFile);
}

QUrl latestManifestSignatureUrl()
{
    return documentUrl(QStringLiteral("latest"), kSignatureFile);
}

QUrl manifestUrlForVersion(const QString &version)
{
    return documentUrl(QString(version).replace(QLatin1Char('/'), QLatin1Char('_')), kManifestFile);
}

QUrl manifestSignatureUrlForVersion(const QString &version)
{
    return documentUrl(QString(version).replace(QLatin1Char('/'), QLatin1Char('_')),
                       kSignatureFile);
}

} // namespace lightning::update
