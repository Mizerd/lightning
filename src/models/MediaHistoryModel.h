#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QPointer>
#include <QVariantMap>
#include <QVector>

#include "matrix/MatrixClient.h"

class MediaHistoryModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory
                   NOTIFY filtersChanged)
    Q_PROPERTY(QString senderFilter READ senderFilter WRITE setSenderFilter
                   NOTIFY filtersChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY filtersChanged)
    Q_PROPERTY(QDateTime fromDate READ fromDate WRITE setFromDate
                   NOTIFY filtersChanged)
    Q_PROPERTY(QDateTime toDate READ toDate WRITE setToDate
                   NOTIFY filtersChanged)

    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool walking READ walking NOTIFY stateChanged)
    Q_PROPERTY(bool complete READ complete NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool encryptedRoom READ encryptedRoom NOTIFY stateChanged)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY countsChanged)
    Q_PROPERTY(int loadedCount READ loadedCount NOTIFY countsChanged)
    Q_PROPERTY(qint64 scannedTotal READ scannedTotal NOTIFY countsChanged)
    Q_PROPERTY(qint64 undecryptableCount READ undecryptableCount
                   NOTIFY countsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
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
        DateGroupRole,
    };
    Q_ENUM(Roles)

    explicit MediaHistoryModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    void retranslate();

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
    Q_INVOKABLE bool setDateRange(const QString &from, const QString &to);
    qint64 fromBoundMs() const;
    qint64 toBoundMs() const;

    bool loading() const { return m_pendingOp != 0; }
    bool complete() const { return m_complete; }
    bool walking() const { return m_walking; }

    static constexpr int kMaxWalkPages = 50;
    static constexpr int kMaxFillPages = 8;
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
    Q_INVOKABLE void loadAll();
    /// Stop a loadAll() walk. A page already requested still lands (there is
    /// no cancel verb on the wire) but nothing further is asked for.
    Q_INVOKABLE void cancelWalk();
    Q_INVOKABLE void fillViewport();
    Q_INVOKABLE void reload();
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    Q_INVOKABLE QVariantList imageEntries() const;
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
        QString mediaKey;
        bool encrypted = false;
        QString url;
        QString host;
        bool gif = false;
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

    QVector<Entry> m_all;
    QVector<int> m_shown;
    quint64 m_pendingOp = 0;
    bool m_restartNext = true;
    bool m_complete = false;
    bool m_walking = false;
    int m_walkPages = 0;
    int m_fillPages = 0;
    bool m_encryptedRoom = false;
    qint64 m_scannedTotal = 0;
    qint64 m_undecryptable = 0;
    QString m_lastError;
    QSet<QString> m_seen;
};
