#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class MatrixClient;

// v0.7.x personal moderation: the account's ignored users
// (m.ignored_user_list — SDK-owned Matrix account data, never a
// Lightning-local database) and message reporting (stable /v3 endpoint).
//
// The ignored set has one source of truth: the account data. Local writes
// and remote changes (another client) both land through the client's
// ignoredUsersChanged push, and the cached set here exists only so
// isIgnored() is cheap for menus and the notification race-window guard.
// Cleared on sign-out so one account's list can never bleed into another.
class ModerationController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool reportSupported READ reportSupported NOTIFY stateChanged)
    Q_PROPERTY(QStringList ignoredUsers READ ignoredUsers NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // Bump-on-change revision for QML surfaces that call isIgnored() —
    // a plain binding cannot observe a Q_INVOKABLE's inputs.
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)
    // The report prompt opens itself off this state (one dialog instance,
    // reachable from every message menu).
    Q_PROPERTY(bool reportPromptActive READ reportPromptActive NOTIFY reportPromptChanged)

public:
    explicit ModerationController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool reportSupported() const;
    QStringList ignoredUsers() const;
    bool busy() const { return m_ignoreOp != 0 || m_reportOp != 0; }
    int revision() const { return m_revision; }
    bool reportPromptActive() const { return m_reportPromptActive; }

    Q_INVOKABLE bool isIgnored(const QString &userId) const
    { return m_ignored.contains(userId); }
    Q_INVOKABLE void ignoreUser(const QString &userId);
    Q_INVOKABLE void unignoreUser(const QString &userId);
    Q_INVOKABLE void refreshIgnoredUsers();

    Q_INVOKABLE void beginReport(const QString &roomId, const QString &eventId);
    Q_INVOKABLE void submitReport(const QString &reason);
    Q_INVOKABLE void cancelReport();

Q_SIGNALS:
    void stateChanged();
    void reportPromptChanged();
    // Terminal outcome of an ignore/unignore write.
    void ignoreActionFinished(const QString &userId, bool ignored, bool ok,
                              const QString &message);
    // Terminal outcome of a report submission.
    void reportFinished(bool ok, const QString &message);

private Q_SLOTS:
    void onIgnoreFinished(quint64 opId, const QString &userId, bool ignored,
                          bool ok, const QString &category);
    void onIgnoredUsersReceived(quint64 opId, bool ok,
                                const QStringList &users);
    void onIgnoredUsersChanged(const QStringList &users);
    void onReportFinished(quint64 opId, const QString &roomId,
                          const QString &eventId, bool ok,
                          const QString &category);
    void onInitialSyncDoneChanged();
    void onLoggedOut();

private:
    void applyIgnoredList(const QStringList &users);
    static QString describeCategory(const QString &category);

    MatrixClient *m_client = nullptr;
    QSet<QString> m_ignored;
    QStringList m_ignoredOrdered;
    int m_revision = 0;
    quint64 m_ignoreOp = 0;
    quint64 m_listOp = 0;
    quint64 m_reportOp = 0;
    bool m_initialListLoaded = false;

    bool m_reportPromptActive = false;
    QString m_reportRoomId;
    QString m_reportEventId;
};
