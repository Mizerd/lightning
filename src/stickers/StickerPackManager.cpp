#include "stickers/StickerPackManager.h"

#include "matrix/MatrixClient.h"
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcStickers, "lightning.stickers")

namespace {

// Bound on one completion list. A completion popup that can grow without
// limit is a completion popup that covers the composer.
constexpr int kMaxCompletionRows = 64;

stickers::Pack packFromVariant(const QVariantMap &map)
{
    stickers::Pack pack;
    pack.id = map.value(QStringLiteral("id")).toString();
    pack.displayName = map.value(QStringLiteral("displayName")).toString();
    pack.avatarUrl = map.value(QStringLiteral("avatarUrl")).toString();
    pack.attribution = map.value(QStringLiteral("attribution")).toString();
    pack.source = map.value(QStringLiteral("source")).toString();
    pack.roomId = map.value(QStringLiteral("roomId")).toString();
    pack.stateKey = map.value(QStringLiteral("stateKey")).toString();
    pack.enabledGlobally =
        map.value(QStringLiteral("enabledGlobally")).toBool();
    pack.canManage = map.value(QStringLiteral("canManage")).toBool();
    const QVariantList images =
        map.value(QStringLiteral("images")).toList();
    pack.images.reserve(images.size());
    for (const QVariant &value : images) {
        const stickers::PackImage image =
            stickers::PackImage::fromVariantMap(value.toMap());
        // A row with no url or no shortcode cannot have come from the Rust
        // parser (both are required there). Dropping it costs nothing and
        // keeps a malformed test fixture or a future payload change from
        // putting an unusable tile in the grid.
        if (image.url.isEmpty() || image.shortcode.isEmpty())
            continue;
        pack.images.append(image);
    }
    return pack;
}

} // namespace

StickerPackManager::StickerPackManager(QObject *parent)
    : QObject(parent)
    , m_packs(new StickerPackModel(this))
    , m_images(new StickerImageModel(this))
{
}

StickerPackManager::~StickerPackManager() = default;

void StickerPackManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::stickerPacksReceived, this,
                &StickerPackManager::onPacksReceived);
        connect(m_client, &MatrixClient::stickerPackAddFinished, this,
                &StickerPackManager::onSaveFinished);
        connect(m_client, &MatrixClient::stickerPackRoomsSet, this,
                &StickerPackManager::onRoomsSet);
        // An account change invalidates every pack: packs are account data.
        connect(m_client, &MatrixClient::loggedOut, this,
                &StickerPackManager::onLoggedOut);
    }
    onLoggedOut();
    Q_EMIT availableChanged();
}

bool StickerPackManager::available() const
{
    return m_client && m_client->supportsStickerPacks();
}

void StickerPackManager::setActiveRoomId(const QString &roomId)
{
    if (m_activeRoomId == roomId)
        return;
    m_activeRoomId = roomId;
    // MARK stale only. Fetching here would issue a global-account-data read
    // plus a bounded /state read on every room the user walks through.
    if (m_snapshotRoomId != roomId)
        m_stale = true;
}

void StickerPackManager::setSelectedPackId(const QString &id)
{
    if (m_selectedPackId == id)
        return;
    // Refuse an id the snapshot does not have. Silently falling back to the
    // first pack would show one pack's images under another pack's tab.
    if (!id.isEmpty() && m_packs->indexOfPack(id) < 0)
        return;
    m_selectedPackId = id;
    rebuildImages();
    Q_EMIT selectedPackIdChanged();
    emitStateChanged();
}

void StickerPackManager::setUsage(const QString &usage)
{
    const QString normalized = usage == QLatin1String("emoticon")
        ? QStringLiteral("emoticon")
        : QStringLiteral("sticker");
    if (m_usage == normalized)
        return;
    m_usage = normalized;
    rebuildImages();
    Q_EMIT usageChanged();
    emitStateChanged();
}

int StickerPackManager::usablePackCount() const
{
    const bool wantSticker = m_usage != QLatin1String("emoticon");
    int usable = 0;
    for (const stickers::Pack &pack : m_packs->packs()) {
        const int n = wantSticker ? pack.stickerCount() : pack.emoticonCount();
        if (n > 0)
            ++usable;
    }
    return usable;
}

