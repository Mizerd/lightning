#include "backdrop/ChatBackdropController.h"

#include "app/SettingsManager.h"
#include "crypto/E2eeDiagnostics.h"
#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"
#include "media/ImageFormatSupport.h"
#include "media/MediaBridge.h"
#include "media/StagedImageStore.h"
#include "media/SvgRasterJob.h"
#include "spaces/SpaceManager.h"
#include "storage/AppDataPaths.h"

#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <utility>

Q_LOGGING_CATEGORY(lcBackdrop, "lightning.backdrop")

namespace {

// Room/Space ids are redacted in logs; no URL, path or content is logged.
QString scopeTag(const QString &scopeId)
{
    return matrix::e2ee::redactId(scopeId);
}

// The shared event stores dim/blur/tint as integer percentages (canonical
// JSON has no floats); the app works in unit values. Converts a content map
// read from a room to the app's form.
QVariantMap contentFromWire(const QVariantMap &content)
{
    if (content.isEmpty())
        return content;
    QVariantMap out = content;
    out.insert(QStringLiteral("presentation"),
               backdrop::presentationFromWire(
                   content.value(QStringLiteral("presentation")).toMap()));
    return out;
}

// Device-and-account appearance preferences (appearanceValue).
constexpr auto kShowSharedKey = "appearance/backdropShowShared";
constexpr auto kDepthKey = "appearance/surfaceDepth";
// Strictly per account: it names rooms (accountScopedValue).
constexpr auto kPersonalKey = "backdrop/personal";
// "Keep my backgrounds on my homeserver": a per-account MIRROR of the
// account-wide switch in the global account data, default on.
constexpr auto kSyncKey = "backdrop/sync";
// A switch change ("on"/"off") this device made and has not yet written to
// the homeserver; replayed until it lands.
constexpr auto kSwitchPendingKey = "backdrop/syncSwitchPending";
// The one-time notice before pictures chosen earlier are uploaded was
// answered.
constexpr auto kNoticeAckKey = "backdrop/syncNoticeAck";

// A sync scope in logs: the default, or a redacted room id.
QString syncTag(const QString &scope)
{
    return scope.isEmpty() ? QStringLiteral("default")
                           : matrix::e2ee::redactId(scope);
}

constexpr auto kStagedPrefix = "image://lightning-staged/";
constexpr int kMaxHiddenRooms = 512;
// Decode bound for measuring: twice the sample edge, so measure() scales
// from a real image rather than a JPEG DC-only thumbnail.
constexpr int kMeasureDecodeEdge = backdrop::kSampleEdge * 2;
// A personal file we wrote ourselves is far below this; anything bigger on
// disk was not written by Lightning.
constexpr qint64 kMaxStoredFileBytes = 32LL * 1024 * 1024;

// The names Lightning gives personal pictures: sha256 of the encoded bytes.
// Anything else in a record (a hand-edited config) is refused, so a record
// can never point outside the backgrounds directory.
const QRegularExpression &storedNameRe()
{
    static const QRegularExpression re(
        QStringLiteral("^[0-9a-f]{64}\\.(jpg|png)$"));
    return re;
}

bool isHexColour(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re.match(value).hasMatch();
}

// The id of a server copy (the ciphertext's SHA-256, unpadded base64), as a
// record keeps it. Anything else is dropped, which makes the record
// local-only: at worst uploaded again, never pointed at a wrong copy.
bool isRemoteId(const QString &value)
{
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9+/_=-]{16,128}$"));
    return re.match(value).hasMatch();
}

// One personal record as stored, cleaned: { file, color, w, h, presentation,
// remote } or {} when unusable.
QVariantMap cleanPersonalRecord(const QVariant &raw)
{
    const QVariantMap record = raw.toMap();
    const QString file = record.value(QStringLiteral("file")).toString();
    if (!storedNameRe().match(file).hasMatch())
        return {};
    QVariantMap out;
    out.insert(QStringLiteral("file"), file);
    const QString remote = record.value(QStringLiteral("remote")).toString();
    if (isRemoteId(remote)) {
        out.insert(QStringLiteral("remote"), remote);
        // A mark set by filling an empty place, not yet confirmed by a read
        // (review N2). Dropping it on a reload would silently confirm it, and
        // the race it guards would be back after any restart (review F2).
        if (record.value(QStringLiteral("unconfirmed")).toBool())
            out.insert(QStringLiteral("unconfirmed"), true);
    }
    // The SHA-256 of the file this picture was re-encoded from (a download):
    // the same picture coming back is recognised by it.
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    const QString source = record.value(QStringLiteral("source")).toString();
    if (sha.match(source).hasMatch())
        out.insert(QStringLiteral("source"), source);
    // A local-only picture that differs from the server's copy, waiting for
    // the user to pick one (ChatBackdropController::resolveConflict).
    const QString conflict = record.value(QStringLiteral("conflict")).toString();
    if (isRemoteId(conflict) && !out.contains(QStringLiteral("remote"))) {
        out.insert(QStringLiteral("conflict"), conflict);
        // The two look alike: the card pre-selects the synced one.
        if (record.value(QStringLiteral("conflictLooksSame")).toBool())
            out.insert(QStringLiteral("conflictLooksSame"), true);
    }
    // A DORMANT mark: the server copy this picture mirrored before sync went
    // off. Recognised again, but never a reason to delete (review R3).
    const QString was = record.value(QStringLiteral("was")).toString();
    if (isRemoteId(was) && !out.contains(QStringLiteral("remote")))
        out.insert(QStringLiteral("was"), was);
    const QString colour = record.value(QStringLiteral("color")).toString();
    if (isHexColour(colour))
        out.insert(QStringLiteral("color"), colour.toUpper());
    const int w = record.value(QStringLiteral("w")).toInt();
    const int h = record.value(QStringLiteral("h")).toInt();
    if (w > 0 && h > 0) {
        out.insert(QStringLiteral("w"), w);
        out.insert(QStringLiteral("h"), h);
    }
    out.insert(QStringLiteral("presentation"),
               backdrop::normalisePresentation(
                   record.value(QStringLiteral("presentation")).toMap()));
    return out;
}

QString toJson(const QVariantMap &map)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

// Decodes raster bytes small, for measuring. The format is pinned from the
// magic bytes with autodetection off, so no SVG or unknown handler can run.
QImage decodeSmall(const QByteArray &bytes, int edge)
{
    const lightning::imagefmt::RasterFormat *format =
        lightning::imagefmt::sniffRaster(bytes);
    if (!format)
        return {};
    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, QByteArray(format->qtFormat));
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(true);
    reader.setAllocationLimit(64);
    const QSize natural = reader.size();
    if (!natural.isValid() || natural.isEmpty())
        return {};
    if (natural.width() > edge || natural.height() > edge)
        reader.setScaledSize(natural.scaled(edge, edge, Qt::KeepAspectRatio));
    return reader.read();
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

ChatBackdropController::ChatBackdropController(QObject *parent)
    : QObject(parent)
{
    m_writeWatchdog.setSingleShot(true);
    m_switchWatchdog.setSingleShot(true);
    connect(&m_writeWatchdog, &QTimer::timeout, this,
            &ChatBackdropController::writeTimedOut);
    connect(&m_switchWatchdog, &QTimer::timeout, this,
            &ChatBackdropController::switchTimedOut);
}

void ChatBackdropController::writeTimedOut()
{
    if (m_syncWriteOp == 0)
        return;
    // Failed like any other write; its answer, if it ever comes, no longer
    // matches m_syncWriteOp and is dropped. The next op may go.
    qCWarning(lcBackdrop) << "sync write op=" << m_syncWriteOp << "result=timeout";
    const SyncOp stuck = m_syncWriting;
    m_syncWriteOp = 0;
    m_syncWriting = SyncOp();
    syncFailed(stuck.scope, stuck.mode, QStringLiteral("timeout"), stuck.intoEmpty);
    if (m_syncClearRequested && m_syncClearOp == 0)
        startClearAll();
    processSyncQueue();
    Q_EMIT syncChanged();
}

void ChatBackdropController::switchTimedOut()
{
    if (m_enableOp == 0)
        return;
    // Still owed (switchPending): replayed on the next start or Retry.
    qCWarning(lcBackdrop) << "sync switch write op=" << m_enableOp << "result=timeout";
    m_enableOp = 0;
    m_switchError = QStringLiteral("timeout");
    Q_EMIT syncChanged();
}

ChatBackdropController::~ChatBackdropController() = default;

// ---- wiring ----------------------------------------------------------------

void ChatBackdropController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clearSession();
    if (m_client) {
        connect(m_client, &MatrixClient::roomBackgroundReceived, this,
                &ChatBackdropController::handleReceived);
        connect(m_client, &MatrixClient::roomBackgroundSet, this,
                &ChatBackdropController::handleSet);
        // A change observed in sync: the only way an OPEN room learns of it.
        connect(m_client, &MatrixClient::roomBackgroundChanged, this,
                &ChatBackdropController::handleChanged);
        // Answers belong to the account that asked.
        connect(m_client, &MatrixClient::loggedOut, this,
                &ChatBackdropController::clearSession);
        // A Space's children moved: the chain is recomputed on next read.
        connect(m_client, &MatrixClient::roomsChanged, this,
                [this] { m_chainCache.clear(); });
        // Personal backgrounds on the homeserver.
        connect(m_client, &MatrixClient::personalBackgroundsRead, this,
                &ChatBackdropController::handleSyncRead);
        connect(m_client, &MatrixClient::personalBackgroundDownloaded, this,
                &ChatBackdropController::handleSyncDownloaded);
        connect(m_client, &MatrixClient::personalBackgroundWritten, this,
                &ChatBackdropController::handleSyncWritten);
        connect(m_client, &MatrixClient::personalBackgroundsCleared, this,
                &ChatBackdropController::handleSyncCleared);
        connect(m_client, &MatrixClient::personalBackgroundChanged, this,
                &ChatBackdropController::handleSyncChanged);
        // Account data is in the store once the first sync has run.
        connect(m_client, &MatrixClient::initialSyncDoneChanged, this,
                &ChatBackdropController::startSync);
    }
    Q_EMIT availableChanged();
    startSync();
}

void ChatBackdropController::setMediaBridge(MediaBridge *bridge)
{
    if (m_bridge == bridge)
        return;
    if (m_bridge)
        disconnect(m_bridge, nullptr, this, nullptr);
    m_bridge = bridge;
    if (m_bridge) {
        connect(m_bridge, &MediaBridge::mediaCached, this,
                &ChatBackdropController::handleMediaCached);
        // Shared with starring, copying and forwarding; only the key this
        // class asked for is consumed (handleMediaBytes).
        connect(m_bridge, &MediaBridge::mediaBytesForStar, this,
                &ChatBackdropController::handleMediaBytes);
    }
}

void ChatBackdropController::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        // The personal store is account-scoped; sessionChanged fires after
        // the active account id has moved (switch, add, sign-out).
        connect(m_settings, &SettingsManager::sessionChanged, this,
                &ChatBackdropController::onSessionChanged);
    }
    onSessionChanged();
}

void ChatBackdropController::setSpaces(SpaceManager *spaces)
{
    if (m_spaces == spaces)
        return;
    if (m_spaces)
        disconnect(m_spaces, nullptr, this, nullptr);
    m_spaces = spaces;
    if (m_spaces) {
        // The active Space picks which parent a multi-Space room inherits.
        connect(m_spaces, &SpaceManager::activeSpaceIdChanged, this, [this] {
            m_chainCache.clear();
            bump();
        });
    }
}

void ChatBackdropController::setStagedImages(StagedImageStore *store)
{
    m_staged = store;
}

// ---- properties ------------------------------------------------------------

bool ChatBackdropController::sharedAvailable() const
{
    return m_client && m_client->supportsRoomBackgrounds();
}

bool ChatBackdropController::showShared() const
{
    if (!m_settings)
        return true;
    return m_settings->appearanceValue(kShowSharedKey, true).toBool();
}

void ChatBackdropController::setShowShared(bool show)
{
    if (!m_settings || showShared() == show)
        return;
    m_settings->setAppearanceValue(kShowSharedKey, show);
    Q_EMIT settingsChanged();
    bump();
}

int ChatBackdropController::surfaceDepth() const
{
    if (!m_settings)
        return 0;
    // Clamped to the two styles that exist; a value from a newer build with
    // more styles falls back to flat rather than to the nearest (§16: an enum
    // needs a fallback, not std::clamp).
    const int depth = m_settings->appearanceValue(kDepthKey, 0).toInt();
    return depth == 1 ? 1 : 0;
}

void ChatBackdropController::setSurfaceDepth(int depth)
{
    const int clean = depth == 1 ? 1 : 0;
    if (!m_settings || surfaceDepth() == clean)
        return;
    m_settings->setAppearanceValue(kDepthKey, clean);
    Q_EMIT settingsChanged();
}

QVariantMap ChatBackdropController::prepared() const
{
    if (!m_prepared)
        return {};
    QVariantMap out;
    out.insert(QStringLiteral("imageUrl"), m_prepared->imageUrl);
    out.insert(QStringLiteral("width"), m_prepared->width);
    out.insert(QStringLiteral("height"), m_prepared->height);
    out.insert(QStringLiteral("color"),
               m_prepared->stats.dominant.isValid()
                   ? m_prepared->stats.dominant.name(QColor::HexRgb).toUpper()
                   : QString());
    out.insert(QStringLiteral("statsKey"),
               QStringLiteral("prepared:") + m_prepared->hash);
    return out;
}

// ---- resolution ------------------------------------------------------------

QStringList ChatBackdropController::spaceChainFor(const QString &roomId) const
{
    if (roomId.isEmpty() || !m_client)
        return {};
    const auto cached = m_chainCache.constFind(roomId);
    if (cached != m_chainCache.constEnd())
        return cached.value();

    QHash<QString, QStringList> childrenOf;
    QStringList order;
    const QList<RoomInfo> rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (!room.isSpace || room.membership != RoomInfo::Joined)
            continue;
        childrenOf.insert(room.id, room.childRoomIds);
        order.append(room.id);
    }
    const QString active = m_spaces ? m_spaces->activeSpaceId() : QString();
    const QStringList chain =
        backdrop::spaceChain(roomId, childrenOf, order, active);
    if (m_chainCache.size() >= kMaxCachedScopes)
        m_chainCache.clear();
    m_chainCache.insert(roomId, chain);
    return chain;
}

