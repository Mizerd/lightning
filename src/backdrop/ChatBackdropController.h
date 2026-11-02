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

class ChatBackdropController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool sharedAvailable READ sharedAvailable NOTIFY availableChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool showShared READ showShared WRITE setShowShared
                   NOTIFY settingsChanged)
    Q_PROPERTY(int surfaceDepth READ surfaceDepth WRITE setSurfaceDepth
                   NOTIFY settingsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantMap prepared READ prepared NOTIFY preparedChanged)
    Q_PROPERTY(bool syncAvailable READ syncAvailable NOTIFY availableChanged)
    Q_PROPERTY(bool syncEnabled READ syncEnabled NOTIFY syncChanged)
    Q_PROPERTY(QVariantMap syncStatus READ syncStatus NOTIFY syncChanged)
    Q_PROPERTY(int syncUnsaved READ syncUnsaved NOTIFY syncChanged)
    // Removals the homeserver has not taken yet: their copies would come
    // back at the next sign-in. Worded separately in the sign-out warning.
    Q_PROPERTY(int syncOwedRemovals READ syncOwedRemovals NOTIFY syncChanged)
    Q_PROPERTY(QVariantList syncConflicts READ syncConflicts NOTIFY syncChanged)
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

    Q_INVOKABLE QVariantMap backdropFor(const QString &roomId) const;
    Q_INVOKABLE void requestRoom(const QString &roomId);
    Q_INVOKABLE void refreshScope(const QString &scopeId);

    Q_INVOKABLE QVariantMap scrimFor(const QString &statsKey,
                                     const QColor &ground,
                                     const QColor &inkPrimary,
                                     const QColor &inkSecondary,
                                     const QColor &inkMuted, double tint,
                                     double dim);
    Q_INVOKABLE QStringList depthStops(const QColor &base, bool darkTheme) const;

    Q_INVOKABLE QVariantMap sharedFor(const QString &scopeId) const;
    Q_INVOKABLE bool canSetShared(const QString &scopeId) const;
    Q_INVOKABLE bool sharedUnsupported(const QString &scopeId) const;
    Q_INVOKABLE bool roomEncrypted(const QString &roomId) const;
    Q_INVOKABLE bool roomEncryptionKnown(const QString &roomId) const;
    Q_INVOKABLE void setShared(const QString &scopeId,
                               const QVariantMap &presentation);
    Q_INVOKABLE void clearShared(const QString &scopeId);

    Q_INVOKABLE QVariantMap personalFor(const QString &roomId) const;
    Q_INVOKABLE bool setPersonal(const QString &roomId,
                                 const QVariantMap &presentation);
    Q_INVOKABLE void clearPersonal(const QString &roomId);
    Q_INVOKABLE bool roomHidden(const QString &roomId) const;
    Q_INVOKABLE void setRoomHidden(const QString &roomId, bool hidden);

    Q_INVOKABLE QVariantMap prepareImage(const QUrl &fileUrl);
    Q_INVOKABLE QString svgMessage(const QString &reason) const;
    Q_INVOKABLE void discardPrepared();
    Q_INVOKABLE void useMessageImage(const QString &roomId,
                                     const QString &mediaKey);

    bool syncAvailable() const;
    bool syncEnabled() const;
    QVariantMap syncStatus() const;
    // On: uploads every local-only picture and follows the server from now
    // on. Off: local-only from now on; with `removeServerCopies` the account
    // data is cleared too (the uploaded, encrypted media cannot be deleted
    // from the media repository; Settings says so).
    Q_INVOKABLE void setSyncEnabled(bool on, bool removeServerCopies = false);
    Q_INVOKABLE void retrySync();
    int syncUnsaved() const;
    int syncOwedRemovals() const;
    QVariantList syncConflicts() const;
    Q_INVOKABLE void retryRemoval();
    bool migrationNoticeNeeded() const;
    Q_INVOKABLE void acknowledgeMigration(bool keepOnThisDeviceOnly);
    Q_INVOKABLE void resolveConflict(const QString &scope, bool useThisDevice);

    void clearSession();

    static constexpr int kMaxEdge = 2560;
    static constexpr qint64 kMaxSourceBytes = 64LL * 1024 * 1024;
    static constexpr int kMaxDecodeEdge = 4096;
    static constexpr qint64 kMaxAnimatedBytes = 8LL * 1024 * 1024;
    static constexpr int kMaxCachedScopes = 256;
    static constexpr int kMaxPersonalRooms = 64;
    static constexpr int kMaxStaged = 4;
    static constexpr int kMaxStats = 32;
    static constexpr qint64 kRefreshIntervalMs = 60 * 1000;
    static constexpr int kMaxDownloads = 2;
    static constexpr int kMaxSingleReads = 6;
    static constexpr int kWriteTimeoutMs = 200 * 1000;
    static constexpr int kSwitchTimeoutMs = 45 * 1000;

    void setSyncTimeoutsForTesting(int writeMs, int switchMs)
    {
        m_writeTimeoutMs = writeMs;
        m_switchTimeoutMs = switchMs;
    }
    void setStorageDirForTesting(const QString &dir) { m_storageOverride = dir; }
    QVariantMap prepareImageForTesting(const QImage &image);
    bool ownsStagedToken(const QString &token) const;
    backdrop::ImageStats measureBytesForTesting(const QByteArray &bytes) const;
    QVariantMap prepareBytesForTesting(const QByteArray &bytes)
    {
        return prepareFromBytes(bytes);
    }
    QStringList spaceChainFor(const QString &roomId) const;

