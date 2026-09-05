#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

class MatrixClient;

// The GLOBAL profile — display name and avatar — of any Matrix user, asked
// from the homeserver once and remembered for the session.
//
// It exists for the users a room's member snapshot cannot name: the target
// of a mention who is not in the room, a lazily-loaded member this client
// has never received a state event for, a sender in a room whose member
// list was never fetched. Before it (2026-09-05, tester report) such a pill
// rendered as the bare localpart and its profile card opened with no name
// and no picture. The timeline's mention pills and the member profile
// popover both consult it; the room's OWN member name always wins over it
// where one exists, because a per-room nick is what the room shows.
//
// Reads are pure: profile() never asks. lookup()/request() ask the server
// at most once per user per session — a refused answer is remembered as a
// failure and re-asked only after failureRetryMs, so a transient error is
// not permanent and a missing user is not hammered. /profile takes a user
// id and nothing else; no room context is sent.
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

    // Bounded: a session that mentions more distinct users than this stops
    // asking rather than growing without limit.
    static constexpr int kMaxProfiles = 4000;

    MatrixClient *m_client = nullptr;
    QHash<QString, Profile> m_profiles;
    QHash<quint64, QString> m_inFlight; // op id -> user asked
    QSet<QString> m_asking;
    QHash<QString, qint64> m_failedAt;
    QElapsedTimer m_clock;
    int m_failureRetryMs = 5 * 60 * 1000;
};
