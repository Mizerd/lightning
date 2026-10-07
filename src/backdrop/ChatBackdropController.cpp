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

// One personal record as stored, cleaned: { file, color, w, h, presentation }
// or {} when unusable.
QVariantMap cleanPersonalRecord(const QVariant &raw)
{
    const QVariantMap record = raw.toMap();
    const QString file = record.value(QStringLiteral("file")).toString();
    if (!storedNameRe().match(file).hasMatch())
        return {};
    QVariantMap out;
    out.insert(QStringLiteral("file"), file);
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
    }
    Q_EMIT availableChanged();
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

    if (m_prepared) {
        const QString dir = storageDir();
        if (dir.isEmpty() || !QDir().mkpath(dir)) {
            setLastError(QStringLiteral("no_account"));
            return false;
        }
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner
                                       | QFile::ExeOwner);
        const QString name = m_prepared->hash + QLatin1Char('.') + m_prepared->suffix;
        const QString path = QDir(dir).filePath(name);
        if (!QFileInfo::exists(path)) {
            QSaveFile out(path);
            if (!out.open(QIODevice::WriteOnly)) {
                setLastError(QStringLiteral("write_failed"));
                return false;
            }
            out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            if (out.write(m_prepared->bytes) != m_prepared->bytes.size()
                || !out.commit()) {
                setLastError(QStringLiteral("write_failed"));
                return false;
            }
        }
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
    return true;
}

void ChatBackdropController::clearPersonal(const QString &roomId)
{
    QVariantMap store = personalStore();
    if (roomId.isEmpty()) {
        if (store.remove(QStringLiteral("default")) == 0)
            return;
    } else {
        QVariantMap rooms = store.value(QStringLiteral("rooms")).toMap();
        if (rooms.remove(roomId) == 0)
            return;
        if (rooms.isEmpty())
            store.remove(QStringLiteral("rooms"));
        else
            store.insert(QStringLiteral("rooms"), rooms);
    }
    savePersonalStore(store);
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

QVariantMap ChatBackdropController::prepareFromBytes(const QByteArray &bytes)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    const auto fail = [this, &result](const QString &category) {
        qCWarning(lcBackdrop) << "prepare result=fail category=" << category;
        setLastError(category);
        result.insert(QStringLiteral("error"), category);
        return result;
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

    auto next = std::make_unique<Prepared>();
    next->bytes = encoded;
    next->mime = alpha ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    next->suffix = alpha ? QStringLiteral("png") : QStringLiteral("jpg");
    next->width = image.width();
    next->height = image.height();
    next->hash = QString::fromLatin1(
        QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
    next->stats = backdrop::measure(image);
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
    Q_EMIT settingsChanged();
    bump();
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
    setLastError(QString());
    if (wasBusy)
        Q_EMIT busyChanged();
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
