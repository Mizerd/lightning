#pragma once

#include "backdrop/BackdropMath.h"

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class MatrixClient;
class MediaBridge;
class QTemporaryDir;
class SettingsManager;
class SpaceManager;
class StagedImageStore;

// Chat backgrounds (`app.backdrops`): which picture a room shows behind its
// timeline, and the "Depth" surface setting. The protocol half is
// rust/src/backdrop.rs; the rules are backdrop::resolve / planScrim
// (BackdropMath.h); this class owns the state between them.
//
// Three sources, resolved per room (personal room > shared room > shared
// Space > personal default > none):
//
//   * SHARED: a state event in the room or one of its Spaces, read through
//     MatrixClient and cached per session. Asked once per room per session and
//     refreshed on open (rate-limited), because sliding sync never delivers
//     this state type as state. A change that arrives in sync as a timeline
//     event (MatrixClient::roomBackgroundChanged) re-reads a scope already
//     shown at once, so an open room, and every room inheriting a Space's
//     picture, follows it live. Answers carry the op id they were asked with;
//     one that is not in flight (a previous account, a superseded read) is
//     dropped.
//   * PERSONAL: pictures only this account sees, kept as Lightning-encoded
//     JPEG/PNG files under <accountRoot>/backgrounds and served to QML from
//     memory through the staged-image store, never as a file:// URL.
//     With "Keep my backgrounds on my homeserver" on (the default; the switch
//     is ACCOUNT-WIDE, in the global account data, and read before any
//     upload), each one is also kept in account data as an SDK-encrypted
//     upload (rust/src/bgsync.rs), so it follows the account to every device
//     and survives a sign-out, which deletes every local copy:
//       - a local record carries `remote`, the id of the server copy it
//         mirrors; one without it is local-only;
//       - a local-only picture is uploaded only into an EMPTY server slot
//         (nothing stored, or cleared), and pictures chosen earlier only
//         after a one-time notice; against a DIFFERENT server picture both
//         are kept as a conflict the user resolves (`conflict`);
//       - a server copy whose id differs from the local mark is downloaded
//         (another device changed it; two at a time, never past the
//         per-room bound, a failed id only again after Retry); a server copy
//         that was CLEARED removes a local record that mirrored it; one that
//         is absent or INVALID changes nothing;
//       - a removal or presentation change the server has not taken yet is
//         OWED (`pending` in the store), replayed on the next start, and the
//         server's old copy is not followed meanwhile; a presentation change
//         names the picture it is for and yields to a newer one;
//       - changes arrive live (MatrixClient::personalBackgroundChanged) and
//         are re-read, never trusted from the poke.
//   * none.
//
// Pictures reach QML only through the media bridge (shared) or the staged
// store (personal and previews). Every picture is MEASURED here from its own
// pixels (backdrop::measure) so the scrim QML draws keeps the timeline's ink
// readable; declared metadata is never trusted for that.
class ChatBackdropController : public QObject
{
    Q_OBJECT