QVariantMap ChatBackdropController::backdropFor(const QString &roomId) const
{
    const QVariantMap store = personalStore();
    backdrop::ResolveInput in;
    in.personalRoom = store.value(QStringLiteral("rooms")).toMap()
                          .value(roomId).toMap();
    in.personalDefault = store.value(QStringLiteral("default")).toMap();
    in.roomHidden = store.value(QStringLiteral("hidden")).toStringList()
                        .contains(roomId);
    in.showShared = showShared();
    in.sharedRoom = m_shared.value(roomId).content;
    for (const QString &space : spaceChainFor(roomId))
        in.sharedSpaces.append(qMakePair(space, m_shared.value(space).content));

    const backdrop::Resolved resolved = backdrop::resolve(roomId, in);
    const bool shared = resolved.source == QLatin1String("room")
                        || resolved.source == QLatin1String("space");
    QVariantMap out = describeRecord(resolved.record, shared);
    out.insert(QStringLiteral("source"), resolved.source);
    out.insert(QStringLiteral("scopeId"), resolved.scopeId);
    return out;
}

QVariantMap ChatBackdropController::describeRecord(const QVariantMap &record,
                                                   bool shared) const
{
    QVariantMap out;
    const QVariantMap presentation = backdrop::normalisePresentation(
        record.value(QStringLiteral("presentation")).toMap());
    for (auto it = presentation.constBegin(); it != presentation.constEnd(); ++it)
        out.insert(it.key(), it.value());
    out.insert(QStringLiteral("kind"), QStringLiteral("none"));
    out.insert(QStringLiteral("mxc"), QString());
    out.insert(QStringLiteral("imageUrl"), QString());
    out.insert(QStringLiteral("statsKey"), QString());
    const QString colour = record.value(QStringLiteral("color")).toString();
    out.insert(QStringLiteral("color"), isHexColour(colour) ? colour : QString());
    if (record.isEmpty())
        return out;

    if (shared) {
        const QString url = record.value(QStringLiteral("url")).toString();
        if (!url.startsWith(QLatin1String("mxc://")))
            return out;
        out.insert(QStringLiteral("kind"), QStringLiteral("mxc"));
        out.insert(QStringLiteral("mxc"), url);
        out.insert(QStringLiteral("statsKey"), QStringLiteral("mxc:") + url);
        return out;
    }
    const QString file = record.value(QStringLiteral("file")).toString();
    if (!storedNameRe().match(file).hasMatch())
        return out;
    const QString url = stagedUrlForFile(file);
    if (url.isEmpty())
        return out;   // missing on disk: render nothing rather than break
    ensureMeasuredFile(file);
    out.insert(QStringLiteral("kind"), QStringLiteral("staged"));
    out.insert(QStringLiteral("imageUrl"), url);
    out.insert(QStringLiteral("statsKey"), QStringLiteral("file:") + file);
    return out;
}

void ChatBackdropController::requestRoom(const QString &roomId)
{
    // This account's own picture for the room, once per session: the store
    // may not hold that room's account data, and a room's own read asks the
    // server when it does not.
    if (syncActive() && roomId.startsWith(QLatin1Char('!'))
        && !m_remote.contains(roomId) && !m_syncAsked.contains(roomId)) {
        if (m_syncAsked.size() >= 2048)
            m_syncAsked.clear();
        m_syncAsked.insert(roomId);
        syncRead(roomId);
    }
    if (!sharedAvailable() || roomId.isEmpty())
        return;
    QStringList scopes{ roomId };
    scopes.append(spaceChainFor(roomId));
    const qint64 now = nowMs();
    for (const QString &scope : scopes) {
        const auto asked = m_lastAsked.constFind(scope);
        if (asked != m_lastAsked.constEnd()
            && now - asked.value() < kRefreshIntervalMs)
            continue;
        refreshScope(scope);
    }
}

void ChatBackdropController::refreshScope(const QString &scopeId)
{
    if (!sharedAvailable() || scopeId.isEmpty())
        return;
    // The cap bounds growth only; a scope already known may always re-read,
    // since sliding sync never delivers this state.
    if (!m_shared.contains(scopeId) && m_shared.size() >= kMaxCachedScopes)
        return;
    for (auto it = m_inFlight.constBegin(); it != m_inFlight.constEnd(); ++it) {
        if (it.value() == scopeId)
            return;   // one read per scope at a time
    }
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, scopeId);
    m_lastAsked.insert(scopeId, nowMs());
    m_client->fetchRoomBackground(scopeId, opId);
}

void ChatBackdropController::handleChanged(const QString &scopeId)
{
    // Only a scope this session shows, or asked about: the open room, or a
    // Space one of its rooms inherits from (requestRoom asks the whole chain).
    // A change anywhere else is read when that room opens, as before.
    if (scopeId.isEmpty()
        || (!m_shared.contains(scopeId) && !m_lastAsked.contains(scopeId)))
        return;
    for (auto it = m_inFlight.constBegin(); it != m_inFlight.constEnd(); ++it) {
        if (it.value() == scopeId) {
            // The read in flight may have been answered before the change;
            // read once more when it lands rather than keep a stale answer.
            m_rereadAfterAnswer.insert(scopeId);
            return;
        }
    }
    qCInfo(lcBackdrop) << "shared changed in sync scope=" << scopeTag(scopeId);
    // Not rate-limited by kRefreshIntervalMs: that bounds re-reads on OPEN,
    // and this is a real change. refreshScope keeps one read per scope.
    refreshScope(scopeId);
}

void ChatBackdropController::handleReceived(quint64 opId, const QString &roomId,
                                            const QVariantMap &content,
                                            bool canSet,
                                            bool unsupportedVersion)
{
    const auto it = m_inFlight.constFind(opId);
    // Not asked by this session, or for another room: a stale answer.
    if (it == m_inFlight.constEnd() || it.value() != roomId) {
        qCDebug(lcBackdrop) << "shared read answer dropped op=" << opId
                            << "scope=" << scopeTag(roomId);
        return;
    }
    m_inFlight.erase(it);

    Shared next;
    // Only a usable mxc url makes a background; anything else is "none".
    const bool has = content.value(QStringLiteral("url")).toString()
                         .startsWith(QLatin1String("mxc://"));
    if (has)
        next.content = contentFromWire(content);
    qCInfo(lcBackdrop) << "shared read op=" << opId << "scope=" << scopeTag(roomId)
                       << "background=" << has << "can_set=" << canSet
                       << "newer_schema=" << unsupportedVersion;
    next.canSet = canSet;
    next.unsupported = unsupportedVersion;
    next.known = true;
    const Shared previous = m_shared.value(roomId);
    m_shared.insert(roomId, next);
    // A change arrived while this read was in flight: read again.
    if (m_rereadAfterAnswer.remove(roomId))
        refreshScope(roomId);
    if (previous.known && previous.content == next.content
        && previous.canSet == next.canSet
        && previous.unsupported == next.unsupported)
        return;
    bump();
}

void ChatBackdropController::handleSet(quint64 opId, const QString &roomId,
                                       bool ok, const QVariantMap &content,
                                       const QString &category)
{
    if (opId != m_pendingWrite || roomId != m_pendingWriteScope) {
        // Logged loudly: a dropped SUCCESS would leave the editor busy for good.
        qCWarning(lcBackdrop) << "shared write answer dropped op=" << opId
                              << "scope=" << scopeTag(roomId)
                              << "pending_op=" << m_pendingWrite << "ok=" << ok;
        return;
    }
    m_pendingWrite = 0;
    m_pendingWriteScope.clear();
    Q_EMIT busyChanged();
    if (!ok) {
        const QString shown = category.isEmpty() ? QStringLiteral("failed") : category;
        qCWarning(lcBackdrop) << "shared write op=" << opId << "scope=" << scopeTag(roomId)
                              << "result=fail category=" << shown;
        setLastError(shown);
        return;
    }
    qCInfo(lcBackdrop) << "shared write op=" << opId << "scope=" << scopeTag(roomId)
                       << "result=ok cleared="
                       << !content.value(QStringLiteral("url")).toString()
                               .startsWith(QLatin1String("mxc://"));
    setLastError(QString());
    // Acknowledged by the server: authoritative without a re-read. A write
    // implies permission, so canSet stays as it was.
    Shared next = m_shared.value(roomId);
    next.content = content.value(QStringLiteral("url")).toString()
                           .startsWith(QLatin1String("mxc://"))
                       ? contentFromWire(content)
                       : QVariantMap();
    next.unsupported = false;
    next.known = true;
    m_shared.insert(roomId, next);
    // The upload copy is no longer needed; a later write prepares afresh.
    if (m_prepared)
        discardPrepared();
    bump();
}

// ---- measuring ---------------------------------------------------------------

void ChatBackdropController::handleMediaCached(const QString &cacheKey)
{
    if (!m_bridge)
        return;
    // Only wide-image entries, and only pictures a room here actually uses.
    for (auto it = m_shared.constBegin(); it != m_shared.constEnd(); ++it) {
        const QString url = it.value().content.value(QStringLiteral("url")).toString();
        if (url.isEmpty() || MediaBridge::wideImageCacheKey(url) != cacheKey)
            continue;
        const QString key = QStringLiteral("mxc:") + url;
        if (m_stats.value(key).measured)
            return;
        measureMxc(url);
        if (m_stats.value(key).measured)
            bump();
        return;
    }
}

void ChatBackdropController::measureMxc(const QString &mxc)
{
    if (!m_bridge || !mxc.startsWith(QLatin1String("mxc://")))
        return;
    const QByteArray bytes =
        m_bridge->cachedBytes(MediaBridge::wideImageCacheKey(mxc));
    if (bytes.isEmpty())
        return;
    const QImage thumb = decodeSmall(bytes, kMeasureDecodeEdge);
    if (thumb.isNull())
        return;   // unmeasured stays worst case: safe
    const QString key = QStringLiteral("mxc:") + mxc;
    if (!m_stats.contains(key)) {
        m_statsOrder.append(key);
        while (m_statsOrder.size() > kMaxStats)
            m_stats.remove(m_statsOrder.takeFirst());
    }
    m_stats.insert(key, backdrop::measure(thumb));
    m_scrimCache.clear();
}

void ChatBackdropController::ensureMeasuredFile(const QString &fileName) const
{
    const QString key = QStringLiteral("file:") + fileName;
    if (m_stats.value(key).measured)
        return;
    const QString dir = storageDir();
    if (dir.isEmpty())
        return;
    QFile file(QDir(dir).filePath(fileName));
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxStoredFileBytes)
        return;
    const QImage thumb = decodeSmall(file.readAll(), kMeasureDecodeEdge);
    if (thumb.isNull())
        return;
    if (!m_stats.contains(key)) {
        m_statsOrder.append(key);
        while (m_statsOrder.size() > kMaxStats)
            m_stats.remove(m_statsOrder.takeFirst());
    }
    m_stats.insert(key, backdrop::measure(thumb));
}

QVariantMap ChatBackdropController::scrimFor(const QString &statsKey,
                                             const QColor &ground,
                                             const QColor &inkPrimary,
                                             const QColor &inkSecondary,
                                             const QColor &inkMuted,
                                             double tint, double dim)
{
    if (statsKey.startsWith(QLatin1String("mxc:"))
        && !m_stats.value(statsKey).measured)
        measureMxc(statsKey.mid(4));
    else if (statsKey.startsWith(QLatin1String("file:")))
        ensureMeasuredFile(statsKey.mid(5));

    const backdrop::ImageStats stats = m_stats.value(statsKey);
    const double t = std::isfinite(tint) ? std::clamp(tint, 0.0, 1.0) : 0.0;
    const QString cacheKey =
        QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
            .arg(statsKey, ground.name(QColor::HexRgb),
                 inkPrimary.name(QColor::HexRgb), inkSecondary.name(QColor::HexRgb),
                 inkMuted.name(QColor::HexRgb))
            .arg(int(std::lround(t * 100)))
            .arg(stats.measured ? 1 : 0);
    QVariantMap out = m_scrimCache.value(cacheKey);
    if (out.isEmpty()) {
        const backdrop::ScrimPlan plan = backdrop::planScrim(
            ground, { inkPrimary, inkSecondary, inkMuted }, stats,
            t * backdrop::kMaxTint);
        out.insert(QStringLiteral("color"), plan.color);
        out.insert(QStringLiteral("floor"), plan.floor);
        out.insert(QStringLiteral("tint"), plan.tint);
        out.insert(QStringLiteral("feasible"), plan.feasible);
        out.insert(QStringLiteral("measured"), stats.measured);
        if (m_scrimCache.size() >= 64)
            m_scrimCache.clear();
        m_scrimCache.insert(cacheKey, out);
    }
    out.insert(QStringLiteral("alpha"),
               backdrop::scrimAlpha(out.value(QStringLiteral("floor")).toDouble(),
                                    dim));
    return out;
}

QStringList ChatBackdropController::depthStops(const QColor &base,
                                               bool darkTheme) const
{
    return backdrop::depthStops(base, darkTheme);
}

// ---- shared editors ----------------------------------------------------------

QVariantMap ChatBackdropController::sharedFor(const QString &scopeId) const
{
    return m_shared.value(scopeId).content;
}

bool ChatBackdropController::canSetShared(const QString &scopeId) const
{
    return m_shared.value(scopeId).canSet;
}

bool ChatBackdropController::sharedUnsupported(const QString &scopeId) const
{
    return m_shared.value(scopeId).unsupported;
}

bool ChatBackdropController::roomEncrypted(const QString &roomId) const
{
    if (!m_client || roomId.isEmpty())
        return false;
    const QList<RoomInfo> rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (room.id == roomId)
            return room.encrypted;
    }
    return false;
}

bool ChatBackdropController::roomEncryptionKnown(const QString &roomId) const
{
    if (!m_client || roomId.isEmpty())
        return false;
    const QList<RoomInfo> rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (room.id == roomId)
            return room.encryptionKnown;
    }
    return false;
}

