#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

class AccountActivityModel;

// Labs (live accounts): what the OTHER live accounts received that would have
// notified, newest first, for the Activity panel's "From your other accounts"
// list. Held in memory only, never persisted (§6).
//
// A child of the AccountActivityModel it is given, so it exists exactly while
// Labs is on. An account's rows go as soon as that account stops being a
// Running background account (switched to, paused, stopped, removed), and
// add() refuses an account that is not Running or whose host epoch is not the
// pool's current one, so a retired host's late event never lands here.
class UnifiedInboxModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("UnifiedInboxModel is exposed via app.unifiedInbox")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        AccountUserIdRole = Qt::UserRole + 1,
        AccountLabelRole,
        RoomIdRole,
        RoomNameRole,
        SenderIdRole,
        SenderNameRole,
        PreviewRole,
        UndecryptableRole,
        TimestampMsRole,
        EventIdRole,
        ThreadRootIdRole,
        HighlightRole,
    };

    struct Entry {
        QString accountUserId;
        QString accountLabel;
        QString roomId;
        QString roomName;
        QString senderId;
        QString senderName;
        QString preview;
        QString eventId;
        QString threadRootId;
        qint64 timestampMs = 0;
        bool highlight = false;
        bool undecryptable = false;
        // Older than the account's catch-up floor: kept only while its room is unread.
        bool backlog = false;
    };

    static constexpr int kMaxEntries = 100;

    explicit UnifiedInboxModel(AccountActivityModel *activity);

    // The pool's current epoch for an account; unset accepts any epoch.
    void setEpochSource(std::function<quint64(const QString &)> epochOf);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(m_entries.size()); }

    // A second copy of an event (its late decryption) replaces the first.
    bool add(quint64 epoch, const Entry &entry);
    void dropAccount(const QString &uid);
    void dropRoom(const QString &uid, const QString &roomId);
    void dropEvent(const QString &uid, const QString &eventId);
    void dropBacklogOutside(const QString &uid, const QSet<QString> &unreadRooms);

    Q_INVOKABLE QVariantMap entryAt(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    bool accepts(const QString &uid, quint64 epoch) const;
    void dropWhere(const std::function<bool(const Entry &)> &drop);

    AccountActivityModel *m_activity;
    std::function<quint64(const QString &)> m_epochOf;
    QList<Entry> m_entries;
};
