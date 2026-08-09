#pragma once

#include "gif/GifResponseParser.h"
#include "gif/GifStarredModel.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QSettings;

// v0.6.6: client-local "star a chat GIF" store (Discord-style: star a GIF a
// contact sent and keep your own copy for reuse from the picker).
//
// SEMANTICS / SECURITY POSITION (documented per review requirement): starring
// is an EXPLICIT user save action, in the same class as the existing "save
// media to disk" feature (MediaBridge::saveAs) in that BOTH require an
// explicit user gesture and BOTH fetch decrypted bytes through the same
// controlled media bridge (never a raw/authenticated URL reaching QML or a
// bare provider fetch) — that much is honestly shared. The two differ in a
// way worth stating plainly rather than glossing over: saveAs writes exactly
// ONE file to a location the user picks and sees in their own file manager,
// with no ongoing app bookkeeping. Starring is different in kind — it
// accumulates an unbounded (well, capped) number of files in a HIDDEN,
// app-managed directory the user never browses directly, indexed by this
// app, rendered back into the picker by this app, and never independently
// reviewed by the user the way a Save As destination naturally is. That is
// exactly why this feature carries obligations Save As does not:
//   - a real, honest deletion path on sign-out and account removal (see
//     AppController::onLoggedOut / AppController::removeAccount, and the
//     "CLEANUP" section below — this is NOT a side effect of some other
//     deletion, it is dedicated code that must be kept correct);
//   - a user-visible way to see and clear what has accumulated (Settings ->
//     Privacy & security -> "GIFs saved from chats": count, total size,
//     "kept on
//     this device only", and a confirmed destructive Clear All);
//   - hard caps that refuse rather than silently grow forever.
// This is a conscious, bounded, DOCUMENTED-AND-ENFORCED exception to
// CLAUDE.md §6's "never persist decrypted private-message plaintext" rule.
// That rule targets AUTOMATIC caching of arbitrary encrypted-room content —
// CacheStore keeps rejecting encrypted timeline rows unconditionally,
// completely untouched by this feature. Only the GIF bytes the user
// explicitly starred are ever written; nothing else about the message
// (body, other media, room state, sender) is touched or persisted.
//
// STORAGE: bytes live at <accountDir>/<sha256-hex>.gif — content-addressed,
// so starring the same GIF twice (even via two different messages) never
// duplicates storage. `accountDir` is supplied by the caller
// (AppController, via matrix::app_data::starredGifsDir(userId) — always the
// CANONICAL account root, never a recorded/divergent store slug); this
// class never derives or trusts a path itself, and never invents a fallback
// location when accountDir is empty (the feature is simply inert/closed).
//
// CLEANUP (the actual mechanism — read this before trusting any comment
// elsewhere that merely gestures at "account removal already deletes it"):
// deleting the whole account root on BACKGROUND-account removal
// (AppController::removeAccount, the branch for an account that is not the
// live session) does incidentally sweep this directory with it, but that is
// NOT how the common case is covered, and that branch ALSO gets its own
// explicit, distinctly-reported deletion (see below) rather than relying on
// the sweep alone. Explicit, server-confirmed sign-out (AuthManager::
// logout()), and "remove account" invoked on the currently-ACTIVE account
// (which routes through that exact same logout), never reach the
// account-root sweep at all — they return immediately, and the local Rust
// store is deleted asynchronously, later, from RustSdkMatrixClient's own
// callback. This store therefore has its OWN explicit deletion for that
// case too, in AppController::onLoggedOut(), keyed off the identity that
// was actually active (m_lastSessionUserId, captured before it is cleared)
// — never re-derived for a different account, and never run while merely
// SWITCHING accounts (onLoggedOut() returns immediately while
// m_accountSwitching is true, before this cleanup runs, so a switch never
// touches the outgoing account's starred files). Neither delete path pulls
// the directory out from under a live handle: the sign-out path closes the
// store unconditionally first (only the active account's store is ever
// open, so that close is always the right one), the background-removal
// path closes it only when it is currently open on the exact directory
// about to be removed. Both report "deleted" vs
// "absent" as distinct outcomes, never folding a no-op into a false
// "success". AppController also closes this store (harmlessly, whether or
// not the above already did) on EVERY MatrixClient::loggedOut, including a
// plain account SWITCH, which never deletes anything but must not leave a
// stale open handle showing the outgoing account's rows in the picker's
// Saved tab while the next account's session is still starting.
//
// PRIVACY: the persisted index (via GifStarredModel/GifStoredModel) carries
// only provider="local", the content hash, safe dimensions and the byte
// count — the exact same "no Matrix identifiers" contract GifFavoritesModel/
// GifRecentModel already enforce for provider GIFs. The room, event id and
// sender are NEVER passed to this class in the first place (see
// AppController::starChatGif, which only ever hands over `mediaKey` — used
// solely as an in-memory, non-persisted session lookup below — and the
// fetched bytes). Provenance is deliberately not tracked: the file IS what
// the user chose to keep, and nothing about where it came from is needed to
// render or send it again.
//
// This class never touches the network or the media bridge directly — see
// AppController::starChatGif for the fetch trigger — so it stays a small,
// dependency-light QtCore class (safe for unit tests with a QTemporaryDir,
// no MediaBridge/MatrixClient linkage).
//
// DURABLE STARRED-STATE DESIGN (v0.6.6 fix — replaces an earlier "accepted
// limitation" that this class does not repeat here): the timeline star must
// answer correctly after a restart and for a second message carrying the
// SAME GIF, not just for the row that was just starred in this session. The
// obvious shortcut — persist `mediaKey` (the Matrix event id, or the SDK's
// local-echo unique id) alongside the hash in index.ini — was rejected: it
// is exactly the Matrix identifier the PRIVACY paragraph above promises this
// index never carries, and (per MediaBridge's own cache-key-tagging comment)
// an SDK media key can embed room/event structure, not just an opaque token.
// The design actually used stays content-addressed and adds nothing to the
// persisted index:
//   - `isStarredThisSession`/`unstarByMediaKey` below remain exactly what
//     they always were — a session-only, non-persisted mediaKey->hash
//     lookup, correct immediately for a GIF starred in THIS run;
//   - AppController::isChatGifStarred/unstarChatGif (the QML-facing entry
//     points MessageDelegate.qml now calls instead of these two directly)
//     add a second, durable tier for everything that lookup misses: they ask
//     MediaBridge for the SHA-256 of whatever full GIF bytes are ALREADY
//     sitting in its ordinary in-RAM display cache for that mediaKey (never
//     triggering a fresh fetch just to answer the question), then check that
//     hash against this store's own `hasHash()` — the same content identity
//     `starBytes` already hashes and dedupes on. That reuses the existing
//     player/preview fetch that already has to happen to SHOW the GIF, so no
//     extra network traffic or decryption is added for this check.
// Trade-off, stated honestly: the durable answer is only as fresh as
// whatever MediaBridge happens to have cached. A starred GIF whose full
// bytes were never fetched in this run — e.g. it sits off-screen, or
// "Animate GIF previews" is set to Never and the row was never opened —
// still shows unstarred (not wrong, just not yet known) until something
// (autoplay, hover-to-play, opening the image) fetches those bytes, at which
// point MessageDelegate.qml's onAnimatedMediaReady/onMediaCached handlers
// re-run this check and the star fills in without a reload. This IS a real,
// honest false negative — the unknown state renders exactly like "not
// starred" until the bytes are cached — never a false POSITIVE (an unstarred
// GIF is never shown filled).
//
// The "never re-downloads or re-persists" property holds only for the QUERY
// path (isChatGifStarred/unstarByMediaKey's durable fallback: reading
// MediaBridge's already-cached bytes, memoized — see MediaBridge::
// cachedFullContentHash — never dispatches a fetch). It does NOT hold for
// ACTIVATING the star while the answer is still unknown: the hover star's
// activate() then takes the "star" branch (the only ambiguity-safe choice —
// an unknown answer must never be treated as "so unstar it", which would
// delete a file the user chose to keep on a coin-flip), which calls
// AppController::starChatGif -> MediaBridge::fetchFullForStar. If those
// bytes are not already cached, that IS a real network fetch and SDK
// decrypt of a file already sitting on disk. starBytes()'s existing-hash
// branch then refuses to rewrite the file (see below), so no bytes are
// duplicated on disk — but it still calls insertLocal(), which re-prepends
// the row (a visible reorder of the Saved tab for something that was
// already there), and starFinished emits with `category` ==
// "already_starred" so the UI can say "Already in your saved GIFs."
// instead of implying a new star happened (see categoryMessage()).
class GifStarredStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY countChanged)
    // GifSavedModel merges this with the provider favorites for the
    // picker's single Saved tab (`gif.saved`)
    // exactly like GifSearchController exposes `favorites`/`recent` — a plain
    // C++ getter is NOT enough: QML only auto-exposes Q_PROPERTY/Q_INVOKABLE/
    // slots, so calling a bare public method from a binding throws, and a
    // thrown QML binding silently keeps the binding's PREVIOUS value instead
    // of failing loudly. That is exactly what happened here before this
    // property existed — `model()` itself (below) is UNCHANGED, a plain
    // accessor; only exposing it as a Q_PROPERTY is new.
    Q_PROPERTY(GifStarredModel *model READ model CONSTANT)