void ChatBackdropController::setShared(const QString &scopeId,
                                       const QVariantMap &presentation)
{
    if (!sharedAvailable() || scopeId.isEmpty() || m_pendingWrite != 0)
        return;
    QVariantMap content;
    content.insert(QStringLiteral("version"), 1);
    // Integer percentages: event content is canonical JSON, which has no
    // floats, and Synapse refuses one with 400 M_BAD_JSON.
    content.insert(QStringLiteral("presentation"),
                   backdrop::presentationToWire(presentation));
    QString localPath;
    const quint64 opId = m_nextOpId;   // the id this write will carry
    if (m_prepared) {
        if (!m_uploadDir)
            m_uploadDir = std::make_unique<QTemporaryDir>();
        if (!m_uploadDir->isValid()) {
            qCWarning(lcBackdrop) << "shared write op=" << opId << "scope="
                                  << scopeTag(scopeId)
                                  << "stage=write_tmp result=fail category=write_failed"
                                  << "detail=no_temp_dir";
            setLastError(QStringLiteral("write_failed"));
            return;
        }
        localPath = m_uploadDir->filePath(m_prepared->hash + QLatin1Char('.')
                                          + m_prepared->suffix);
        QSaveFile out(localPath);
        if (!out.open(QIODevice::WriteOnly)) {
            qCWarning(lcBackdrop) << "shared write op=" << opId << "scope="
                                  << scopeTag(scopeId)
                                  << "stage=write_tmp result=fail category=write_failed"
                                  << "detail=open";
            setLastError(QStringLiteral("write_failed"));
            return;
        }
        out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (out.write(m_prepared->bytes) != m_prepared->bytes.size()
            || !out.commit()) {
            qCWarning(lcBackdrop) << "shared write op=" << opId << "scope="
                                  << scopeTag(scopeId)
                                  << "stage=write_tmp result=fail category=write_failed"
                                  << "detail=write";
            setLastError(QStringLiteral("write_failed"));
            return;
        }
        QVariantMap info;
        info.insert(QStringLiteral("mimetype"), m_prepared->mime);
        info.insert(QStringLiteral("size"), qint64(m_prepared->bytes.size()));
        info.insert(QStringLiteral("w"), m_prepared->width);
        info.insert(QStringLiteral("h"), m_prepared->height);
        content.insert(QStringLiteral("info"), info);
        if (m_prepared->stats.dominant.isValid())
            content.insert(QStringLiteral("color"),
                           m_prepared->stats.dominant.name(QColor::HexRgb).toUpper());
    } else {
        // Presentation only: the picture stays, nothing is uploaded again.
        const QVariantMap current = m_shared.value(scopeId).content;
        const QString url = current.value(QStringLiteral("url")).toString();
        if (!url.startsWith(QLatin1String("mxc://"))) {
            qCWarning(lcBackdrop) << "shared write op=" << opId << "scope="
                                  << scopeTag(scopeId)
                                  << "stage=prepare result=fail category=no_picture";
            setLastError(QStringLiteral("no_picture"));
            return;
        }
        content.insert(QStringLiteral("url"), url);
        if (current.contains(QStringLiteral("info")))
            content.insert(QStringLiteral("info"), current.value(QStringLiteral("info")));
        if (current.contains(QStringLiteral("color")))
            content.insert(QStringLiteral("color"), current.value(QStringLiteral("color")));
    }
    setLastError(QString());
    m_pendingWrite = m_nextOpId++;
    m_pendingWriteScope = scopeId;
    qCInfo(lcBackdrop) << "shared write op=" << m_pendingWrite << "scope="
                       << scopeTag(scopeId) << "stage=start kind="
                       << (localPath.isEmpty() ? "presentation" : "upload")
                       << "bytes=" << (m_prepared ? m_prepared->bytes.size() : 0);
    Q_EMIT busyChanged();
    m_client->setRoomBackground(scopeId, localPath, toJson(content), m_pendingWrite);
}

void ChatBackdropController::clearShared(const QString &scopeId)
{
    if (!sharedAvailable() || scopeId.isEmpty() || m_pendingWrite != 0)
        return;
    setLastError(QString());
    m_pendingWrite = m_nextOpId++;
    m_pendingWriteScope = scopeId;
    qCInfo(lcBackdrop) << "shared write op=" << m_pendingWrite << "scope="
                       << scopeTag(scopeId) << "stage=start kind=clear";
    Q_EMIT busyChanged();
    // No path and no url: Rust sends `{}`.
    m_client->setRoomBackground(scopeId, QString(), QStringLiteral("{}"),
                                m_pendingWrite);
}

// ---- personal ------------------------------------------------------------------

QString ChatBackdropController::storageDir() const
{
    if (!m_storageOverride.isEmpty())
        return m_storageOverride;
    if (!m_settings)
        return {};
    const QString userId = m_settings->userId();
    if (userId.isEmpty())
        return {};
    const QString root = matrix::app_data::accountRoot(userId);
    return root.isEmpty() ? QString() : root + QStringLiteral("/backgrounds");
}

QVariantMap ChatBackdropController::personalStore() const
{
    if (m_personalLoaded)
        return m_personalCache;
    m_personalLoaded = true;
    m_personalCache.clear();
    if (!m_settings)
        return m_personalCache;
    const QString json =
        m_settings->accountScopedValue(kPersonalKey, QString()).toString();
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return m_personalCache;
    const QVariantMap raw = doc.object().toVariantMap();
    // A hand-editable file: every record is cleaned on the way in.
    const QVariantMap def = cleanPersonalRecord(raw.value(QStringLiteral("default")));
    if (!def.isEmpty())
        m_personalCache.insert(QStringLiteral("default"), def);
    QVariantMap rooms;
    const QVariantMap rawRooms = raw.value(QStringLiteral("rooms")).toMap();
    for (auto it = rawRooms.constBegin(); it != rawRooms.constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1Char('!')) || rooms.size() >= kMaxPersonalRooms)
            continue;
        const QVariantMap record = cleanPersonalRecord(it.value());
        if (!record.isEmpty())
            rooms.insert(it.key(), record);
    }
    if (!rooms.isEmpty())
        m_personalCache.insert(QStringLiteral("rooms"), rooms);
    QStringList hidden;
    for (const QVariant &id : raw.value(QStringLiteral("hidden")).toList()) {
        const QString room = id.toString();
        if (room.startsWith(QLatin1Char('!')) && !hidden.contains(room)
            && hidden.size() < kMaxHiddenRooms)
            hidden.append(room);
    }
    if (!hidden.isEmpty())
        m_personalCache.insert(QStringLiteral("hidden"), hidden);
    // Server writes still owed (a removal or a presentation change that has
    // not reached the homeserver yet): scope ("" or a room id) -> "clear" |
    // "presentation". Replayed on the next start (startSync).
    QVariantMap pending;
    const QVariantMap rawPending = raw.value(QStringLiteral("pending")).toMap();
    for (auto it = rawPending.constBegin(); it != rawPending.constEnd(); ++it) {
        const QString what = it.value().toString();
        if ((it.key().isEmpty() || it.key().startsWith(QLatin1Char('!')))
            && (what == QLatin1String("clear") || what == QLatin1String("presentation"))
            && pending.size() < kMaxPersonalRooms + 1)
            pending.insert(it.key(), what);
    }
    if (!pending.isEmpty())
        m_personalCache.insert(QStringLiteral("pending"), pending);
    return m_personalCache;
}

void ChatBackdropController::savePersonalStore(const QVariantMap &store)
{
    m_personalCache = store;
    m_personalLoaded = true;
    if (m_settings)
        m_settings->setAccountScopedValue(kPersonalKey, toJson(store));
    dropOrphanFiles(store);
    bump();
    // Conflicts and unsaved counts are read from the store.
    Q_EMIT syncChanged();
}

void ChatBackdropController::dropOrphanFiles(const QVariantMap &store)
{
    const QString dir = storageDir();
    if (dir.isEmpty())
        return;
    QSet<QString> referenced;
    referenced.insert(store.value(QStringLiteral("default")).toMap()
                          .value(QStringLiteral("file")).toString());
    const QVariantMap rooms = store.value(QStringLiteral("rooms")).toMap();
    for (auto it = rooms.constBegin(); it != rooms.constEnd(); ++it)
        referenced.insert(it.value().toMap().value(QStringLiteral("file")).toString());
    const QStringList files = QDir(dir).entryList(QDir::Files);
    for (const QString &name : files) {
        // Only files this class names; anything else in the folder is left.
        if (!storedNameRe().match(name).hasMatch() || referenced.contains(name))
            continue;
        QFile::remove(QDir(dir).filePath(name));
        const QString token = m_stagedTokens.take(name);
        m_stagedOrder.removeAll(name);
        if (!token.isEmpty() && m_staged)
            m_staged->remove(token);
        m_stats.remove(QStringLiteral("file:") + name);
    }
}

QString ChatBackdropController::stagedUrlForFile(const QString &fileName) const
{
    const auto known = m_stagedTokens.constFind(fileName);
    if (known != m_stagedTokens.constEnd())
        return QLatin1String(kStagedPrefix) + known.value();
    if (!m_staged)
        return {};
    const QString dir = storageDir();
    if (dir.isEmpty())
        return {};
    QFile file(QDir(dir).filePath(fileName));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
        || file.size() > kMaxStoredFileBytes)
        return {};
    const QByteArray bytes = file.readAll();
    // Our own file, but a disk is not a trust boundary: raster magic only.
    if (!lightning::imagefmt::sniffRaster(bytes))
        return {};
    const QString token = m_staged->add(bytes);
    if (token.isEmpty())
        return {};
    m_stagedTokens.insert(fileName, token);
    m_stagedOrder.append(fileName);
    while (m_stagedOrder.size() > kMaxStaged) {
        const QString oldest = m_stagedOrder.takeFirst();
        const QString oldToken = m_stagedTokens.take(oldest);
        if (!oldToken.isEmpty())
            m_staged->remove(oldToken);
    }
    return QLatin1String(kStagedPrefix) + token;
}

QVariantMap ChatBackdropController::personalFor(const QString &roomId) const
{
    const QVariantMap store = personalStore();
    const QVariantMap record = roomId.isEmpty()
        ? store.value(QStringLiteral("default")).toMap()
        : store.value(QStringLiteral("rooms")).toMap().value(roomId).toMap();
    if (record.isEmpty())
        return {};
    return describeRecord(record, false);
}

bool ChatBackdropController::setPersonal(const QString &roomId,
                                         const QVariantMap &presentation)
{
    QVariantMap store = personalStore();
    QVariantMap rooms = store.value(QStringLiteral("rooms")).toMap();
    QVariantMap record = roomId.isEmpty()
        ? store.value(QStringLiteral("default")).toMap()
        : rooms.value(roomId).toMap();

    const bool newPicture = m_prepared != nullptr;
    if (m_prepared) {
        const QString name = storeEncoded(*m_prepared);
        if (name.isEmpty())
            return false;
        // A new picture: no server copy mirrors it yet (`remote` dropped).
        record.clear();
        record.insert(QStringLiteral("file"), name);
        record.insert(QStringLiteral("w"), m_prepared->width);
        record.insert(QStringLiteral("h"), m_prepared->height);
        if (m_prepared->stats.dominant.isValid())
            record.insert(QStringLiteral("color"),
                          m_prepared->stats.dominant.name(QColor::HexRgb).toUpper());
        const QString statsKey = QStringLiteral("file:") + name;
        if (!m_stats.contains(statsKey))
            m_statsOrder.append(statsKey);
        m_stats.insert(statsKey, m_prepared->stats);
        m_scrimCache.clear();
    } else if (record.isEmpty()) {
        setLastError(QStringLiteral("no_picture"));
        return false;
    }
    record.insert(QStringLiteral("presentation"),
                  backdrop::normalisePresentation(presentation));
    // A presentation edit keeps the picture's mark (live or dormant): it is
    // still the same picture, and the change is OWED to the server rather
    // than turning the picture local-only, which would come back as a false
    // conflict once sync is on again (review R4). Only a NEW picture is
    // local-only (record.clear() above).
    const bool mirrored = record.contains(QStringLiteral("remote"));
    const bool owedWhileOff = !syncActive() && !newPicture
                              && (mirrored || record.contains(QStringLiteral("was")));
    // A new picture settles a conflict in favour of this device; a
    // presentation change to a conflicted picture stays local until the user
    // picks one (resolveConflict), so it cannot overwrite the other device's.
    const bool conflicted = !newPicture && record.contains(QStringLiteral("conflict"));

    if (roomId.isEmpty()) {
        store.insert(QStringLiteral("default"), record);
    } else {
        if (!rooms.contains(roomId) && rooms.size() >= kMaxPersonalRooms) {
            setLastError(QStringLiteral("too_many"));
            return false;
        }
        rooms.insert(roomId, record);
        store.insert(QStringLiteral("rooms"), rooms);
    }
    if (m_prepared)
        discardPrepared();
    setLastError(QString());
    savePersonalStore(store);
    if (newPicture) {
        // An earlier upload names an earlier picture, never this one (R1),
        // and nothing owed for the old picture applies to the new one.
        m_lastUploaded.remove(roomId);
        m_lastUploadedFile.remove(roomId);
        if (pendingFor(roomId) == QLatin1String("presentation"))
            setPending(roomId, SyncMode::Upload);
    }
    if (owedWhileOff) {
        // Sent (with the picture's id) when sync is on again: startSync
        // replays it.
        setPending(roomId, SyncMode::Presentation);
        return true;
    }
    // Shown locally at once; the server copy follows. A presentation change
    // while this picture's upload is still in flight goes after it, as a
    // presentation write, rather than uploading the same picture twice.
    if (syncActive() && !conflicted) {
        const bool uploading = m_syncWriteOp != 0 && m_syncWriting.scope == roomId
                               && m_syncWriting.mode == SyncMode::Upload;
        enqueueSync(roomId, !newPicture && (mirrored || uploading)
                                ? SyncMode::Presentation
                                : SyncMode::Upload);
    }
    return true;
}

QString ChatBackdropController::storeEncoded(const Prepared &picture)
{
    const QString dir = storageDir();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        setLastError(QStringLiteral("no_account"));
        return {};
    }
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner
                                   | QFile::ExeOwner);
    const QString name = picture.hash + QLatin1Char('.') + picture.suffix;
    const QString path = QDir(dir).filePath(name);
    if (!QFileInfo::exists(path)) {
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            setLastError(QStringLiteral("write_failed"));
            return {};
        }
        out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (out.write(picture.bytes) != picture.bytes.size() || !out.commit()) {
            setLastError(QStringLiteral("write_failed"));
            return {};
        }
    }
    return name;
}

