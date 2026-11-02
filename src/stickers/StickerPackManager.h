#pragma once

#include "stickers/StickerPack.h"
// Complete types: moc needs the QMetaType of Q_PROPERTY pointee classes.
#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackModel.h"

#include <QHash>
#include <QObject>
#include <functional>
#include <memory>

class QTimer;
#include <QUrl>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QTemporaryDir;
class MatrixClient;

class StickerPackManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("StickerPackManager is exposed via app.stickers")

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(StickerPackModel *packs READ packs CONSTANT)
    Q_PROPERTY(StickerImageModel *images READ images CONSTANT)
    Q_PROPERTY(StickerImageModel *editorImages READ editorImages CONSTANT)
    Q_PROPERTY(QString selectedPackId READ selectedPackId WRITE setSelectedPackId
                   NOTIFY selectedPackIdChanged)
    Q_PROPERTY(QString editorPackId READ editorPackId WRITE setEditorPackId
                   NOTIFY selectedPackIdChanged)
    Q_PROPERTY(QString usage READ usage WRITE setUsage NOTIFY usageChanged)
    Q_PROPERTY(int usablePackCount READ usablePackCount NOTIFY stateChanged)
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY stateChanged)
    Q_PROPERTY(bool editing READ editing NOTIFY stateChanged)
    Q_PROPERTY(bool togglingRoomPack READ togglingRoomPack NOTIFY stateChanged)
    Q_PROPERTY(bool migrating READ migrating NOTIFY stateChanged)
    Q_PROPERTY(bool transferring READ transferring NOTIFY stateChanged)
    Q_PROPERTY(QString transferKind READ transferKind NOTIFY stateChanged)
    Q_PROPERTY(int transferDone READ transferDone NOTIFY stateChanged)
    Q_PROPERTY(int transferTotal READ transferTotal NOTIFY stateChanged)

