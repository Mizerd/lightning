#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <memory>

class QMimeData;
class QTemporaryDir;

namespace lightning::svgraster {
struct Result;
}

class MatrixClient;
class StagedImageStore;
class VideoPosterExtractor;

class AttachmentQueueModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        LocalUrlRole,
        MimeRole,
        SizeBytesRole,
        SizeLabelRole,
        IsImageRole,
        StateRole,
        ErrorRole,
        PreviewSourceRole,
        RetryableRole,
        SpoilerRole,
        SpoilerAllowedRole,
    };

    struct Entry {
        QString localPath;
        QString sourcePath;
        bool converted = false;
        QByteArray data;
        QString fileName;
        QString mime;
        qint64 sizeBytes = 0;
        int width = 0;
        int height = 0;
        bool isImage = false;
        bool animated = false;
        // Token into StagedImageStore for in-memory (clipboard) images.
        // Released when the entry leaves the queue, whichever way it leaves.
        QString stagedToken;
        QString editPreviewPath;
        QString state = QStringLiteral("queued");
        QString error;
        bool retryable = true;
        quint64 opId = 0;
        bool committed = false;
        bool isVideo = false;
        bool isAudio = false;
        bool isSvg = false;
        bool posterPending = false;
        bool sendRequested = false;
        QString posterTag;
        QByteArray poster;
        int posterWidth = 0;
        int posterHeight = 0;
        qint64 durationMs = 0;
        QString caption;
        QString replyToEventId;
        QString threadRootId;
        bool spoiler = false;
    };

    explicit AttachmentQueueModel(QObject *parent = nullptr);
    ~AttachmentQueueModel() override;

    void setClient(MatrixClient *client);
    void setStagedImages(StagedImageStore *store) { m_stagedImages = store; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString addFile(const QUrl &fileUrl);
    static QString detectMime(const QString &path);

    // --- Dropped and pasted files -------------------------------------------
    // A drop or paste names files by URL, and the URL is not always one this
    // process can open: a sandboxed (Flatpak) app is handed the HOST path, which
    // does not exist inside the sandbox, and the file is reachable only through
    // the FileTransfer portal key the source put beside it.

    static QString portalKeyFrom(const QByteArray &raw);
    static QString portalKeyFromMimeData(const QMimeData *mime);
    static bool isDocumentPortalUrl(const QUrl &url);
    static QUrl normalizeDroppedUrl(const QUrl &url);
    using PortalRetrieveHook = std::function<void(
        const QString &key,
        std::function<void(bool ok, const QStringList &paths)> done)>;
    static void setPortalRetrieveHook(PortalRetrieveHook hook);
    static QString sandboxKind();
    static void setSandboxKindForTesting(const QString &kind);
    static void resolveDroppedUrls(
        const QList<QUrl> &urls, const QString &portalKey, QObject *context,
        std::function<void(const QList<QUrl> &resolved)> done);

    QString addImageData(const QByteArray &bytes, const QString &mime,
                         int width, int height);
    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void retryAt(int row);
    Q_INVOKABLE void setSpoilerAt(int row, bool spoiler);
    bool spoilerAllowed(const Entry &entry) const;
    Q_INVOKABLE QVariantMap renderImageEdit(int row, qreal left, qreal top,
        qreal right, qreal bottom, int quarterTurns, bool mirror,
        const QVariantList &strokes);
    Q_INVOKABLE bool commitImageEdit(int row);
    Q_INVOKABLE void cancelImageEdit(int row);

    bool isEmpty() const { return m_entries.isEmpty(); }
    QList<Entry> &entries() { return m_entries; }
    void updateEntry(int row);
    // Hands every entry over without releasing it; the caller owns them now.
    QList<Entry> takeAll();
    void restoreEntries(const QList<Entry> &entries);
    void discardEntries(const QList<Entry> &entries);

    static QString humanSize(qint64 bytes);

    qint64 uploadLimit() const;
    bool exceedsUploadLimit(qint64 bytes) const;
    QString uploadLimitMessage() const;

    using PosterRequestHook =
        std::function<void(const QString &tag, const QString &localPath)>;
    void setPosterRequestHook(PosterRequestHook hook);
    void applyPoster(const QString &tag, const QByteArray &poster,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

Q_SIGNALS:
    void countChanged();
    void entryPrepared(int row);

private:
    void startPosterJob(int row);
    void startSvgConversion(int row);
    void finishSvgConversion(const QString &tag,
                             const lightning::svgraster::Result &result);
    QString convertedDirectory();
    int rowForPosterTag(const QString &tag) const;

    // Drops an entry's staged-image registration. Every removal path must call
    // it, or the bytes are held for the whole session.
    void releaseStaged(const Entry &entry);

    MatrixClient *m_client = nullptr;
    StagedImageStore *m_stagedImages = nullptr;
    QList<Entry> m_entries;
    VideoPosterExtractor *m_posterExtractor = nullptr;
    PosterRequestHook m_posterHook;
    quint64 m_nextPosterTag = 1;
    std::unique_ptr<QTemporaryDir> m_convertedDir;
    quint64 m_nextConverted = 1;
};