    // The backend can carry shared backgrounds (custom room state).
    Q_PROPERTY(bool sharedAvailable READ sharedAvailable NOTIFY availableChanged)
    // Bumped whenever anything a backdropFor()/sharedFor() binding reads
    // changes; those are method calls, so bindings read this as a dependency.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // Settings -> Appearance: "Show backgrounds set by others" (default on).
    Q_PROPERTY(bool showShared READ showShared WRITE setShowShared
                   NOTIFY settingsChanged)
    // Settings -> Appearance: surface depth. 0 flat, 1 depth.
    Q_PROPERTY(int surfaceDepth READ surfaceDepth WRITE setSurfaceDepth
                   NOTIFY settingsChanged)
    // A shared write is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Category of the last failure ("" when none): "forbidden",
    // "rate_limited", "unsupported_image", "unreadable", "too_large",
    // "undecodable", "no_picture", "failed", ...
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // The picture picked in an editor and not yet applied:
    // { imageUrl, width, height, color, statsKey } or {} when none.
    Q_PROPERTY(QVariantMap prepared READ prepared NOTIFY preparedChanged)
    // The backend can keep personal backgrounds on the homeserver.
    Q_PROPERTY(bool syncAvailable READ syncAvailable NOTIFY availableChanged)
    // Settings -> "Keep my backgrounds on my homeserver" (default on). The
    // switch is ACCOUNT-WIDE (global account data `enabled`); this is this
    // device's mirror of it. Changed through setSyncEnabled(), which can also
    // remove the server copies.
    Q_PROPERTY(bool syncEnabled READ syncEnabled NOTIFY syncChanged)
    // { state: "unavailable"|"off"|"idle"|"working"|"failed"|"removing",
    //   pending, uploading, downloading, failed (counts), error (category of
    //   the last failure), removal: ""|"ok"|"partial"|"failed", removed,
    //   removeFailed (failed + skipped), switchError }
    Q_PROPERTY(QVariantMap syncStatus READ syncStatus NOTIFY syncChanged)
    // How many of this account's pictures (and owed server changes) a
    // sign-out now would lose: the sign-out dialog warns when non-zero.
    Q_PROPERTY(int syncUnsaved READ syncUnsaved NOTIFY syncChanged)
    // Removals the homeserver has not taken yet: their copies would come
    // back at the next sign-in. Worded separately in the sign-out warning.
    Q_PROPERTY(int syncOwedRemovals READ syncOwedRemovals NOTIFY syncChanged)
    // Local-only pictures that differ from the server's copy, kept both ways
    // until the user picks: [{ scope, name }] ("" scope = every room).
    Q_PROPERTY(QVariantList syncConflicts READ syncConflicts NOTIFY syncChanged)
    // Pictures chosen before sync existed are waiting for the one-time
    // notice (BackgroundSyncPrompt) before they are uploaded.
    Q_PROPERTY(bool migrationNoticeNeeded READ migrationNoticeNeeded
                   NOTIFY syncChanged)

public:
    explicit ChatBackdropController(QObject *parent = nullptr);
    ~ChatBackdropController() override;

    void setClient(MatrixClient *client);
    void setMediaBridge(MediaBridge *bridge);
    void setSettings(SettingsManager *settings);
    void setSpaces(SpaceManager *spaces);
    void setStagedImages(StagedImageStore *store);

    bool sharedAvailable() const;
    int revision() const { return m_revision; }
    bool showShared() const;
    void setShowShared(bool show);
    int surfaceDepth() const;
    void setSurfaceDepth(int depth);
    bool busy() const { return m_pendingWrite != 0; }
    QString lastError() const { return m_lastError; }
    QVariantMap prepared() const;

    // ---- what a room shows ---------------------------------------------

    // The resolved background for `roomId`:
    //   { source: "personal-room"|"room"|"space"|"personal-default"|"none",
    //     scopeId, kind: "mxc"|"staged"|"none", mxc (kind mxc), imageUrl
    //     (kind staged; "" for mxc, which QML resolves through
    //     app.mediaBridge.wideImageSource), statsKey, color, dim, blur, tint,
    //     fit, align }
    // Pure read: asks nothing. Call requestRoom() when a room opens.
    Q_INVOKABLE QVariantMap backdropFor(const QString &roomId) const;
    // Reads the room's and its Spaces' shared backgrounds once per session,
    // and again on open after kRefreshIntervalMs.
    Q_INVOKABLE void requestRoom(const QString &roomId);
    // Asks again regardless (editors, after a permission change).
    Q_INVOKABLE void refreshScope(const QString &scopeId);

    // ---- the scrim -----------------------------------------------------

    // The scrim plan for a picture (statsKey from backdropFor/prepared) on
    // the live palette: { color, floor, alpha, tint, feasible, measured }.
    // `alpha` is what to draw for `dim`. Cached per inputs.
    Q_INVOKABLE QVariantMap scrimFor(const QString &statsKey,
                                     const QColor &ground,
                                     const QColor &inkPrimary,
                                     const QColor &inkSecondary,
                                     const QColor &inkMuted, double tint,
                                     double dim);
    // The "Depth" stops for a surface colour, top to bottom.
    Q_INVOKABLE QStringList depthStops(const QColor &base, bool darkTheme) const;

    // ---- editors: shared -------------------------------------------------

