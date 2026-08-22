#include "profile/ProfileBannerManager.h"

#include <QRegularExpression>
#include <QUrl>

ProfileBannerManager::ProfileBannerManager(QObject *parent)
    : QObject(parent)
{
}

void ProfileBannerManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clearSession();
    if (!m_client) {
        Q_EMIT availableChanged();
        return;
    }
    connect(m_client, &MatrixClient::profileBannerReceived, this,
            &ProfileBannerManager::handleReceived);
    connect(m_client, &MatrixClient::profileBannerSet, this,
            &ProfileBannerManager::handleSet);
    connect(m_client, &MatrixClient::roomBannerReceived, this,
            &ProfileBannerManager::handleRoomReceived);
    connect(m_client, &MatrixClient::roomBannerSet, this,
            &ProfileBannerManager::handleRoomSet);
    // A banner belongs to the account that fetched it. The next account's
    // profile cards must not inherit the previous one's answers.
    connect(m_client, &MatrixClient::loggedOut, this,
            &ProfileBannerManager::clearSession);
    Q_EMIT availableChanged();
}

bool ProfileBannerManager::available() const
{
    return m_client && m_client->supportsProfileBanners();
}

bool ProfileBannerManager::roomBannersAvailable() const
{
    return m_client && m_client->supportsRoomBanners();
}

QString ProfileBannerManager::bannerFor(const QString &userId) const
{
    return m_cache.value(userId);
}

QString ProfileBannerManager::ownBanner() const
{
    if (!m_client)
        return {};
    return bannerFor(m_client->currentUserId());
}

void ProfileBannerManager::request(const QString &userId)
{
    if (!available() || userId.isEmpty() || !m_supported)
        return;
    // Once per user per session, and once per user in flight: a profile
    // popover that opens, closes and opens again must not cost two requests.
    if (m_asked.contains(userId))
        return;
    if (m_cache.size() >= kMaxCached)
        return;
    m_asked.insert(userId);
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, userId);
    m_client->fetchProfileBanner(userId, opId);
}

