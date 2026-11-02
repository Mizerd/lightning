#pragma once

#include <QHash>
#include <QTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class PushRulesController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(bool masterMute READ masterMute NOTIFY stateChanged)
    Q_PROPERTY(bool dmAll READ dmAll NOTIFY stateChanged)
    Q_PROPERTY(bool roomsAll READ roomsAll NOTIFY stateChanged)
    Q_PROPERTY(bool dmDrift READ dmDrift NOTIFY stateChanged)
    Q_PROPERTY(bool roomsDrift READ roomsDrift NOTIFY stateChanged)
    Q_PROPERTY(int mentionUserId READ mentionUserId NOTIFY stateChanged)
    Q_PROPERTY(int mentionDisplayName READ mentionDisplayName NOTIFY stateChanged)
    Q_PROPERTY(int mentionUsername READ mentionUsername NOTIFY stateChanged)
    Q_PROPERTY(int mentionRoom READ mentionRoom NOTIFY stateChanged)
    Q_PROPERTY(int eventReactions READ eventReactions NOTIFY stateChanged)
    Q_PROPERTY(int eventInvites READ eventInvites NOTIFY stateChanged)
    Q_PROPERTY(int eventNotices READ eventNotices NOTIFY stateChanged)
    Q_PROPERTY(QVariantList keywords READ keywords NOTIFY stateChanged)

public:
    explicit PushRulesController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool loaded() const { return m_loaded; }
    bool busy() const { return !m_pending.isEmpty() || m_refreshOp != 0; }
    QString errorText() const { return m_error; }
    bool masterMute() const { return flag(QStringLiteral("masterMute")); }
    bool dmAll() const { return flag(QStringLiteral("dmAll")); }
    bool roomsAll() const { return flag(QStringLiteral("roomsAll")); }
    bool dmDrift() const { return flag(QStringLiteral("dmDrift")); }
    bool roomsDrift() const { return flag(QStringLiteral("roomsDrift")); }
    int mentionUserId() const { return level(QStringLiteral("mentionUserId")); }
    int mentionDisplayName() const
    { return level(QStringLiteral("mentionDisplayName")); }
    int mentionUsername() const { return level(QStringLiteral("mentionUsername")); }
    int mentionRoom() const { return level(QStringLiteral("mentionRoom")); }
    int eventReactions() const { return level(QStringLiteral("eventReactions")); }
    int eventInvites() const { return level(QStringLiteral("eventInvites")); }
    int eventNotices() const { return level(QStringLiteral("eventNotices")); }
    QVariantList keywords() const;

    bool masterMuteKnown() const { return m_loaded; }

    Q_INVOKABLE void refresh();
    void setHoldMsForTest(qint64 ms) { m_holdMs = ms; }
    Q_INVOKABLE void setMasterMute(bool on);
    Q_INVOKABLE void setDefaultAll(bool directMessages, bool allMessages);
    Q_INVOKABLE void setMention(const QString &key, int level);
    Q_INVOKABLE void setEventRule(const QString &key, int level);
    Q_INVOKABLE QString keywordProblem(const QString &keyword) const;
    Q_INVOKABLE bool addKeyword(const QString &keyword, int level);
    Q_INVOKABLE void setKeywordLevel(const QString &keyword, int level);
    Q_INVOKABLE void removeKeyword(const QString &keyword);

    void resetForAccountChange() { onLoggedOut(); }

Q_SIGNALS:
    void stateChanged();
    void writeFailed(const QString &message);
    void accountDefaultChanged();

private Q_SLOTS:
    void onSnapshot(quint64 opId, bool ok, const QVariantMap &snapshot,
                    const QString &category);
    void onWriteFinished(quint64 opId, const QString &kind, bool ok,
                         const QString &category);
    void onInitialSyncDoneChanged();
    void onLoggedOut();
    void onPushRulesChanged();
    void onHoldExpired();

private:
    bool flag(const QString &key) const { return m_snap.value(key).toBool(); }
    int level(const QString &key) const
    { return m_snap.value(key, -1).toInt(); }
    void write(const QString &kind, const QString &key, const QString &value,
               const QVariantMap &optimistic);
    void applyServer(const QVariantMap &server);
    void armHoldTimer();
    int keywordIndex(const QString &keyword, bool exact) const;
    static QString describeCategory(const QString &category);
    static QString levelName(int level);

    MatrixClient *m_client = nullptr;
    QVariantMap m_snap;
    QVariantMap m_lastServer;
    QHash<quint64, QString> m_pending;
    QHash<quint64, QVariantMap> m_optimistic;
    // Choices the server accepted but no snapshot reflects yet. Any snapshot
    // is read from the SDK's account-data store (Account::push_rules), which
    // only moves when a sync delivers the change, so one read before that is
    // the state from BEFORE the write. A held field is shown until a snapshot
    // agrees with it or kHoldMs passes, whichever is first.
    struct Hold { QVariant value; qint64 expiresMs = 0; };
    QHash<QString, Hold> m_hold;
    QTimer m_holdTimer;
    qint64 m_holdMs = 15000;
    quint64 m_refreshOp = 0;
    bool m_refreshQueued = false;
    bool m_refreshAfterSync = false;
    bool m_errorFromRefresh = false;
    bool m_loaded = false;
    QString m_error;
};
