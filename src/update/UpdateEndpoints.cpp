#include "update/UpdateEndpoints.h"

#include <QLatin1Char>
#include <QLatin1String>

// Canonical host and project id: compile-time constants that nothing at
// runtime can change.
#ifndef LIGHTNING_UPDATE_HOST
#define LIGHTNING_UPDATE_HOST "gitlab.smetonis.net"
#endif
#ifndef LIGHTNING_UPDATE_PROJECT_ID
// Project 6 is the Lightning project whose Generic Package Registry holds the
// published packages.
#define LIGHTNING_UPDATE_PROJECT_ID "6"
#endif
#ifndef LIGHTNING_UPDATE_MIRROR_HOSTS
// Hosts that may serve artifact bytes. GitHub release downloads redirect from
// github.com to *.githubusercontent.com, so both are needed. An empty list
// means no mirror is trusted: a manifest naming one is refused.
#define LIGHTNING_UPDATE_MIRROR_HOSTS                                                              \
    "github.com;objects.githubusercontent.com;release-assets.githubusercontent.com"
#endif

#ifndef LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE
// Base URL of the mirrored manifest pair, read only after the canonical host
// failed. Empty disables the fallback; a host outside the mirror list is
// dropped. A fixed tag whose assets the pipeline replaces, never a "latest"
// URL that would let GitHub choose the release. No API is called; the
// signature makes the bytes trustworthy. update-manager-state's source scan
// enforces this.
#define LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE                                                      \
    "https://github.com/Mizerd/lightning/releases/download/update-latest"
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
    // Credentials in an update URL are a leak or a redirect trick.
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

// Parsed once. Anything that is not a bare host (scheme, path, port,
// credentials, wildcard, whitespace) is dropped. ',' is accepted besides ';'
// because a ';' splits a CMake compile definition.
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
    // Canonical-only: GitLab decides what may be installed.
    return isAllowedUrlOn(url, QStringList{ canonicalUpdateHost() });
}

bool isAllowedFallbackManifestUrl(const QUrl &url)
{
    // Mirror hosts only; the canonical copy has its own predicate.
    return isAllowedUrlOn(url, mirrorArtifactHosts());
}

QUrl mirrorLatestManifestUrl()
{
    const QString base = QString::fromLatin1(LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE);
    if (base.isEmpty())
        return {};
    const QUrl url(base + QLatin1Char('/') + QLatin1String(kManifestFile));
    // A base outside the mirror hosts is dropped: a build-time typo must not
    // point metadata at a third party.
    return isAllowedFallbackManifestUrl(url) ? url : QUrl{};
}

QUrl mirrorLatestManifestSignatureUrl()
{
    const QString base = QString::fromLatin1(LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE);
    if (base.isEmpty())
        return {};
    const QUrl url(base + QLatin1Char('/') + QLatin1String(kSignatureFile));
    return isAllowedFallbackManifestUrl(url) ? url : QUrl{};
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
