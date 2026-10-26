#include "stickers/StickerPackManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>

#include "matrix/MatrixClient.h"
#include "media/SvgRasterJob.h"
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackArchive.h"
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
    const QString ns = map.value(QStringLiteral("namespace")).toString();
    pack.ns = ns == QLatin1String("stable") ? QStringLiteral("stable")
                                            : QStringLiteral("legacy");
    pack.legacyTwin = map.value(QStringLiteral("legacyTwin")).toBool();
    pack.legacySubscribed =
        map.value(QStringLiteral("legacySubscribed")).toBool();
    pack.usageUnsupported =
        map.value(QStringLiteral("usageUnsupported")).toBool();
    pack.available = map.value(QStringLiteral("available"), true).toBool();
    pack.unavailable = map.value(QStringLiteral("unavailable")).toString();
    pack.fromSpace = map.value(QStringLiteral("fromSpace")).toBool();
    const QVariantList images =
        map.value(QStringLiteral("images")).toList();
    pack.images.reserve(images.size());
    for (const QVariant &value : images) {
        const stickers::PackImage image =
            stickers::PackImage::fromVariantMap(value.toMap());
        // The Rust parser requires a url. The key may be empty or malformed in
        // a pack another client wrote: such a row stays (identified by its
        // exact key) so it can be inspected, removed or renamed.
        if (image.url.isEmpty())
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
    , m_editorImages(new StickerImageModel(this))
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
        connect(m_client, &MatrixClient::stickerPackSnapshotMeta, this,
                [this](quint64 opId, const QVariantMap &meta) {
                    if (opId != 0 && opId == m_fetchOp)
                        m_pendingMeta = meta;
                });
        connect(m_client, &MatrixClient::stickerPacksChanged, this,
                &StickerPackManager::onPacksChanged);
        connect(m_client, &MatrixClient::stickerPackMigrated, this,
                [this](quint64 opId, bool ok, const QString &category,
                       const QVariantMap &detail) {
                    if (opId == 0 || opId != m_migrateOp)
                        return;
                    m_migrateOp = 0;
                    // Whatever landed is re-read, success or part-way.
                    m_stale = true;
                    refresh();
                    emitStateChanged();
                    Q_EMIT migrationFinished(ok, category, detail);
                });
        // Export fetches pack images by their own op ids; every other op id
        // is ignored here.
        connect(m_client, &MatrixClient::mediaReady, this,
                [this](quint64 opId, const QString &, int,
                       const QByteArray &bytes, const QString &,
                       const QString &) { onMediaFetched(opId, bytes); });
        connect(m_client, &MatrixClient::mediaFailed, this,
                [this](quint64 opId, const QString &, int, const QString &) {
                    onMediaFetchFailed(opId);
                });
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
            // Navigation alone never reads a room's whole state.
            if (!m_activeRoomId.isEmpty()
                && (!m_loaded || m_stale || m_snapshotRoomId != m_activeRoomId))
                refreshWith(false);
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

void StickerPackManager::setEditorPackId(const QString &id)
{
    if (m_editorPackId == id)
        return;
    m_editorPackId = id;
    rebuildImages();
    Q_EMIT selectedPackIdChanged();
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
    // An explicit refresh comes from a pack surface the user opened (picker,
    // manager, editor), so the room's full state may be read once.
    refreshWith(true);
}

void StickerPackManager::refreshWith(bool deep)
{
    if (!available())
        return;
    if (m_fetchOp != 0) {
        m_refreshOwed = true;
        m_refreshOwedDeep = m_refreshOwedDeep || deep;
        return;
    }
    const quint64 opId = m_nextOpId++;
    m_fetchOp = opId;
    m_pendingMeta.clear();
    // Once a deep read has covered this room, later refreshes keep reading it
    // deep (the scan is cached in Rust, so it costs nothing until it expires
    // or sync says the room's packs changed).
    if (deep)
        m_deepRoomId = m_activeRoomId;
    const bool readDeep = deep || (!m_activeRoomId.isEmpty()
                                   && m_deepRoomId == m_activeRoomId);
    // The answer carries the room back; applySnapshot records it.
    m_client->fetchStickerPacks(m_activeRoomId, opId, readDeep);
    emitStateChanged();
}

void StickerPackManager::refreshIfStale()
{
    // A snapshot taken on plain navigation never read the room's own state
    // (sliding sync does not deliver pack events), so a pack surface opening
    // in a room no deep read has covered yet is stale too. Live GUI run
    // 2026-10-26: the room's stable packs appeared only after Reload packs.
    if (!m_loaded || m_stale || m_snapshotRoomId != m_activeRoomId
        || (!m_activeRoomId.isEmpty() && m_deepRoomId != m_activeRoomId))
        refresh();
}

void StickerPackManager::onPacksChanged(const QString &roomId)
{
    // A pack or subscription changed in sync (another device, another member,
    // or our own write's echo). Only relevant when it can change what this
    // snapshot shows: account data always can; a room only when it is the
    // snapshot's room or one of its packs is subscribed.
    if (!roomId.isEmpty() && roomId != m_snapshotRoomId) {
        bool referenced = false;
        for (const stickers::Pack &pack : m_packs->packs()) {
            if (pack.roomId == roomId) {
                referenced = true;
                break;
            }
        }
        if (!referenced)
            return;
    }
    m_stale = true;
    if (!m_loaded)
        return;
    // Coalesce a burst (several state events in one sync) into one read.
    if (!m_changeRefresh) {
        m_changeRefresh = new QTimer(this);
        m_changeRefresh->setSingleShot(true);
        m_changeRefresh->setInterval(250);
        connect(m_changeRefresh, &QTimer::timeout, this,
                [this] { refreshWith(false); });
    }
    m_changeRefresh->start();
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
    startUpload(fileUrl, shortcode, QStringLiteral("sticker"));
}

void StickerPackManager::uploadImage(const QUrl &fileUrl,
                                     const QString &shortcode,
                                     const QString &usage)
{
    if (m_transfer.active)
        return;
    startUpload(fileUrl, shortcode,
                usage == QLatin1String("emoticon") || usage == QLatin1String("both")
                    ? usage : QStringLiteral("sticker"));
}

void StickerPackManager::startUpload(const QUrl &fileUrl,
                                     const QString &shortcode,
                                     const QString &usage)
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

    // An SVG (by shape: the first non-space byte is `<`) is converted to a PNG
    // before anything is uploaded; Rust would refuse the SVG anyway.
    namespace svg = lightning::svgraster;
    QByteArray head;
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            head = file.read(svg::kMaxSourceBytes + 1);
    }
    const bool svgz = svg::isGzip(head)
        && path.endsWith(QLatin1String(".svgz"), Qt::CaseInsensitive);
    if (svg::looksLikeSvg(head) || svgz) {
        emitStateChanged();
        QPointer<StickerPackManager> self(this);
        svg::rasterizeAsync(
            head, svg::stickerPolicy(), this,
            [self, opId, seed, usage](const svg::Result &converted) {
                if (!self || self->m_saveOp != opId)
                    return; // signed out, or superseded
                if (!converted.refusal.isEmpty()) {
                    self->onSaveFinished(
                        opId, false,
                        QStringLiteral("svg_") + converted.refusal, QString());
                    return;
                }
                if (!self->m_convertedDir || !self->m_convertedDir->isValid()) {
                    const QString root = svg::hooks().scratchRoot
                        ? svg::hooks().scratchRoot() : QDir::tempPath();
                    self->m_convertedDir = std::make_unique<QTemporaryDir>(
                        root + QStringLiteral("/lightning-svgsticker-XXXXXX"));
                }
                const QString dir = self->m_convertedDir->isValid()
                    ? self->m_convertedDir->path() : QString();
                const QString file = dir.isEmpty() ? QString()
                    : QDir(dir).filePath(QStringLiteral("%1.png")
                                             .arg(self->m_nextConverted++));
                QFile out(file);
                if (file.isEmpty() || !out.open(QIODevice::WriteOnly)
                    || out.write(converted.png) != converted.png.size()) {
                    self->onSaveFinished(opId, false,
                                         QStringLiteral("svg_encode"), QString());
                    return;
                }
                out.close();
                self->m_client->uploadStickerToUserPack(seed, seed, file, usage, opId);
            });
        return;
    }

    m_client->uploadStickerToUserPack(seed, seed, path, usage, opId);
    emitStateChanged();
}

