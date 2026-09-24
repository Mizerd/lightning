#include "stickers/StickerPackManager.h"

#include <QFileInfo>
#include <QTimer>

#include "matrix/MatrixClient.h"
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcStickers, "lightning.stickers")

namespace {

// Keeps a completion popup from covering the composer.
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
        // The Rust parser requires both; drop malformed rows rather than show
        // an unusable tile.
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
        connect(m_client, &MatrixClient::stickerPackEditFinished, this,
                &StickerPackManager::onEditFinished);
        connect(m_client, &MatrixClient::stickerPackRoomsSet, this,
                &StickerPackManager::onRoomsSet);
        // Packs are account data.
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
    if (m_snapshotRoomId == roomId)
        return;
    m_stale = true;

    // Fetch packs for the room the user is in, so "Add to this room's stickers"
    // is available without first opening the picker. One read per room entered,
    // coalesced through a zero-interval timer so keyboard navigation fetches
    // only for the room it lands on.
    if (!m_activeRoomFetch) {
        m_activeRoomFetch = new QTimer(this);
        m_activeRoomFetch->setSingleShot(true);
        m_activeRoomFetch->setInterval(0);
        connect(m_activeRoomFetch, &QTimer::timeout, this, [this] {
            if (!m_activeRoomId.isEmpty())
                refreshIfStale();
        });
    }
    m_activeRoomFetch->start();
}

void StickerPackManager::setSelectedPackId(const QString &id)
{
    if (m_selectedPackId == id)
        return;
    // Refuse unknown ids rather than show one pack under another's tab.
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
    // The answer carries the room back; applySnapshot records it.
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
    // No room-send fallback: a thread sticker must fail rather than land in the
    // main timeline (§8).
    m_client->sendSticker(roomId, rootId, row.url,
                          row.body.isEmpty() ? row.shortcode : row.body,
                          row.mimetype, static_cast<quint64>(qMax(0, row.width)),
                          static_cast<quint64>(qMax(0, row.height)),
                          static_cast<quint64>(qMax<qint64>(0, row.size)));
}

void StickerPackManager::uploadSticker(const QUrl &fileUrl,
                                       const QString &shortcode)
{
    // Single-flight, like every pack write: racing writes to one account-data
    // event would misreport the loser.
    if (!m_client || m_saveOp != 0 || !available())
        return;
    // Local files only; anything else is refused before the FFI.
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                               : QString{};
    if (path.isEmpty())
        return;
    const quint64 opId = m_nextOpId++;
    m_saveOp = opId;
    m_saveScope = QStringLiteral("account");
    // The body seeds the shortcode when none is given; Rust sanitizes it.
    const QString seed = shortcode.trimmed().isEmpty()
        ? QFileInfo(path).completeBaseName()
        : shortcode.trimmed();
    m_client->uploadStickerToUserPack(seed, seed, path, opId);
    emitStateChanged();
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
    // The shortcode is derived from the body and sanitized to MSC2545's
    // alphabet in Rust. (Deriving it from the event id, as Sable does, yields
    // characters the MSC forbids.)
    m_client->addStickerToUserPack(body, url, body, mimetype,
                                   static_cast<quint64>(qMax(0, width)),
                                   static_cast<quint64>(qMax(0, height)),
                                   static_cast<quint64>(qMax<qint64>(0, size)),
                                   opId);
    emitStateChanged();
}

// Pack management (MSC2545 CRUD): four verbs sharing one op slot and one
// dispatcher, so the permission and completion rules exist once.

bool StickerPackManager::canManagePack(const QString &packId) const
{
    if (!m_client || !available() || packId.isEmpty())
        return false;
    const int row = m_packs->indexOfPack(packId);
    if (row < 0)
        return false;
    const stickers::Pack &pack = m_packs->packs().at(row);
    // The account's own pack: always writable.
    if (pack.source != QLatin1String("room"))
        return true;
    // A room pack: only when the snapshot recorded permission. Absence is not
    // permission.
    return pack.canManage && !pack.roomId.isEmpty();
}

QVariantMap StickerPackManager::packInfo(const QString &packId) const
{
    const int row = packId.isEmpty() ? -1 : m_packs->indexOfPack(packId);
    if (row < 0)
        return {};
    const stickers::Pack &pack = m_packs->packs().at(row);
    return QVariantMap{
        { QStringLiteral("packId"), pack.id },
        { QStringLiteral("displayName"), pack.displayName },
        { QStringLiteral("source"), pack.source },
        { QStringLiteral("canManage"), canManagePack(packId) },
    };
}

void StickerPackManager::editPack(const QString &packId, const QString &action,
                                  const QString &argA, const QString &argB)
{
    if (!m_client || m_editOp != 0 || !canManagePack(packId))
        return;
    const int row = m_packs->indexOfPack(packId);
    if (row < 0)
        return;
    const stickers::Pack &pack = m_packs->packs().at(row);
    const bool isRoom = pack.source == QLatin1String("room");
    const quint64 opId = m_nextOpId++;
    m_editOp = opId;
    m_client->editStickerPack(isRoom ? pack.roomId : QString(),
                              isRoom ? pack.stateKey : QString(),
                              action, argA, argB, opId);
    emitStateChanged();
}

void StickerPackManager::removeImageFromPack(const QString &packId,
                                             const QString &shortcode)
{
    if (shortcode.isEmpty())
        return;
    editPack(packId, QStringLiteral("remove_image"), shortcode, QString());
}

void StickerPackManager::renameImageInPack(const QString &packId,
                                           const QString &from,
                                           const QString &to)
{
    if (from.isEmpty() || to.trimmed().isEmpty())
        return;
    editPack(packId, QStringLiteral("rename_image"), from, to);
}

void StickerPackManager::renamePack(const QString &packId, const QString &name)
{
    // An empty name is meaningful: it clears the display name (a room pack then
    // falls back to the room's name).
    editPack(packId, QStringLiteral("set_name"), name, QString());
}

void StickerPackManager::deletePack(const QString &packId)
{
    editPack(packId, QStringLiteral("delete_pack"), QString(), QString());
}

void StickerPackManager::onEditFinished(quint64 opId, bool ok,
                                        const QString &category,
                                        const QString &shortcode)
{
    if (opId == 0 || opId != m_editOp)
        return;
    m_editOp = 0;
    if (ok) {
        // Nothing was applied optimistically; the next pack read is
        // authoritative. A failed read keeps the last known pack.
        m_stale = true;
        refresh();
    }
    emitStateChanged();
    Q_EMIT editFinished(ok, category, shortcode);
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
    // The account's own pack is global by definition; `im.ponies.emote_rooms`
    // cannot describe it.
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
        // Not optimistic: the switch moves when the snapshot says so.
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
    // The empty state key is the room's default pack (MSC2545). Choosing among
    // several packs belongs in a pack editor.
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
    // The snapshot for this room must have granted write permission; absence is
    // not permission.
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
    // Packs hold plain mxc URLs; an encrypted sticker has none.
    if (!url.startsWith(QLatin1String("mxc://")))
        return false;
    // Not gated on isSaved(); Rust refuses duplicates authoritatively.
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
            // First shortcode wins; the account's own pack comes first.
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
    // A late answer from a previous account or superseded request is dropped.
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
        // Not optimistic: the next pack read is authoritative. A failed read
        // keeps the last known pack.
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
    m_editOp = 0;
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
    // with content for the current usage.
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
