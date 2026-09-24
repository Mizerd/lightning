#pragma once

#include "gif/GifResponseParser.h"
#include "gif/GifStarredModel.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QSettings;

// Client-local store for chat images the user starred to reuse from the picker.
//
// Security position: starring is an explicit user save, like Save As, and the
// bytes come through the same controlled media bridge. Unlike Save As, files
// accumulate in a hidden app-managed directory, so this feature carries extra
// obligations:
//   - dedicated deletion on sign-out and account removal (see CLEANUP);
//   - a Settings view of what has accumulated, with a confirmed Clear All;
//   - hard caps that refuse rather than grow.
// This is a bounded, deliberate exception to CLAUDE.md §6's rule against
// persisting decrypted plaintext. Only the bytes the user starred are written;
// nothing else about the message is stored, and CacheStore still rejects
// encrypted timeline rows.
//
// Storage: <accountDir>/<sha256-hex>.<ext>, content-addressed so the same
// image is stored once. `accountDir` comes from the caller
// (matrix::app_data::starredGifsDir); with none, the store stays closed and
// never falls back to a shared location. GIF/PNG/JPEG/WebP are stored exactly
// as received, never transcoded; the suffix comes from byte validation
// (gif::validateRasterBytes), never a claimed type. A row without `ext` is a
// legacy entry and means "gif".
//
// CLEANUP: sign-out and removal of the active account go through
// AuthManager::logout(), which never reaches the account-root sweep, so
// AppController::onLoggedOut() deletes this directory explicitly, keyed on the
// identity that was active. It does nothing while switching accounts.
// Removing a background account sweeps the account root and also deletes this
// directory explicitly. Both paths close the store first if it is open on that
// directory, and report "deleted" and "absent" as distinct outcomes. The store
// is also closed on every loggedOut, including a switch, so the picker never
// shows the previous account's rows.
//
// Privacy: the persisted index carries only provider="local", the hash,
// dimensions and byte count, the same contract as the provider favorites. The
// room, event and sender never reach this class; `mediaKey` is used only in an
// in-memory session map.
//
// No network or media-bridge access here; see AppController::starChatGif.
//
// Starred state for the timeline: persisting `mediaKey` would put a Matrix
// identifier in the index, so the durable check is content-based instead.
// isStarredThisSession() covers rows starred in this run; for others,
// AppController::isChatGifStarred hashes the bytes MediaBridge already has in
// its display cache and asks hasHash(). Until those bytes are cached the row
// shows unstarred: a possible false negative, never a false positive.
// Activating the star while the answer is unknown always stars (never
// unstars on a guess), which may re-fetch bytes already on disk; starBytes()
// then does not rewrite the file but re-prepends the row and reports
// "already_starred".
class GifStarredStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY countChanged)
    // Exposed as a property: QML cannot call a plain C++ getter, and a throwing
    // binding silently keeps its previous value.
    Q_PROPERTY(GifStarredModel *model READ model CONSTANT)

