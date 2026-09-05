#pragma once

#include "stickers/StickerPack.h"
// Both models are Q_PROPERTY types, so a forward declaration is not enough:
// moc takes a QMetaType of the pointed-to class and that needs it COMPLETE.
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

// `app.stickers` — MSC2545 image packs, the sticker picker's controller, and
// the send/save paths.
//
// # What this owns and what it deliberately does not
//
// It owns C++-side POLICY (CLAUDE.md §5): which room's packs are in the
// snapshot, when a refresh is due, which pack is selected, which usage a
// surface is asking for, and how a save reconciles with the authoritative
// pack that follows it. It owns NO parsing: every pack was validated in
// `rust/src/stickers.rs`, and re-checking a url or a mimetype here would
// create a second rule to drift from the first.
//
// # A pack sticker is PUBLIC media, and the code says so out loud
//
// Sending a pack sticker sends the pack's own `mxc://` inside an `m.sticker`
// event. In an encrypted room the EVENT is encrypted by the SDK exactly like
// every other event — but the bitmap it points at is ordinary unencrypted
// media, because that is what a shared MSC2545 pack IS. This is inherent to
// the MSC and true of every client that implements it, not a Lightning
// choice. The consequence, stated rather than glossed: fetching the sticker
// tells the media repository which sticker was sent. Lightning therefore
// never presents a pack sticker as private content.
//
// Encrypting it per send would mean downloading the pack image and
// re-uploading it encrypted for every message — a second uploader beside the
// SDK attachment path, a different `m.sticker` shape (`file` rather than
// `url`), and a copy of the same bitmap in the media repo per send. Not done.
//
// # Refresh policy: nothing polls
//
// A refresh costs one global-account-data read plus a bounded `/state` read
// per room pack. It happens when a surface actually needs packs
// (`refreshIfStale()` from the picker on open), when the account's own pack
// was just written, and never on room navigation on its own — the room-list
// lesson in CLAUDE.md §16: an indicator that refreshes itself issues one
// request per room. Changing rooms only MARKS the snapshot stale.
class StickerPackManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("StickerPackManager is exposed via app.stickers")

    // Backend capability alone. QML hides the whole surface when false, so a
    // backend with no packs shows no sticker button rather than an empty
    // picker that looks broken.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // True once ANY snapshot has landed. "No packs yet" and "nothing loaded
    // yet" are different facts and the picker says different things.
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(StickerPackModel *packs READ packs CONSTANT)
    Q_PROPERTY(StickerImageModel *images READ images CONSTANT)
    // The selected pack's id, or "" before the first snapshot. Setting an id
    // that is not in the snapshot is REFUSED rather than silently mapped to
    // the first pack: a tab that quietly becomes a different tab is worse
    // than one that does not move.
    Q_PROPERTY(QString selectedPackId READ selectedPackId WRITE setSelectedPackId
                   NOTIFY selectedPackIdChanged)
    // "sticker" or "emoticon" — which usage the grid is showing.
    Q_PROPERTY(QString usage READ usage WRITE setUsage NOTIFY usageChanged)
    // How many packs in the snapshot hold at least one image of the current
    // usage. The picker's empty state distinguishes "you have no packs" from
    // "your packs hold no stickers".
    Q_PROPERTY(int usablePackCount READ usablePackCount NOTIFY stateChanged)
    // Bumped on every state change, so a binding on a Q_INVOKABLE
    // (isSaved / canSave) re-evaluates — the PresenceManager revision idiom.
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)
    // A save ("add to my stickers") is in flight. One at a time.
    Q_PROPERTY(bool saving READ saving NOTIFY stateChanged)
    // A pack edit is in flight. Separate from `saving` because the two are
    // different actions on different surfaces, and one disabling the other
    // would be a mystery rather than a safeguard.
    Q_PROPERTY(bool editing READ editing NOTIFY stateChanged)
    // A room-pack enable/disable is in flight. One at a time: two concurrent
    // read-modify-writes of the same account-data event would race, and the
    // second would be built on a copy the first had already superseded.
    Q_PROPERTY(bool togglingRoomPack READ togglingRoomPack NOTIFY stateChanged)