public:
    explicit GifStarredStore(QObject *parent = nullptr);
    // Declared (defined out-of-line in the .cpp, where <QSettings> is a
    // complete type) rather than left implicit: an implicitly-defaulted
    // destructor for a class holding std::unique_ptr<QSettings> is odr-used
    // wherever moc's generated metaobject code first needs it, which can be
    // in a translation unit that only ever saw the forward declaration —
    // exactly what GifSearchController's own ~GifSearchController() already
    // works around for the same reason.
    ~GifStarredStore() override;

    // Presentation model for the picker (role-compatible with
    // GifResultModel, exactly like GifFavoritesModel/GifRecentModel). This
    // instance is constructed once and never destroyed/recreated — openFor()
    // repoints its persistence and reloads in place — so the picker's own
    // Saved tab (GifSavedModel attaches to this pointer) keeps a
    // valid, stable QObject identity across account switches.
    GifStarredModel *model() const { return m_model.get(); }

    // Point the store at `accountDir` (already account-scoped; see the class
    // comment). Closes/reloads: previous rows are dropped and the new
    // directory's index (if any) is loaded and re-validated against what is
    // actually on disk — a stale or tampered index entry whose backing file
    // is missing is silently dropped, never surfaced as a playable/sendable
    // tile. An empty or uncreatable `accountDir` leaves the store closed
    // (inert — never falls back to a shared/global location).
    void openFor(const QString &accountDir);
    void close();
    bool isOpen() const { return !m_dir.isEmpty(); }
    // The exact directory this instance is currently open on ("" when
    // closed) — used ONLY by AppController to decide whether a store it is
    // about to delete on disk is this live instance's own open directory,
    // in which case it must close() first (never delete out from under an
    // open QSettings handle / leave stale in-memory rows behind).
    QString currentDirectory() const { return m_dir; }

    // Validate + content-hash + (if new) write `bytes` to disk and register
    // the entry; deduplicates by content hash (re-starring an
    // already-stored GIF is a cap-exempt no-op that just refreshes recency —
    // see GifStoredModel::insertFront). `mediaKey` is remembered ONLY in an
    // in-memory, per-session, NEVER-persisted map so the chat message hover
    // star can render filled/"Remove from saved GIFs" for a row saved
    // earlier in this session — see the class comment on why provenance is
    // not persisted; this is a UI convenience, not a provenance record, and
    // does not survive restart.
    // Emits starFinished(mediaKey, ok, category, message). `category` is the
    // stable, machine-readable token ("not_a_gif", "invalid_media",
    // "too_large" — gif::validateGifBytes; "cap_items", "cap_bytes" — bounded
    // store, refused rather than silently evicting a GIF the user chose to
    // keep; "write_failed"; "not_open" — no account directory is currently
    // open; "unavailable"/"timeout" — relayed fetch failures) that tests
    // assert on; `message` is the ALREADY-TRANSLATED, user-ready sentence
    // (never a raw category token reaching the UI — see categoryMessage()).
    // On a genuine SUCCESS `category`/`message` are both empty, EXCEPT one
    // case: `ok` true with `category` == "already_starred" means this exact
    // content hash was already indexed (the durable-check "unknown" ->
    // activate() -> re-fetch path documented in the class comment) — no
    // bytes were rewritten, but the row was re-prepended, and the UI should
    // say so rather than implying a new star just happened.
    void starBytes(const QString &mediaKey, const QByteArray &bytes);
    // Relays a failure that happened BEFORE bytes ever reached this class
    // (the media-bridge fetch itself failed/timed out) through the same
    // starFinished() signal, so QML has one signal to listen to regardless
    // of which stage failed. Called only by AppController::starChatGif's
    // MediaBridge::mediaBytesForStar handler.
    void reportFetchFailed(const QString &mediaKey, const QString &category)
    { Q_EMIT starFinished(mediaKey, false, category, categoryMessage(category)); }

    // Remove by content hash: deletes the file and the index entry. Safe to
    // call for an absent/invalid hash (no-op — emits nothing when nothing
    // was actually removed). Emits unstarFinished(hash) on real removal, for
    // the same banner-feedback mechanism starFinished drives.
    Q_INVOKABLE void unstar(const QString &hash);
    // Delete every starred GIF's file and clear the index — the Settings ->
    // Privacy & security -> "GIFs saved from chats" -> Clear All. A no-op
    // (and no signal) when already empty/closed.
    Q_INVOKABLE void clearAll();
    // Convenience for the chat message hover star, which only knows
    // `mediaKey`: unstars whatever hash this session remembers for it (see
    // starBytes). A row starred in an EARLIER session (no session-local
    // mapping yet) cannot be unstarred this way — that is exactly why
    // AppController::unstarChatGif (the QML-facing entry point) falls back
    // to a durable, content-hash lookup through MediaBridge when this
    // session map does not know `mediaKey` — see the class comment's
    // "DURABLE STARRED-STATE DESIGN" section. This method itself is
    // unchanged: fast, session-only, exact.
    Q_INVOKABLE void unstarByMediaKey(const QString &mediaKey);
    // Session-only fast path: true only for a mediaKey this exact process
    // starred (see starBytes). Always correct when true; a false here does
    // NOT mean "not starred" — see AppController::isChatGifStarred, which is
    // what MessageDelegate.qml actually calls, for the durable answer that
    // also covers a restart or a second message with identical bytes.
    Q_INVOKABLE bool isStarredThisSession(const QString &mediaKey) const
    { return m_mediaKeyToHash.contains(mediaKey); }
    // Durable, content-addressed answer: true iff a GIF with this exact
    // sha256 content hash is currently starred. The caller (AppController)
    // is responsible for turning a timeline row's mediaKey into a hash
    // (via MediaBridge's already-cached bytes) without ever persisting the
    // mediaKey itself — see the class comment.
    Q_INVOKABLE bool hasHash(const QString &hash) const
    { return isOpen() && isSafeHashHex(hash) && m_model->hasHash(hash); }

    // Bytes for a previously starred hash, read fresh from disk every call
    // (never trusted from a cached copy) — used by GifSendController to
    // send the EXACT stored file. Empty when unknown, missing, or the store
    // is closed.
    QByteArray readBytes(const QString &hash) const;

    // Safe, re-validated (hash format + on-disk existence, checked fresh
    // every call) local playback source for the picker's tiles — "" when
    // unavailable. Never trusts a path from the persisted index; the only
    // input is the hash, and the location is always <dir>/<hash>.gif under
    // whichever account directory is currently open.
    Q_INVOKABLE QString source(const QString &hash) const;

    qint64 totalBytes() const { return m_model->totalBytes(); }
    int count() const { return m_model->count(); }

    static constexpr int kMaxItems = GifStarredModel::kMaxStarred;
    static constexpr qint64 kMaxTotalBytes = 64LL * 1024 * 1024; // 64 MiB

    // Test seam: override the caps so a refusal test does not need to
    // allocate real GiB-scale content. Defaults to the production caps
    // above; production code never calls this.
    void setCapsForTest(int maxItems, qint64 maxTotalBytes)
    { m_maxItems = maxItems; m_maxTotalBytes = maxTotalBytes; }

Q_SIGNALS:
    void starFinished(const QString &mediaKey, bool ok,
                      const QString &category, const QString &message);
    void unstarFinished(const QString &hash);
    void countChanged();

private:
    QString filePath(const QString &hash) const;
    static bool isSafeHashHex(const QString &hash);
    // Creates the store directory (0700) on first real write — deferred out
    // of openFor()/every login so an account that never stars anything
    // never gets an empty starred-gifs directory materialized on disk.
    // Returns false (refuses the write) if the directory cannot be created.
    bool ensureDirectory() const;
    // Translates a stable category token into a ready-to-display sentence —
    // the C++ boundary, matching MediaBridge::saveFinished's own precedent
    // of carrying a translated message rather than a raw token QML would
    // have to interpret. Cap refusals state the actual configured limit.
    QString categoryMessage(const QString &category) const;

    QString m_dir;
    int m_maxItems = kMaxItems;
    qint64 m_maxTotalBytes = kMaxTotalBytes;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<GifStarredModel> m_model;
    // Session-only mediaKey -> content hash. Never persisted, never logged
    // (see the class comment). Cleared whenever the store is repointed
    // (openFor/close), i.e. on every login/switch/logout.
    QHash<QString, QString> m_mediaKeyToHash;
};