    // Canonical content of a room's or Space's own shared background, or {}.
    Q_INVOKABLE QVariantMap sharedFor(const QString &scopeId) const;
    // Whether this account may change it; false until the room answered.
    Q_INVOKABLE bool canSetShared(const QString &scopeId) const;
    // The scope's event uses a schema newer than this build reads.
    Q_INVOKABLE bool sharedUnsupported(const QString &scopeId) const;
    // Whether a room is end-to-end encrypted (for the cleartext-state notice).
    // False when unknown, and the notice is then shown anyway by QML's own
    // `!roomEncryptionKnown` check.
    Q_INVOKABLE bool roomEncrypted(const QString &roomId) const;
    Q_INVOKABLE bool roomEncryptionKnown(const QString &roomId) const;
    // Applies the prepared picture (uploaded) with `presentation`, or, with
    // nothing prepared, re-sends the current picture with new presentation.
    Q_INVOKABLE void setShared(const QString &scopeId,
                               const QVariantMap &presentation);
    Q_INVOKABLE void clearShared(const QString &scopeId);

    // ---- editors: personal -----------------------------------------------

    // This account's own picture for a room ("" = the default for every
    // room): { imageUrl, statsKey, color, dim, blur, tint, fit, align } or {}.
    Q_INVOKABLE QVariantMap personalFor(const QString &roomId) const;
    // Stores the prepared picture (or, with none prepared, new presentation
    // for the existing one). Returns false with lastError set on failure.
    Q_INVOKABLE bool setPersonal(const QString &roomId,
                                 const QVariantMap &presentation);
    Q_INVOKABLE void clearPersonal(const QString &roomId);
    // Per-room "Hide this room's background" (shared levels only).
    Q_INVOKABLE bool roomHidden(const QString &roomId) const;
    Q_INVOKABLE void setRoomHidden(const QString &roomId, bool hidden);

    // ---- picking a picture -------------------------------------------------

    // Reads a user-picked file (file:// URL or path), refuses anything but
    // the five raster formats by magic bytes (SVG included), decodes the
    // FIRST frame only, scales to <= kMaxEdge and re-encodes it (JPEG, or PNG
    // when it has transparency), measures it and stages a preview. Returns
    // { ok, error, imageUrl, width, height, color, statsKey }.
    //
    // An SVG is converted to a PNG first, in a helper process ("rasterize on
    // send", media/SvgRasterJob.h): the call then returns { ok: false,
    // pending: true } and the same map arrives later on imagePrepared(). The
    // picture that is previewed, stored and uploaded is that PNG. A refusal
    // sets lastError to "svg_<reason>"; svgMessage() words it.
    Q_INVOKABLE QVariantMap prepareImage(const QUrl &fileUrl);
    Q_INVOKABLE QString svgMessage(const QString &reason) const;
    Q_INVOKABLE void discardPrepared();
    // "Use as my background here" on an image message: fetches the full
    // payload through the media bridge and stores it as this account's own
    // picture for the room, through the same sniff/re-encode as a picked
    // file. Refused (lastError "encrypted_room") unless the room is KNOWN to
    // be unencrypted: keeping an encrypted room's picture as a file would be
    // decrypted media at rest (CLAUDE.md §6).
    Q_INVOKABLE void useMessageImage(const QString &roomId,
                                     const QString &mediaKey);

    // ---- personal backgrounds on the homeserver --------------------------

    bool syncAvailable() const;
    bool syncEnabled() const;
    QVariantMap syncStatus() const;
    // On: uploads every local-only picture and follows the server from now
    // on. Off: local-only from now on; with `removeServerCopies` the account
    // data is cleared too (the uploaded, encrypted media cannot be deleted
    // from the media repository; Settings says so).
    Q_INVOKABLE void setSyncEnabled(bool on, bool removeServerCopies = false);
    // Retries every failed upload, change and download, and reads again.
    Q_INVOKABLE void retrySync();
    int syncUnsaved() const;
    int syncOwedRemovals() const;
    QVariantList syncConflicts() const;
    // "Remove server copies" again after a partial or failed pass, without
    // turning sync on (which would upload this device's pictures).
    Q_INVOKABLE void retryRemoval();
    bool migrationNoticeNeeded() const;
    // The one-time notice: OK uploads the earlier pictures; "this device
    // only" turns the account-wide switch off.
    Q_INVOKABLE void acknowledgeMigration(bool keepOnThisDeviceOnly);
    // A conflict: keep this device's picture (uploaded over the server's) or
    // take the synced one (downloaded over this one).
    Q_INVOKABLE void resolveConflict(const QString &scope, bool useThisDevice);

