#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

// A display-name colour the user chooses, carried in their profile
// (`org.lightning.name_color`, MSC4133) so other Lightning clients see it.
// The protocol half is rust/src/namecolor.rs.
//
// This carries the choice only; `AppTheme.userColor` adapts it to the viewer's
// background, since a colour legible on the sender's theme may be invisible on
// the viewer's.
//
// Fetched lazily, once per user (`m_asked`), so colorFor() is safe in bindings
// that re-evaluate constantly. No colour and not-yet-asked both render as
// nothing. A homeserver without extended profile fields is
// `supported == false`, which hides the editing control.
class NameColorManager : public QObject
{
    Q_OBJECT

    // The backend can carry name colours at all.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // ...and this homeserver supports it; false once it reports the endpoint is
    // unknown.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // Bumped when any cached colour changes, so colorFor() bindings
    // re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The account's own colour, or "".
    Q_PROPERTY(QString ownColor READ ownColor NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit NameColorManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    QString ownColor() const;
    bool busy() const { return m_pendingSet != 0; }
    QString lastError() const { return m_lastError; }

    /// The user's colour, or "" when none, not yet asked, or unsupported. The
    /// first call for a user schedules the fetch.
    Q_INVOKABLE QString colorFor(const QString &userId);
    /// Set or clear (empty) the local account's colour.
    Q_INVOKABLE void setOwnColor(const QString &value);
    /// How long a fetched colour is trusted before a read re-asks. A changed
    /// answer bumps `revision`. Tests set 0.
    void setRefreshIntervalForTest(int ms) { m_refreshMs = ms; }
    // Runs one periodic sweep (see sweepRecentlyRead) immediately.
    void sweepForTest() { sweepRecentlyRead(); }
    void setRecentReadWindowForTest(int ms) { m_recentReadMs = ms; }
    /// Drops everything; a new account must not inherit the map.
    void clear();

Q_SIGNALS:
    void availableChanged();
    void supportedChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void setSupported(bool supported);
    void setLastError(const QString &error);
    void dispatchFetch(const QString &userId);
    /// Periodically re-asks users whose colour was read recently (names on
    /// screen), so a changed colour propagates without a re-render. Bounded per
    /// sweep, oldest ask first.
    void sweepRecentlyRead();

    MatrixClient *m_client = nullptr;
    // userId -> "#rrggbb", or "" for "asked, and they have none".
    QHash<QString, QString> m_colors;
    // Every user asked. Separate from m_colors so "no colour" is an answer, not
    // re-asked forever.
    QSet<QString> m_asked;
    QHash<quint64, QString> m_pending;
    // Last ask time per user (m_clock ms) and asks in flight: at most one ask
    // per interval, and the cached answer is served meanwhile.
    QHash<QString, qint64> m_askedAt;
    QSet<QString> m_inFlight;
    QElapsedTimer m_clock;
    // A read past this re-asks; the sweep re-asks recently read users on the
    // same cadence.
    int m_refreshMs = 20 * 1000;
    QHash<QString, qint64> m_lastRead;
    int m_recentReadMs = 5 * 60 * 1000;
    static constexpr int kSweepMs = 20 * 1000;
    static constexpr int kSweepCap = 24;
    QTimer m_sweep;
    quint64 m_nextOp = 1;
    quint64 m_pendingSet = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
