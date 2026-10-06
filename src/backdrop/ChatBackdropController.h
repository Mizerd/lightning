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
//     this state type. Answers carry the op id they were asked with; one that
//     is not in flight (a previous account, a superseded read) is dropped.
//   * PERSONAL: pictures only this account sees, kept as Lightning-encoded
//     JPEG/PNG files under <accountRoot>/backgrounds and served to QML from
//     memory through the staged-image store, never as a file:// URL.
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

    // ---- test seams ----------------------------------------------------------

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
    void handleMediaCached(const QString &cacheKey);
    void handleMediaBytes(const QString &mediaKey, bool ok,
                          const QByteArray &bytes, const QString &category);
    void onSessionChanged();
    void bump();
    void setLastError(const QString &error);

    QVariantMap prepareFromBytes(const QByteArray &bytes);
    QVariantMap describeRecord(const QVariantMap &record, bool shared) const;
    QVariantMap personalStore() const;
    void savePersonalStore(const QVariantMap &store);
    QString storageDir() const;
    QString stagedUrlForFile(const QString &fileName) const;
    void measureMxc(const QString &mxc);
    void ensureMeasuredFile(const QString &fileName) const;
    void dropOrphanFiles(const QVariantMap &store);

    QPointer<MatrixClient> m_client;
    QPointer<MediaBridge> m_bridge;
    QPointer<SettingsManager> m_settings;
    QPointer<SpaceManager> m_spaces;
    QPointer<StagedImageStore> m_staged;

    QHash<QString, Shared> m_shared;          // scopeId -> answer
    QHash<quint64, QString> m_inFlight;       // opId -> scopeId
    QHash<QString, qint64> m_lastAsked;       // scopeId -> ms since epoch
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
};