    // Account switch / sign-out: forget everything session-scoped.
    void clearSession();

    // ---- bounds ------------------------------------------------------------

    static constexpr int kMaxEdge = 2560;
    static constexpr qint64 kMaxSourceBytes = 64LL * 1024 * 1024;
    static constexpr int kMaxDecodeEdge = 4096;
    static constexpr int kMaxCachedScopes = 256;
    static constexpr int kMaxPersonalRooms = 64;
    static constexpr int kMaxStaged = 4;
    static constexpr int kMaxStats = 32;
    static constexpr qint64 kRefreshIntervalMs = 60 * 1000;
    // Encrypted downloads in flight at once.
    static constexpr int kMaxDownloads = 2;
    // Single-scope reads in flight at once; more wait their turn (a burst of
    // change notices must not become a burst of requests).
    static constexpr int kMaxSingleReads = 6;
    // Above the backend's own bounds: two 15 s reads, a 120 s upload, a
    // switch read and the PUT.
    static constexpr int kWriteTimeoutMs = 200 * 1000;
    static constexpr int kSwitchTimeoutMs = 45 * 1000;

    // ---- test seams ----------------------------------------------------------

    // Shorter watchdogs for a write / switch answer that never comes.
    void setSyncTimeoutsForTesting(int writeMs, int switchMs)
    {
        m_writeTimeoutMs = writeMs;
        m_switchTimeoutMs = switchMs;
    }
    // Where personal pictures live; defaults to <accountRoot>/backgrounds.
    void setStorageDirForTesting(const QString &dir) { m_storageOverride = dir; }
    // Encodes `image` as prepareImage would, without a file.
    QVariantMap prepareImageForTesting(const QImage &image);
    // Runs the space chain for a room against the client's rooms().
    QStringList spaceChainFor(const QString &roomId) const;

Q_SIGNALS:
    void availableChanged();
    void revisionChanged();
    void settingsChanged();
    void busyChanged();
    void lastErrorChanged();
    void preparedChanged();
    void syncChanged();
    // The outcome of a prepareImage() that returned `pending`. Not emitted when
    // discardPrepared() or another prepareImage() ran in between.
    void imagePrepared(const QVariantMap &result);

private:
    struct Shared {
        QVariantMap content;      // canonical, {} = none
        bool canSet = false;
        bool unsupported = false;
        bool known = false;       // an answer arrived this session
    };
    struct Prepared {
        QByteArray bytes;
        QString mime;
        QString suffix;
        int width = 0;
        int height = 0;
        QString hash;             // sha256 hex of `bytes`
        backdrop::ImageStats stats;
        QString imageUrl;         // staged preview
    };

    void handleReceived(quint64 opId, const QString &roomId,
                        const QVariantMap &content, bool canSet,
                        bool unsupportedVersion);
    void handleSet(quint64 opId, const QString &roomId, bool ok,
                   const QVariantMap &content, const QString &category);
    // MatrixClient::roomBackgroundChanged: re-reads a scope this session
    // shows (or has asked about), bypassing the open-time rate limit.
    void handleChanged(const QString &scopeId);
    void handleMediaCached(const QString &cacheKey);
    void handleMediaBytes(const QString &mediaKey, bool ok,
                          const QByteArray &bytes, const QString &category);
    void onSessionChanged();
    void bump();
    void setLastError(const QString &error);

    QVariantMap prepareFromBytes(const QByteArray &bytes);
    // Sniff, decode the first frame, scale and re-encode `bytes` (what every
    // picture goes through); `error` is the category on failure.
    bool encodePicture(const QByteArray &bytes, Prepared &out,
                       QString &error) const;
    // Writes an encoded picture into the backgrounds directory (once per
    // content) and returns its stored name, or "" with lastError set.
    QString storeEncoded(const Prepared &picture);
    QVariantMap describeRecord(const QVariantMap &record, bool shared) const;
    QVariantMap personalStore() const;
    void savePersonalStore(const QVariantMap &store);
    QString storageDir() const;
    QString stagedUrlForFile(const QString &fileName) const;
    void measureMxc(const QString &mxc);
    void ensureMeasuredFile(const QString &fileName) const;
    void dropOrphanFiles(const QVariantMap &store);

