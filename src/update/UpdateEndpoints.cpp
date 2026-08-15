#include "update/UpdateEndpoints.h"

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

} // namespace

QString canonicalUpdateHost()
{
    return QString::fromLatin1(LIGHTNING_UPDATE_HOST).toLower();
}

QStringList allowedUpdateHosts()
{
    return { canonicalUpdateHost() };
}

bool isAllowedUpdateUrl(const QUrl &url)
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
    return allowedUpdateHosts().contains(host);
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
