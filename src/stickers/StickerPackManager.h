#pragma once

#include "stickers/StickerPack.h"
// Complete types: moc needs the QMetaType of Q_PROPERTY pointee classes.
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackModel.h"

#include <QObject>

class QTimer;
#include <QUrl>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// `app.stickers`: MSC2545 image packs, the sticker picker's controller, and
// the send/save paths.
//
// Owns C++-side policy only (which packs are in the snapshot, refresh timing,
// selection, usage, save reconciliation). Packs are validated in
// rust/src/stickers.rs and not re-checked here.
//
// A pack sticker is public media: in an encrypted room the `m.sticker` event is
// encrypted, but the pack bitmap it points to is ordinary unencrypted media,
// as MSC2545 defines it. Fetching it reveals to the media repository which
// sticker was sent, so pack stickers are never presented as private content.
// Re-uploading an encrypted copy per send is deliberately not done.
//
// Nothing polls. A refresh (one account-data read plus a bounded `/state` read
// per room pack) happens when the picker opens (refreshIfStale()) or after
// the account's own pack was written. Room navigation only marks the snapshot
// stale.
class StickerPackManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("StickerPackManager is exposed via app.stickers")

    // Backend capability. When false QML hides the sticker button entirely.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // True once any snapshot has landed; "no packs" and "not loaded" differ.
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(StickerPackModel *packs READ packs CONSTANT)
    Q_PROPERTY(StickerImageModel *images READ images CONSTANT)
    // Selected pack id, or "" before the first snapshot. An id not in the
    // snapshot is refused rather than mapped to another pack.
    Q_PROPERTY(QString selectedPackId READ selectedPackId WRITE setSelectedPackId
                   NOTIFY selectedPackIdChanged)
    // "sticker" or "emoticon" — which usage the grid is showing.
    Q_PROPERTY(QString usage READ usage WRITE setUsage NOTIFY usageChanged)
    // Packs holding at least one image of the current usage, so the empty state
    // can tell "no packs" from "no stickers in your packs".
    Q_PROPERTY(int usablePackCount READ usablePackCount NOTIFY stateChanged)
    // Bumped on every state change so bindings on isSaved()/canSave()
    // re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)
    // A save ("add to my stickers") is in flight. One at a time.
    Q_PROPERTY(bool saving READ saving NOTIFY stateChanged)
    // A pack edit is in flight; separate from `saving`, which is a different
    // action on a different surface.
    Q_PROPERTY(bool editing READ editing NOTIFY stateChanged)
    // A room-pack enable/disable is in flight. One at a time: concurrent
    // read-modify-writes of the same account-data event would race.
    Q_PROPERTY(bool togglingRoomPack READ togglingRoomPack NOTIFY stateChanged)

public:
    explicit StickerPackManager(QObject *parent = nullptr);
    ~StickerPackManager() override;

    void setClient(MatrixClient *client);
    // The room whose own packs belong in the snapshot. Marks the snapshot
    // stale; issues no request.
    void setActiveRoomId(const QString &roomId);

    bool available() const;
    bool loading() const { return m_fetchOp != 0; }
    bool loaded() const { return m_loaded; }
    bool saving() const { return m_saveOp != 0; }
    bool editing() const { return m_editOp != 0; }
    StickerPackModel *packs() const { return m_packs; }
    StickerImageModel *images() const { return m_images; }
    QString selectedPackId() const { return m_selectedPackId; }
    void setSelectedPackId(const QString &id);
    QString usage() const { return m_usage; }
    void setUsage(const QString &usage);
    int usablePackCount() const;
    int revision() const { return m_revision; }

    // Unconditional re-read.
    Q_INVOKABLE void refresh();
    // Re-read only when there is no snapshot or it is for another room. Called
    // when a picker opens.
    Q_INVOKABLE void refreshIfStale();

    // Sends one pack image as `m.sticker`. Empty `rootId` targets the room
    // timeline, otherwise an SDK-built `m.thread` reply. The destination is
    // used immediately, so a room switch cannot reroute a send.
    Q_INVOKABLE void sendToRoom(const QString &roomId,
                                const QVariantMap &image);
    Q_INVOKABLE void sendToThread(const QString &roomId, const QString &rootId,
                                  const QVariantMap &image);

    // Uploads a local image and adds it to the account's own pack; the only way
    // to create a pack from nothing. `fileUrl` must be a file:// URL.
    // `shortcode` may be empty; Rust derives and sanitizes one.
    Q_INVOKABLE void uploadSticker(const QUrl &fileUrl,
                                   const QString &shortcode);

    // Adds one image to the account's `im.ponies.user_emotes`. `shortcode` may
    // be empty.
    Q_INVOKABLE void saveSticker(const QString &url, const QString &body,
                                 const QString &mimetype, int width,
                                 int height, qint64 size);

    // Adds one image to the active room's default `im.ponies.room_emotes` pack.
    // This is room state, so it is power-level gated: Rust answers "forbidden"
    // without sending when the account lacks the level. Reports on saveFinished
    // like the account path; nothing is applied optimistically.
    Q_INVOKABLE void saveStickerToRoom(const QString &roomId,
                                       const QString &url,
                                       const QString &body,
                                       const QString &mimetype, int width,
                                       int height, qint64 size);
    // Whether to offer "Add to this room's stickers". Requires a loaded
    // snapshot that reported `canManage` for a pack in that room; false means
    // "not known to be permitted".
    Q_INVOKABLE bool canSaveToRoom(const QString &roomId,
                                   const QString &url) const;

    // Pack management (MSC2545). An account pack has no room; a room pack has a
    // room id and state key, and every edit is refused unless canManagePack()
    // allows it. All edits share one op slot (`m_editOp`), so the UI can
    // disable itself on `editing`.
    Q_INVOKABLE void removeImageFromPack(const QString &packId,
                                         const QString &shortcode);
    Q_INVOKABLE void renameImageInPack(const QString &packId,
                                       const QString &from, const QString &to);
    Q_INVOKABLE void renamePack(const QString &packId, const QString &name);
    Q_INVOKABLE void deletePack(const QString &packId);
    /// Whether this account may edit the pack. Account packs always; room packs
    /// only when the snapshot recorded permission.
    Q_INVOKABLE bool canManagePack(const QString &packId) const;
    /// Id, display name and writability of one pack, or an empty map. One call
    /// so the permission rule does not end up half in QML.
    Q_INVOKABLE QVariantMap packInfo(const QString &packId) const;
    // Turns a room pack on or off globally (`im.ponies.emote_rooms`). User
    // packs are refused. Not optimistic: the snapshot is re-read after the
    // write.
    Q_INVOKABLE void setRoomPackEnabled(const QString &packId, bool enabled);
    // A room-pack enable/disable is in flight.
    bool togglingRoomPack() const { return m_roomsOp != 0; }

    // True when the account's own pack already holds this mxc. False may mean
    // "not known", so it never gates the menu item.
    Q_INVOKABLE bool isSaved(const QString &url) const;
    // Whether to offer "Add to my stickers". Does not require a snapshot; Rust
    // refuses duplicates authoritatively ("duplicate"). Requires a backend with
    // packs, a plain mxc source (encrypted stickers have no url) and no save in
    // flight.
    Q_INVOKABLE bool canSave(const QString &url) const;

    // Custom-emoji lookup for the composer, case-insensitive on the shortcode.
    // An empty prefix returns the first `limit` emoticons. Rows match the image
    // model's shape plus `packName`.
    Q_INVOKABLE QVariantList findEmoticons(const QString &prefix,
                                           int limit) const;
    // Shortcode for a known emoticon mxc, or "" (packs may not be loaded yet
    // when a reaction arrives), for labelling custom-emoji reactions.
    Q_INVOKABLE QString shortcodeForUrl(const QString &url) const;
    // One emoticon by exact shortcode, or an empty map. The account's own pack
    // comes first, so its shortcodes win.
    Q_INVOKABLE QVariantMap emoticon(const QString &shortcode) const;

    // Test seam: apply a snapshot without a backend.
    void applySnapshotForTest(const QString &roomId, bool roomCanManage,
                              const QVariantList &packs);