public:
    explicit StickerPackManager(QObject *parent = nullptr);
    ~StickerPackManager() override;

    void setClient(MatrixClient *client);
    // The room whose OWN packs belong in the snapshot. Marks the snapshot
    // stale; issues no request of its own.
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
    // Re-read only when there is no snapshot, or the snapshot was taken for
    // a different room. This is what a picker calls when it opens.
    Q_INVOKABLE void refreshIfStale();

    // Send one pack image as an `m.sticker`. `rootId` empty targets the room
    // timeline; non-empty is a real `m.thread` reply built by the SDK.
    // The destination is passed in and used immediately, so a room switch
    // cannot reroute a send.
    Q_INVOKABLE void sendToRoom(const QString &roomId,
                                const QVariantMap &image);
    Q_INVOKABLE void sendToThread(const QString &roomId, const QString &rootId,
                                  const QVariantMap &image);

    // Upload a LOCAL image file and add it to this account's own pack.
    //
    // THE ONLY WAY TO CREATE A PACK FROM NOTHING. saveSticker below needs an
    // mxc that already exists, so an account with no packs and nobody sending
    // it stickers could never get one. `fileUrl` is a file:// URL from a
    // picker; anything else is refused here rather than handed to the FFI.
    // `shortcode` may be empty — Rust derives and sanitizes one.
    Q_INVOKABLE void uploadSticker(const QUrl &fileUrl,
                                   const QString &shortcode);

    // "Add to my stickers" — write one image into this account's own
    // `im.ponies.user_emotes`. `shortcode` may be empty; Rust derives one.
    Q_INVOKABLE void saveSticker(const QString &url, const QString &body,
                                 const QString &mimetype, int width,
                                 int height, qint64 size);

    // "Add to this room's stickers" — write one image into the ACTIVE room's
    // `im.ponies.room_emotes` default pack (the empty state key).
    //
    // This is the one MSC2545 write that is ROOM STATE, so it is POWER-LEVEL
    // GATED: Rust asks the SDK for the room's own required level and answers
    // "forbidden" without sending anything when this account lacks it. It
    // reports on the SAME saveFinished signal as the account-pack path, so a
    // caller has one place to report from. Nothing is applied optimistically.
    Q_INVOKABLE void saveStickerToRoom(const QString &roomId,
                                       const QString &url,
                                       const QString &body,
                                       const QString &mimetype, int width,
                                       int height, qint64 size);
    // Whether "Add to this room's stickers" should be OFFERED for `roomId`.
    //
    // Unlike canSave(), this one DOES require a loaded snapshot — and only
    // answers true when a snapshot actually reported `canManage` for a pack
    // in that room. A room-state write the server will refuse is not worth
    // offering, and the offer would be a claim about permission that nothing
    // has checked. FALSE therefore means "not known to be permitted", which
    // is the only claim the data supports.
    Q_INVOKABLE bool canSaveToRoom(const QString &roomId,
                                   const QString &url) const;

    // Turn a ROOM pack on or off globally (`im.ponies.emote_rooms`).
    // `packId` is a pack id from the snapshot; a user pack is refused —
    // the account's own pack is global by definition and has nothing to
    // enable. NOT applied optimistically: the write completes, the
    // authoritative snapshot is re-read, so a refusal cannot leave a switch
    // showing a state the account does not have.
    // ── Pack management (MSC2545 CRUD, v0.9.0) ──────────────────────────
    //
    // `packId` is the same identifier the models expose. An account pack has
    // no room; a room pack carries both a room id and a state key, and every
    // one of these is refused unless `canManagePack(packId)` says this
    // account may write it.
    //
    // All four go through ONE op slot (`m_editOp`), so the UI can disable
    // itself on `editing` and a second click cannot race the first.
    Q_INVOKABLE void removeImageFromPack(const QString &packId,
                                         const QString &shortcode);
    Q_INVOKABLE void renameImageInPack(const QString &packId,
                                       const QString &from, const QString &to);
    Q_INVOKABLE void renamePack(const QString &packId, const QString &name);
    Q_INVOKABLE void deletePack(const QString &packId);
    /// Whether this account may EDIT the named pack. An account pack is
    /// always yes; a room pack asks the snapshot's own recorded permission,
    /// and the ABSENCE of that claim is not permission. Mirrors canSaveToRoom.
    Q_INVOKABLE bool canManagePack(const QString &packId) const;
    /// Everything a management surface needs about one pack, in one call:
    /// its id, its display name, and whether this account may write it.
    /// Empty map when the id is unknown. One call rather than a row lookup
    /// plus `get(row)` plus `canManagePack`, because that arrangement puts
    /// the permission rule half in QML.
    Q_INVOKABLE QVariantMap packInfo(const QString &packId) const;
    Q_INVOKABLE void setRoomPackEnabled(const QString &packId, bool enabled);
    // A room-pack enable/disable is in flight.
    bool togglingRoomPack() const { return m_roomsOp != 0; }

    // True when the account's own pack ALREADY holds this exact mxc.
    // Answers only from a loaded snapshot — false also means "not known to
    // be saved", which is why it never gates the menu item (see canSave).
    Q_INVOKABLE bool isSaved(const QString &url) const;
    // Whether "Add to my stickers" should be OFFERED. Deliberately does NOT
    // require a loaded snapshot: gating on data nobody has fetched would
    // grey the action out because nothing LOOKED, and the duplicate case is
    // refused authoritatively in Rust anyway (category "duplicate"). It
    // requires a backend that has packs, a plain mxc source — an ENCRYPTED
    // sticker has an EncryptedFile and no url, so it structurally cannot go
    // in a pack — and no save already in flight.
    Q_INVOKABLE bool canSave(const QString &url) const;

    // Custom-emoji lookup for the composer. `prefix` is matched
    // case-insensitively against the shortcode; an EMPTY prefix returns the
    // first `limit` emoticons, which is what an empty `:` completion shows.
    // Rows are the same map shape the image model exposes, with `packName`
    // added so a completion row can say where a shortcode came from.
    Q_INVOKABLE QVariantList findEmoticons(const QString &prefix,
                                           int limit) const;
    // The shortcode a known emoticon mxc belongs to, or "" — for LABELLING a
    // custom-emoji reaction. Often "" in practice: packs are only read when
    // a surface asks for them, so a reaction can legitimately arrive before
    // any pack is loaded, and the caller must have a name for that case
    // rather than reading a raw mxc aloud.
    Q_INVOKABLE QString shortcodeForUrl(const QString &url) const;
    // One emoticon by exact shortcode, or an empty map. First match wins:
    // the account's own pack is first in the snapshot, so a user's own
    // shortcode beats a room pack's with the same name.
    Q_INVOKABLE QVariantMap emoticon(const QString &shortcode) const;

    // Test seam: apply a snapshot without a backend. Used by
    // tests/StickerPackManagerTest.cpp; production always goes through the
    // client's signal.
    void applySnapshotForTest(const QString &roomId, bool roomCanManage,
                              const QVariantList &packs);