// A file the user picked reaches QML as a URL, and stripping "file://" by
// hand is wrong on Windows: file:///C:/x.png becomes /C:/x.png, a path with a
// leading slash before the drive letter. QUrl knows the platform rule;
// AttachmentQueueModel::addFile has always used it, and this is the same
// conversion in the same place — at the C++ edge, so no caller can get it
// wrong. A value that is already a plain path passes through untouched.
static QString localPathFrom(const QString &pathOrUrl)
{
    if (pathOrUrl.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(pathOrUrl);
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }
    // Any OTHER scheme is refused rather than passed on as though it were a
    // path — an uploader handed "https://…" would report a missing file, and
    // the reason would look like the user's fault.
    //
    // Matched on "scheme://" and NOT with QUrl::scheme(), because a Windows
    // drive path is a URL with a scheme: QUrl("C:/x.png").scheme() is "c".
    // The authority slashes are what separate the two.
    static const QRegularExpression scheme(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    if (scheme.match(pathOrUrl).hasMatch())
        return {};
    return pathOrUrl;
}

void ProfileBannerManager::setOwnBanner(const QString &pathOrUrl)
{
    const QString localPath = localPathFrom(pathOrUrl);
    if (!available() || localPath.isEmpty() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setProfileBanner(localPath, m_pendingWrite);
}

void ProfileBannerManager::clearOwnBanner()
{
    if (!available() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    // An empty path IS the clear; the Rust side deletes both field names.
    m_client->setProfileBanner(QString(), m_pendingWrite);
}

void ProfileBannerManager::handleReceived(quint64 opId, const QString &userId,
                                          const QString &mxc, bool supported)
{
    const auto it = m_inFlight.constFind(opId);
    if (it == m_inFlight.constEnd())
        return;   // a stale answer from a previous session
    m_inFlight.erase(it);

    if (!supported && m_supported) {
        // Latched for the session: the server has said it does not know the
        // endpoint, so every further request would ask the same question and
        // get the same answer. It clears with the session.
        m_supported = false;
        Q_EMIT supportedChanged();
    }
    const QString previous = m_cache.value(userId);
    m_cache.insert(userId, mxc);
    if (previous == mxc)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::handleSet(quint64 opId, bool ok, const QString &mxc,
                                     const QString &category)
{
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        // A write that came back unrecognised settles the same question the
        // read settles, and settles it more definitively — the endpoint is
        // not there. Without this the account could be invited to fail at
        // the same upload indefinitely, because nothing else on this surface
        // ever asks. It clears with the session, like the read's latch.
        if (category == QLatin1String("unsupported") && m_supported) {
            m_supported = false;
            Q_EMIT supportedChanged();
        }
        return;
    }
    setLastError({});
    if (!m_client)
        return;
    // Nothing is applied optimistically anywhere else in this application and
    // nothing is here either — but the server has now ACKNOWLEDGED the write,
    // so the local answer for our own user is authoritative and does not need
    // a round trip to re-read.
    const QString self = m_client->currentUserId();
    if (self.isEmpty())
        return;
    m_cache.insert(self, mxc);
    m_asked.insert(self);
    ++m_revision;
    Q_EMIT revisionChanged();
}

QString ProfileBannerManager::roomBannerFor(const QString &roomId) const
{
    return m_roomCache.value(roomId);
}

bool ProfileBannerManager::canSetRoomBanner(const QString &roomId) const
{
    return m_roomWritable.contains(roomId);
}

void ProfileBannerManager::requestRoom(const QString &roomId)
{
    if (roomId.isEmpty() || m_roomAsked.contains(roomId))
        return;
    refreshRoom(roomId);
}

void ProfileBannerManager::refreshRoom(const QString &roomId)
{
    if (!roomBannersAvailable() || roomId.isEmpty())
        return;
    if (m_roomCache.size() >= kMaxCached)
        return;
    m_roomAsked.insert(roomId);
    const quint64 opId = m_nextOpId++;
    m_roomInFlight.insert(opId, roomId);
    m_client->fetchRoomBanner(roomId, opId);
}

void ProfileBannerManager::setRoomBanner(const QString &roomId,
                                         const QString &pathOrUrl)
{
    const QString localPath = localPathFrom(pathOrUrl);
    if (!roomBannersAvailable() || roomId.isEmpty() || localPath.isEmpty()
        || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setRoomBanner(roomId, localPath, m_pendingWrite);
}

void ProfileBannerManager::clearRoomBanner(const QString &roomId)
{
    if (!roomBannersAvailable() || roomId.isEmpty() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    // An empty path IS the clear; Rust sends an empty content object.
    m_client->setRoomBanner(roomId, QString(), m_pendingWrite);
}

void ProfileBannerManager::handleRoomReceived(quint64 opId,
                                              const QString &roomId,
                                              const QString &mxc, bool canSet)
{
    const auto it = m_roomInFlight.constFind(opId);
    if (it == m_roomInFlight.constEnd())
        return;   // a stale answer from a previous session
    m_roomInFlight.erase(it);

    const QString previous = m_roomCache.value(roomId);
    const bool couldSet = m_roomWritable.contains(roomId);
    m_roomCache.insert(roomId, mxc);
    if (canSet)
        m_roomWritable.insert(roomId);
    else
        m_roomWritable.remove(roomId);
    if (previous == mxc && couldSet == canSet)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::handleRoomSet(quint64 opId, const QString &roomId,
                                         bool ok, const QString &mxc,
                                         const QString &category)
{
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        return;
    }
    setLastError({});
    // The server ACKNOWLEDGED the state event, so this answer is
    // authoritative for the room and needs no round trip to re-read.
    m_roomCache.insert(roomId, mxc);
    m_roomAsked.insert(roomId);
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::clearSession()
{
    m_cache.clear();
    m_asked.clear();
    m_inFlight.clear();
    m_roomCache.clear();
    m_roomAsked.clear();
    m_roomWritable.clear();
    m_roomInFlight.clear();
    m_pendingWrite = 0;
    setLastError({});
    // A different account may be on a server that DOES implement extended
    // profiles; the latch is per session.
    if (!m_supported) {
        m_supported = true;
        Q_EMIT supportedChanged();
    }
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT busyChanged();
}

void ProfileBannerManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