void StickerPackManager::refresh()
{
    if (!available())
        return;
    if (m_fetchOp != 0) {
        m_refreshOwed = true;
        return;
    }
    const quint64 opId = m_nextOpId++;
    m_fetchOp = opId;
    // The room is captured in the request; the answer carries it back, and
    // applySnapshot records it as the snapshot's room.
    m_client->fetchStickerPacks(m_activeRoomId, opId);
    emitStateChanged();
}

void StickerPackManager::refreshIfStale()
{
    if (!m_loaded || m_stale || m_snapshotRoomId != m_activeRoomId)
        refresh();
}

void StickerPackManager::sendToRoom(const QString &roomId,
                                    const QVariantMap &image)
{
    if (!m_client || roomId.isEmpty())
        return;
    const stickers::PackImage row = stickers::PackImage::fromVariantMap(image);
    if (row.url.isEmpty())
        return;
    m_client->sendSticker(roomId, QString(), row.url,
                          row.body.isEmpty() ? row.shortcode : row.body,
                          row.mimetype, static_cast<quint64>(qMax(0, row.width)),
                          static_cast<quint64>(qMax(0, row.height)),
                          static_cast<quint64>(qMax<qint64>(0, row.size)));
}

void StickerPackManager::sendToThread(const QString &roomId,
                                      const QString &rootId,
                                      const QVariantMap &image)
{
    if (!m_client || roomId.isEmpty() || rootId.isEmpty())
        return;
    const stickers::PackImage row = stickers::PackImage::fromVariantMap(image);
    if (row.url.isEmpty())
        return;
    // NO room-send fallback, ever: a thread sticker that cannot reach its
    // thread must fail rather than land in the main timeline (§8, the same
    // rule as thread voice messages).
    m_client->sendSticker(roomId, rootId, row.url,
                          row.body.isEmpty() ? row.shortcode : row.body,
                          row.mimetype, static_cast<quint64>(qMax(0, row.width)),
                          static_cast<quint64>(qMax(0, row.height)),
                          static_cast<quint64>(qMax<qint64>(0, row.size)));
}

void StickerPackManager::saveSticker(const QString &url, const QString &body,
                                     const QString &mimetype, int width,
                                     int height, qint64 size)
{
    if (!canSave(url))
        return;
    const quint64 opId = m_nextOpId++;
    m_saveOp = opId;
    m_saveScope = QStringLiteral("account");
    // The shortcode is derived from the sticker's BODY, sanitized to
    // MSC2545's own alphabet in Rust. Sable derives it from the EVENT ID
    // instead, which is illegal under the MSC (`$`, and a `:` in room
    // versions 1-2) and unusable in another client's `:shortcode:`
    // completion — see the round notes.
    m_client->addStickerToUserPack(body, url, body, mimetype,
                                   static_cast<quint64>(qMax(0, width)),
                                   static_cast<quint64>(qMax(0, height)),
                                   static_cast<quint64>(qMax<qint64>(0, size)),
                                   opId);
    emitStateChanged();
}

void StickerPackManager::setRoomPackEnabled(const QString &packId,
                                            bool enabled)
{
    if (!m_client || !available() || m_roomsOp != 0)
        return;
    const int row = m_packs->indexOfPack(packId);
    if (row < 0)
        return;
    const stickers::Pack &pack = m_packs->packs().at(row);
    // The account's own pack is global by definition — there is nothing in
    // `im.ponies.emote_rooms` that could describe it, and writing its id
    // there would be inventing a shape the MSC does not have.
    if (pack.source != QLatin1String("room") || pack.roomId.isEmpty())
        return;
    const quint64 opId = m_nextOpId++;
    m_roomsOp = opId;
    m_client->setStickerRoomPackEnabled(pack.roomId, pack.stateKey, enabled,
                                        opId);
    emitStateChanged();
}

void StickerPackManager::onRoomsSet(quint64 opId, bool ok,
                                    const QString &category, const QString &,
                                    const QString &, bool enabled)
{
    if (opId == 0 || opId != m_roomsOp)
        return;
    m_roomsOp = 0;
    if (ok) {
        // Nothing was applied optimistically, so the switch only moves once
        // the authoritative snapshot says it moved.
        m_stale = true;
        refresh();
    }
    emitStateChanged();
    Q_EMIT roomPackToggleFinished(ok, category, enabled);
}