public:
    explicit StickerPackManager(QObject *parent = nullptr);
    ~StickerPackManager() override;

    void setClient(MatrixClient *client);
    void setActiveRoomId(const QString &roomId);

    bool available() const;
    bool loading() const { return m_fetchOp != 0; }
    bool loaded() const { return m_loaded; }
    bool saving() const { return m_saveOp != 0; }
    bool editing() const { return m_editOp != 0; }
    StickerPackModel *packs() const { return m_packs; }
    StickerImageModel *images() const { return m_images; }
    StickerImageModel *editorImages() const { return m_editorImages; }
    QString selectedPackId() const { return m_selectedPackId; }
    QString editorPackId() const { return m_editorPackId; }
    void setEditorPackId(const QString &id);
    void setSelectedPackId(const QString &id);
    QString usage() const { return m_usage; }
    void setUsage(const QString &usage);
    int usablePackCount() const;
    int revision() const { return m_revision; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshIfStale();

    Q_INVOKABLE void sendToRoom(const QString &roomId,
                                const QVariantMap &image);
    Q_INVOKABLE void sendToThread(const QString &roomId, const QString &rootId,
                                  const QVariantMap &image);

    Q_INVOKABLE void uploadSticker(const QUrl &fileUrl,
                                   const QString &shortcode);
    Q_INVOKABLE void uploadImage(const QUrl &fileUrl, const QString &shortcode,
                                 const QString &usage);
    Q_INVOKABLE QString svgMessage(const QString &reason) const;

    Q_INVOKABLE void saveSticker(const QString &url, const QString &body,
                                 const QString &mimetype, int width,
                                 int height, qint64 size);

    Q_INVOKABLE void saveStickerToRoom(const QString &roomId,
                                       const QString &url,
                                       const QString &body,
                                       const QString &mimetype, int width,
                                       int height, qint64 size);
    Q_INVOKABLE bool canSaveToRoom(const QString &roomId,
                                   const QString &url) const;

    Q_INVOKABLE void removeImageFromPack(const QString &packId,
                                         const QString &key);
    Q_INVOKABLE void renameImageInPack(const QString &packId,
                                       const QString &from, const QString &to);
    Q_INVOKABLE void renamePack(const QString &packId, const QString &name);
    Q_INVOKABLE void setImageDescription(const QString &packId,
                                         const QString &key,
                                         const QString &description);
    Q_INVOKABLE void deletePack(const QString &packId);
    Q_INVOKABLE bool canManagePack(const QString &packId) const;
    Q_INVOKABLE QVariantMap packInfo(const QString &packId) const;
    Q_INVOKABLE void createRoomPack(const QString &roomId, const QString &name,
                                    const QString &usage);
    Q_INVOKABLE bool canCreateRoomPack(const QString &roomId) const;
    Q_INVOKABLE QVariantMap userPackMigrationPreview() const;
    Q_INVOKABLE void migrateUserPack(bool startNew = false);
    bool migrating() const { return m_migrateOp != 0; }
    Q_INVOKABLE void setPackUsage(const QString &packId, const QString &usage);
    void setPrivateMediaFetch(std::function<void(const QString &mediaKey)> fetch)
    {
        m_privateMediaFetch = std::move(fetch);
    }
    // No name or description is taken from the private message (its caption
    // or file name would become plaintext account data): the image is added
    // as "sticker" (suffixed if taken) for the user to rename.
    Q_INVOKABLE void republishPrivateImage(const QString &mediaKey);
    Q_INVOKABLE void cancelRepublish();
    bool republishing() const { return !m_republishKey.isEmpty(); }
    void onPrivateMediaBytes(const QString &mediaKey, bool ok,
                             const QByteArray &bytes);

    Q_INVOKABLE QString shareText(const QString &packId) const;
    Q_INVOKABLE QString shareLink(const QString &packId) const;
    Q_INVOKABLE bool copyShareText(const QString &packId) const;

    Q_INVOKABLE QVariantMap packImageMap(const QString &packId) const;
    Q_INVOKABLE QVariantMap comparePack(const QString &packId,
                                        const QVariantMap &base) const;

    void setLocalStatePath(const QString &path);
    void forgetAccount(const QString &userId);
    Q_INVOKABLE void setPackHidden(const QString &packId, bool hidden);
    Q_INVOKABLE bool isPackHidden(const QString &packId) const;
    Q_INVOKABLE void recordCustomUse(const QVariantMap &row);
    Q_INVOKABLE QVariantList customRecents() const;
    Q_INVOKABLE void toggleFavourite(const QVariantMap &row);
    Q_INVOKABLE bool isFavourite(const QString &url) const;
    Q_INVOKABLE QVariantList favourites() const;
    Q_INVOKABLE void forgetCustomEntry(const QString &url);

    Q_INVOKABLE QVariantMap snapshotMeta() const { return m_snapshotMeta; }
    Q_INVOKABLE bool lastMirrorFailed() const { return m_lastMirrorFailed; }
    Q_INVOKABLE void setRoomPackEnabled(const QString &packId, bool enabled);
    bool togglingRoomPack() const { return m_roomsOp != 0; }

    Q_INVOKABLE QString sanitizeShortcode(const QString &raw) const;
    Q_INVOKABLE QString shortcodeError(const QString &packId,
                                       const QString &current,
                                       const QString &candidate) const;

    Q_INVOKABLE void exportPack(const QString &packId, const QUrl &zipFile);
    Q_INVOKABLE void uploadImagesToPack(const QString &packId,
                                        const QVariantList &fileUrls);
    Q_INVOKABLE void retryFailedUploads();
    Q_INVOKABLE int failedUploadCount() const { return int(m_failedUploads.size()); }
    Q_INVOKABLE void importPack(const QUrl &zipFile);

    Q_INVOKABLE QString transferMessage(const QString &kind, bool ok,
                                        const QString &category, int added,
                                        int skipped, int failed,
                                        const QString &name) const;

    Q_INVOKABLE QVariantList emoticonsInHtml(const QString &html) const;
    Q_INVOKABLE void saveEmoticons(const QVariantList &emoticons);

    Q_INVOKABLE bool isSaved(const QString &url) const;
    Q_INVOKABLE bool canSave(const QString &url) const;

    Q_INVOKABLE QVariantList reactionEmoticons() const;

    Q_INVOKABLE QVariantList findEmoticons(const QString &prefix,
                                           int limit) const;
    Q_INVOKABLE QString shortcodeForUrl(const QString &url) const;
    Q_INVOKABLE QVariantMap emoticon(const QString &shortcode) const;
    Q_INVOKABLE QString uniqueEmoticonUrl(const QString &shortcode) const;

    bool transferring() const { return m_transfer.active; }
    QString transferKind() const { return m_transfer.kind; }
    int transferDone() const { return m_transfer.done; }
    int transferTotal() const { return m_transfer.total; }

    void applySnapshotForTest(const QString &roomId, bool roomCanManage,
                              const QVariantList &packs);

Q_SIGNALS:
    void availableChanged();
    void stateChanged();
    void selectedPackIdChanged();
    void usageChanged();
    void saveFinished(bool ok, const QString &category,
                      const QString &shortcode, const QString &scope);
    void editFinished(bool ok, const QString &category,
                      const QString &shortcode);
    void roomPackToggleFinished(bool ok, const QString &category,
                                bool enabled);
    void migrationFinished(bool ok, const QString &category,
                           const QVariantMap &detail);
    void transferFinished(const QString &kind, bool ok,
                          const QString &category, int added, int skipped,
                          int failed, const QString &name);

private:
    struct BatchItem {
        QString shortcode;
        QString body;
        QString url;
        QString path;
        QString mimetype;
        QString usage;
        QString roomId;
        QString stateKey;
        QString ns;
        QString reuseUrl;
    };
    struct Transfer {
        bool active = false;
        QString kind;
        QString name;
        int total = 0;
        int done = 0;
        int added = 0;
        int skipped = 0;
        int failed = 0;
        QString category;
        QString firstCode;
        QList<BatchItem> items;
        int index = 0;
        quint64 op = 0;
        QSet<QString> taken;
        QString lastUploaded;
        QList<BatchItem> retry;
        stickers::Pack frozen;
        QString packId;
        QString destination;
        QHash<quint64, int> pending;
        QList<QByteArray> bytes;
        int nextFetch = 0;
        quint64 generation = 0;
    };
    Transfer m_transfer;
    quint64 m_transferGeneration = 0;
    QTimer *m_transferWatchdog = nullptr;
    std::unique_ptr<QTemporaryDir> m_importDir;

    void startUpload(const QUrl &fileUrl, const QString &shortcode,
                     const QString &usage);
    void beginTransfer(const QString &kind, const QString &name, int total);
    void finishTransfer(bool ok, const QString &category);
    void advanceBatch();
    void onBatchItemFinished(bool ok, const QString &category,
                             const QString &shortcode);
    void pumpExport();
    void finishExport();
    void onMediaFetched(quint64 opId, const QByteArray &bytes);
    void onMediaFetchFailed(quint64 opId);
    void startBatch(const QString &kind, const QString &name,
                    const QList<BatchItem> &items, int alreadySkipped);
    QSet<QString> userPackShortcodes() const;

    QTimer *m_activeRoomFetch = nullptr;
    std::unique_ptr<QTemporaryDir> m_convertedDir;
    quint64 m_nextConverted = 1;
    void onPacksReceived(quint64 opId, const QString &roomId,
                         bool roomCanManage, const QVariantList &packs);
    void onSaveFinished(quint64 opId, bool ok, const QString &category,
                        const QString &shortcode);
    void onEditFinished(quint64 opId, bool ok, const QString &category,
                        const QString &shortcode);
    void editPack(const QString &packId, const QString &action,
                  const QString &argA, const QString &argB);
    void onRoomsSet(quint64 opId, bool ok, const QString &category,
                    const QString &roomId, const QString &stateKey,
                    bool enabled, const QString &mirror);
    void onPacksChanged(const QString &roomId);
    void refreshWith(bool deep);
    struct RoomPackTarget {
        QString stateKey;
        QString ns;
    };
    RoomPackTarget roomSaveTarget(const QString &roomId) const;
    QString freshStateKey(const QString &roomId) const;
    void onLoggedOut();
    void applySnapshot(const QString &roomId, bool roomCanManage,
                       const QVariantList &packs);
    void rebuildImages();
    void emitStateChanged();
    QVariantList collectEmoticons(const QString &prefix, int limit,
                                  bool uniqueShortcodes) const;

    MatrixClient *m_client = nullptr;
    StickerPackModel *m_packs = nullptr;
    StickerImageModel *m_images = nullptr;
    StickerImageModel *m_editorImages = nullptr;

    QString m_activeRoomId;
    QString m_snapshotRoomId;
    bool m_loaded = false;
    bool m_stale = true;
    bool m_snapshotRoomCanManage = false;

    QString m_selectedPackId;
    QString m_editorPackId;
    QString m_usage = QStringLiteral("sticker");

    quint64 m_nextOpId = 1;
    quint64 m_fetchOp = 0;
    quint64 m_saveOp = 0;
    quint64 m_editOp = 0;
    QString m_saveScope;
    quint64 m_roomsOp = 0;
    quint64 m_migrateOp = 0;
    QList<BatchItem> m_failedUploads;
    std::function<void(const QString &)> m_privateMediaFetch;
    QString m_republishKey;
    QString m_republishAccount;
    // The decrypted copy being uploaded, removed when its upload answers.
    std::unique_ptr<QTemporaryDir> m_republishDir;
    quint64 m_republishOp = 0;
    void dropRepublishScratch();
    QString m_failedUploadsAccount;
    static QString localGroupFor(const QString &userId);
    bool m_refreshOwed = false;
    bool m_refreshOwedDeep = false;
    QString m_deepRoomId;
    QTimer *m_changeRefresh = nullptr;
    QVariantMap m_pendingMeta;
    QVariantMap m_snapshotMeta;
    QStringList m_snapshotAncestors;
    QString m_localStatePath;
    QHash<QString, QVariantList> m_localMemory;
    QString localGroup() const;
    QVariantList readLocalList(const QString &name) const;
    void writeLocalList(const QString &name, const QVariantList &list);
    QVariantList markStale(QVariantList rows) const;
    bool m_lastMirrorFailed = false;

    QSet<QString> m_savedUrls;
    int m_revision = 0;
};