Q_SIGNALS:
    void availableChanged();
    void revisionChanged();
    void settingsChanged();
    void busyChanged();
    void lastErrorChanged();
    void preparedChanged();
    void syncChanged();
    void imagePrepared(const QVariantMap &result);

private:
    struct Shared {
        QVariantMap content;
        bool canSet = false;
        bool unsupported = false;
        bool known = false;
    };
    struct Prepared {
        QByteArray bytes;
        QString mime;
        QString suffix;
        int width = 0;
        int height = 0;
        QString hash;
        backdrop::ImageStats stats;
        QString imageUrl;
        bool animated = false;
    };

    void handleReceived(quint64 opId, const QString &roomId,
                        const QVariantMap &content, bool canSet,
                        bool unsupportedVersion);
    void handleSet(quint64 opId, const QString &roomId, bool ok,
                   const QVariantMap &content, const QString &category);
    void handleChanged(const QString &scopeId);
    void handleMediaCached(const QString &cacheKey);
    void handleMediaBytes(const QString &mediaKey, bool ok,
                          const QByteArray &bytes, const QString &category);
    void onSessionChanged();
    void bump();
    void setLastError(const QString &error);

    QVariantMap prepareFromBytes(const QByteArray &bytes);
    bool encodePicture(const QByteArray &bytes, Prepared &out,
                       QString &error) const;
    QString storeEncoded(const Prepared &picture);
    QVariantMap describeRecord(const QVariantMap &record, bool shared) const;
    QVariantMap personalStore() const;
    void savePersonalStore(const QVariantMap &store);
    QString storageDir() const;
    QString stagedUrlForFile(const QString &fileName) const;
    void measureMxc(const QString &mxc);
    void ensureMeasuredFile(const QString &fileName) const;
    void dropOrphanFiles(const QVariantMap &store);

    enum class SyncMode { Upload = 0, Presentation = 1, Clear = 2, SetEnabled = 3 };
    struct SyncOp {
        QString scope;
        SyncMode mode = SyncMode::Upload;
        QString file;
        bool intoEmpty = false;
    };
    struct RemoteEntry {
        QString state;
        QString id;
        QVariantMap presentation;
        QString color;
    };
    bool syncWatching() const;
    bool syncActive() const;
    bool syncWritable() const;
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
    QStringList migrationCandidates() const;
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

    QHash<QString, Shared> m_shared;
    QHash<quint64, QString> m_inFlight;
    QHash<QString, qint64> m_lastAsked;
    QSet<QString> m_rereadAfterAnswer;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    QString m_pendingWriteScope;

    mutable QHash<QString, backdrop::ImageStats> m_stats;
    mutable QStringList m_statsOrder;
    mutable QHash<QString, QString> m_stagedTokens;
    mutable QStringList m_stagedOrder;
    QHash<QString, QVariantMap> m_scrimCache;
    mutable QHash<QString, QStringList> m_chainCache;

    std::unique_ptr<Prepared> m_prepared;
    // Bumped by every prepareImage() and discardPrepared(): an SVG conversion
    // that finishes after either is dropped.
    quint64 m_prepareGeneration = 0;
    QString m_pendingMediaKey;
    QString m_pendingMediaRoom;
    std::unique_ptr<QTemporaryDir> m_uploadDir;
    QString m_storageOverride;
    mutable QVariantMap m_personalCache;
    mutable bool m_personalLoaded = false;
    int m_revision = 0;
    QString m_lastError;

    QString m_syncUserId;
    QHash<QString, RemoteEntry> m_remote;
    QHash<quint64, QString> m_syncReads;
    QHash<quint64, QString> m_syncDownloads;
    QStringList m_downloadQueue;
    QHash<QString, QString> m_downloadWanted;
    QSet<QString> m_downloadReplacesConflict;
    QSet<QString> m_downloadCompares;
    QStringList m_syncReadBacklog;
    // Room change notices that arrived while the start-up full read was in
    // flight (it may have read the store before them): read after it.
    QSet<QString> m_pokedDuringFullRead;
    QSet<QString> m_syncReadAgain;
    QList<SyncOp> m_syncQueue;
    quint64 m_syncWriteOp = 0;
    SyncOp m_syncWriting;
    QList<SyncOp> m_syncFailedOps;
    QHash<QString, QString> m_failedDownloadIds;
    QHash<QString, QString> m_lastUploaded;
    QHash<QString, QString> m_lastUploadedFile;
    QStringList m_lastClearRooms;
    QHash<QString, quint64> m_writtenAt;
    quint64 m_switchWrittenAt = 0;
    QTimer m_writeWatchdog;
    QTimer m_switchWatchdog;
    int m_writeTimeoutMs = kWriteTimeoutMs;
    int m_switchTimeoutMs = kSwitchTimeoutMs;
    int m_serverSwitch = -1;
    quint64 m_enableOp = 0;
    bool m_enableTarget = true;
    QString m_switchError;
    bool m_migrationNotice = false;
    QSet<QString> m_syncRereadAfterWrite;
    QSet<QString> m_syncAsked;
    QString m_syncError;
    bool m_syncStarted = false;
    quint64 m_syncClearOp = 0;
    // "Remove the copies on my homeserver" asked while a write was in
    // flight: runs when that write answers, so it cannot land after it.
    bool m_syncClearRequested = false;
    QStringList m_syncClearRooms;
    QString m_syncRemoval;
    int m_syncRemoved = 0;
    int m_syncRemoveFailed = 0;
};
