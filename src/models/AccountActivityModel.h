#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

// Labs (concurrent live accounts): one row per account the pool or the
// foreground has reported on, carrying a background account's unread and
// mention SUMMARY (integers only, never a room id, a sender or a body) and
// its live state. The hosts feed it; QML reads badges from it.
//
// Rules this model enforces so its feeders cannot get them wrong:
//  * Epochs. Every summary is stamped with the account's epoch, which the pool
//    bumps on every retire. A summary older than the row's epoch is a late
//    result from a previous host and is refused (setSummary() returns false).
//  * Removal is a tombstone. removeAccount() refuses every later
//    setSummary() and setLiveState() for that account, whether or not it had
//    a row, until reopenAccount() lifts it. A retiring host's last state or
//    summary therefore cannot bring a removed account's row back, and a
//    re-added account starts clean (§9: a late callback never mutates the
//    next account).
//  * One row at a time. A change is one row's dataChanged; the model is never
//    reset, so a badge update cannot rebuild a delegate list.
//  * A summary counts only while the row is Running: a paused, stopped or
//    non-live account reads zero everywhere (the roles, activityFor() and the
//    totals).
//
// Only AppController constructs it, and only while Labs is active. GUI thread.
class AccountActivityModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AccountActivityModel is exposed via app.accountActivity")
    // Accounts other than the active one with something to show (an unread
    // room, an invite or a mention). The rail's "new messages on other
    // accounts" dot.
    Q_PROPERTY(int otherAccountsWithActivity READ otherAccountsWithActivity
                   NOTIFY totalsChanged)

public:
    enum LiveState {
        NotLive = 0,
        Active,
        Starting,
        Running,
        Paused,
        NeedsSignIn,
        KeyringLocked,
        Stopped,
        StoreBusy,
        EncryptionDataMissing,
        OpenToFinishSetup,
        Stopping
    };
    Q_ENUM(LiveState)

    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        LiveStateRole,
        MentionCountRole,
        HasUnreadRole,
        MentionsLowerBoundRole,
        UnreadRoomsRole,
        InvitesRole,
        UpdatedAtMsRole
    };

    struct Summary {
        int unreadRooms = 0;
        int mentions = 0;
        int invites = 0;
        bool mentionsLowerBound = false;
        bool operator==(const Summary &o) const
        {
            return unreadRooms == o.unreadRooms && mentions == o.mentions
                && invites == o.invites && mentionsLowerBound == o.mentionsLowerBound;
        }
        bool operator!=(const Summary &o) const { return !(*this == o); }
    };

    struct Totals {
        int unreadRooms = 0;
        int mentions = 0;
        int accountsWithActivity = 0;
        bool operator==(const Totals &o) const
        {
            return unreadRooms == o.unreadRooms && mentions == o.mentions
                && accountsWithActivity == o.accountsWithActivity;
        }
        bool operator!=(const Totals &o) const { return !(*this == o); }
    };

    explicit AccountActivityModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Ignored (returns false) when epoch < the row's epoch, for an account
    // without a row below its floor, and for a removed account. An accepted
    // summary raises the row's epoch to `epoch`. Creates the row (NotLive)
    // when absent. One-row dataChanged; never a reset. Negative counts read 0.
    bool setSummary(const QString &uid, quint64 epoch, const Summary &s);
    // Creates the row when absent, at the floor. Ignored for a removed account.
    void setLiveState(const QString &uid, LiveState state);
    // Zero the summary and raise the row's epoch to newEpoch, keeping the row
    // and its live state; ignored when newEpoch is below the row's epoch. For
    // an account without a row it only raises the floor. Never lifts a
    // removal.
    void resetAccount(const QString &uid, quint64 newEpoch);
    // Drops the row (if any) and tombstones the account: every setSummary()
    // and setLiveState() for it is refused until reopenAccount(). The floor
    // rises to the removed row's epoch (not past it: an account removed while
    // not live has no retire to bump the pool's epoch, and the reopen must
    // still be accepted at epochFor(uid)).
    void removeAccount(const QString &uid);
    // The ONLY way to lift a removal: a new host for a re-added account, at
    // its start, with the pool's current epochFor(uid). Refused (false) when
    // epoch is below the floor; the floor then becomes epoch. True and no
    // change for an account that was never removed.
    bool reopenAccount(const QString &uid, quint64 epoch);
    bool isRemoved(const QString &uid) const { return m_removed.contains(uid); }
    // Excluded from otherAccountsWithActivity (the foreground has its own
    // surfaces).
    void setActiveUserId(const QString &uid);
    QString activeUserId() const { return m_activeUserId; }

    // Summed over Running rows other than `uid` (the tray: AppController).
    Totals totalsExcluding(const QString &uid) const;
    int otherAccountsWithActivity() const;
    // NotLive for an account without a row.
    LiveState liveStateOf(const QString &uid) const;
    // The row's epoch, or the floor for an account without one.
    quint64 epochOf(const QString &uid) const;

    // {mentionCount, hasUnread, mentionsLowerBound, invites, liveState}, the
    // same values the roles read. For Connections handlers and onCompleted
    // ONLY, never inside a binding.
    Q_INVOKABLE QVariantMap activityFor(const QString &uid) const;

    // Test seam: the wall clock stamped into updatedAtMs. nullptr restores
    // QDateTime::currentMSecsSinceEpoch.
    void setClockForTest(std::function<qint64()> clock);

Q_SIGNALS:
    // activityFor(userId) changed (a summary, or a state that hides or shows it).
    void summaryChanged(const QString &userId);
    void liveStateChanged(const QString &userId);
    // totalsExcluding(activeUserId()) changed.
    void totalsChanged();

private:
    struct Row {
        QString userId;
        quint64 epoch = 0;
        Summary summary;
        LiveState liveState = NotLive;
        qint64 updatedAtMs = 0;
    };

    int indexOf(const QString &uid) const;
    int insertRow(const QString &uid, quint64 epoch);
    // What the roles show: the summary while Running, zeros otherwise.
    static Summary visibleSummary(const Row &row);
    static QVariantMap activityOf(const Row *row);
    void emitRowChanged(int row);
    void refreshTotals();
    qint64 now() const;

    QVector<Row> m_rows;
    // uid -> the lowest epoch a row for it may be (re)created at. Raised by
    // removeAccount() to the removed row's epoch, by resetAccount() on an
    // account with no row, and set by reopenAccount().
    QHash<QString, quint64> m_floor;
    // Removed accounts; only reopenAccount() takes one out.
    QSet<QString> m_removed;
    QString m_activeUserId;
    Totals m_totals;
    std::function<qint64()> m_clock;
};
