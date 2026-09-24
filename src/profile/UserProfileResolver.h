#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

class MatrixClient;

// The global profile (display name, avatar) of any Matrix user, asked once
// and remembered for the session. For users the room's member snapshot cannot
// name: a mentioned non-member, a lazily loaded member, a sender whose room
// members were never fetched. Mention pills and the profile popover use it; a
// room's own member name always wins.
//
// profile() never asks. lookup()/request() ask at most once per user per
// session; a failure is re-asked only after failureRetryMs. /profile gets a
// user id and no room context.
//
// The session ends at sign-out (MatrixClient::loggedOut, also emitted by an
// account switch via detachSession()), since the client object is never
// replaced.
class UserProfileResolver : public QObject
{
    Q_OBJECT

public:
    struct Profile {
        QString displayName;
        QString avatarUrl;
        bool known = false; // an answer has arrived, even an empty one
    };

    explicit UserProfileResolver(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void clear();

    // Cached only; never asks.
    Profile profile(const QString &userId) const;

    // Cached answer as a QML map {displayName, avatarUrl, known}; asks the
    // server when nothing is cached yet.
    Q_INVOKABLE QVariantMap lookup(const QString &userId);
    Q_INVOKABLE void request(const QString &userId);

    int inFlightCount() const { return m_inFlight.size(); }
    int cachedCount() const { return m_profiles.size(); }
    void setFailureRetryForTest(int ms) { m_failureRetryMs = ms; }

Q_SIGNALS:
    void resolved(const QString &userId, const QString &displayName,
                  const QString &avatarUrl);

private:
    void onFinished(quint64 opId, bool ok, const QString &userId,
                    const QString &displayName, const QString &avatarUrl,
                    const QString &category);

    // Bounded: past this many distinct users the resolver stops asking.
    static constexpr int kMaxProfiles = 4000;

    MatrixClient *m_client = nullptr;
    QHash<QString, Profile> m_profiles;
    QHash<quint64, QString> m_inFlight; // op id -> user asked
    QSet<QString> m_asking;
    QHash<QString, qint64> m_failedAt;
    QElapsedTimer m_clock;
    int m_failureRetryMs = 5 * 60 * 1000;
};