    // ---- sync ----
    // The FFI's write modes. Upload doubles as "nothing owed" in setPending.
    enum class SyncMode { Upload = 0, Presentation = 1, Clear = 2, SetEnabled = 3 };
    struct SyncOp {
        QString scope;
        SyncMode mode = SyncMode::Upload;
        QString file;   // the record's file when the write was sent
        // A migration upload: only where the server holds nothing (a
        // different picture there becomes a conflict, never overwritten).
        bool intoEmpty = false;
    };
    struct RemoteEntry {
        QString state;            // "present" | "cleared" | "unsupported"
        QString id;
        QVariantMap presentation; // wire form: integer percentages
        QString color;
    };
    bool syncWatching() const;     // available and signed in: follow the switch
    bool syncActive() const;       // ...and this device's mirror is on
    bool syncWritable() const;     // ...and the server's switch is known on
    QString switchPending() const;
    void setSwitchPending(const QString &value);
    void sendSwitch(bool on);
    void handleSwitchWritten(bool ok, const QVariantMap &entry,
                             const QString &category);
    void applyServerSwitch(bool on, bool fromOwnWrite = false, bool restart = true);
    void startSync();
    void syncRead(const QString &scope);
    void requestDownload(const QString &scope, const QString &id,
                         bool replaceConflict, bool compare = false);
    // The synced picture against this device's: the same bytes (or the
    // bytes this one was made from) adopt it with no question; alike pixels
    // only pre-select the synced one on the conflict card.
    enum class SameAs { Different, LooksSame, Identical };
    SameAs sameAsLocal(const QVariantMap &record, const QByteArray &raw,
                       const Prepared &synced) const;
    static constexpr int kLookEdge = 512;
    void pumpDownloads();
    void setPending(const QString &scope, SyncMode mode);
    QString pendingFor(const QString &scope) const;
    void enqueueSync(const QString &scope, SyncMode mode, bool intoEmpty = false);
    void processSyncQueue();
    bool syncBusyFor(const QString &scope) const;
    void reconcile(const QString &scope, const RemoteEntry &entry);
    void migrateLocalOnly();
    QStringList migrationCandidates() const;   // what the notice waits for
    void resetSyncState();
    QVariantMap recordFor(const QVariantMap &store, const QString &scope) const;
    void putRecord(QVariantMap &store, const QString &scope,
                   const QVariantMap &record) const;
    QStringList knownSyncedRooms() const;
    static RemoteEntry remoteFromMap(const QVariantMap &entry);
    void handleSyncRead(quint64 opId, const QString &scope,
                        const QVariantList &entries, const QStringList &failed,
                        int enabled);
    void handleSyncDownloaded(quint64 opId, const QString &scope, bool ok,
                              const QVariantMap &entry, const QByteArray &bytes,
                              const QString &category);
    void handleSyncWritten(quint64 opId, const QString &scope, bool ok,
                           const QVariantMap &entry, const QString &category);
    void handleSyncCleared(quint64 opId, bool ok, int cleared, int failed,
                           int skipped, bool switchedOff);
    void makeMarksDormant();
    void writeTimedOut();
    void switchTimedOut();
    void handleSyncChanged(const QString &scope);
    void syncFailed(const QString &scope, SyncMode mode, const QString &category,
                    bool intoEmpty = false);
    void startClearAll();
    bool isJoinedRoom(const QString &roomId) const;

    QPointer<MatrixClient> m_client;
    QPointer<MediaBridge> m_bridge;
    QPointer<SettingsManager> m_settings;
    QPointer<SpaceManager> m_spaces;
    QPointer<StagedImageStore> m_staged;

    QHash<QString, Shared> m_shared;          // scopeId -> answer
    QHash<quint64, QString> m_inFlight;       // opId -> scopeId
    QHash<QString, qint64> m_lastAsked;       // scopeId -> ms since epoch
    // Scopes that changed in sync while a read was in flight; read again
    // when that read answers.
    QSet<QString> m_rereadAfterAnswer;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    QString m_pendingWriteScope;

