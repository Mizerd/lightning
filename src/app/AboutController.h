#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;
class MessageSearchController;

/// What the About page and its Developer tools need from the app: a copyable
/// version summary, what the homeserver says about itself, the "Reset local
/// cache" action and a read-only account-data browser.
///
/// Reset local cache clears ONLY things that can be rebuilt from the server:
/// the SDK's event cache, the media cache and the local search index. It never
/// touches the crypto store, the state store or the credentials (CLAUDE.md §6),
/// and it is not a sign-out. The account-data browser shows secret types
/// (secret storage, cross-signing, backup) as a type name only: the content
/// is redacted in Rust before it reaches this class.
class AboutController : public QObject
{
    Q_OBJECT

    /// Version, commit, platform and package kind, one per line.
    Q_PROPERTY(QString versionSummary READ versionSummary
                   NOTIFY versionSummaryChanged)
    Q_PROPERTY(QString reportIssueUrl READ reportIssueUrl CONSTANT)
    /// The GStreamer runtime the call engine would load; empty in a build
    /// without the call engine.
    Q_PROPERTY(QString gstreamerVersion READ gstreamerVersion CONSTANT)

    /// "idle" | "loading" | "ready" | "failed" | "unsupported".
    Q_PROPERTY(QString homeserverState READ homeserverState
                   NOTIFY homeserverChanged)
    /// { address, softwareName, softwareVersion, versions[], features[],
    /// category }; empty until the first answer.
    Q_PROPERTY(QVariantMap homeserver READ homeserver NOTIFY homeserverChanged)

    /// "idle" | "running" | "done" | "failed".
    Q_PROPERTY(QString resetState READ resetState NOTIFY resetChanged)
    /// Plain-language outcome of the last reset, empty while idle.
    Q_PROPERTY(QString resetSummary READ resetSummary NOTIFY resetChanged)

    /// "idle" | "loading" | "ready" | "failed" | "unsupported".
    Q_PROPERTY(QString accountDataState READ accountDataState
                   NOTIFY accountDataChanged)
    /// [{ type, redacted, json, cut }], sorted by type.
    Q_PROPERTY(QVariantList accountDataEntries READ accountDataEntries
                   NOTIFY accountDataChanged)
    Q_PROPERTY(bool accountDataTruncated READ accountDataTruncated
                   NOTIFY accountDataChanged)
    /// The room the listing is for; empty is the global account data.
    Q_PROPERTY(QString accountDataRoomId READ accountDataRoomId
                   NOTIFY accountDataChanged)
    Q_PROPERTY(QString accountDataError READ accountDataError
                   NOTIFY accountDataChanged)

    /// Bumped whenever a user's session list arrives or is dropped, so a QML
    /// binding that calls userClients() re-evaluates.
    Q_PROPERTY(int userClientsRevision READ userClientsRevision
                   NOTIFY userClientsChanged)

public:
    /// How long a clear step may stay silent before the reset reads as not
    /// finished.
    static constexpr int kResetWaitMs = 60 * 1000;
    /// How long a compaction report is waited for after the clear. Longer than
    /// the Rust side's own compaction budget (COMPACT_BUDGET, 90 s in
    /// rust/src/aboutinfo.rs), so the budget runs out first and says so.
    static constexpr int kCompactionWaitMs = 100 * 1000;
    static_assert(kCompactionWaitMs > 90 * 1000,
                  "the UI must outwait the compaction budget");

    explicit AboutController(QObject *parent = nullptr);
    /// Test seam: shorter waits.
    void setWaitsForTest(int resetMs, int compactionMs);

    void setClient(MatrixClient *client);
    /// The search index is cleared through its own controller so the
    /// Settings counters stay true.
    void setSearch(MessageSearchController *search) { m_search = search; }
    /// The install kind's label ("AppImage", "Debian package" ...), supplied
    /// by the update manager that detects it.
    void setPackageKind(const QString &label);
    void setGstreamerVersion(const QString &version) { m_gstreamerVersion = version; }
    QString gstreamerVersion() const { return m_gstreamerVersion; }