QString StickerPackManager::svgMessage(const QString &reason) const
{
    return lightning::svgraster::userMessage(reason);
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
                                   QStringLiteral("sticker"), opId);
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
    // permission, and an unreadable pack cannot be edited at all.
    return pack.available && pack.canManage && !pack.roomId.isEmpty();
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
        { QStringLiteral("namespace"), pack.ns },
        { QStringLiteral("roomId"), pack.roomId },
        { QStringLiteral("stateKey"), pack.stateKey },
        { QStringLiteral("available"), pack.available },
        { QStringLiteral("unavailable"), pack.unavailable },
        { QStringLiteral("legacyTwin"), pack.legacyTwin },
        { QStringLiteral("legacySubscribed"), pack.legacySubscribed },
        { QStringLiteral("usageUnsupported"), pack.usageUnsupported },
        { QStringLiteral("enabledGlobally"), pack.enabledGlobally },
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
                              isRoom ? pack.ns : QStringLiteral("legacy"),
                              action, argA, argB, opId);
    emitStateChanged();
}

void StickerPackManager::removeImageFromPack(const QString &packId,
                                             const QString &key)
{
    // An empty key is a real (malformed) key another client may have written,
    // and must stay removable: the backend answers not_found when absent.
    editPack(packId, QStringLiteral("remove_image"), key, QString());
}

void StickerPackManager::renameImageInPack(const QString &packId,
                                           const QString &from,
                                           const QString &to)
{
    if (to.isEmpty())
        return;
    editPack(packId, QStringLiteral("rename_image"), from, to);
}

