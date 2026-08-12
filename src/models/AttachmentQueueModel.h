#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QUrl>

#include <functional>

class MatrixClient;
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
    };

    explicit AttachmentQueueModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

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
    qint64 uploadLimit() const;
    void startPosterJob(int row);
    int rowForPosterTag(const QString &tag) const;

    MatrixClient *m_client = nullptr;
    QList<Entry> m_entries;
    // Created lazily on the first video, so a session that never attaches
    // one never constructs a QMediaPlayer.
    VideoPosterExtractor *m_posterExtractor = nullptr;
    PosterRequestHook m_posterHook;
    quint64 m_nextPosterTag = 1;
};