void ChatBackdropController::clearPersonal(const QString &roomId)
{
    QVariantMap store = personalStore();
    const QVariantMap record = recordFor(store, roomId);
    if (record.isEmpty())
        return;
    // The server copy goes too when this device knows of one, so another
    // device (and the next sign-in) does not bring it back. An upload still
    // in flight counts: it would land after the removal. A CONFLICTED local
    // picture is not the server's: removing it leaves the other device's
    // copy alone, and that copy is what this device shows next.
    const bool conflicted = record.contains(QStringLiteral("conflict"));
    const bool serverCopy = !conflicted
        && (record.contains(QStringLiteral("remote"))
            || m_remote.value(roomId).state == QLatin1String("present")
            || syncBusyFor(roomId));
    putRecord(store, roomId, QVariantMap());
    savePersonalStore(store);
    // After the local save: enqueueSync persists its own tombstone. While
    // sync is off, a picture that had a server copy (a dormant mark) owes
    // its removal too, or turning sync on again would bring it back.
    if (syncActive() && serverCopy)
        enqueueSync(roomId, SyncMode::Clear);
    else if (syncActive() && conflicted)
        syncRead(roomId);
    else if (!syncActive() && !conflicted
             && (record.contains(QStringLiteral("remote"))
                 || record.contains(QStringLiteral("was"))))
        setPending(roomId, SyncMode::Clear);
}

bool ChatBackdropController::roomHidden(const QString &roomId) const
{
    return personalStore().value(QStringLiteral("hidden")).toStringList()
        .contains(roomId);
}

void ChatBackdropController::setRoomHidden(const QString &roomId, bool hidden)
{
    if (!roomId.startsWith(QLatin1Char('!')))
        return;
    QVariantMap store = personalStore();
    QStringList list = store.value(QStringLiteral("hidden")).toStringList();
    if (hidden == list.contains(roomId))
        return;
    if (hidden) {
        if (list.size() >= kMaxHiddenRooms)
            list.removeFirst();
        list.append(roomId);
    } else {
        list.removeAll(roomId);
    }
    if (list.isEmpty())
        store.remove(QStringLiteral("hidden"));
    else
        store.insert(QStringLiteral("hidden"), list);
    savePersonalStore(store);
}

// ---- picking -------------------------------------------------------------------

QVariantMap ChatBackdropController::prepareImage(const QUrl &fileUrl)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    // A local file only; any other scheme is refused rather than read as a
    // path (QUrl("C:/x.png").scheme() is "c", hence isLocalFile()).
    QString path;
    if (fileUrl.isLocalFile())
        path = fileUrl.toLocalFile();
    else if (fileUrl.scheme().isEmpty() || fileUrl.scheme().size() == 1)
        path = fileUrl.toString();
    if (path.isEmpty()) {
        setLastError(QStringLiteral("unreadable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setLastError(QStringLiteral("unreadable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }
    if (file.size() <= 0 || file.size() > kMaxSourceBytes) {
        setLastError(QStringLiteral("too_large"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }
    const QByteArray bytes = file.readAll();
    // An SVG is not refused: it is converted locally to a PNG and only that
    // PNG enters the pipeline below (which still refuses SVG by magic bytes).
    namespace svg = lightning::svgraster;
    if (!lightning::imagefmt::sniffRaster(bytes)) {
        const bool svgz = svg::isGzip(bytes)
            && path.endsWith(QLatin1String(".svgz"), Qt::CaseInsensitive);
        if (svg::looksLikeSvg(bytes) || svgz) {
            const quint64 generation = ++m_prepareGeneration;
            svg::rasterizeAsync(
                bytes, svg::backgroundPolicy(), this,
                [this, generation](const svg::Result &converted) {
                    if (generation != m_prepareGeneration)
                        return;
                    QVariantMap outcome;
                    if (!converted.refusal.isEmpty()) {
                        setLastError(QStringLiteral("svg_") + converted.refusal);
                        outcome.insert(QStringLiteral("ok"), false);
                        outcome.insert(QStringLiteral("error"), m_lastError);
                    } else {
                        outcome = prepareFromBytes(converted.png);
                    }
                    Q_EMIT imagePrepared(outcome);
                });
            result.insert(QStringLiteral("pending"), true);
            result.insert(QStringLiteral("error"), QString());
            return result;
        }
    }
    return prepareFromBytes(bytes);
}

QString ChatBackdropController::svgMessage(const QString &reason) const
{
    return lightning::svgraster::userMessage(reason);
}

QVariantMap ChatBackdropController::prepareImageForTesting(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, image.hasAlphaChannel() ? "PNG" : "JPEG");
    buffer.close();
    return prepareFromBytes(bytes);
}

bool ChatBackdropController::encodePicture(const QByteArray &bytes,
                                           Prepared &picture,
                                           QString &error) const
{
    const auto fail = [&error](const QString &category) {
        error = category;
        return false;
    };

    // Magic bytes decide, never a name: SVG, HTML and video are refused here.
    const lightning::imagefmt::RasterFormat *format =
        lightning::imagefmt::sniffRaster(bytes);
    if (!format)
        return fail(QStringLiteral("unsupported_image"));

    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    if (!buffer.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("unreadable"));
    QImageReader reader(&buffer, QByteArray(format->qtFormat));
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(true);    // honour EXIF orientation
    reader.setAllocationLimit(256);
    const QSize natural = reader.size();
    if (!natural.isValid() || natural.isEmpty())
        return fail(QStringLiteral("undecodable"));
    const int edge = qMin(int(kMaxEdge), int(kMaxDecodeEdge));
    if (natural.width() > edge || natural.height() > edge)
        reader.setScaledSize(natural.scaled(edge, edge, Qt::KeepAspectRatio));
    // read() decodes the FIRST frame only: an animation becomes a still.
    QImage image = reader.read();
    if (image.isNull() || image.width() < 1 || image.height() < 1)
        return fail(QStringLiteral("undecodable"));
    if (image.width() > kMaxEdge || image.height() > kMaxEdge)
        image = image.scaled(kMaxEdge, kMaxEdge, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);

    // Re-encoded by us, so nothing of the original file (EXIF, GPS, XMP,
    // comments, extra frames) is kept or uploaded. QImage carries a PNG's text
    // chunks and a JPEG's comments in text(), and QImageWriter writes them
    // back out, so the pixels are copied into a fresh image that has none.
    // Colours are converted to sRGB first, since the copy drops the profile.
    const bool alpha = image.hasAlphaChannel();
    if (image.colorSpace().isValid()
        && image.colorSpace() != QColorSpace(QColorSpace::SRgb))
        image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    image = image.convertToFormat(alpha ? QImage::Format_ARGB32
                                        : QImage::Format_RGB32);
    image = QImage(image.constBits(), image.width(), image.height(),
                   image.bytesPerLine(), image.format())
                .copy();
    if (image.isNull())
        return fail(QStringLiteral("undecodable"));
    QByteArray encoded;
    {
        QBuffer out(&encoded);
        if (!out.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("encode_failed"));
        QImageWriter writer(&out, alpha ? QByteArrayLiteral("png")
                                        : QByteArrayLiteral("jpeg"));
        if (!alpha)
            writer.setQuality(88);
        const QImage toWrite =
            alpha ? image : image.convertToFormat(QImage::Format_RGB32);
        if (!writer.write(toWrite))
            return fail(QStringLiteral("encode_failed"));
    }
    if (encoded.isEmpty())
        return fail(QStringLiteral("encode_failed"));

    picture.bytes = encoded;
    picture.mime = alpha ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    picture.suffix = alpha ? QStringLiteral("png") : QStringLiteral("jpg");
    picture.width = image.width();
    picture.height = image.height();
    picture.hash = QString::fromLatin1(
        QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
    picture.stats = backdrop::measure(image);
    return true;
}

QVariantMap ChatBackdropController::prepareFromBytes(const QByteArray &bytes)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    auto next = std::make_unique<Prepared>();
    QString error;
    if (!encodePicture(bytes, *next, error)) {
        qCWarning(lcBackdrop) << "prepare result=fail category=" << error;
        setLastError(error);
        result.insert(QStringLiteral("error"), error);
        return result;
    }
    const QByteArray &encoded = next->bytes;
    if (m_staged) {
        const QString token = m_staged->add(encoded);
        if (!token.isEmpty())
            next->imageUrl = QLatin1String(kStagedPrefix) + token;
    }
    discardPrepared();
    const QString statsKey = QStringLiteral("prepared:") + next->hash;
    if (!m_stats.contains(statsKey))
        m_statsOrder.append(statsKey);
    m_stats.insert(statsKey, next->stats);
    while (m_statsOrder.size() > kMaxStats)
        m_stats.remove(m_statsOrder.takeFirst());
    m_prepared = std::move(next);
    qCInfo(lcBackdrop) << "prepare result=ok w=" << m_prepared->width
                       << "h=" << m_prepared->height << "mime=" << m_prepared->mime
                       << "bytes=" << m_prepared->bytes.size()
                       << "measured=" << m_prepared->stats.measured;
    setLastError(QString());
    Q_EMIT preparedChanged();

    result = prepared();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), QString());
    return result;
}

void ChatBackdropController::discardPrepared()
{
    ++m_prepareGeneration; // an SVG still converting is no longer wanted
    if (!m_prepared)
        return;
    if (m_staged && m_prepared->imageUrl.startsWith(QLatin1String(kStagedPrefix)))
        m_staged->remove(m_prepared->imageUrl.mid(int(qstrlen(kStagedPrefix))));
    m_prepared.reset();
    Q_EMIT preparedChanged();
}

void ChatBackdropController::useMessageImage(const QString &roomId,
                                             const QString &mediaKey)
{
    if (!m_bridge || roomId.isEmpty() || mediaKey.isEmpty())
        return;
    if (!roomEncryptionKnown(roomId) || roomEncrypted(roomId)) {
        setLastError(QStringLiteral("encrypted_room"));
        return;
    }
    setLastError(QString());
    m_pendingMediaKey = mediaKey;
    m_pendingMediaRoom = roomId;
    m_bridge->fetchFullForStar(mediaKey);
}

void ChatBackdropController::handleMediaBytes(const QString &mediaKey, bool ok,
                                              const QByteArray &bytes,
                                              const QString &category)
{
    if (m_pendingMediaKey.isEmpty() || mediaKey != m_pendingMediaKey)
        return;
    const QString roomId = m_pendingMediaRoom;
    m_pendingMediaKey.clear();
    m_pendingMediaRoom.clear();
    if (!ok || bytes.isEmpty()) {
        setLastError(category.isEmpty() ? QStringLiteral("unreadable") : category);
        return;
    }
    // Re-checked: the room may have turned encryption on meanwhile.
    if (!roomEncryptionKnown(roomId) || roomEncrypted(roomId)) {
        setLastError(QStringLiteral("encrypted_room"));
        return;
    }
    const QVariantMap result = prepareFromBytes(bytes);
    if (!result.value(QStringLiteral("ok")).toBool())
        return;
    setPersonal(roomId, QVariantMap());
}

// ---- personal backgrounds on the homeserver ---------------------------------------
//
// The account-wide switch lives in the global account data (`enabled`), so
// an opt-out on one device holds on every device: this one keeps a local
// MIRROR (kSyncKey) for the time before the server answers, and never writes
// anything (no upload, no change, no removal) until the server's switch is
// known to be on this session (m_serverSwitch). A switch change this device
// made and could not yet send is kept (kSwitchPendingKey) and replayed, and
// the server's value does not override it meanwhile.

bool ChatBackdropController::syncAvailable() const
{
    return m_client && m_client->supportsPersonalBackgroundSync();
}

bool ChatBackdropController::syncEnabled() const
{
    if (!m_settings)
        return true;
    return m_settings->accountScopedValue(kSyncKey, true).toBool();
}

bool ChatBackdropController::syncWatching() const
{
    return syncAvailable() && m_client->isLoggedIn();
}

bool ChatBackdropController::syncActive() const
{
    return syncWatching() && syncEnabled();
}

bool ChatBackdropController::syncWritable() const
{
    return syncActive() && m_serverSwitch == 1 && m_syncClearOp == 0
           && !m_syncClearRequested;
}

QString ChatBackdropController::switchPending() const
{
    if (!m_settings)
        return {};
    const QString value =
        m_settings->accountScopedValue(kSwitchPendingKey, QString()).toString();
    return value == QLatin1String("on") || value == QLatin1String("off") ? value
                                                                        : QString();
}

void ChatBackdropController::setSwitchPending(const QString &value)
{
    if (m_settings && switchPending() != value)
        m_settings->setAccountScopedValue(kSwitchPendingKey, value);
}

QVariantMap ChatBackdropController::syncStatus() const
{
    const int pending = int(m_syncQueue.size()) + (m_syncWriteOp != 0 ? 1 : 0)
                        + int(m_syncDownloads.size()) + int(m_downloadQueue.size());
    const int failed = int(m_syncFailedOps.size())
                       + int(m_failedDownloadIds.size());
    QString state;
    if (!syncAvailable())
        state = QStringLiteral("unavailable");
    else if (m_syncClearOp != 0 || m_syncClearRequested)
        state = QStringLiteral("removing");
    else if (!syncEnabled())
        state = QStringLiteral("off");
    else if (pending > 0 || m_enableOp != 0)
        state = QStringLiteral("working");
    else if (failed > 0 || !m_syncError.isEmpty())
        state = QStringLiteral("failed");
    else
        state = QStringLiteral("idle");
    QVariantMap out;
    out.insert(QStringLiteral("state"), state);
    out.insert(QStringLiteral("pending"), pending);
    out.insert(QStringLiteral("uploading"),
               int(m_syncQueue.size()) + (m_syncWriteOp != 0 ? 1 : 0));
    out.insert(QStringLiteral("downloading"),
               int(m_syncDownloads.size()) + int(m_downloadQueue.size()));
    out.insert(QStringLiteral("failed"), failed);
    out.insert(QStringLiteral("error"), m_syncError);
    out.insert(QStringLiteral("removal"), m_syncRemoval);
    out.insert(QStringLiteral("removed"), m_syncRemoved);
    out.insert(QStringLiteral("removeFailed"), m_syncRemoveFailed);
    out.insert(QStringLiteral("switchError"), m_switchError);
    return out;
}