Q_SIGNALS:
    void availableChanged();
    void stateChanged();
    void selectedPackIdChanged();
    void usageChanged();
    // A save finished. `category` is empty on success; "duplicate",
    // "pack_full", "forbidden" (the room-state write only) or a coarse
    // room-error class otherwise. `shortcode` is the name the image actually
    // got, which may carry a numeric suffix. `scope` is "account" or "room" —
    // both writes report here, and a notice that cannot tell them apart would
    // tell the user their sticker went somewhere it did not.
    void saveFinished(bool ok, const QString &category,
                      const QString &shortcode, const QString &scope);
    /// A pack edit finished. `category` is empty on success and carries the
    /// bridge's own class otherwise ("not_found", "shortcode_taken",
    /// "forbidden", ...). `shortcode` is the code a rename actually applied,
    /// which can differ from what was typed because it is sanitized.
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
    /// The one dispatcher behind all four editing verbs.
    void editPack(const QString &packId, const QString &action,
                  const QString &argA, const QString &argB);
    void onRoomsSet(quint64 opId, bool ok, const QString &category,
                    const QString &roomId, const QString &stateKey,
                    bool enabled);
    void onLoggedOut();
    void applySnapshot(const QString &roomId, bool roomCanManage,
                       const QVariantList &packs);
    // Re-narrow the grid to the selected pack and the current usage.
    void rebuildImages();
    void emitStateChanged();

    MatrixClient *m_client = nullptr;
    StickerPackModel *m_packs = nullptr;
    StickerImageModel *m_images = nullptr;

    QString m_activeRoomId;
    // The room the CURRENT snapshot was taken for. Compared against
    // m_activeRoomId to decide staleness.
    QString m_snapshotRoomId;
    bool m_loaded = false;
    bool m_stale = true;
    // Whether the SNAPSHOT's room reported that this account may write its
    // `im.ponies.room_emotes`. Scoped to m_snapshotRoomId: a permission
    // learned about one room says nothing about another.
    bool m_snapshotRoomCanManage = false;

    QString m_selectedPackId;
    QString m_usage = QStringLiteral("sticker");

    quint64 m_nextOpId = 1;
    quint64 m_fetchOp = 0;
    quint64 m_saveOp = 0;
    quint64 m_editOp = 0;
    // What the in-flight save targets: "account" or "room". Both paths share
    // one slot and one report, so the report has to carry which it was.
    QString m_saveScope;
    quint64 m_roomsOp = 0;
    // A refresh became due while one was in flight. The in-flight read
    // predates whatever made it due (our own completed save), so its answer
    // is stale by construction — the same reasoning as
    // PinnedMessagesController's owed refresh.
    bool m_refreshOwed = false;

    // Every mxc in the account's own pack, for isSaved().
    QSet<QString> m_savedUrls;
    int m_revision = 0;
};
