#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QTemporaryDir;

namespace lightning::svgraster {
struct Result;
}

class MatrixClient;
class StagedImageStore;
class VideoPosterExtractor;

// Attachments prepared in the composer before sending.
//
// Entries come from the file picker, drag-and-drop or clipboard paste.
// Validated on add: regular, readable, non-empty file, MIME detected from
// content, bounded by the server's m.upload.size when known. Nothing uploads
// until the user sends; MessageComposer owns dispatch. Clipboard images stay
// in memory; no temporary file is written.
//
// A queued video also gets a poster frame extracted on add so the event can
// carry a thumbnail. A queued SVG is CONVERTED: it is rasterised to a PNG in a
// helper process (media/SvgRaster.h, SvgRasterJob.h) and the entry becomes that
// PNG, a local file in a scratch directory with the PNG's real size and
// dimensions and mime image/png; the SVG is never uploaded, and never handed
// to a QML Image. Dispatchers wait for entryPrepared() for that entry. A
// refused conversion fails the entry with a sentence naming the reason.
class AttachmentQueueModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        LocalUrlRole,     // file:// URL for picked files; empty for pasted data
        MimeRole,
        SizeBytesRole,
        SizeLabelRole,
        IsImageRole,
        StateRole,        // "queued" | "dispatching" | "failed"
        ErrorRole,
        // Image source for previewing the entry before sending: the file URL
        // for a picked file, image://lightning-staged/<token> for clipboard
        // bytes, empty for anything that is not a still image. An SVG
        // previews as its rendered PNG, and as nothing until that exists.
        PreviewSourceRole,
    };

    struct Entry {
        QString localPath;   // empty for in-memory (clipboard) data
        // The SVG the user picked, once localPath names its converted PNG.
        // Kept only to refuse the same file twice.
        QString sourcePath;
        // localPath is a PNG this model wrote into its scratch directory and
        // removes with the entry.
        bool converted = false;
        QByteArray data;     // clipboard image bytes; empty for files
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
        QString state = QStringLiteral("queued");
        QString error;
        quint64 opId = 0;    // set while dispatching
        // Videos are postered locally as soon as they are queued (see
        // startPosterJob); `posterPending` holds only this entry's dispatch
        // until it resolves. Extraction failure is not send failure: the video
        // goes out without a poster.
        bool isVideo = false;
        // Decoded for its DURATION only; there is no poster to grab.
        bool isAudio = false;
        // image/svg+xml (or SVGZ) not yet converted. Never dispatched: the
        // conversion replaces it with a PNG file and clears this.
        bool isSvg = false;
        bool posterPending = false;
        bool sendRequested = false;
        QString posterTag;
        // JPEG (video) bytes; empty when unavailable.
        QByteArray poster;
        int posterWidth = 0;
        int posterHeight = 0;
        qint64 durationMs = 0;  // 0 when the decoder never reported one
        // The composer text to send as this attachment's caption, or empty.
        // Stored on the entry because dispatch is not in row order (a video
        // awaiting its poster is held back) and a retry must keep it.
        QString caption;
        // The event this attachment replies to, captured when the user sent
        // (the composer clears its reply state right after dispatch, but a
        // video waiting for its poster dispatches later). Empty for a plain
        // send. In a thread it is the thread message being replied to.
        QString replyToEventId;
    };

    explicit AttachmentQueueModel(QObject *parent = nullptr);
    ~AttachmentQueueModel() override;

    void setClient(MatrixClient *client);
    // Where clipboard bytes are registered for preview. Optional: without it a
    // pasted image has no preview.
    void setStagedImages(StagedImageStore *store) { m_stagedImages = store; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Add a local file (picker / drop). Returns an empty string on success
    // or a safe, user-facing reason on rejection. Never logs the path.
    Q_INVOKABLE QString addFile(const QUrl &fileUrl);
    // Add clipboard image bytes (already encoded, e.g. PNG).
    QString addImageData(const QByteArray &bytes, const QString &mime,
                         int width, int height);
    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void retryAt(int row);

    bool isEmpty() const { return m_entries.isEmpty(); }
    QList<Entry> &entries() { return m_entries; }
    void updateEntry(int row);

    static QString humanSize(qint64 bytes);

    // The homeserver's m.upload.size, or 0 when unknown. 0 is never treated as
    // unlimited nor replaced by a client-side default; see the definition.
    qint64 uploadLimit() const;
    // True only when a real server limit is known and `bytes` exceeds it (the
    // limit itself is allowed). Shared by every send path.
    bool exceedsUploadLimit(qint64 bytes) const;
    // Human-readable refusal for an oversized payload, e.g. for a notice.
    // Only meaningful when exceedsUploadLimit() is true.
    QString uploadLimitMessage() const;

    // Test seam replacing the video decoder: receives (tag, localPath) and
    // later calls applyPoster() with that tag. Never set in production.
    using PosterRequestHook =
        std::function<void(const QString &tag, const QString &localPath)>;
    void setPosterRequestHook(PosterRequestHook hook);
    // Deliver a poster outcome (a video frame). An empty
    // `poster` means none could be made; the entry becomes dispatchable
    // without a thumbnail.
    void applyPoster(const QString &tag, const QByteArray &poster,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

Q_SIGNALS:
    void countChanged();
    // A queued entry finished preparing (its poster resolved) and can be
    // dispatched.
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
    // Created lazily on the first video so sessions without one never construct
    // a QMediaPlayer.
    VideoPosterExtractor *m_posterExtractor = nullptr;
    PosterRequestHook m_posterHook;
    quint64 m_nextPosterTag = 1;
    // Converted SVGs live here, one numbered subdirectory each so two files
    // with the same name never collide.
    std::unique_ptr<QTemporaryDir> m_convertedDir;
    quint64 m_nextConverted = 1;
};
