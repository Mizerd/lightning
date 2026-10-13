#pragma once

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

public:
    explicit AboutController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    /// The search index is cleared through its own controller so the
    /// Settings counters stay true.
    void setSearch(MessageSearchController *search) { m_search = search; }
    /// The install kind's label ("AppImage", "Debian package" ...), supplied
    /// by the update manager that detects it.
    void setPackageKind(const QString &label);

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

Q_SIGNALS:
    void versionSummaryChanged();
    void homeserverChanged();
    void resetChanged();
    void accountDataChanged();

private:
    void onHomeserverInfo(quint64 opId, const QVariantMap &info);
    void onAccountData(quint64 opId, const QVariantMap &listing);
    void onMediaCleared(quint64 opId, bool ok);
    void onEventCacheCleared(quint64 opId, bool ok);
    void onLoggedOut();
    void stepFinished(const QString &what, bool ok);
    void finishReset(bool timedOut);

    MatrixClient *m_client = nullptr;
    MessageSearchController *m_search = nullptr;
    QString m_packageKind;

    QString m_homeserverState = QStringLiteral("idle");
    QVariantMap m_homeserver;
    quint64 m_homeserverOp = 0;

    QString m_resetState = QStringLiteral("idle");
    QString m_resetSummary;
    quint64 m_mediaOp = 0;
    quint64 m_eventCacheOp = 0;
    QStringList m_resetFailed;
    int m_resetPending = 0;
    QTimer m_resetWatchdog;

    QString m_accountDataState = QStringLiteral("idle");
    QVariantList m_entries;
    bool m_truncated = false;
    QString m_accountDataRoom;
    QString m_accountDataError;
    quint64 m_accountDataOp = 0;
};