int ChatBackdropController::syncUnsaved() const
{
    // What a sign-out now would lose: pictures not (yet) on the homeserver
    // and server changes still owed. Only while syncing: with it off, the
    // user chose this device only and Settings says sign-out deletes them.
    if (!syncActive())
        return 0;
    const QVariantMap store = personalStore();
    int count = 0;
    QStringList scopes{ QString() };
    scopes.append(store.value(QStringLiteral("rooms")).toMap().keys());
    for (const QString &scope : std::as_const(scopes)) {
        const QVariantMap record = recordFor(store, scope);
        if (record.isEmpty() || (!scope.isEmpty() && !isJoinedRoom(scope)))
            continue;
        if (!record.contains(QStringLiteral("remote"))
            || pendingFor(scope) == QLatin1String("presentation"))
            ++count;
    }
    return count;
}

int ChatBackdropController::syncOwedRemovals() const
{
    // Removals the homeserver has not taken: its copy would come back on the
    // next sign-in. Rooms this account has left owe nothing (the write can
    // never happen), so they do not count.
    if (!syncActive())
        return 0;
    int count = 0;
    const QVariantMap pending = personalStore().value(QStringLiteral("pending")).toMap();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        if (it.value().toString() == QLatin1String("clear")
            && (it.key().isEmpty() || isJoinedRoom(it.key())))
            ++count;
    }
    return count;
}

void ChatBackdropController::retryRemoval()
{
    if (syncEnabled() || m_syncClearOp != 0
        || (m_syncRemoval != QLatin1String("partial")
            && m_syncRemoval != QLatin1String("failed")))
        return;
    qCInfo(lcBackdrop) << "sync remove-all retry";
    m_syncClearRooms = m_lastClearRooms;
    m_syncRemoval.clear();
    startClearAll();
}

QVariantList ChatBackdropController::syncConflicts() const
{
    QVariantList out;
    if (!syncActive())
        return out;
    const QVariantMap store = personalStore();
    QStringList scopes{ QString() };
    scopes.append(store.value(QStringLiteral("rooms")).toMap().keys());
    for (const QString &scope : std::as_const(scopes)) {
        if (!recordFor(store, scope).contains(QStringLiteral("conflict")))
            continue;
        QString name;
        if (m_client && !scope.isEmpty()) {
            const QList<RoomInfo> rooms = m_client->rooms();
            for (const RoomInfo &room : rooms) {
                if (room.id == scope)
                    name = room.name;
            }
        }
        out.append(QVariantMap{
            { QStringLiteral("scope"), scope },
            { QStringLiteral("name"), name },
            { QStringLiteral("looksSame"),
              recordFor(store, scope).value(QStringLiteral("conflictLooksSame")).toBool() } });
    }
    return out;
}

bool ChatBackdropController::migrationNoticeNeeded() const
{
    // Recomputed: a picture that turned into a conflict (or was uploaded,
    // removed, or its room left) since the notice was raised no longer
    // waits for it, and a notice with nothing behind it must not stay up.
    return m_migrationNotice && syncWritable() && !migrationCandidates().isEmpty();
}

void ChatBackdropController::acknowledgeMigration(bool keepOnThisDeviceOnly)
{
    m_migrationNotice = false;
    if (keepOnThisDeviceOnly) {
        qCInfo(lcBackdrop) << "sync notice answer=this-device-only";
        setSyncEnabled(false, false);
        return;
    }
    qCInfo(lcBackdrop) << "sync notice answer=ok";
    if (m_settings)
        m_settings->setAccountScopedValue(kNoticeAckKey, true);
    migrateLocalOnly();
    Q_EMIT syncChanged();
}

void ChatBackdropController::resolveConflict(const QString &scope,
                                             bool useThisDevice)
{
    QVariantMap store = personalStore();
    QVariantMap record = recordFor(store, scope);
    if (!record.contains(QStringLiteral("conflict")))
        return;
    qCInfo(lcBackdrop) << "sync conflict scope=" << syncTag(scope)
                       << "resolved=" << (useThisDevice ? "this-device" : "synced");
    if (useThisDevice) {
        record.remove(QStringLiteral("conflict"));
        putRecord(store, scope, record);
        savePersonalStore(store);
        if (syncActive())
            enqueueSync(scope, SyncMode::Upload);
    } else {
        // The synced picture replaces this one when it has downloaded; the
        // conflict mark keeps the local one from uploading meanwhile.
        m_failedDownloadIds.remove(scope);
        requestDownload(scope, record.value(QStringLiteral("conflict")).toString(),
                        /*replaceConflict=*/true);
    }
    Q_EMIT syncChanged();
}

void ChatBackdropController::setSyncEnabled(bool on, bool removeServerCopies)
{
    if (!m_settings)
        return;
    if (on) {
        if (syncEnabled() && switchPending().isEmpty() && m_serverSwitch != 0)
            return;
        m_settings->setAccountScopedValue(kSyncKey, true);
        qCInfo(lcBackdrop) << "sync setting=on";
        m_syncFailedOps.clear();
        m_failedDownloadIds.clear();
        m_syncError.clear();
        m_syncRemoval.clear();
        m_switchError.clear();
        setSwitchPending(QStringLiteral("on"));
        sendSwitch(true);
        Q_EMIT syncChanged();
        return;
    }
    if (!syncEnabled() && !removeServerCopies && switchPending().isEmpty())
        return;
    // Before anything forgets which rooms have a copy.
    const QStringList known = knownSyncedRooms();
    m_settings->setAccountScopedValue(kSyncKey, false);
    qCInfo(lcBackdrop) << "sync setting=off remove_server_copies="
                       << removeServerCopies;
    // Nothing more is sent; a write already sent finishes and its answer is
    // still recorded (handleSyncWritten). Pending removals and presentation
    // changes stay persisted and run if sync is turned on again.
    m_serverSwitch = 0;
    m_syncQueue.clear();
    m_downloadQueue.clear();
    m_syncFailedOps.clear();
    m_failedDownloadIds.clear();
    m_syncReads.clear();
    m_syncReadAgain.clear();
    m_syncReadBacklog.clear();
    m_pokedDuringFullRead.clear();
    m_syncDownloads.clear();
    m_downloadCompares.clear();
    m_syncRereadAfterWrite.clear();
    m_syncAsked.clear();
    m_syncError.clear();
    m_switchError.clear();
    m_syncStarted = false;
    m_migrationNotice = false;
    setSwitchPending(QStringLiteral("off"));
    if (removeServerCopies) {
        // Nothing local mirrors a server copy any more, nothing is owed to
        // it, and no conflict with it is left: turning sync on again uploads
        // these pictures afresh.
        QVariantMap store = personalStore();
        QStringList scopes{ QString() };
        scopes.append(store.value(QStringLiteral("rooms")).toMap().keys());
        for (const QString &scope : std::as_const(scopes)) {
            QVariantMap record = recordFor(store, scope);
            if (record.remove(QStringLiteral("remote"))
                    + record.remove(QStringLiteral("was"))
                    + record.remove(QStringLiteral("conflict")) > 0)
                putRecord(store, scope, record);
        }
        store.remove(QStringLiteral("pending"));
        savePersonalStore(store);
        m_syncRemoval.clear();
        m_syncRemoved = 0;
        m_syncRemoveFailed = 0;
        m_syncClearRooms = known;
        m_syncClearRequested = true;
        if (m_syncWriteOp == 0)
            startClearAll();
    } else {
        sendSwitch(false);
    }
    m_remote.clear();
    Q_EMIT syncChanged();
}

void ChatBackdropController::sendSwitch(bool on)
{
    if (!syncWatching() || m_enableOp != 0)
        return;   // replayed by startSync / the answer in flight
    m_enableOp = m_nextOpId++;
    m_enableTarget = on;
    m_switchWatchdog.start(m_switchTimeoutMs);
    qCInfo(lcBackdrop) << "sync switch write op=" << m_enableOp << "on=" << on;
    m_client->writePersonalBackground(
        QString(), int(SyncMode::SetEnabled), QString(),
        toJson(QVariantMap{ { QStringLiteral("enabled"), on } }), m_enableOp);
}

void ChatBackdropController::startClearAll()
{
    m_syncClearRequested = false;
    if (!syncWatching()) {
        m_syncRemoval = QStringLiteral("failed");
        Q_EMIT syncChanged();
        return;
    }
    m_syncClearOp = m_nextOpId++;
    m_lastClearRooms = m_syncClearRooms;   // for retryRemoval()
    qCInfo(lcBackdrop) << "sync remove-all op=" << m_syncClearOp
                       << "known_rooms=" << m_syncClearRooms.size();
    Q_EMIT syncChanged();
    m_client->clearAllPersonalBackgrounds(m_syncClearRooms, m_syncClearOp);
}

void ChatBackdropController::handleSyncCleared(quint64 opId, bool ok,
                                               int cleared, int failed,
                                               int skipped, bool switchedOff)
{
    if (opId == 0 || opId != m_syncClearOp)
        return;
    m_syncClearOp = 0;
    m_syncClearRooms.clear();
    m_syncRemoved = cleared;
    // Skipped scopes (a room left, a newer schema) were not removed either.
    m_syncRemoveFailed = failed + skipped;
    // The switch went off first: that much is no longer owed.
    if (switchedOff)
        setSwitchPending(QString());
    if (ok && m_syncRemoveFailed == 0) {
        m_syncRemoval = QStringLiteral("ok");
    } else if (cleared > 0) {
        m_syncRemoval = QStringLiteral("partial");
    } else {
        m_syncRemoval = QStringLiteral("failed");
    }
    qCInfo(lcBackdrop) << "sync remove-all op=" << opId << "result="
                       << m_syncRemoval << "cleared=" << cleared
                       << "failed=" << failed << "skipped=" << skipped;
    Q_EMIT syncChanged();
}

void ChatBackdropController::retrySync()
{
    if (!syncActive())
        return;
    const QList<SyncOp> ops = m_syncFailedOps;
    const QStringList downloads = m_failedDownloadIds.keys();
    m_syncFailedOps.clear();
    m_failedDownloadIds.clear();
    m_syncError.clear();
    m_switchError.clear();
    qCInfo(lcBackdrop) << "sync retry ops=" << ops.size()
                       << "downloads=" << downloads.size();
    if (!switchPending().isEmpty())
        sendSwitch(switchPending() == QLatin1String("on"));
    for (const SyncOp &op : ops)
        enqueueSync(op.scope, op.mode, op.intoEmpty);
    // A read decides whether the download is still wanted.
    for (const QString &scope : downloads)
        syncRead(scope);
    if (!m_syncStarted)
        startSync();
    else
        migrateLocalOnly();
    Q_EMIT syncChanged();
}

void ChatBackdropController::startSync()
{
    if (!syncWatching() || !m_client->initialSyncDone())
        return;
    // A switch change this device could not send yet goes first.
    const QString owed = switchPending();
    if (!owed.isEmpty() && m_enableOp == 0 && m_syncClearOp == 0)
        sendSwitch(owed == QLatin1String("on"));
    if (m_syncStarted)
        return;
    for (auto it = m_syncReads.constBegin(); it != m_syncReads.constEnd(); ++it) {
        if (it.value() == QLatin1String("*"))
            return;   // one full read at a time
    }
    // Owed removals and presentation changes from an earlier session.
    if (syncActive()) {
        const QVariantMap pending =
            personalStore().value(QStringLiteral("pending")).toMap();
        for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
            if (syncBusyFor(it.key()))
                continue;
            // A room this account has left can never take the write: the
            // debt is dropped rather than pinning the sign-out warning.
            if (!it.key().isEmpty() && !isJoinedRoom(it.key())) {
                setPending(it.key(), SyncMode::Upload);
                continue;
            }
            SyncOp op;
            op.scope = it.key();
            op.mode = it.value().toString() == QLatin1String("clear")
                          ? SyncMode::Clear
                          : SyncMode::Presentation;
            m_syncQueue.append(op);
        }
        if (!pending.isEmpty())
            qCInfo(lcBackdrop) << "sync replay owed=" << pending.size();
    }
    syncRead(QStringLiteral("*"));
}

void ChatBackdropController::syncRead(const QString &scope)
{
    if (!syncWatching())
        return;
    for (auto it = m_syncReads.constBegin(); it != m_syncReads.constEnd(); ++it) {
        if (it.value() == scope) {
            // One read per scope at a time; one more after it, if asked.
            m_syncReadAgain.insert(scope);
            return;
        }
    }
    // A bound on single-scope reads in flight (the start-up full read is
    // exempt); the rest wait their turn rather than being dropped.
    if (scope != QLatin1String("*")) {
        int single = 0;
        for (const QString &inFlight : std::as_const(m_syncReads))
            single += inFlight != QLatin1String("*") ? 1 : 0;
        if (single >= kMaxSingleReads) {
            if (!m_syncReadBacklog.contains(scope) && m_syncReadBacklog.size() < 2048)
                m_syncReadBacklog.append(scope);
            return;
        }
    }
    const quint64 opId = m_nextOpId++;
    m_syncReads.insert(opId, scope);
    qCDebug(lcBackdrop) << "sync read op=" << opId << "scope="
                        << (scope == QLatin1String("*") ? QStringLiteral("all")
                                                        : syncTag(scope));
    m_client->readPersonalBackgrounds(scope, opId);
}

void ChatBackdropController::requestDownload(const QString &scope,
                                             const QString &id,
                                             bool replaceConflict, bool compare)
{
    if (m_downloadQueue.contains(scope))
        return;
    for (auto it = m_syncDownloads.constBegin(); it != m_syncDownloads.constEnd();
         ++it) {
        if (it.value() == scope)
            return;
    }
    // A picture that already failed waits for the user's retry, rather than
    // being fetched again on every poke.
    if (m_failedDownloadIds.contains(scope) && m_failedDownloadIds.value(scope) == id)
        return;
    // The per-room bound, before anything is downloaded.
    if (!scope.isEmpty()) {
        const QVariantMap rooms = personalStore().value(QStringLiteral("rooms")).toMap();
        if (!rooms.contains(scope) && rooms.size() >= kMaxPersonalRooms) {
            m_failedDownloadIds.insert(scope, id);
            m_syncError = QStringLiteral("too_many");
            qCWarning(lcBackdrop) << "sync download scope=" << syncTag(scope)
                                  << "result=skip category=too_many";
            Q_EMIT syncChanged();
            return;
        }
    }
    m_downloadWanted.insert(scope, id);
    if (replaceConflict)
        m_downloadReplacesConflict.insert(scope);
    if (compare)
        m_downloadCompares.insert(scope);
    m_downloadQueue.append(scope);
    pumpDownloads();
}