    // statsKey ("mxc:<uri>" or "file:<name>" or "prepared:<hash>") -> stats.
    mutable QHash<QString, backdrop::ImageStats> m_stats;
    mutable QStringList m_statsOrder;
    // Personal file name -> staged token, oldest first.
    mutable QHash<QString, QString> m_stagedTokens;
    mutable QStringList m_stagedOrder;
    // scrimFor() results by input key; cleared with the stats they used.
    QHash<QString, QVariantMap> m_scrimCache;
    mutable QHash<QString, QStringList> m_chainCache;

    std::unique_ptr<Prepared> m_prepared;
    // Bumped by every prepareImage() and discardPrepared(): an SVG conversion
    // that finishes after either is dropped.
    quint64 m_prepareGeneration = 0;
    // One message picture in flight for useMessageImage().
    QString m_pendingMediaKey;
    QString m_pendingMediaRoom;
    std::unique_ptr<QTemporaryDir> m_uploadDir;
    QString m_storageOverride;
    mutable QVariantMap m_personalCache;
    mutable bool m_personalLoaded = false;
    int m_revision = 0;
    QString m_lastError;

    // Sync state, all session-scoped (resetSyncState), for this account.
    QString m_syncUserId;
    QHash<QString, RemoteEntry> m_remote;      // scope -> last server answer
    QHash<quint64, QString> m_syncReads;       // op -> scope ("*" = all)
    QHash<quint64, QString> m_syncDownloads;   // op -> scope, in flight
    QStringList m_downloadQueue;               // waiting, kMaxDownloads at once
    QHash<QString, QString> m_downloadWanted;  // scope -> id asked for
    QSet<QString> m_downloadReplacesConflict;  // resolveConflict(synced)
    // Fetched only to compare with a local-only picture: a match adopts the
    // server's copy, a difference is a conflict. Nothing is replaced.
    QSet<QString> m_downloadCompares;
    QStringList m_syncReadBacklog;             // past kMaxSingleReads
    // Room change notices that arrived while the start-up full read was in
    // flight (it may have read the store before them): read after it.
    QSet<QString> m_pokedDuringFullRead;
    QSet<QString> m_syncReadAgain;             // one more read after this one
    QList<SyncOp> m_syncQueue;
    quint64 m_syncWriteOp = 0;
    SyncOp m_syncWriting;
    // Failed operations, retried by retrySync(); a failed download is kept
    // by the id it failed for, which is not fetched again until then.
    // as mode Upload's counterpart, a scope to read again.
    QList<SyncOp> m_syncFailedOps;
    QHash<QString, QString> m_failedDownloadIds;
    // The last picture this device uploaded per scope: the expected id of a
    // presentation change queued behind that upload.
    QHash<QString, QString> m_lastUploaded;
    QHash<QString, QString> m_lastUploadedFile;   // ...and the file it was
    QStringList m_lastClearRooms;                 // for retryRemoval()
    // When this device's own write of a scope (or the switch) answered, as
    // the op counter then: a read with a lower op id may predate it.
    QHash<QString, quint64> m_writtenAt;
    quint64 m_switchWrittenAt = 0;
    // Bounds on a write or switch answer that never comes (a stalled
    // request): the op is failed, and a late answer is dropped by its id.
    QTimer m_writeWatchdog;
    QTimer m_switchWatchdog;
    int m_writeTimeoutMs = kWriteTimeoutMs;
    int m_switchTimeoutMs = kSwitchTimeoutMs;
    int m_serverSwitch = -1;                   // -1 unknown, 0 off, 1 on
    quint64 m_enableOp = 0;
    bool m_enableTarget = true;
    QString m_switchError;
    bool m_migrationNotice = false;
    QSet<QString> m_syncRereadAfterWrite;
    QSet<QString> m_syncAsked;                 // scopes read on room open
    QString m_syncError;
    bool m_syncStarted = false;                // the "*" read answered
    quint64 m_syncClearOp = 0;
    // "Remove the copies on my homeserver" asked while a write was in
    // flight: runs when that write answers, so it cannot land after it.
    bool m_syncClearRequested = false;
    QStringList m_syncClearRooms;
    QString m_syncRemoval;                     // "", "ok", "partial", "failed"
    int m_syncRemoved = 0;
    int m_syncRemoveFailed = 0;
};