void StickerPackManager::saveStickerToRoom(const QString &roomId,
                                           const QString &url,
                                           const QString &body,
                                           const QString &mimetype, int width,
                                           int height, qint64 size)
{
    if (!canSaveToRoom(roomId, url))
        return;
    const quint64 opId = m_nextOpId++;
    m_saveOp = opId;
    m_saveScope = QStringLiteral("room");
    // The empty state key is the room's DEFAULT pack, which is what MSC2545
    // means by it — not a missing key. A room may publish several packs; this
    // surface writes the default one, and choosing among several is a pack
    // editor's job, not a one-click action's.
    m_client->addStickerToRoomPack(roomId, QString(), body, url, body,
                                   mimetype,
                                   static_cast<quint64>(qMax(0, width)),
                                   static_cast<quint64>(qMax(0, height)),
                                   static_cast<quint64>(qMax<qint64>(0, size)),
                                   opId);
    emitStateChanged();
}

bool StickerPackManager::canSaveToRoom(const QString &roomId,
                                       const QString &url) const
{
    if (!canSave(url) || roomId.isEmpty())
        return false;
    // A snapshot for THIS room must actually have said this account may write
    // its pack. Absence of the claim is NOT permission, and a permission
    // learned about a different room says nothing about this one.
    return m_loaded && m_snapshotRoomId == roomId && m_snapshotRoomCanManage;
}

bool StickerPackManager::isSaved(const QString &url) const
{
    return !url.isEmpty() && m_savedUrls.contains(url);
}

bool StickerPackManager::canSave(const QString &url) const
{
    if (!available() || m_saveOp != 0)
        return false;
    // A pack holds a plain mxc. An encrypted sticker carries an
    // EncryptedFile and no url at all, so there is nothing a pack could
    // hold — the action is genuinely unavailable, not merely refused.
    if (!url.startsWith(QLatin1String("mxc://")))
        return false;
    // Deliberately NOT gated on isSaved(): see the header. A duplicate is
    // refused authoritatively in Rust and reported as such.
    return true;
}

QVariantList StickerPackManager::findEmoticons(const QString &prefix,
                                               int limit) const
{
    const int bound = limit > 0 ? qMin(limit, kMaxCompletionRows)
                                : kMaxCompletionRows;
    QVariantList rows;
    QSet<QString> seen;
    for (const stickers::Pack &pack : m_packs->packs()) {
        for (const stickers::PackImage &image : pack.images) {
            if (rows.size() >= bound)
                return rows;
            if (!image.isEmoticon)
                continue;
            if (!prefix.isEmpty()
                && !image.shortcode.startsWith(prefix, Qt::CaseInsensitive))
                continue;
            // One shortcode wins once. The account's own pack comes first in
            // the snapshot, so a user's own name beats a room pack's.
            if (seen.contains(image.shortcode))
                continue;
            seen.insert(image.shortcode);
            QVariantMap row = image.toVariantMap();
            row.insert(QStringLiteral("packName"), pack.displayName);
            rows.append(row);
        }
    }
    return rows;
}

QString StickerPackManager::shortcodeForUrl(const QString &url) const
{
    if (url.isEmpty())
        return {};
    for (const stickers::Pack &pack : m_packs->packs()) {
        for (const stickers::PackImage &image : pack.images) {
            if (image.isEmoticon && image.url == url)
                return image.shortcode;
        }
    }
    return {};
}

QVariantMap StickerPackManager::emoticon(const QString &shortcode) const
{
    if (shortcode.isEmpty())
        return {};
    for (const stickers::Pack &pack : m_packs->packs()) {
        for (const stickers::PackImage &image : pack.images) {
            if (image.isEmoticon && image.shortcode == shortcode) {
                QVariantMap row = image.toVariantMap();
                row.insert(QStringLiteral("packName"), pack.displayName);
                return row;
            }
        }
    }
    return {};
}

void StickerPackManager::applySnapshotForTest(const QString &roomId,
                                              bool roomCanManage,
                                              const QVariantList &packs)
{
    applySnapshot(roomId, roomCanManage, packs);
}