void ChatBackdropController::pumpDownloads()
{
    while (!m_downloadQueue.isEmpty() && m_syncDownloads.size() < kMaxDownloads
           && syncActive()) {
        const QString scope = m_downloadQueue.takeFirst();
        const quint64 opId = m_nextOpId++;
        m_syncDownloads.insert(opId, scope);
        qCInfo(lcBackdrop) << "sync download op=" << opId << "scope="
                           << syncTag(scope) << "stage=start";
        m_client->downloadPersonalBackground(scope, m_downloadWanted.value(scope), opId);
    }
    Q_EMIT syncChanged();
}

bool ChatBackdropController::syncBusyFor(const QString &scope) const
{
    if (m_syncWriteOp != 0 && m_syncWriting.scope == scope)
        return true;
    for (const SyncOp &op : m_syncQueue) {
        if (op.scope == scope)
            return true;
    }
    if (m_downloadQueue.contains(scope))
        return true;
    for (auto it = m_syncDownloads.constBegin(); it != m_syncDownloads.constEnd();
         ++it) {
        if (it.value() == scope)
            return true;
    }
    return false;
}

void ChatBackdropController::setPending(const QString &scope, SyncMode mode)
{
    QVariantMap store = personalStore();
    QVariantMap pending = store.value(QStringLiteral("pending")).toMap();
    const QString value = mode == SyncMode::Clear ? QStringLiteral("clear")
                          : mode == SyncMode::Presentation ? QStringLiteral("presentation")
                                                           : QString();
    if (value.isEmpty()) {
        if (pending.remove(scope) == 0)
            return;
    } else {
        if (pending.value(scope).toString() == value)
            return;
        pending.insert(scope, value);
    }
    if (pending.isEmpty())
        store.remove(QStringLiteral("pending"));
    else
        store.insert(QStringLiteral("pending"), pending);
    savePersonalStore(store);
}

QString ChatBackdropController::pendingFor(const QString &scope) const
{
    return personalStore().value(QStringLiteral("pending")).toMap()
        .value(scope).toString();
}

void ChatBackdropController::enqueueSync(const QString &scope, SyncMode mode,
                                         bool intoEmpty)
{
    // The latest intent for a scope wins over one still waiting.
    m_syncQueue.erase(std::remove_if(m_syncQueue.begin(), m_syncQueue.end(),
                                     [&scope](const SyncOp &op) {
                                         return op.scope == scope;
                                     }),
                      m_syncQueue.end());
    m_syncFailedOps.erase(std::remove_if(m_syncFailedOps.begin(),
                                         m_syncFailedOps.end(),
                                         [&scope](const SyncOp &op) {
                                             return op.scope == scope;
                                         }),
                          m_syncFailedOps.end());
    m_failedDownloadIds.remove(scope);
    m_downloadQueue.removeAll(scope);
    // Owed writes survive a restart (startSync replays them); an upload
    // needs no mark, its record without `remote` is one.
    setPending(scope, mode);
    SyncOp op;
    op.scope = scope;
    op.mode = mode;
    op.intoEmpty = intoEmpty && mode == SyncMode::Upload;
    m_syncQueue.append(op);
    processSyncQueue();
    Q_EMIT syncChanged();
}

void ChatBackdropController::processSyncQueue()
{
    // Nothing is written before the server's switch is known to be on.
    if (m_syncWriteOp != 0 || !syncWritable())
        return;
    while (!m_syncQueue.isEmpty()) {
        SyncOp op = m_syncQueue.takeFirst();
        const QVariantMap record = recordFor(personalStore(), op.scope);
        QString path;
        QVariantMap requested;
        if (!op.scope.isEmpty() && !isJoinedRoom(op.scope)) {
            // Left meanwhile: nothing can be written there any more.
            setPending(op.scope, SyncMode::Upload);
            continue;
        }
        if (op.mode != SyncMode::Clear) {
            if (record.isEmpty()) {
                setPending(op.scope, SyncMode::Upload);   // nothing owed
                continue;   // removed meanwhile; its Clear (if any) follows
            }
            requested.insert(QStringLiteral("presentation"),
                             backdrop::presentationToWire(
                                 record.value(QStringLiteral("presentation")).toMap()));
            const QString colour = record.value(QStringLiteral("color")).toString();
            if (isHexColour(colour))
                requested.insert(QStringLiteral("color"), colour);
        }
        if (op.mode == SyncMode::Presentation) {
            // Only for the picture this device knows the server holds: the
            // mark, the mark it had before sync went off (`was`), or this
            // device's own upload of THIS file, still unconfirmed by a mark
            // (a presentation change queued behind it). Never an earlier
            // upload of another picture (review R1).
            QString expected = record.value(QStringLiteral("remote")).toString();
            if (expected.isEmpty())
                expected = record.value(QStringLiteral("was")).toString();
            if (expected.isEmpty()
                && m_lastUploadedFile.value(op.scope)
                       == record.value(QStringLiteral("file")).toString())
                expected = m_lastUploaded.value(op.scope);
            if (expected.isEmpty()) {
                op.mode = SyncMode::Upload;
                setPending(op.scope, SyncMode::Upload);
            } else {
                requested.insert(QStringLiteral("expected_id"), expected);
            }
        }
        if (op.mode == SyncMode::Upload) {
            const QString file = record.value(QStringLiteral("file")).toString();
            const QString dir = storageDir();
            path = dir.isEmpty() ? QString() : QDir(dir).filePath(file);
            if (!storedNameRe().match(file).hasMatch() || path.isEmpty()
                || !QFileInfo::exists(path)) {
                syncFailed(op.scope, op.mode, QStringLiteral("read_failed"), op.intoEmpty);
                continue;
            }
            QVariantMap info;
            info.insert(QStringLiteral("mimetype"),
                        file.endsWith(QLatin1String(".png"))
                            ? QStringLiteral("image/png")
                            : QStringLiteral("image/jpeg"));
            if (record.contains(QStringLiteral("w"))) {
                info.insert(QStringLiteral("w"), record.value(QStringLiteral("w")).toInt());
                info.insert(QStringLiteral("h"), record.value(QStringLiteral("h")).toInt());
            }
            requested.insert(QStringLiteral("info"), info);
            if (op.intoEmpty)
                requested.insert(QStringLiteral("only_if_empty"), true);
        }
        op.file = record.value(QStringLiteral("file")).toString();
        m_syncWriteOp = m_nextOpId++;
        m_syncWriting = op;
        m_writeWatchdog.start(m_writeTimeoutMs);
        qCInfo(lcBackdrop) << "sync write op=" << m_syncWriteOp << "scope="
                           << syncTag(op.scope) << "mode=" << int(op.mode)
                           << "stage=start";
        Q_EMIT syncChanged();
        // Integer percentages only: account data is kept canonical (no
        // floats), as rust/src/bgsync.rs enforces again.
        m_client->writePersonalBackground(op.scope, int(op.mode), path,
                                          toJson(requested), m_syncWriteOp);
        break;
    }
}

void ChatBackdropController::syncFailed(const QString &scope, SyncMode mode,
                                        const QString &category, bool intoEmpty)
{
    const QString shown = category.isEmpty() ? QStringLiteral("failed") : category;
    qCWarning(lcBackdrop) << "sync write scope=" << syncTag(scope)
                          << "mode=" << int(mode) << "result=fail category="
                          << shown;
    m_syncFailedOps.erase(std::remove_if(m_syncFailedOps.begin(),
                                         m_syncFailedOps.end(),
                                         [&scope](const SyncOp &op) {
                                             return op.scope == scope;
                                         }),
                          m_syncFailedOps.end());
    SyncOp op;
    op.scope = scope;
    op.mode = mode;
    op.intoEmpty = intoEmpty;
    m_syncFailedOps.append(op);
    m_syncError = shown;
}

void ChatBackdropController::handleSyncWritten(quint64 opId, const QString &scope,
                                               bool ok, const QVariantMap &entry,
                                               const QString &category)
{
    if (opId != 0 && opId == m_enableOp) {
        handleSwitchWritten(ok, entry, category);
        return;
    }
    if (opId == 0 || opId != m_syncWriteOp || scope != m_syncWriting.scope) {
        qCDebug(lcBackdrop) << "sync write answer dropped op=" << opId;
        return;
    }
    const SyncOp done = m_syncWriting;
    m_syncWriteOp = 0;
    m_syncWriting = SyncOp();
    m_writeWatchdog.stop();
    // Anything newer queued for this scope keeps its owed mark; only a newer
    // PICTURE (or a removal) keeps this upload from being marked as mirrored:
    // a presentation change queued behind it is for this very picture.
    const bool removing = m_syncClearRequested || m_syncClearOp != 0;
    bool newer = removing;
    bool newerPicture = removing;
    for (const SyncOp &op : std::as_const(m_syncQueue)) {
        if (op.scope != scope)
            continue;
        newer = true;
        newerPicture = newerPicture || op.mode != SyncMode::Presentation;
    }
    if (!ok && category == QLatin1String("changed")) {
        // Another device replaced (or removed) the picture this presentation
        // change was for, or filled the place a migration upload was for.
        // Nothing is owed any more: the read that follows decides (a new
        // picture is downloaded; against a local-only one it is a conflict).
        qCInfo(lcBackdrop) << "sync write op=" << opId << "scope=" << syncTag(scope)
                           << "result=changed-on-server";
        if (!newer)
            setPending(scope, SyncMode::Upload);
        m_syncRereadAfterWrite.insert(scope);
    } else if (!ok && category == QLatin1String("sync_disabled")) {
        // Turned off on another device after this device last read: stop.
        syncFailed(scope, done.mode, category, done.intoEmpty);
        m_syncRereadAfterWrite.insert(QString());
    } else if (!ok && category == QLatin1String("not_joined")) {
        // Left the room: the write can never happen, nothing is owed.
        setPending(scope, SyncMode::Upload);
    } else if (!ok) {
        syncFailed(scope, done.mode, category, done.intoEmpty);
        if (done.mode == SyncMode::Upload) {
            // The picture never reached the server. A presentation change
            // queued behind it is for that picture: it rides with the
            // upload's retry (the failed Upload above), and must not run as
            // a presentation write against an EARLIER picture's id
            // (review R1). Nothing of this picture is owed as a presentation.
            m_syncQueue.erase(std::remove_if(m_syncQueue.begin(), m_syncQueue.end(),
                                             [&scope](const SyncOp &op) {
                                                 return op.scope == scope
                                                        && op.mode == SyncMode::Presentation;
                                             }),
                              m_syncQueue.end());
            if (pendingFor(scope) == QLatin1String("presentation"))
                setPending(scope, SyncMode::Upload);
            m_lastUploaded.remove(scope);
            m_lastUploadedFile.remove(scope);
        }
    } else {
        const RemoteEntry remote = remoteFromMap(entry);
        m_remote.insert(scope, remote);
        // A read issued before now may have been answered before this write
        // landed: its answer for this scope is not trusted (handleSyncRead).
        m_writtenAt.insert(scope, m_nextOpId);
        if (!newer)
            setPending(scope, SyncMode::Upload);   // nothing owed any more
        qCInfo(lcBackdrop) << "sync write op=" << opId << "scope=" << syncTag(scope)
                           << "result=ok state=" << remote.state;
        if (done.mode == SyncMode::Upload && remote.state == QLatin1String("present")) {
            m_lastUploaded.insert(scope, remote.id);
            m_lastUploadedFile.insert(scope, done.file);
        }
        // The local record now mirrors this copy, unless a newer choice is
        // already waiting to replace it, or the server copies are about to be
        // removed (sync turned off while this write was in flight): a mark
        // then would make the next "cleared" delete the local picture.
        if (done.mode != SyncMode::Clear && !newerPicture
            && remote.state == QLatin1String("present")) {
            QVariantMap store = personalStore();
            QVariantMap record = recordFor(store, scope);
            // Only the picture this write was for: a record whose file has
            // changed since is not this upload.
            if (!record.isEmpty()
                && record.value(QStringLiteral("file")).toString() == done.file
                && record.value(QStringLiteral("remote")).toString() != remote.id) {
                record.insert(QStringLiteral("remote"), remote.id);
                record.remove(QStringLiteral("was"));
                record.remove(QStringLiteral("conflict"));
                // Filled an empty place: confirmed by the next read only.
                if (done.intoEmpty)
                    record.insert(QStringLiteral("unconfirmed"), true);
                else
                    record.remove(QStringLiteral("unconfirmed"));
                putRecord(store, scope, record);
                savePersonalStore(store);
            }
        }
    }
    // Sync was turned off with removal while this write was in flight.
    if (m_syncClearRequested && m_syncClearOp == 0)
        startClearAll();
    for (const QString &again : QStringList{ scope, QString() }) {
        if (m_syncRereadAfterWrite.remove(again))
            syncRead(again);
    }
    processSyncQueue();
    Q_EMIT syncChanged();
}

void ChatBackdropController::handleSwitchWritten(bool ok, const QVariantMap &entry,
                                                 const QString &category)
{
    const bool target = m_enableTarget;
    m_enableOp = 0;
    m_switchWatchdog.stop();
    if (!ok) {
        // Kept owed (switchPending) and replayed on the next start or retry;
        // the server's value does not override it meanwhile.
        m_switchError = category.isEmpty() ? QStringLiteral("failed") : category;
        qCWarning(lcBackdrop) << "sync switch write result=fail category="
                              << m_switchError;
        Q_EMIT syncChanged();
        return;
    }
    m_switchError.clear();
    m_switchWrittenAt = m_nextOpId;   // older reads' `enabled` is stale
    const QString owed = switchPending();
    const bool settled = owed == (target ? QLatin1String("on") : QLatin1String("off"));
    if (settled)
        setSwitchPending(QString());
    qCInfo(lcBackdrop) << "sync switch write result=ok on=" << target;
    if (!settled) {
        // The user changed it again while this was in flight.
        sendSwitch(owed == QLatin1String("on"));
        Q_EMIT syncChanged();
        return;
    }
    Q_UNUSED(entry);   // a switch write answers with no picture entry
    applyServerSwitch(target, /*fromOwnWrite=*/true);
}