Q_SIGNALS:
    void availableChanged();
    void stateChanged();
    void selectedPackIdChanged();
    void usageChanged();
    // `category` is empty on success, otherwise "duplicate", "pack_full",
    // "forbidden" (room writes) or a coarse error class. `shortcode` is the
    // name actually used (may carry a numeric suffix). `scope` is "account" or
    // "room".
    void saveFinished(bool ok, const QString &category,
                      const QString &shortcode, const QString &scope);
    /// `category` is empty on success, otherwise the bridge's class
    /// ("not_found", "shortcode_taken", "forbidden", ...). `shortcode` is the
    /// sanitized code a rename applied.
    void editFinished(bool ok, const QString &category,
                      const QString &shortcode);
    // A room-pack enable/disable finished. `category` is empty on success.
    void roomPackToggleFinished(bool ok, const QString &category,
                                bool enabled);

private:
    QTimer *m_activeRoomFetch = nullptr;
    void onPacksReceived(quint64 opId, const QString &roomId,
                         bool roomCanManage, const QVariantList &packs);
    void onSaveFinished(quint64 opId, bool ok, const QString &category,
                        const QString &shortcode);
    void onEditFinished(quint64 opId, bool ok, const QString &category,
                        const QString &shortcode);
    /// Dispatcher behind the four editing verbs.
    void editPack(const QString &packId, const QString &action,
                  const QString &argA, const QString &argB);
    void onRoomsSet(quint64 opId, bool ok, const QString &category,
                    const QString &roomId, const QString &stateKey,
                    bool enabled);
    void onLoggedOut();
    void applySnapshot(const QString &roomId, bool roomCanManage,
                       const QVariantList &packs);
    // Narrows the grid to the selected pack and usage.
    void rebuildImages();
    void emitStateChanged();

    MatrixClient *m_client = nullptr;
    StickerPackModel *m_packs = nullptr;
    StickerImageModel *m_images = nullptr;

    QString m_activeRoomId;
    // The room the current snapshot was taken for; compared for staleness.
    QString m_snapshotRoomId;
    bool m_loaded = false;
    bool m_stale = true;
    // Whether the snapshot's room allows this account to write its
    // `im.ponies.room_emotes`. Valid for m_snapshotRoomId only.
    bool m_snapshotRoomCanManage = false;

    QString m_selectedPackId;
    QString m_usage = QStringLiteral("sticker");

    quint64 m_nextOpId = 1;
    quint64 m_fetchOp = 0;
    quint64 m_saveOp = 0;
    quint64 m_editOp = 0;
    // "account" or "room": both saves share one slot and one report.
    QString m_saveScope;
    quint64 m_roomsOp = 0;
    // A refresh became due during one in flight; that read predates our own
    // save and is stale.
    bool m_refreshOwed = false;

    // Every mxc in the account's own pack, for isSaved().
    QSet<QString> m_savedUrls;
    int m_revision = 0;
};