public:
    explicit GifStarredStore(QObject *parent = nullptr);
    // Out-of-line so std::unique_ptr<QSettings> is destroyed where QSettings is
    // complete (moc may instantiate the destructor elsewhere).
    ~GifStarredStore() override;

    // Picker model, role-compatible with GifResultModel. Never recreated;
    // openFor() repoints it, so GifSavedModel keeps a stable pointer across
    // account switches.
    GifStarredModel *model() const { return m_model.get(); }

    // Points the store at an account-scoped `accountDir` and reloads its index,
    // dropping entries whose files are missing. An empty or uncreatable
    // directory leaves the store closed.
    void openFor(const QString &accountDir);
    void close();
    bool isOpen() const { return !m_dir.isEmpty(); }
    // The directory currently open ("" when closed), so AppController can close
    // the store before deleting that directory.
    QString currentDirectory() const { return m_dir; }

    // Validates, hashes and (if new) writes `bytes`, then registers the entry.
    // Re-starring existing content only refreshes recency. `mediaKey` is kept
    // only in the in-memory session map. Emits starFinished(mediaKey, ok,
    // category, message). `category` is a stable token ("invalid_media",
    // "too_large", "unsupported_format", "cap_items", "cap_bytes",
    // "write_failed", "not_open", "unavailable", "timeout") and `message` is
    // the translated sentence. Success has empty category and message, except
    // "already_starred" (ok true): the content was already indexed and only its
    // row was re-prepended.
    void starBytes(const QString &mediaKey, const QByteArray &bytes);
    // Relays a failure from the media-bridge fetch through starFinished(), so
    // QML has one signal for every stage.
    void reportFetchFailed(const QString &mediaKey, const QString &category)
    { Q_EMIT starFinished(mediaKey, false, category, categoryMessage(category)); }

    // Deletes the file and index entry. No-op, and no signal, for an unknown
    // hash; emits unstarFinished(hash) on removal.
    Q_INVOKABLE void unstar(const QString &hash);
    // Deletes every starred file and clears the index (Settings, Clear All). No
    // signal when already empty or closed.
    Q_INVOKABLE void clearAll();
    // Unstars the hash this session recorded for `mediaKey`. Rows starred in an
    // earlier session are handled by AppController::unstarChatGif's
    // content-hash fallback.
    Q_INVOKABLE void unstarByMediaKey(const QString &mediaKey);
    // True only for a mediaKey starred in this process. False does not mean
    // "not starred"; see AppController::isChatGifStarred.
    Q_INVOKABLE bool isStarredThisSession(const QString &mediaKey) const
    { return m_mediaKeyToHash.contains(mediaKey); }
    // True iff content with this sha256 is starred.
    Q_INVOKABLE bool hasHash(const QString &hash) const
    { return isOpen() && isSafeHashHex(hash) && m_model->hasHash(hash); }

    // Bytes of a starred hash, read from disk on every call, for sending the
    // exact stored file. Empty when unknown, missing or closed.
    QByteArray readBytes(const QString &hash) const;

    // Local playback source for a picker tile, or "". The hash is shape-checked
    // and the suffix whitelisted when the index is read, so the path is always
    // <dir>/<hash>.<ext>; the file's existence is checked on every call.
    Q_INVOKABLE QString source(const QString &hash) const;
    // Informational format suffix for the tile badge; "" when unknown.
    Q_INVOKABLE QString sourceExt(const QString &hash) const;

    qint64 totalBytes() const { return m_model->totalBytes(); }
    int count() const { return m_model->count(); }

    static constexpr int kMaxItems = GifStarredModel::kMaxStarred;
    static constexpr qint64 kMaxTotalBytes = 64LL * 1024 * 1024; // 64 MiB

    // Test seam for the caps.
    void setCapsForTest(int maxItems, qint64 maxTotalBytes)
    { m_maxItems = maxItems; m_maxTotalBytes = maxTotalBytes; }

Q_SIGNALS:
    void starFinished(const QString &mediaKey, bool ok,
                      const QString &category, const QString &message);
    void unstarFinished(const QString &hash);
    void countChanged();

private:
    // Empty `ext` means "gif" (legacy entries).
    QString filePath(const QString &hash, const QString &ext = QString()) const;
    // Persisted suffix for a known hash ("gif" for legacy rows), "" otherwise.
    // Callers still check the file exists.
    QString extForHash(const QString &hash) const;
    static bool isSafeHashHex(const QString &hash);
    // Creates the directory (0700) on the first write, so accounts that never
    // star anything get no directory. False when it cannot be created.
    bool ensureDirectory() const;
    // Translated sentence for a category token; cap refusals state the limit.
    QString categoryMessage(const QString &category) const;

    QString m_dir;
    int m_maxItems = kMaxItems;
    qint64 m_maxTotalBytes = kMaxTotalBytes;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<GifStarredModel> m_model;
    // Session-only mediaKey -> hash; never persisted or logged. Cleared on
    // openFor()/close().
    QHash<QString, QString> m_mediaKeyToHash;
};