void ChatBackdropController::applyServerSwitch(bool on, bool fromOwnWrite,
                                               bool restart)
{
    // A change this device owes the server wins over what it still says.
    if (!fromOwnWrite && (!switchPending().isEmpty() || m_enableOp != 0))
        return;
    const int before = m_serverSwitch;
    m_serverSwitch = on ? 1 : 0;
    if (syncEnabled() != on && m_settings) {
        qCInfo(lcBackdrop) << "sync switch from the homeserver on=" << on;
        m_settings->setAccountScopedValue(kSyncKey, on);
    }
    if (!on) {
        // Opted out (here or on another device): nothing more is sent, and
        // nothing on this device mirrors the server any more (review R3).
        // Another device's "Remove server copies" turns the switch off before
        // it clears anything; with the marks dormant here, a cleared server
        // copy can never delete a picture this device holds, now or after
        // sync is turned on again. `was` keeps the id only to recognise the
        // same picture again (no false conflict, a presentation edit made
        // while off still names it).
        m_syncQueue.clear();
        m_downloadQueue.clear();
        m_migrationNotice = false;
        m_remote.clear();
        makeMarksDormant();
    } else if (before != 1 && restart) {
        // Turned on (here or elsewhere): read everything again. Not from
        // inside a full read, which goes on to reconcile and migrate itself.
        m_syncStarted = false;
        startSync();
    }
    processSyncQueue();
    Q_EMIT syncChanged();
}

ChatBackdropController::RemoteEntry
ChatBackdropController::remoteFromMap(const QVariantMap &entry)
{
    RemoteEntry out;
    const QString state = entry.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("present") || state == QLatin1String("cleared")
        || state == QLatin1String("unsupported") || state == QLatin1String("invalid"))
        out.state = state;
    const QString id = entry.value(QStringLiteral("id")).toString();
    if (out.state == QLatin1String("present")) {
        if (!isRemoteId(id)) {
            out.state = QStringLiteral("invalid");   // unusable: changes nothing
            return out;
        }
        out.id = id;
    }
    // Wire form, normalised the same way the app reads it back.
    out.presentation = backdrop::presentationToWire(backdrop::presentationFromWire(
        entry.value(QStringLiteral("presentation")).toMap()));
    const QString colour = entry.value(QStringLiteral("color")).toString();
    if (isHexColour(colour))
        out.color = colour.toUpper();
    return out;
}

bool ChatBackdropController::isJoinedRoom(const QString &roomId) const
{
    if (!m_client || !roomId.startsWith(QLatin1Char('!')))
        return false;
    const QList<RoomInfo> rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (room.id == roomId)
            return room.membership == RoomInfo::Joined;
    }
    return false;
}

void ChatBackdropController::handleSyncRead(quint64 opId, const QString &scope,
                                            const QVariantList &entries,
                                            const QStringList &failed,
                                            int enabled)
{
    const auto it = m_syncReads.constFind(opId);
    if (it == m_syncReads.constEnd() || it.value() != scope)
        return;
    m_syncReads.erase(it);
    if (!syncWatching()) {
        Q_EMIT syncChanged();
        return;
    }
    // The account-wide switch first, from THIS read: it decides whether
    // anything follows. A room read that answers "cleared" because another
    // device removed the server copies carries the switch that went off
    // before them, so the marks go dormant before the entry is looked at.
    // ...unless this read was issued before this device's own switch write
    // answered: it may have asked before that write landed.
    if (enabled >= 0 && opId >= m_switchWrittenAt)
        applyServerSwitch(enabled == 1, false, scope != QLatin1String("*"));

    int present = 0;
    QStringList askAgain;
    if (syncActive()) {
        for (const QVariant &value : entries) {
            const QVariantMap map = value.toMap();
            const QString entryScope = map.value(QStringLiteral("scope")).toString();
            if (!entryScope.isEmpty() && !entryScope.startsWith(QLatin1Char('!')))
                continue;
            // A room's own read answers for that room only.
            if (scope != QLatin1String("*") && entryScope != scope)
                continue;
            const RemoteEntry remote = remoteFromMap(map);
            if (remote.state.isEmpty())
                continue;
            // A scope this device has written: an answer that may predate
            // that write (issued before it answered), or a room the full read
            // took from the store (which lags our own writes until their
            // echo), is asked of the server again rather than acted on. A
            // stale "cleared" would delete the picture just chosen; a stale
            // id would download the one it replaced.
            const auto written = m_writtenAt.constFind(entryScope);
            if (written != m_writtenAt.constEnd()
                && (opId < written.value()
                    || (scope == QLatin1String("*") && !entryScope.isEmpty()))) {
                askAgain.append(entryScope);
                continue;
            }
            if (remote.state == QLatin1String("present"))
                ++present;
            reconcile(entryScope, remote);
        }
    }
    if (!failed.isEmpty()) {
        m_syncError = QStringLiteral("read_failed");
        qCWarning(lcBackdrop) << "sync read op=" << opId << "result=fail scopes="
                              << failed.size();
    }
    qCInfo(lcBackdrop) << "sync read op=" << opId << "scope="
                       << (scope == QLatin1String("*") ? QStringLiteral("all")
                                                       : syncTag(scope))
                       << "entries=" << entries.size() << "present=" << present;
    if (scope == QLatin1String("*") && syncActive()) {
        // No global object at all: a dormant default mark has nothing to
        // recognise any more (the picture stays, local-only).
        bool sawDefault = failed.contains(QString());
        for (const QVariant &value : entries)
            sawDefault = sawDefault
                         || value.toMap().value(QStringLiteral("scope")).toString().isEmpty();
        if (!sawDefault) {
            QVariantMap store = personalStore();
            QVariantMap record = recordFor(store, QString());
            if (record.remove(QStringLiteral("was")) > 0) {
                putRecord(store, QString(), record);
                savePersonalStore(store);
            }
        }
        m_syncStarted = true;
        migrateLocalOnly();
    }
    if (m_syncReadAgain.remove(scope))
        syncRead(scope);
    if (scope == QLatin1String("*")) {
        // Notices that came in while this read was out may postdate what it
        // read from the store.
        const QSet<QString> poked = std::exchange(m_pokedDuringFullRead, {});
        for (const QString &room : poked)
            syncRead(room);
    }
    while (!m_syncReadBacklog.isEmpty()) {
        int single = 0;
        for (const QString &inFlight : std::as_const(m_syncReads))
            single += inFlight != QLatin1String("*") ? 1 : 0;
        if (single >= kMaxSingleReads)
            break;
        syncRead(m_syncReadBacklog.takeFirst());
    }
    for (const QString &again : std::as_const(askAgain)) {
        qCDebug(lcBackdrop) << "sync read op=" << opId << "scope=" << syncTag(again)
                            << "predates our own write, asked again";
        syncRead(again);
    }
    processSyncQueue();
    Q_EMIT syncChanged();
}

void ChatBackdropController::reconcile(const QString &scope,
                                       const RemoteEntry &entry)
{
    // A write or download for this scope, or one owed from an earlier
    // session, decides first; read again after it.
    if (syncBusyFor(scope) || !pendingFor(scope).isEmpty()) {
        m_syncRereadAfterWrite.insert(scope);
        m_remote.insert(scope, entry);
        return;
    }
    m_remote.insert(scope, entry);
    QVariantMap store = personalStore();
    QVariantMap record = recordFor(store, scope);
    // A dormant mark (`was`, set when sync went off) is recognised like a
    // mark, except that it never deletes: a cleared copy only drops it.
    const QString was = record.value(QStringLiteral("was")).toString();
    if (!was.isEmpty()) {
        // Present: back to a mark (the same picture is followed, another one
        // downloaded below). Cleared: dropped, the picture stays local-only.
        // Invalid or newer: kept waiting.
        record.remove(QStringLiteral("was"));
        if (entry.state == QLatin1String("present"))
            record.insert(QStringLiteral("remote"), was);
        else if (entry.state != QLatin1String("cleared"))
            record.insert(QStringLiteral("was"), was);
        putRecord(store, scope, record);
        savePersonalStore(store);
        if (entry.state == QLatin1String("cleared")) {
            qCInfo(lcBackdrop) << "sync dormant mark dropped scope=" << syncTag(scope);
            return;
        }
    }
    const QString mark = record.value(QStringLiteral("remote")).toString();
    // A mark this device set by filling an EMPTY place (a migration upload)
    // is not trusted until a read shows the server still holds it: two
    // devices can both find the place empty and both write it (review N2).
    const bool unconfirmed = record.value(QStringLiteral("unconfirmed")).toBool();
    if (entry.state == QLatin1String("present")) {
        if (!mark.isEmpty() && mark == entry.id) {
            // The same picture: follow the server's presentation.
            bool changed = false;
            if (unconfirmed) {
                record.remove(QStringLiteral("unconfirmed"));
                changed = true;
                qCInfo(lcBackdrop) << "sync mark confirmed scope=" << syncTag(scope);
            }
            const QVariantMap mine = backdrop::presentationToWire(
                record.value(QStringLiteral("presentation")).toMap());
            if (mine != entry.presentation) {
                record.insert(QStringLiteral("presentation"),
                              backdrop::presentationFromWire(entry.presentation));
                changed = true;
            }
            if (changed) {
                putRecord(store, scope, record);
                savePersonalStore(store);
            }
        } else if (record.isEmpty() || (!mark.isEmpty() && !unconfirmed)) {
            // Set on another device (or restored after a sign-out).
            requestDownload(scope, entry.id, false);
        } else if (record.value(QStringLiteral("conflict")).toString() != entry.id) {
            // A picture only this device has (or one whose own upload another
            // device's then replaced), and a DIFFERENT one on the server:
            // keep both, never download over it. The synced one is fetched
            // to compare: the same picture adopts the server's copy, another
            // one is a conflict the user resolves (syncConflicts).
            if (unconfirmed) {
                qCInfo(lcBackdrop) << "sync unconfirmed mark replaced scope="
                                   << syncTag(scope);
                record.remove(QStringLiteral("remote"));
                record.remove(QStringLiteral("unconfirmed"));
                putRecord(store, scope, record);
                savePersonalStore(store);
            }
            requestDownload(scope, entry.id, false, /*compare=*/true);
        }
    } else if (entry.state == QLatin1String("cleared")) {
        if (!mark.isEmpty()) {
            // Removed on another device.
            qCInfo(lcBackdrop) << "sync removed remotely scope=" << syncTag(scope);
            putRecord(store, scope, QVariantMap());
            savePersonalStore(store);
        } else if (record.remove(QStringLiteral("conflict")) > 0) {
            // The other device's picture is gone: this one is just local now.
            putRecord(store, scope, record);
            savePersonalStore(store);
        }
    }
    // "invalid" and "unsupported": the server holds something this build
    // cannot use; nothing local changes and nothing is uploaded over it.
}