void StickerPackManager::onPacksReceived(quint64 opId, const QString &roomId,
                                         bool roomCanManage,
                                         const QVariantList &packs)
{
    // A late answer from a previous account (or a request this manager no
    // longer owns) must never populate the current one.
    if (opId == 0 || opId != m_fetchOp)
        return;
    m_fetchOp = 0;
    applySnapshot(roomId, roomCanManage, packs);
    if (m_refreshOwed) {
        m_refreshOwed = false;
        refresh();
    }
}

void StickerPackManager::onSaveFinished(quint64 opId, bool ok,
                                        const QString &category,
                                        const QString &shortcode)
{
    if (opId == 0 || opId != m_saveOp)
        return;
    m_saveOp = 0;
    const QString scope = m_saveScope.isEmpty() ? QStringLiteral("account")
                                                : m_saveScope;
    m_saveScope.clear();
    if (ok) {
        // Nothing is applied optimistically: the pack that follows is the
        // authoritative one. A failed READ afterwards keeps the last known
        // pack rather than emptying it.
        m_stale = true;
        refresh();
    }
    emitStateChanged();
    Q_EMIT saveFinished(ok, category, shortcode, scope);
}

void StickerPackManager::onLoggedOut()
{
    m_fetchOp = 0;
    m_saveOp = 0;
    m_saveScope.clear();
    m_roomsOp = 0;
    m_refreshOwed = false;
    m_loaded = false;
    m_stale = true;
    m_snapshotRoomId.clear();
    m_snapshotRoomCanManage = false;
    m_savedUrls.clear();
    m_packs->clear();
    m_images->clear();
    if (!m_selectedPackId.isEmpty()) {
        m_selectedPackId.clear();
        Q_EMIT selectedPackIdChanged();
    }
    emitStateChanged();
}

void StickerPackManager::applySnapshot(const QString &roomId,
                                       bool roomCanManage,
                                       const QVariantList &packs)
{
    QList<stickers::Pack> rows;
    rows.reserve(packs.size());
    m_savedUrls.clear();
    for (const QVariant &value : packs) {
        stickers::Pack pack = packFromVariant(value.toMap());
        if (pack.id.isEmpty())
            continue;
        if (pack.source == QLatin1String("user")) {
            for (const stickers::PackImage &image : pack.images)
                m_savedUrls.insert(image.url);
        }
        rows.append(pack);
    }
    m_packs->reset(rows);
    m_snapshotRoomId = roomId;
    m_snapshotRoomCanManage = roomCanManage;
    m_loaded = true;
    m_stale = false;

    // Keep the selection if the pack survived; otherwise pick the first pack
    // that actually holds something of the current usage, so opening the
    // picker never lands on an empty tab when a populated one exists.
    const QString previous = m_selectedPackId;
    if (m_selectedPackId.isEmpty() || m_packs->indexOfPack(m_selectedPackId) < 0) {
        m_selectedPackId.clear();
        const bool wantSticker = m_usage != QLatin1String("emoticon");
        for (const stickers::Pack &pack : rows) {
            const int n =
                wantSticker ? pack.stickerCount() : pack.emoticonCount();
            if (n > 0) {
                m_selectedPackId = pack.id;
                break;
            }
        }
        if (m_selectedPackId.isEmpty() && !rows.isEmpty())
            m_selectedPackId = rows.first().id;
    }
    rebuildImages();
    if (m_selectedPackId != previous)
        Q_EMIT selectedPackIdChanged();
    emitStateChanged();
}

void StickerPackManager::rebuildImages()
{
    const int row = m_packs->indexOfPack(m_selectedPackId);
    if (row < 0) {
        m_images->clear();
        return;
    }
    const bool wantSticker = m_usage != QLatin1String("emoticon");
    const stickers::Pack &pack = m_packs->packs().at(row);
    QList<stickers::PackImage> narrowed;
    narrowed.reserve(pack.images.size());
    for (const stickers::PackImage &image : pack.images) {
        if (wantSticker ? image.isSticker : image.isEmoticon)
            narrowed.append(image);
    }
    m_images->reset(narrowed);
}

void StickerPackManager::emitStateChanged()
{
    ++m_revision;
    Q_EMIT stateChanged();
}