    QString versionSummary() const;
    QString reportIssueUrl() const;
    QString homeserverState() const { return m_homeserverState; }
    QVariantMap homeserver() const { return m_homeserver; }
    QString resetState() const { return m_resetState; }
    QString resetSummary() const { return m_resetSummary; }
    QString accountDataState() const { return m_accountDataState; }
    QVariantList accountDataEntries() const { return m_entries; }
    bool accountDataTruncated() const { return m_truncated; }
    QString accountDataRoomId() const { return m_accountDataRoom; }
    QString accountDataError() const { return m_accountDataError; }

    /// Copies versionSummary() to the clipboard.
    Q_INVOKABLE void copyVersion();
    /// Asks the homeserver again.
    Q_INVOKABLE void refreshHomeserver();
    /// Event cache, media cache and search index; see the class comment.
    Q_INVOKABLE void resetLocalCache();
    /// Lists the account data; global for an empty `roomId`.
    Q_INVOKABLE void loadAccountData(const QString &roomId = QString());
    /// Drops what the browser holds (closing the Developer tools).
    Q_INVOKABLE void clearAccountData();

    /// Developer mode: which sessions a user has, as far as this device's
    /// crypto store knows. Only the device id and the name the OWNER gave the
    /// session exist in the protocol; nothing here infers a client. Asks once
    /// per user until cleared.
    Q_INVOKABLE void requestUserClients(const QString &userId);
    /// { state: "idle"|"loading"|"ready"|"failed", devices: [{deviceId,
    /// name}] }. `name` is empty when the owner set none.
    Q_INVOKABLE QVariantMap userClients(const QString &userId) const;
    /// Forgets every user's sessions (Developer mode switched off, sign-out).
    Q_INVOKABLE void clearUserClients();
    int userClientsRevision() const { return m_clientsRevision; }

Q_SIGNALS:
    void versionSummaryChanged();
    void homeserverChanged();
    void resetChanged();
    /// The SDK's event cache was emptied (the clear step succeeded). Every
    /// timeline built on it is now empty and will not refill by itself, so
    /// whoever shows one must reopen it.
    void eventCacheWasCleared();
    void accountDataChanged();
    void userClientsChanged();

private:
    void onHomeserverInfo(quint64 opId, const QVariantMap &info);
    void onAccountData(quint64 opId, const QVariantMap &listing);
    void onUserClients(quint64 opId, const QVariantMap &result);
    void onMediaCleared(quint64 opId, bool ok);
    void onEventCacheCleared(quint64 opId, bool ok);
    void onCacheCompacted(quint64 opId, const QString &store,
                          const QString &outcome, qint64 bytesBefore,
                          qint64 bytesAfter);
    QString reclaimSummary() const;
    void refreshSummary();
    void onLoggedOut();
    void stepFinished(const QString &what, bool ok);
    void finishReset(bool timedOut);

    MatrixClient *m_client = nullptr;
    MessageSearchController *m_search = nullptr;
    QString m_packageKind;
    QString m_gstreamerVersion;

    QString m_homeserverState = QStringLiteral("idle");
    QVariantMap m_homeserver;
    quint64 m_homeserverOp = 0;

    QString m_resetState = QStringLiteral("idle");
    QString m_resetSummary;
    quint64 m_mediaOp = 0;
    quint64 m_eventCacheOp = 0;
    QStringList m_resetFailed;
    // Compaction is its own, later report per store (a clear that succeeded is
    // never made to look unfinished by a slow VACUUM). `expected`: a clear
    // succeeded on a backend that reports compaction, so a report is due.
    struct Compaction {
        quint64 op = 0;
        bool expected = false;
        bool got = false;
        // The wait ran out with no report (a backend task that died sends none).
        bool gaveUp = false;
        QString outcome;
        qint64 before = 0;
        qint64 after = 0;
    };
    Compaction m_eventCompaction;
    Compaction m_mediaCompaction;
    qint64 m_keptFilesFreed = 0;
    // The summary without the disk sentence, which is recomposed as
    // compaction reports arrive.
    QString m_clearSummary;
    QTimer m_compactionWatchdog;
    QStringList m_resetDone;
    int m_resetPending = 0;
    QTimer m_resetWatchdog;

    QString m_accountDataState = QStringLiteral("idle");
    QVariantList m_entries;
    bool m_truncated = false;
    QString m_accountDataRoom;
    QString m_accountDataError;
    quint64 m_accountDataOp = 0;

    QHash<QString, QVariantMap> m_userClients;
    QHash<quint64, QString> m_userClientOps;
    int m_clientsRevision = 0;
};