void StickerPackManager::renamePack(const QString &packId, const QString &name)
{
    // An empty name is meaningful: it clears the display name (a room pack then
    // falls back to the room's name).
    editPack(packId, QStringLiteral("set_name"), name, QString());
}

void StickerPackManager::setImageDescription(const QString &packId,
                                             const QString &key,
                                             const QString &description)
{
    editPack(packId, QStringLiteral("set_body"), key, description);
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
    m_client->setStickerRoomPackEnabled(pack.roomId, pack.stateKey, pack.ns,
                                        enabled, opId);
    emitStateChanged();
}

void StickerPackManager::onRoomsSet(quint64 opId, bool ok,
                                    const QString &category, const QString &,
                                    const QString &, bool enabled,
                                    const QString &mirror)
{
    if (opId == 0 || opId != m_roomsOp)
        return;
    m_roomsOp = 0;
    // The standard list saved; the copy older clients read did not. Reported,
    // not hidden, and never turned into a failure of the saved part.
    m_lastMirrorFailed = ok && mirror == QLatin1String("failed");
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
    // The destination is an exact (dialect, state key). The empty key has no
    // "default pack" meaning in Matrix v1.19, so it is not assumed: the first
    // writable pack the room already has is used (stable before legacy), and a
    // room with none gets a NEW stable pack under a fresh key (the create
    // refuses if that key turns out to be taken).
    const RoomPackTarget target = roomSaveTarget(roomId);
    if (target.stateKey.isNull())
        return;
    const quint64 opId = m_nextOpId++;
    m_saveOp = opId;
    m_saveScope = QStringLiteral("room");
    m_client->addStickerToRoomPack(roomId, target.stateKey, target.ns, body,
                                   url, body,
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
    // The snapshot for this room must have granted write permission for the
    // pack that would receive it; absence is not permission.
    return m_loaded && m_snapshotRoomId == roomId
        && !roomSaveTarget(roomId).stateKey.isNull();
}

StickerPackManager::RoomPackTarget
StickerPackManager::roomSaveTarget(const QString &roomId) const
{
    RoomPackTarget legacy;
    for (const stickers::Pack &pack : m_packs->packs()) {
        if (pack.source != QLatin1String("room") || pack.roomId != roomId
            || !pack.available || !pack.canManage)
            continue;
        if (pack.ns == QLatin1String("stable"))
            return { pack.stateKey.isNull() ? QStringLiteral("") : pack.stateKey,
                     QStringLiteral("stable") };
        if (legacy.stateKey.isNull())
            legacy = { pack.stateKey.isNull() ? QStringLiteral("") : pack.stateKey,
                       QStringLiteral("legacy") };
    }
    if (!legacy.stateKey.isNull())
        return legacy;
    if (m_snapshotRoomId == roomId && m_snapshotRoomCanManage)
        return { freshStateKey(roomId), QStringLiteral("stable") };
    return {};
}

QString StickerPackManager::freshStateKey(const QString &roomId) const
{
    QSet<QString> taken;
    for (const stickers::Pack &pack : m_packs->packs()) {
        if (pack.roomId == roomId)
            taken.insert(pack.stateKey);
    }
    QString key = QStringLiteral("stickers");
    for (int n = 2; taken.contains(key); ++n)
        key = QStringLiteral("stickers-%1").arg(n);
    return key;
}

void StickerPackManager::createRoomPack(const QString &roomId,
                                        const QString &name,
                                        const QString &usage)
{
    if (!m_client || !available() || m_editOp != 0 || roomId.isEmpty())
        return;
    if (!(m_snapshotRoomId == roomId && m_snapshotRoomCanManage))
        return;
    const QString kind = usage == QLatin1String("emoticon")
            || usage == QLatin1String("sticker")
        ? usage : QStringLiteral("both");
    const quint64 opId = m_nextOpId++;
    m_editOp = opId;
    m_client->createStickerRoomPack(roomId, freshStateKey(roomId),
                                    name.trimmed(), kind, opId);
    emitStateChanged();
}

void StickerPackManager::setPackUsage(const QString &packId,
                                      const QString &usage)
{
    const int row = m_packs->indexOfPack(packId);
    if (row < 0 || m_packs->packs().at(row).ns != QLatin1String("stable"))
        return;
    if (usage != QLatin1String("emoticon") && usage != QLatin1String("sticker")
        && usage != QLatin1String("both"))
        return;
    editPack(packId, QStringLiteral("set_usage"), usage, QString());
}

QVariantMap StickerPackManager::userPackMigrationPreview() const
{
    const int row = m_packs->indexOfPack(QStringLiteral("user"));
    if (row < 0)
        return {};
    int emoji = 0, stickersOnly = 0, both = 0, invalid = 0;
    for (const stickers::PackImage &image : m_packs->packs().at(row).images) {
        if (!image.keyValid) {
            ++invalid;
            continue;
        }
        if (image.isEmoticon && !image.isSticker)
            ++emoji;
        else if (image.isSticker && !image.isEmoticon)
            ++stickersOnly;
        else
            ++both;
    }
    return QVariantMap{
        { QStringLiteral("emoji"), emoji },
        { QStringLiteral("stickers"), stickersOnly },
        { QStringLiteral("both"), both },
        { QStringLiteral("invalid"), invalid },
        { QStringLiteral("total"), emoji + stickersOnly + both + invalid },
    };
}

void StickerPackManager::migrateUserPack()
{
    if (!m_client || !available() || m_migrateOp != 0 || m_editOp != 0
        || m_transfer.active)
        return;
    if (m_packs->indexOfPack(QStringLiteral("user")) < 0)
        return;
    m_migrateOp = m_nextOpId++;
    m_client->migrateUserStickerPack(m_migrateOp);
    emitStateChanged();
}

bool StickerPackManager::canCreateRoomPack(const QString &roomId) const
{
    return available() && !roomId.isEmpty() && m_loaded
        && m_snapshotRoomId == roomId && m_snapshotRoomCanManage;
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

QVariantList StickerPackManager::reactionEmoticons() const
{
    return collectEmoticons(QString(), 0, false);
}

QVariantList StickerPackManager::findEmoticons(const QString &prefix,
                                               int limit) const
{
    const int bound = limit > 0 ? qMin(limit, kMaxCompletionRows)
                                : kMaxCompletionRows;
    return collectEmoticons(prefix, bound, true);
}

QVariantList StickerPackManager::collectEmoticons(const QString &prefix,
                                                 int limit,
                                                 bool uniqueShortcodes) const
{
    QVariantList rows;
    QSet<QString> seen;
    for (const stickers::Pack &pack : m_packs->packs()) {
        for (const stickers::PackImage &image : pack.images) {
            if (limit > 0 && rows.size() >= limit)
                return rows;
            if (!image.isEmoticon)
                continue;
            // A key outside the grammar cannot round-trip as `:key:` text.
            if (uniqueShortcodes && !image.keyValid)
                continue;
            if (!prefix.isEmpty()
                && !image.shortcode.startsWith(prefix, Qt::CaseInsensitive))
                continue;
            // Every image stays reachable: two packs' `:cat:` are two rows,
            // each naming its pack, and the user's choice is what is sent
            // (PACK-PRO-004). Only the same image reached twice (same key and
            // url, e.g. subscribed and in this room) is shown once.
            if (uniqueShortcodes) {
                const QString identity = image.key + QChar(0x1f) + image.url;
                if (seen.contains(identity))
                    continue;
                seen.insert(identity);
            }
            QVariantMap row = image.toVariantMap();
            row.insert(QStringLiteral("packName"), pack.displayName);
            row.insert(QStringLiteral("packId"), pack.id);
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

QString StickerPackManager::uniqueEmoticonUrl(const QString &shortcode) const
{
    // A typed `:shortcode:` nobody chose from a list resolves only when it is
    // unambiguous: every usable image with that exact key is the same media.
    // Otherwise it stays literal text rather than silently becoming the first
    // pack's image (Matrix v1.19 asks for disambiguation).
    if (shortcode.isEmpty())
        return {};
    QString found;
    for (const stickers::Pack &pack : m_packs->packs()) {
        for (const stickers::PackImage &image : pack.images) {
            if (!image.isEmoticon || !image.keyValid || image.key != shortcode)
                continue;
            if (!found.isEmpty() && found != image.url)
                return {};
            found = image.url;
        }
    }
    return found;
}

QVariantMap StickerPackManager::emoticon(const QString &shortcode) const
{
    if (shortcode.isEmpty())
        return {};
    for (const stickers::Pack &pack : m_packs->packs()) {
        for (const stickers::PackImage &image : pack.images) {
            if (image.isEmoticon && image.keyValid && image.key == shortcode) {
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
    m_snapshotMeta = m_pendingMeta;
    m_pendingMeta.clear();
    applySnapshot(roomId, roomCanManage, packs);
    if (m_refreshOwed) {
        const bool deep = m_refreshOwedDeep;
        m_refreshOwed = false;
        m_refreshOwedDeep = false;
        refreshWith(deep);
    }
}

void StickerPackManager::onSaveFinished(quint64 opId, bool ok,
                                        const QString &category,
                                        const QString &shortcode)
{
    if (opId == 0 || opId != m_saveOp)
        return;
    m_saveOp = 0;
    if (m_transfer.active && opId == m_transfer.op) {
        // One image of a batch: no per-image notice, and no per-image re-read.
        onBatchItemFinished(ok, category, shortcode);
        return;
    }
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
    // A job belongs to the account that started it: nothing it was waiting for
    // may complete into the next one.
    ++m_transferGeneration;
    m_transfer = Transfer();
    m_importDir.reset();
    if (m_transferWatchdog)
        m_transferWatchdog->stop();
    m_fetchOp = 0;
    m_saveOp = 0;
    m_editOp = 0;
    m_saveScope.clear();
    m_roomsOp = 0;
    m_migrateOp = 0;
    m_refreshOwed = false;
    m_refreshOwedDeep = false;
    m_deepRoomId.clear();
    m_pendingMeta.clear();
    m_snapshotMeta.clear();
    m_lastMirrorFailed = false;
    m_loaded = false;
    m_stale = true;
    m_snapshotRoomId.clear();
    m_snapshotRoomCanManage = false;
    m_savedUrls.clear();
    m_packs->clear();
    m_images->clear();
    m_editorImages->clear();
    if (!m_selectedPackId.isEmpty()) {
        m_selectedPackId.clear();
        Q_EMIT selectedPackIdChanged();
    }
    m_editorPackId.clear();
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
    // The editor's rows follow ITS pack, not the picker's selection.
    const QString editorId =
        m_editorPackId.isEmpty() ? m_selectedPackId : m_editorPackId;
    const int editorRow = m_packs->indexOfPack(editorId);
    if (editorRow < 0)
        m_editorImages->clear();
    else
        m_editorImages->reset(m_packs->packs().at(editorRow).images);
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

// -- Shortcode validation ----------------------------------------------------

QString StickerPackManager::sanitizeShortcode(const QString &raw) const
{
    return stickers::archive::sanitizeShortcode(raw);
}

QString StickerPackManager::shortcodeError(const QString &packId,
                                           const QString &current,
                                           const QString &candidate) const
{
    // Matrix v1.19: `[A-Za-z0-9_-]{1,100}`, case-sensitive. Nothing stricter
    // (`__`, a leading `-` and 65-100 characters are all valid) and nothing
    // looser; the backend validates the same way and never repairs a rename.
    if (candidate.isEmpty())
        return tr("Enter a name.");
    if (candidate.size() > 100)
        return tr("Names can be at most 100 characters (this one has %1).")
            .arg(candidate.size());
    if (!stickers::archive::isValidShortcode(candidate))
        return tr("Use only letters, numbers, - and _ (no spaces, colons "
                  "or accents).");
    const int row = m_packs->indexOfPack(packId);
    if (row >= 0 && candidate != current) {
        for (const stickers::PackImage &image : m_packs->packs().at(row).images) {
            // Exact: `Cat` and `cat` are two different names.
            if (image.key == candidate && image.key != current)
                return tr("Another image in this pack already uses that name.");
        }
    }
    return {};
}

QSet<QString> StickerPackManager::userPackShortcodes() const
{
    QSet<QString> taken;
    const int row = m_packs->indexOfPack(QStringLiteral("user"));
    if (row < 0)
        return taken;
    for (const stickers::PackImage &image : m_packs->packs().at(row).images)
        taken.insert(image.key);
    return taken;
}

// -- Transfers ---------------------------------------------------------------
//
// Export, import and multi-emoji saves share one slot (`m_transfer`): a second
// job while one runs is refused, never queued, so two jobs can never
// interleave read-modify-writes of the same account-data event.

void StickerPackManager::beginTransfer(const QString &kind, const QString &name,
                                       int total)
{
    m_transfer = Transfer();
    m_transfer.active = true;
    m_transfer.kind = kind;
    m_transfer.name = name;
    m_transfer.total = total;
    m_transfer.generation = ++m_transferGeneration;
    if (!m_transferWatchdog) {
        m_transferWatchdog = new QTimer(this);
        m_transferWatchdog->setSingleShot(true);
        m_transferWatchdog->setInterval(120 * 1000);
        connect(m_transferWatchdog, &QTimer::timeout, this, [this] {
            if (!m_transfer.active)
                return;
            // Nothing moved for two minutes: end the job with what it has.
            if (m_transfer.kind == QLatin1String("export")) {
                m_transfer.failed += int(m_transfer.pending.size())
                    + (m_transfer.total - m_transfer.nextFetch);
                m_transfer.pending.clear();
                m_transfer.nextFetch = m_transfer.total;
                finishExport();
            } else {
                m_saveOp = 0;
                finishTransfer(m_transfer.added > 0, QStringLiteral("timeout"));
            }
        });
    }
    m_transferWatchdog->start();
    emitStateChanged();
}

void StickerPackManager::finishTransfer(bool ok, const QString &category)
{
    const Transfer done = m_transfer;
    m_transfer = Transfer();
    m_importDir.reset();
    if (m_transferWatchdog)
        m_transferWatchdog->stop();
    if (done.kind != QLatin1String("export") && done.added > 0) {
        m_stale = true;
        refresh();
    }
    emitStateChanged();
    Q_EMIT transferFinished(done.kind, ok, category, done.added, done.skipped,
                            done.failed,
                            done.kind == QLatin1String("emoji") ? done.firstCode
                                                                : done.name);
}

// Starts a batch of pack additions. `alreadySkipped` counts entries dropped
// before the batch (refused or already present), so the report is complete.
void StickerPackManager::startBatch(const QString &kind, const QString &name,
                                    const QList<BatchItem> &items,
                                    int alreadySkipped)
{
    beginTransfer(kind, name, int(items.size()) + alreadySkipped);
    m_transfer.skipped = alreadySkipped;
    m_transfer.done = alreadySkipped;
    m_transfer.items = items;
    m_transfer.taken = userPackShortcodes();
    advanceBatch();
}

void StickerPackManager::advanceBatch()
{
    while (m_transfer.active) {
        if (m_transfer.index >= m_transfer.items.size()) {
            const bool failedAll =
                m_transfer.added == 0 && m_transfer.failed > 0;
            finishTransfer(!failedAll,
                           failedAll ? (m_transfer.category.isEmpty()
                                            ? QStringLiteral("rejected")
                                            : m_transfer.category)
                                     : QString());
            return;
        }
        const BatchItem item = m_transfer.items.at(m_transfer.index++);
        if (!item.path.isEmpty()
            && m_transfer.taken.contains(item.shortcode)) {
            ++m_transfer.skipped;
            ++m_transfer.done;
            continue;
        }
        if (!m_client || !available()) {
            ++m_transfer.failed;
            ++m_transfer.done;
            continue;
        }
        const quint64 opId = m_nextOpId++;
        m_saveOp = opId;
        m_saveScope = QStringLiteral("account");
        m_transfer.op = opId;
        m_transfer.taken.insert(item.shortcode);
        m_transferWatchdog->start();
        if (!item.path.isEmpty()) {
            m_client->uploadStickerToUserPack(item.shortcode, item.body,
                                              item.path, item.usage, opId);
        } else {
            m_client->addStickerToUserPack(item.shortcode, item.url, item.body,
                                           item.mimetype, 0, 0, 0, item.usage,
                                           opId);
        }
        emitStateChanged();
        return;  // continues in onBatchItemFinished
    }
}

void StickerPackManager::onBatchItemFinished(bool ok, const QString &category,
                                             const QString &shortcode)
{
    if (!m_transfer.active)
        return;
    ++m_transfer.done;
    m_transfer.op = 0;
    if (ok) {
        ++m_transfer.added;
        if (m_transfer.firstCode.isEmpty())
            m_transfer.firstCode = shortcode;
    } else if (category == QLatin1String("duplicate")) {
        ++m_transfer.skipped;
    } else if (category == QLatin1String("pack_full")) {
        // Every remaining image would be refused the same way.
        const int left = int(m_transfer.items.size()) - m_transfer.index;
        m_transfer.failed += 1 + left;
        m_transfer.done += left;
        m_transfer.index = int(m_transfer.items.size());
        m_transfer.category = category;
    } else {
        ++m_transfer.failed;
        if (m_transfer.category.isEmpty())
            m_transfer.category = category;
    }
    advanceBatch();
}

// -- Import ------------------------------------------------------------------

void StickerPackManager::importPack(const QUrl &zipFile)
{
    if (!m_client || !available() || m_transfer.active || m_saveOp != 0
        || m_editOp != 0)
        return;
    const QString zipPath = zipFile.isLocalFile() ? zipFile.toLocalFile()
                                                  : QString{};
    if (zipPath.isEmpty())
        return;
    m_importDir = std::make_unique<QTemporaryDir>();
    if (!m_importDir->isValid()) {
        m_importDir.reset();
        Q_EMIT transferFinished(QStringLiteral("import"), false,
                                QStringLiteral("write_failed"), 0, 0, 0,
                                QString());
        return;
    }
    beginTransfer(QStringLiteral("import"), QString(), 0);
    const quint64 generation = m_transfer.generation;
    const QString workDir = m_importDir->path();
    QPointer<StickerPackManager> self(this);
    // Unzipping up to ~100 MB is not GUI-thread work.
    QThreadPool::globalInstance()->start([self, zipPath, workDir, generation] {
        stickers::archive::ReadResult read =
            stickers::archive::readArchive(zipPath, workDir);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, read, generation] {
                if (!self || !self->m_transfer.active
                    || self->m_transfer.generation != generation)
                    return;
                if (read.error != stickers::archive::ReadError::None) {
                    const char *category =
                        read.error == stickers::archive::ReadError::NoManifest
                            ? "no_manifest"
                        : read.error == stickers::archive::ReadError::BadManifest
                            ? "bad_manifest"
                        : read.error == stickers::archive::ReadError::NoImages
                            ? "no_images" : "unreadable";
                    self->finishTransfer(false, QString::fromLatin1(category));
                    return;
                }
                QList<BatchItem> items;
                const QSet<QString> taken = self->userPackShortcodes();
                int skipped = read.refused;
                for (const stickers::archive::Image &image : read.pack.images) {
                    if (taken.contains(image.shortcode)) {
                        ++skipped;
                        continue;
                    }
                    BatchItem item;
                    item.shortcode = image.shortcode;
                    item.body = image.body;
                    item.path = image.path;
                    item.mimetype = image.mimetype;
                    item.usage = image.usage;
                    items.append(item);
                }
                // The work directory stays: the uploads read from it, and
                // finishTransfer() removes it.
                self->startBatch(QStringLiteral("import"), read.pack.name,
                                 items, skipped);
            },
            Qt::QueuedConnection);
    });
}

QString StickerPackManager::transferMessage(const QString &kind, bool ok,
                                            const QString &category, int added,
                                            int skipped, int failed,
                                            const QString &name) const
{
    Q_UNUSED(name);
    const bool isExport = kind == QLatin1String("export");
    const bool isEmoji = kind == QLatin1String("emoji");
    if (!ok) {
        if (category == QLatin1String("no_images"))
            return isExport ? tr("This pack has no images to export.")
                            : tr("That pack has no usable images. SVG files "
                                 "are not accepted.");
        if (category == QLatin1String("fetch_failed"))
            return tr("None of the images could be downloaded, so nothing was "
                      "exported.");
        if (category == QLatin1String("write_failed"))
            return tr("The file could not be written.");
        if (category == QLatin1String("unreadable"))
            return tr("That file is not a readable sticker pack.");
        if (category == QLatin1String("no_manifest"))
            return tr("That ZIP is not a Lightning sticker pack (it has no "
                      "pack.json).");
        if (category == QLatin1String("bad_manifest"))
            return tr("That pack file is damaged, or was made by a newer "
                      "version of Lightning.");
        if (category == QLatin1String("pack_full"))
            return tr("Your pack is full. Remove some images in another "
                      "client to add more.");
        if (category == QLatin1String("not_found"))
            return tr("That pack is no longer available.");
        return isExport ? tr("The pack could not be exported.")
               : isEmoji ? tr("The emoji could not be added.")
                         : tr("The pack could not be imported.");
    }
    QString text;
    if (isExport) {
        text = tr("Exported %n image(s).", nullptr, added);
    } else if (isEmoji) {
        text = added == 0 ? tr("Already in your custom emoji.")
                          : tr("Added %n custom emoji to your pack.", nullptr,
                               added);
    } else {
        text = added == 0 ? tr("Everything in that pack is already in yours.")
                          : tr("Added %n image(s) to your pack.", nullptr,
                               added);
    }
    if (added > 0 && skipped > 0)
        text += QLatin1Char(' ')
                + (isExport ? tr("%n could not be included.", nullptr, skipped)
                            : tr("%n already there or not usable.", nullptr,
                                 skipped));
    if (failed > 0)
        text += QLatin1Char(' ')
                + (isExport ? tr("%n could not be downloaded.", nullptr, failed)
                            : tr("%n failed.", nullptr, failed));
    return text;
}

// -- Export ------------------------------------------------------------------

void StickerPackManager::exportPack(const QString &packId,
                                    const QUrl &zipFile)
{
    if (!m_client || !available() || m_transfer.active)
        return;
    const QString dest = zipFile.isLocalFile() ? zipFile.toLocalFile()
                                               : QString{};
    const int row = m_packs->indexOfPack(packId);
    if (dest.isEmpty() || row < 0)
        return;
    const stickers::Pack pack = m_packs->packs().at(row);
    if (pack.images.isEmpty()) {
        Q_EMIT transferFinished(QStringLiteral("export"), false,
                                QStringLiteral("no_images"), 0, 0, 0,
                                pack.displayName);
        return;
    }
    beginTransfer(QStringLiteral("export"), pack.displayName,
                  int(pack.images.size()));
    m_transfer.frozen = pack;
    m_transfer.packId = packId;
    m_transfer.destination = dest;
    m_transfer.bytes.resize(pack.images.size());
    pumpExport();
}

void StickerPackManager::pumpExport()
{
    if (!m_transfer.active || m_transfer.kind != QLatin1String("export"))
        return;
    // The frozen manifest, not the live model: a refresh mid-export changes
    // nothing about what this export is.
    const QList<stickers::PackImage> &images = m_transfer.frozen.images;
    // A few at a time: enough to hide latency, few enough not to flood the
    // media queue the timeline shares.
    constexpr int kInFlight = 3;
    while (m_transfer.pending.size() < kInFlight
           && m_transfer.nextFetch < images.size()) {
        const int index = m_transfer.nextFetch++;
        const quint64 opId = m_client->fetchMxcThumbnail(images.at(index).url,
                                                         0, 0);
        if (opId == 0) {
            ++m_transfer.failed;
            ++m_transfer.done;
        } else {
            m_transfer.pending.insert(opId, index);
        }
    }
    if (m_transfer.pending.isEmpty() && m_transfer.nextFetch >= images.size()) {
        finishExport();
        return;
    }
    emitStateChanged();
}

void StickerPackManager::onMediaFetched(quint64 opId, const QByteArray &bytes)
{
    if (!m_transfer.active || !m_transfer.pending.contains(opId))
        return;
    const int index = m_transfer.pending.take(opId);
    m_transfer.bytes[index] = bytes;
    ++m_transfer.done;
    m_transferWatchdog->start();
    pumpExport();
}

void StickerPackManager::onMediaFetchFailed(quint64 opId)
{
    if (!m_transfer.active || !m_transfer.pending.contains(opId))
        return;
    m_transfer.pending.remove(opId);
    ++m_transfer.failed;
    ++m_transfer.done;
    m_transferWatchdog->start();
    pumpExport();
}

void StickerPackManager::finishExport()
{
    stickers::archive::Pack out;
    {
        const stickers::Pack &pack = m_transfer.frozen;
        out.name = pack.displayName;
        out.attribution = pack.attribution;
        for (int i = 0; i < pack.images.size() && i < m_transfer.bytes.size();
             ++i) {
            const stickers::PackImage &image = pack.images.at(i);
            if (m_transfer.bytes.at(i).isEmpty())
                continue;
            stickers::archive::Image entry;
            entry.shortcode = image.key;
            entry.body = image.body;
            entry.data = m_transfer.bytes.at(i);
            entry.usage = (image.isSticker && image.isEmoticon)
                              ? QStringLiteral("both")
                          : image.isEmoticon ? QStringLiteral("emoticon")
                                             : QStringLiteral("sticker");
            out.images.append(entry);
        }
    }
    int skipped = 0;
    const QByteArray zip = stickers::archive::buildArchive(out, &skipped);
    // Images fetched but not archivable (an SVG, a truncated file) count as
    // skipped; ones never fetched are the failures already counted.
    m_transfer.skipped += skipped;
    const int written = int(out.images.size()) - skipped;
    if (written <= 0) {
        finishTransfer(false, QStringLiteral("fetch_failed"));
        return;
    }
    QSaveFile file(m_transfer.destination);
    if (!file.open(QIODevice::WriteOnly) || file.write(zip) != zip.size()
        || !file.commit()) {
        finishTransfer(false, QStringLiteral("write_failed"));
        return;
    }
    m_transfer.added = written;
    finishTransfer(true, QString());
}

// -- Custom emoji in a message -----------------------------------------------

QVariantList StickerPackManager::emoticonsInHtml(const QString &html) const
{
    QVariantList out;
    if (html.isEmpty() || !html.contains(QLatin1String("data-mx-emoticon")))
        return out;
    static const QRegularExpression tagRe(
        QStringLiteral("<img\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression markerRe(
        QStringLiteral("(?:^|\\s)data-mx-emoticon(?:\\s|=|/|>|$)"),
        QRegularExpression::CaseInsensitiveOption);
    auto attr = [](const QString &tag, const QString &name) {
        const QRegularExpression re(
            QStringLiteral("\\b%1\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')").arg(name),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(tag);
        if (!m.hasMatch())
            return QString();
        return m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
    };
    QSet<QString> seen;
    auto it = tagRe.globalMatch(html);
    while (it.hasNext() && out.size() < 16) {
        const QString tag = it.next().captured(0);
        if (!markerRe.match(tag).hasMatch())
            continue;
        const QString src = attr(tag, QStringLiteral("src"));
        // Only a plain mxc URI: the same rule the pack writer enforces.
        if (!src.startsWith(QLatin1String("mxc://")) || src.size() > 512
            || src.contains(QLatin1Char(' ')) || src.contains(QLatin1Char('<'))
            || seen.contains(src))
            continue;
        seen.insert(src);
        QString label = attr(tag, QStringLiteral("alt"));
        if (label.isEmpty())
            label = attr(tag, QStringLiteral("title"));
        label.remove(QLatin1Char(':'));
        QString code = stickers::archive::sanitizeShortcode(label);
        if (code.isEmpty())
            code = QStringLiteral("emoji");
        QVariantMap row;
        row.insert(QStringLiteral("url"), src);
        row.insert(QStringLiteral("shortcode"), code);
        row.insert(QStringLiteral("body"), label.left(160));
        row.insert(QStringLiteral("saved"), isSaved(src));
        out.append(row);
    }
    return out;
}

void StickerPackManager::saveEmoticons(const QVariantList &emoticons)
{
    if (!m_client || !available() || m_transfer.active || m_saveOp != 0
        || m_editOp != 0)
        return;
    QList<BatchItem> items;
    int skipped = 0;
    for (const QVariant &value : emoticons) {
        const QVariantMap row = value.toMap();
        const QString url = row.value(QStringLiteral("url")).toString();
        if (!url.startsWith(QLatin1String("mxc://")))
            continue;
        if (isSaved(url)) {
            ++skipped;
            continue;
        }
        BatchItem item;
        item.url = url;
        item.shortcode = row.value(QStringLiteral("shortcode")).toString();
        item.body = row.value(QStringLiteral("body")).toString();
        item.usage = QStringLiteral("emoticon");
        items.append(item);
    }
    if (items.isEmpty() && skipped == 0)
        return;
    startBatch(QStringLiteral("emoji"), QString(), items, skipped);
}

void StickerPackManager::emitStateChanged()
{
    ++m_revision;
    Q_EMIT stateChanged();
}
