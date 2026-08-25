#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QUrl>

#include <functional>

class MatrixClient;
class StagedImageStore;
class VideoPosterExtractor;

// v0.5.9: attachments prepared in the composer before sending.
//
// Entries come from the file picker, drag-and-drop, or clipboard image
// paste. Validation happens on add: regular readable non-empty file, MIME
// detected from *content* (QMimeDatabase), bounded against the server's
// m.upload.size when known. Nothing is uploaded until the user sends;
// dispatch itself is owned by MessageComposer. Clipboard images stay in
// memory (QByteArray) — no temporary file is ever written.
//
// v0.7: a queued VIDEO additionally gets a poster frame extracted from the
// file on add, so the outgoing Matrix event can carry a real thumbnail. The
// model owns that job; the dispatcher (MessageComposer / ThreadController)
// only waits for entryPrepared() before sending that one entry.
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
        // What QML should point an Image at to preview this entry BEFORE it
        // is sent: the file URL for a picked file, an
        // image://lightning-staged/<token> URL for clipboard bytes (which
        // have no file), and empty for anything that is not a still image.
        // LocalUrlRole is empty for pasted data, which is why the composer
        // chip showed a generic icon for every pasted screenshot.
        PreviewSourceRole,
    };

    struct Entry {
        QString localPath;   // empty for in-memory (clipboard) data
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
        // v0.7 video round: a picked video is postered locally so the
        // outgoing Matrix event carries a real thumbnail (see
        // startPosterJob). Extraction runs the moment the file is queued —
        // by the time the user presses send it is normally already done —
        // and `posterPending` holds the dispatch of THIS entry only until
        // it resolves either way. Extraction failure is not send failure:
        // the poster stays empty and the video sends without one.
        bool isVideo = false;
        bool posterPending = false;
        bool sendRequested = false;
        QString posterTag;
        QByteArray poster;      // JPEG bytes; empty when unavailable
        int posterWidth = 0;
        int posterHeight = 0;
        qint64 durationMs = 0;  // 0 when the decoder never reported one
        // The composer's typed text, when the user has asked for it to be
        // sent as this attachment's caption. Empty otherwise, which is the
        // previous behaviour exactly. It lives on the ENTRY rather than on
        // the composer because dispatch is NOT in row order — a video whose
        // poster is still being extracted is held back while the next entry
        // goes out — so "the composer's pending caption" would land on
        // whichever event happened to be dispatched first, and a retry of a
        // failed entry would lose it.
        QString caption;
    };

    explicit AttachmentQueueModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Where clipboard bytes are registered so QML can preview them. Optional:
    // without a store a pasted image simply has no preview, exactly as before.
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

    // The homeserver's advertised m.upload.size, or 0 when it is UNKNOWN
    // (not advertised, not answered yet, or the lookup failed). 0 is never
    // treated as unlimited and never replaced by a client-side default —
    // see the definition for why an invented ceiling is worse than none.
    qint64 uploadLimit() const;
    // True only when a real server limit is known AND `bytes` exceeds it.
    // Exactly at the limit is allowed. Shared by every send path so the
    // preflight cannot drift between composers.
    bool exceedsUploadLimit(qint64 bytes) const;
    // Human-readable refusal for an oversized payload, e.g. for a notice.
    // Only meaningful when exceedsUploadLimit() is true.
    QString uploadLimitMessage() const;

    // v0.7 video round. Test seam: replaces the offscreen video decoder so
    // a poster outcome can be exercised without a real media backend or a
    // real video file. The hook receives (tag, localPath) and is expected
    // to call applyPoster() with that tag later. Production never sets it.
    using PosterRequestHook =
        std::function<void(const QString &tag, const QString &localPath)>;
    void setPosterRequestHook(PosterRequestHook hook);
    // Deliver a poster outcome for a queued video. An empty `jpeg` is the
    // honest "no poster could be produced" answer: the entry still becomes
    // dispatchable, it simply carries no thumbnail.
    void applyPoster(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

Q_SIGNALS:
    void countChanged();
    // A queued entry finished preparing (today: its poster resolved) and is
    // now dispatchable. Carries the row as it stands at emit time.
    void entryPrepared(int row);

private:
    void startPosterJob(int row);
    int rowForPosterTag(const QString &tag) const;

    // Drops an entry's staged-image registration, whichever way it is
    // leaving the queue. Every removal path must call it: a token left
    // behind holds the bytes for the life of the session.
    void releaseStaged(const Entry &entry);

    MatrixClient *m_client = nullptr;
    StagedImageStore *m_stagedImages = nullptr;
    QList<Entry> m_entries;
    // Created lazily on the first video, so a session that never attaches
    // one never constructs a QMediaPlayer.
    VideoPosterExtractor *m_posterExtractor = nullptr;
    PosterRequestHook m_posterHook;
    quint64 m_nextPosterTag = 1;
};
