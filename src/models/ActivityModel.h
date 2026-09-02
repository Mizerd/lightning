#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class MatrixClient;

// v0.9 (phase 2): the global Activity Center.
//
// One account-wide list of the things that were addressed TO the user,
// across every room: mentions (@user and @room), replies to the user's
// messages, replies in threads the user started or took part in, reactions
// to the user's messages, room invites, and keyword highlights. It is fed
// INCREMENTALLY from the same `eventAppended` tap NotificationManager uses
// (AppController), plus the invite diff and a reaction lane, and it never
// scans timelines on its own.
//
// Seen state is its OWN: the badge counts entries newer than a per-account
// "seen up to" marker (plus per-entry marks), independent of read receipts
// — reading a room does not silently clear the Activity list, and opening
// the Activity list sends no receipt.
//
// PRIVACY: an entry's preview can be decrypted plaintext. Entries are
// memory-only and die with the session; the ONLY persisted state is the
// seen marker and the keyword list, through the `Store` callbacks
// (account-scoped in SettingsManager). Previews are never logged.
class ActivityModel : public QAbstractListModel
{
    Q_OBJECT
    // Rows after the filter.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Entries not yet seen, filter-independent — the badge.
    Q_PROPERTY(int unseenCount READ unseenCount NOTIFY unseenCountChanged)
    // "all" | "mentions" | "replies" | "threads" | "reactions" | "invites"
    // | "keywords"
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Case-insensitive whole-word highlights (bounded; see kMaxKeywords).
    Q_PROPERTY(QStringList keywords READ keywords WRITE setKeywords
                   NOTIFY keywordsChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        KindRole,          // mention | room_mention | reply | thread | reaction | invite | keyword
        RoomIdRole,
        RoomNameRole,
        SenderIdRole,
        SenderNameRole,
        PreviewRole,       // bounded plain text; "" when unavailable
        TimestampMsRole,
        EventIdRole,       // the event to navigate to ("" for an invite)
        ThreadRootIdRole,
        SeenRole,
        EncryptedRole,     // preview withheld because the event is undecryptable
        ReactionKeyRole,   // reaction entries: the key
    };
    Q_ENUM(Roles)

    struct Store {
        std::function<QVariantMap()> load;             // {seenUpToMs, keywords}
        std::function<void(const QVariantMap &)> save;
    };

    static constexpr int kMaxEntries = 400;
    static constexpr int kMaxKeywords = 32;
    static constexpr int kMaxKeywordLength = 64;
    static constexpr int kPreviewChars = 140;

    explicit ActivityModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setStore(Store store);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_visible.size(); }
    int unseenCount() const;
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QStringList keywords() const { return m_keywords; }
    void setKeywords(const QStringList &keywords);

    // ---- feeds (AppController). `event` must already be the ROOM copy
    // (thread-timeline duplicates filtered by the caller, as for
    // notifications). Returns true when an entry was added.
    bool ingest(const TimelineEvent &event, const QString &roomName);
    // A reaction event from any room (sender, key, target). Only reactions
    // to the user's OWN messages become entries.
    bool noteReaction(const QString &roomId, const QString &roomName,
                      const QString &reactionEventId, const QString &targetEventId,
                      const QString &senderId, const QString &senderName,
                      const QString &key, qint64 timestampMs);
    // A room the user has been invited to (idempotent per room).
    bool noteInvite(const RoomInfo &room);
    // The invite was answered (accepted or declined): drop its entry.
    void inviteResolved(const QString &roomId);
    // Entries the server remembers (GET /notifications) for a fresh
    // session, oldest first; ids already present are skipped.
    void seed(const QVariantList &entries);

    Q_INVOKABLE void markAllSeen();
    Q_INVOKABLE void markSeen(const QString &id);
    // Navigate: emits openRequested with the exact target and marks seen.
    Q_INVOKABLE void open(const QString &id);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    // Pure classifier, exposed for tests: the kind an event would get, or
    // "" when it is not activity for `selfUserId`.
    static QString classify(const TimelineEvent &event, const QString &selfUserId,
                            const QSet<QString> &ownEventIds,
                            const QSet<QString> &ownThreadRoots,
                            const QStringList &keywords);
    static bool matchesKeyword(const QString &body, const QString &keyword);

Q_SIGNALS:
    void countChanged();
    void unseenCountChanged();
    void filterChanged();
    void keywordsChanged();
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);

private:
    struct Entry {
        QString id;
        QString kind;
        QString roomId;
        QString roomName;
        QString senderId;
        QString senderName;
        QString preview;
        qint64 timestampMs = 0;
        QString eventId;
        QString threadRootId;
        QString reactionKey;
        bool encrypted = false;
        bool seenMark = false;
    };

    bool isSeen(const Entry &e) const;
    bool passesFilter(const Entry &e) const;
    void rebuildVisible();
    bool addEntry(Entry entry); // false = already listed
    void rememberOwn(const TimelineEvent &event);
    void loadStore();
    void saveStore();
    static QString previewOf(const TimelineEvent &event);

    MatrixClient *m_client = nullptr;
    Store m_store;
    QList<Entry> m_entries;      // newest first
    QList<int> m_visible;        // indexes into m_entries after the filter
    QSet<QString> m_ids;
    QSet<QString> m_ownEventIds;
    QList<QString> m_ownEventOrder; // bounded LRU order for m_ownEventIds
    QSet<QString> m_ownThreadRoots;
    QString m_filter = QStringLiteral("all");
    QStringList m_keywords;
    qint64 m_seenUpToMs = 0;
    bool m_storeLoaded = false;
};