ChatBackdropController::SameAs
ChatBackdropController::sameAsLocal(const QVariantMap &record, const QByteArray &raw,
                                    const Prepared &synced) const
{
    const QString file = record.value(QStringLiteral("file")).toString();
    const QString dir = storageDir();
    if (!storedNameRe().match(file).hasMatch() || dir.isEmpty())
        return SameAs::Different;
    // Only IDENTITY adopts the server's copy silently: the same bytes as this
    // device's file (named by its SHA-256), or the file this one was
    // re-encoded from (`source`, set on download).
    const QString rawHash = QString::fromLatin1(
        QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
    if (rawHash == file.section(QLatin1Char('.'), 0, 0)
        || rawHash == record.value(QStringLiteral("source")).toString())
        return SameAs::Identical;
    // Otherwise the pixels are only a hint for the card, never a decision:
    // at most kLookEdge px on the long side, and EVERY 16x16 tile within a
    // few levels on average (a single average hides a different region).
    // The local file is decoded like any other: format pinned from its magic
    // bytes, no autodetection.
    QFile localFile(QDir(dir).filePath(file));
    if (!localFile.open(QIODevice::ReadOnly) || localFile.size() > 16LL * 1024 * 1024)
        return SameAs::Different;
    const QImage mine = decodeSmall(localFile.readAll(), kLookEdge)
                            .convertToFormat(QImage::Format_RGB32);
    const QImage theirs = decodeSmall(synced.bytes, kLookEdge)
                              .convertToFormat(QImage::Format_RGB32);
    if (mine.isNull() || theirs.isNull() || mine.size() != theirs.size())
        return SameAs::Different;
    constexpr int tile = 16;
    for (int ty = 0; ty < mine.height(); ty += tile) {
        for (int tx = 0; tx < mine.width(); tx += tile) {
            qint64 total = 0;
            int count = 0;
            for (int y = ty; y < qMin(ty + tile, mine.height()); ++y) {
                const QRgb *pa = reinterpret_cast<const QRgb *>(mine.constScanLine(y));
                const QRgb *pb = reinterpret_cast<const QRgb *>(theirs.constScanLine(y));
                for (int x = tx; x < qMin(tx + tile, mine.width()); ++x) {
                    total += qAbs(qRed(pa[x]) - qRed(pb[x]))
                             + qAbs(qGreen(pa[x]) - qGreen(pb[x]))
                             + qAbs(qBlue(pa[x]) - qBlue(pb[x]));
                    count += 3;
                }
            }
            if (count > 0 && double(total) / count > 8.0)
                return SameAs::Different;
        }
    }
    return SameAs::LooksSame;
}

void ChatBackdropController::makeMarksDormant()
{
    QVariantMap store = personalStore();
    QStringList scopes{ QString() };
    scopes.append(store.value(QStringLiteral("rooms")).toMap().keys());
    int moved = 0;
    for (const QString &scope : std::as_const(scopes)) {
        QVariantMap record = recordFor(store, scope);
        const QString mark = record.value(QStringLiteral("remote")).toString();
        if (mark.isEmpty())
            continue;
        record.remove(QStringLiteral("remote"));
        record.insert(QStringLiteral("was"), mark);
        putRecord(store, scope, record);
        ++moved;
    }
    if (moved > 0) {
        qCInfo(lcBackdrop) << "sync marks dormant=" << moved;
        savePersonalStore(store);
    }
}

QStringList ChatBackdropController::migrationCandidates() const
{
    const QVariantMap store = personalStore();
    QStringList scopes{ QString() };
    scopes.append(store.value(QStringLiteral("rooms")).toMap().keys());
    QStringList candidates;
    for (const QString &scope : std::as_const(scopes)) {
        const QVariantMap record = recordFor(store, scope);
        if (record.isEmpty() || record.contains(QStringLiteral("remote"))
            || record.contains(QStringLiteral("was"))
            || record.contains(QStringLiteral("conflict")) || syncBusyFor(scope))
            continue;
        // Only into an empty slot: nothing stored, or explicitly cleared. A
        // different picture on the server is a conflict (reconcile), and an
        // invalid or newer one is never overwritten by a migration.
        const QString state = m_remote.value(scope).state;
        if (!state.isEmpty() && state != QLatin1String("cleared"))
            continue;
        bool failedBefore = false;
        for (const SyncOp &op : std::as_const(m_syncFailedOps))
            failedBefore = failedBefore || op.scope == scope;
        if (failedBefore)
            continue;   // retrySync() decides, not every read
        if (!scope.isEmpty() && !isJoinedRoom(scope))
            continue;   // room account data needs the room
        candidates.append(scope);
    }
    return candidates;
}

void ChatBackdropController::migrateLocalOnly()
{
    if (!syncWritable())
        return;
    const QStringList candidates = migrationCandidates();
    if (candidates.isEmpty())
        return;
    // Pictures chosen before this feature existed go to the server only
    // after the user has been told once (BackgroundSyncPrompt).
    const bool told = m_settings
                      && m_settings->accountScopedValue(kNoticeAckKey, false).toBool();
    if (!told) {
        if (!m_migrationNotice)
            qCInfo(lcBackdrop) << "sync migrate waiting-for-notice=" << candidates.size();
        m_migrationNotice = true;
        Q_EMIT syncChanged();
        return;
    }
    // Into empty places only: every device turned on again at once migrates
    // at once, and a place another device filled first is a conflict.
    for (const QString &scope : std::as_const(candidates))
        enqueueSync(scope, SyncMode::Upload, /*intoEmpty=*/true);
    qCInfo(lcBackdrop) << "sync migrate local-only=" << candidates.size();
}

void ChatBackdropController::handleSyncDownloaded(quint64 opId,
                                                  const QString &scope, bool ok,
                                                  const QVariantMap &entry,
                                                  const QByteArray &bytes,
                                                  const QString &category)
{
    const auto it = m_syncDownloads.constFind(opId);
    if (it == m_syncDownloads.constEnd() || it.value() != scope)
        return;
    m_syncDownloads.erase(it);
    const QString wanted = m_downloadWanted.take(scope);
    const bool replaceConflict = m_downloadReplacesConflict.remove(scope);
    const bool compare = m_downloadCompares.remove(scope);
    const bool reread = m_syncRereadAfterWrite.remove(scope);
    const auto finish = [this, &scope, reread] {
        if (reread)
            syncRead(scope);
        pumpDownloads();
    };
    if (!syncActive())
        return finish();
    // The user chose something here meanwhile: that choice wins.
    bool userActed = false;
    for (const SyncOp &op : std::as_const(m_syncQueue))
        userActed = userActed || op.scope == scope;
    if ((m_syncWriteOp != 0 && m_syncWriting.scope == scope) || userActed)
        return finish();
    if (!ok && category == QLatin1String("changed")) {
        // The picture changed again since the read that asked for it: not a
        // failure of this one, the next read decides.
        qCInfo(lcBackdrop) << "sync download op=" << opId << "scope=" << syncTag(scope)
                           << "result=changed-on-server";
        if (!reread)
            syncRead(scope);   // (finish() reads it when already owed)
        return finish();
    }
    const RemoteEntry remote = remoteFromMap(entry);
    Prepared picture;
    QString error = ok ? QString() : (category.isEmpty() ? QStringLiteral("failed") : category);
    // The same sniff, first-frame decode and re-encode as a picked file:
    // what another device uploaded is not trusted further than that.
    if (error.isEmpty() && remote.state != QLatin1String("present"))
        error = QStringLiteral("invalid");
    if (error.isEmpty() && !encodePicture(bytes, picture, error) && error.isEmpty())
        error = QStringLiteral("undecodable");
    QVariantMap store = personalStore();
    if (compare) {
        // Only to compare with a local-only picture: nothing is stored or
        // replaced. The same picture adopts the server's copy; anything
        // else (or a fetch that failed) is a conflict the user resolves.
        QVariantMap record = recordFor(store, scope);
        const QString id = remote.id.isEmpty() ? wanted : remote.id;
        if (record.isEmpty() || !record.value(QStringLiteral("remote")).toString().isEmpty()
            || id.isEmpty())
            return finish();
        QElapsedTimer compareTimer;
        compareTimer.start();
        const SameAs same = error.isEmpty() ? sameAsLocal(record, bytes, picture)
                                            : SameAs::Different;
        qCDebug(lcBackdrop) << "sync compare scope=" << syncTag(scope)
                            << "ms=" << compareTimer.elapsed();
        record.remove(QStringLiteral("conflictLooksSame"));
        if (same == SameAs::Identical) {
            qCInfo(lcBackdrop) << "sync conflict scope=" << syncTag(scope)
                               << "resolved= same-picture";
            record.remove(QStringLiteral("conflict"));
            record.insert(QStringLiteral("remote"), id);
            record.insert(QStringLiteral("presentation"),
                          backdrop::presentationFromWire(remote.presentation));
            m_remote.insert(scope, remote);
        } else {
            if (same == SameAs::LooksSame)
                record.insert(QStringLiteral("conflictLooksSame"), true);
            if (error.isEmpty())
                qCInfo(lcBackdrop) << "sync conflict scope=" << syncTag(scope)
                                   << "looks_same=" << (same == SameAs::LooksSame);
            else
                qCInfo(lcBackdrop) << "sync conflict scope=" << syncTag(scope)
                                   << "compare_failed=" << error;
            record.insert(QStringLiteral("conflict"), id);
        }
        putRecord(store, scope, record);
        savePersonalStore(store);
        return finish();
    }
    QString name;
    if (error.isEmpty()) {
        name = storeEncoded(picture);
        if (name.isEmpty())
            error = QStringLiteral("write_failed");
    }
    if (!error.isEmpty()) {
        // Remembered by id: this picture is not fetched again until retry.
        m_failedDownloadIds.insert(scope, remote.id.isEmpty() ? wanted : remote.id);
        m_syncError = error;
        qCWarning(lcBackdrop) << "sync download op=" << opId << "scope="
                              << syncTag(scope) << "result=fail category=" << error;
        return finish();
    }
    // A conflicted local picture is replaced only when the user chose the
    // synced one; a conflict that appeared during the download stays.
    if (recordFor(store, scope).contains(QStringLiteral("conflict")) && !replaceConflict)
        return finish();
    QVariantMap record;
    record.insert(QStringLiteral("file"), name);
    record.insert(QStringLiteral("w"), picture.width);
    record.insert(QStringLiteral("h"), picture.height);
    const QString colour = !remote.color.isEmpty()
        ? remote.color
        : (picture.stats.dominant.isValid()
               ? picture.stats.dominant.name(QColor::HexRgb).toUpper()
               : QString());
    if (!colour.isEmpty())
        record.insert(QStringLiteral("color"), colour);
    record.insert(QStringLiteral("presentation"),
                  backdrop::presentationFromWire(remote.presentation));
    record.insert(QStringLiteral("remote"), remote.id);
    // What the uploader's file was (this one is a re-encode of it): the same
    // picture coming back later is recognised, not asked about (sameAsLocal).
    record.insert(QStringLiteral("source"),
                  QString::fromLatin1(QCryptographicHash::hash(
                      bytes, QCryptographicHash::Sha256).toHex()));
    const QString statsKey = QStringLiteral("file:") + name;
    if (!m_stats.contains(statsKey))
        m_statsOrder.append(statsKey);
    m_stats.insert(statsKey, picture.stats);
    while (m_statsOrder.size() > kMaxStats)
        m_stats.remove(m_statsOrder.takeFirst());
    m_scrimCache.clear();
    putRecord(store, scope, record);
    m_remote.insert(scope, remote);
    m_failedDownloadIds.remove(scope);
    qCInfo(lcBackdrop) << "sync download op=" << opId << "scope=" << syncTag(scope)
                       << "result=ok w=" << picture.width << "h=" << picture.height;
    savePersonalStore(store);
    finish();
}

void ChatBackdropController::handleSyncChanged(const QString &scope)
{
    if (!syncWatching())
        return;
    if (!scope.isEmpty() && !scope.startsWith(QLatin1Char('!')))
        return;
    // Room changes matter only while syncing; the default also carries the
    // switch, which is followed even while this device is off.
    if (!scope.isEmpty() && !syncActive())
        return;
    // Until the start-up full read has answered, a room's notice is that
    // read's job (initial sync pokes every room at once). One that arrives
    // while it is out is read after it.
    if (!scope.isEmpty() && !m_syncStarted) {
        for (const QString &inFlight : std::as_const(m_syncReads)) {
            if (inFlight == QLatin1String("*")) {
                m_pokedDuringFullRead.insert(scope);
                break;
            }
        }
        return;
    }
    if (syncBusyFor(scope)) {
        m_syncRereadAfterWrite.insert(scope);
        return;
    }
    qCInfo(lcBackdrop) << "sync changed in sync scope=" << syncTag(scope);
    syncRead(scope);
}

QVariantMap ChatBackdropController::recordFor(const QVariantMap &store,
                                              const QString &scope) const
{
    if (scope.isEmpty())
        return store.value(QStringLiteral("default")).toMap();
    return store.value(QStringLiteral("rooms")).toMap().value(scope).toMap();
}

void ChatBackdropController::putRecord(QVariantMap &store, const QString &scope,
                                       const QVariantMap &record) const
{
    if (scope.isEmpty()) {
        if (record.isEmpty())
            store.remove(QStringLiteral("default"));
        else
            store.insert(QStringLiteral("default"), record);
        return;
    }
    QVariantMap rooms = store.value(QStringLiteral("rooms")).toMap();
    if (record.isEmpty())
        rooms.remove(scope);
    else
        rooms.insert(scope, record);
    if (rooms.isEmpty())
        store.remove(QStringLiteral("rooms"));
    else
        store.insert(QStringLiteral("rooms"), rooms);
}

QStringList ChatBackdropController::knownSyncedRooms() const
{
    QStringList out;
    const QVariantMap store = personalStore();
    const QVariantMap rooms = store.value(QStringLiteral("rooms")).toMap();
    for (auto it = rooms.constBegin(); it != rooms.constEnd(); ++it) {
        const QVariantMap record = it.value().toMap();
        if (record.contains(QStringLiteral("remote"))
            || record.contains(QStringLiteral("was")))
            out.append(it.key());
    }
    for (auto it = m_remote.constBegin(); it != m_remote.constEnd(); ++it) {
        if (!it.key().isEmpty() && it.value().state != QLatin1String("cleared")
            && !out.contains(it.key()))
            out.append(it.key());
    }
    // Removals still owed go too.
    const QVariantMap pending = store.value(QStringLiteral("pending")).toMap();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        if (!it.key().isEmpty() && !out.contains(it.key()))
            out.append(it.key());
    }
    return out;
}

void ChatBackdropController::resetSyncState()
{
    m_remote.clear();
    m_syncReads.clear();
    m_syncReadAgain.clear();
    m_syncReadBacklog.clear();
    m_pokedDuringFullRead.clear();
    m_syncDownloads.clear();
    m_downloadQueue.clear();
    m_downloadWanted.clear();
    m_downloadReplacesConflict.clear();
    m_downloadCompares.clear();
    m_failedDownloadIds.clear();
    m_syncQueue.clear();
    m_syncWriteOp = 0;
    m_syncWriting = SyncOp();
    m_syncFailedOps.clear();
    m_syncRereadAfterWrite.clear();
    m_syncAsked.clear();
    m_lastUploaded.clear();
    m_lastUploadedFile.clear();
    m_lastClearRooms.clear();
    m_writtenAt.clear();
    m_switchWrittenAt = 0;
    m_writeWatchdog.stop();
    m_switchWatchdog.stop();
    m_syncError.clear();
    m_switchError.clear();
    m_syncStarted = false;
    m_serverSwitch = -1;
    m_enableOp = 0;
    m_migrationNotice = false;
    m_syncClearOp = 0;
    m_syncClearRequested = false;
    m_syncClearRooms.clear();
    m_syncRemoval.clear();
    m_syncRemoved = 0;
    m_syncRemoveFailed = 0;
}

// ---- session ---------------------------------------------------------------------

void ChatBackdropController::onSessionChanged()
{
    // Another account's personal store and staged copies.
    m_personalLoaded = false;
    m_personalCache.clear();
    for (const QString &token : std::as_const(m_stagedTokens)) {
        if (m_staged)
            m_staged->remove(token);
    }
    m_stagedTokens.clear();
    m_stagedOrder.clear();
    // Another account's server copies and operations. sessionChanged also
    // fires for the same account (a saved session), which must not drop an
    // upload in flight.
    const QString user = m_settings ? m_settings->userId() : QString();
    if (user != m_syncUserId) {
        resetSyncState();
        m_syncUserId = user;
    }
    Q_EMIT settingsChanged();
    Q_EMIT syncChanged();
    bump();
    startSync();
}

void ChatBackdropController::clearSession()
{
    m_shared.clear();
    m_inFlight.clear();
    m_lastAsked.clear();
    m_rereadAfterAnswer.clear();
    const bool wasBusy = m_pendingWrite != 0;
    m_pendingWrite = 0;
    m_pendingWriteScope.clear();
    m_stats.clear();
    m_statsOrder.clear();
    m_scrimCache.clear();
    m_chainCache.clear();
    for (const QString &token : std::as_const(m_stagedTokens)) {
        if (m_staged)
            m_staged->remove(token);
    }
    m_stagedTokens.clear();
    m_stagedOrder.clear();
    discardPrepared();
    m_pendingMediaKey.clear();
    m_pendingMediaRoom.clear();
    m_uploadDir.reset();
    m_personalLoaded = false;
    m_personalCache.clear();
    resetSyncState();
    m_syncUserId.clear();
    setLastError(QString());
    if (wasBusy)
        Q_EMIT busyChanged();
    Q_EMIT syncChanged();
    bump();
}

void ChatBackdropController::bump()
{
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ChatBackdropController::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
