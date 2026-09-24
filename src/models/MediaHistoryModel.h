#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QPointer>
#include <QVariantMap>
#include <QVector>

#include "matrix/MatrixClient.h"

/// A room's media, files and links, browsed independently of the timeline via
/// its own backwards walk (rust/src/mediahistory.rs), so older attachments are
/// reachable without moving the reader.
///
/// Completeness is reported, never implied: `loadedCount` is not a total;
/// `complete` means the walk reached the start of accessible history;
/// `scannedTotal` counts events examined; `undecryptableCount` counts history
/// that exists but cannot be read.
///
/// Filters run over what has been loaded (the server cannot filter encrypted
/// rooms), and the UI says how much history a filtered view has seen.
class MediaHistoryModel : public QAbstractListModel
{
    Q_OBJECT

    /// The room being browsed. Setting it restarts the walk.
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    /// "" (everything) or one of image/video/audio/voice/file/link.
    /// "media" is the combined visual view: image + video.
    Q_PROPERTY(QString category READ category WRITE setCategory
                   NOTIFY filtersChanged)
    /// A Matrix user id, or "" for every sender.
    Q_PROPERTY(QString senderFilter READ senderFilter WRITE setSenderFilter
                   NOTIFY filtersChanged)
    /// Matches filename, caption/body, sender, link URL and host, and mimetype.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY filtersChanged)
    /// Inclusive day bounds, or invalid for "no bound".
    Q_PROPERTY(QDateTime fromDate READ fromDate WRITE setFromDate
                   NOTIFY filtersChanged)
    Q_PROPERTY(QDateTime toDate READ toDate WRITE setToDate
                   NOTIFY filtersChanged)

    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    /// The walk reached the start of accessible history.
    Q_PROPERTY(bool complete READ complete NOTIFY stateChanged)
    /// The backend cannot walk history at all (no Rust backend).
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool encryptedRoom READ encryptedRoom NOTIFY stateChanged)
    /// Rows currently shown, after filters.
    Q_PROPERTY(int shownCount READ shownCount NOTIFY countsChanged)
    /// Rows held, before filters.
    Q_PROPERTY(int loadedCount READ loadedCount NOTIFY countsChanged)
    Q_PROPERTY(qint64 scannedTotal READ scannedTotal NOTIFY countsChanged)
    Q_PROPERTY(qint64 undecryptableCount READ undecryptableCount
                   NOTIFY countsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    /// Every sender seen so far, for the sender filter's menu.
    Q_PROPERTY(QStringList knownSenders READ knownSenders NOTIFY countsChanged)

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        TimestampMsRole,
        KindRole,
        BodyRole,
        FilenameRole,
        MimetypeRole,
        SizeRole,
        WidthRole,
        HeightRole,
        DurationMsRole,
        MxcRole,
        ThumbnailMxcRole,
        EncryptedRole,
        MediaKeyRole,
        UrlRole,
        HostRole,
        /// "Today" / "Yesterday" / "This week" / "March 2026" / "Older",
        /// resolved against the viewer's own locale and time zone.
        DateGroupRole,
    };
    Q_ENUM(Roles)

    explicit MediaHistoryModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString category() const { return m_category; }
    void setCategory(const QString &category);
    QString senderFilter() const { return m_sender; }
    void setSenderFilter(const QString &sender);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QDateTime fromDate() const { return m_from; }
    void setFromDate(const QDateTime &from);
    QDateTime toDate() const { return m_to; }
    void setToDate(const QDateTime &to);

    bool loading() const { return m_pendingOp != 0; }
    bool complete() const { return m_complete; }
    bool available() const;
    bool encryptedRoom() const { return m_encryptedRoom; }
    int shownCount() const { return int(m_shown.size()); }
    int loadedCount() const { return int(m_all.size()); }
    qint64 scannedTotal() const { return m_scannedTotal; }
    qint64 undecryptableCount() const { return m_undecryptable; }
    QString lastError() const { return m_lastError; }
    QStringList knownSenders() const;

    /// Fetch the next page. A no-op while one is in flight or once the walk
    /// is complete, so a view that calls it on every scroll cannot storm the
    /// homeserver.
    Q_INVOKABLE void loadMore();
    /// Begin again at the live edge — for a manual refresh.
    Q_INVOKABLE void reload();
    /// The row's entry as a map, for handing to the media/jump paths.
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    /// Every shown image row, in view order, shaped like
    /// TimelineModel::imageEntries(). The image viewer takes this list and an
    /// index, since this model reaches media the timeline never loaded.
    Q_INVOKABLE QVariantList imageEntries() const;
    /// Index into imageEntries() of shown row `row`, or -1 if it is not an
    /// image. Computed here so ordering cannot disagree with QML.
    Q_INVOKABLE int imageIndexForRow(int row) const;

Q_SIGNALS:
    void roomIdChanged();
    void filtersChanged();
    void stateChanged();
    void countsChanged();
    void availableChanged();

private:
    struct Entry {
        QString eventId;
        QString sender;
        qint64 timestampMs = 0;
        QString kind;
        QString body;
        QString filename;
        QString mimetype;
        qint64 size = 0;
        int width = 0;
        int height = 0;
        qint64 durationMs = 0;
        QString mxc;
        QString thumbnailMxc;
        /// Media registry key a tile fetches through (the event id as the Rust
        /// scanner registered it); empty for rows without a source.
        QString mediaKey;
        bool encrypted = false;
        QString url;
        QString host;
    };

    void onPage(quint64 opId, const QString &roomId,
                const QVariantList &entries, qint64 scanned,
                qint64 scannedTotal, qint64 undecryptable, bool complete,
                bool encryptedRoom);
    void onFailed(quint64 opId, const QString &roomId, const QString &message);
    void rebuild();
    bool matches(const Entry &entry) const;
    void clearAll();

    QPointer<MatrixClient> m_client;
    QString m_roomId;
    QString m_category;
    QString m_sender;
    QString m_query;
    QDateTime m_from;
    QDateTime m_to;

    QVector<Entry> m_all;      // everything loaded, newest first
    QVector<int> m_shown;      // indices into m_all that pass the filters
    quint64 m_pendingOp = 0;
    bool m_complete = false;
    bool m_encryptedRoom = false;
    qint64 m_scannedTotal = 0;
    qint64 m_undecryptable = 0;
    QString m_lastError;
    /// Guards against duplicates: pages can overlap, and a link event yields
    /// one row per URL.
    QSet<QString> m_seen;
};
