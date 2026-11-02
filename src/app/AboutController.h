#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;
class MessageSearchController;

class AboutController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString versionSummary READ versionSummary
                   NOTIFY versionSummaryChanged)
    Q_PROPERTY(QString reportIssueUrl READ reportIssueUrl CONSTANT)
    Q_PROPERTY(QString gstreamerVersion READ gstreamerVersion CONSTANT)

    Q_PROPERTY(QString homeserverState READ homeserverState
                   NOTIFY homeserverChanged)
    Q_PROPERTY(QVariantMap homeserver READ homeserver NOTIFY homeserverChanged)

    Q_PROPERTY(QString resetState READ resetState NOTIFY resetChanged)
    Q_PROPERTY(QString resetSummary READ resetSummary NOTIFY resetChanged)

    Q_PROPERTY(QString accountDataState READ accountDataState
                   NOTIFY accountDataChanged)
    Q_PROPERTY(QVariantList accountDataEntries READ accountDataEntries
                   NOTIFY accountDataChanged)
    Q_PROPERTY(bool accountDataTruncated READ accountDataTruncated
                   NOTIFY accountDataChanged)
    Q_PROPERTY(QString accountDataRoomId READ accountDataRoomId
                   NOTIFY accountDataChanged)
    Q_PROPERTY(QString accountDataError READ accountDataError
                   NOTIFY accountDataChanged)

    Q_PROPERTY(int userClientsRevision READ userClientsRevision
                   NOTIFY userClientsChanged)

public:
    static constexpr int kResetWaitMs = 60 * 1000;
    /// How long a compaction report is waited for after the clear. Longer than
    /// the Rust side's own compaction budget (COMPACT_BUDGET, 90 s in
    /// rust/src/aboutinfo.rs), so the budget runs out first and says so.
    static constexpr int kCompactionWaitMs = 100 * 1000;
    static_assert(kCompactionWaitMs > 90 * 1000,
                  "the UI must outwait the compaction budget");

    explicit AboutController(QObject *parent = nullptr);
    void setWaitsForTest(int resetMs, int compactionMs);

    void setClient(MatrixClient *client);
    void setSearch(MessageSearchController *search) { m_search = search; }
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

    Q_INVOKABLE void copyVersion();
    Q_INVOKABLE void refreshHomeserver();
    Q_INVOKABLE void resetLocalCache();
    Q_INVOKABLE void loadAccountData(const QString &roomId = QString());
    Q_INVOKABLE void clearAccountData();

    /// Developer mode: which sessions a user has, as far as this device's
    /// crypto store knows. Only the device id and the name the OWNER gave the
    /// session exist in the protocol; nothing here infers a client. Asks again
    /// for a user only after the refresh interval (30 s), and never while an
    /// ask is on the way.
    Q_INVOKABLE void requestUserClients(const QString &userId);
    Q_INVOKABLE QVariantMap userClients(const QString &userId) const;
    Q_INVOKABLE void clearUserClients();
    int userClientsRevision() const { return m_clientsRevision; }
    void setUserClientsRefreshMs(qint64 ms) { m_clientsRefreshMs = ms; }

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
    struct Compaction {
        quint64 op = 0;
        bool expected = false;
        bool got = false;
        bool gaveUp = false;
        QString outcome;
        qint64 before = 0;
        qint64 after = 0;
    };
    Compaction m_eventCompaction;
    Compaction m_mediaCompaction;
    qint64 m_keptFilesFreed = 0;
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
    QSet<quint64> m_userClientRefreshOps;
    QHash<QString, qint64> m_userClientAskedMs;
    qint64 m_clientsRefreshMs = 30000;
    int m_clientsRevision = 0;
};
