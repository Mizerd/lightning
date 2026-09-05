#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

/// Mjolnir-style POLICY LISTS: `m.policy.rule.*` events published in a room,
/// describing entities somebody recommends banning.
///
/// # What this offers, and the line it does not cross
///
/// It reads a policy room, writes rules where the account has the power
/// level, keeps the list of rooms this account follows, and answers whether
/// those lists cover a given user or server.
///
/// It never acts on a match by itself. A subscribed list is somebody else's
/// judgement; silently hiding people on the strength of it — with no way to
/// see that it happened or why — is a different feature from showing that a
/// list covers someone and offering to act. Lightning already has ignore
/// (server-side, account-wide) and kick/ban; this feeds them. The user
/// decides.
///
/// # Ops
///
/// Every call carries an op id and only the LATEST answer for each kind of
/// question is applied, so a slow read of one room cannot overwrite a faster
/// read of the one the user has since moved to.
class PolicyRuleModel : public QAbstractListModel
{
    Q_OBJECT
    // QAbstractListModel has NO `count` in QML — a ListView supplies one, the
    // model does not — so a binding on `model.count` reads `undefined`. That
    // reached qsTr() as a plural argument and the QML-warning gates caught it
    // ("third argument (n) must be a number"). Every model here that QML
    // counts declares this explicitly.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        EntityRole,
        RecommendationRole,
        IsBanRole,
        ReasonRole,
        StateKeyRole,
    };

    explicit PolicyRuleModel(QObject *parent = nullptr)
        : QAbstractListModel(parent) {}

    int count() const { return int(m_rules.size()); }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRules(const QVariantList &rules);
    void clear();

Q_SIGNALS:
    void countChanged();

private:
    QVariantList m_rules;
};

class PolicyListController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    /// The room whose rules `rules` holds.
    Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
    Q_PROPERTY(PolicyRuleModel *rules READ rules CONSTANT)
    /// Whether this account may publish rules in `roomId`.
    Q_PROPERTY(bool canWrite READ canWrite NOTIFY stateChanged)
    /// Whether the read hit its bound. A partial answer that does not say so
    /// reads as a complete one.
    Q_PROPERTY(bool truncated READ truncated NOTIFY stateChanged)
    /// The policy rooms this account follows.
    Q_PROPERTY(QStringList subscriptions READ subscriptions
                   NOTIFY subscriptionsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)

public:
    explicit PolicyListController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool loading() const { return m_rulesOp != 0; }
    QString roomId() const { return m_roomId; }
    PolicyRuleModel *rules() { return &m_rules; }
    bool canWrite() const { return m_canWrite; }
    bool truncated() const { return m_truncated; }
    QStringList subscriptions() const { return m_subscriptions; }
    QString lastError() const { return m_lastError; }

    /// Load one policy room's rules.
    Q_INVOKABLE void openRoom(const QString &roomId);
    /// Publish a rule. `kind` is "user", "server" or "room".
    Q_INVOKABLE void addRule(const QString &kind, const QString &entity,
                             const QString &reason);
    /// Remove a rule by the entity it names.
    Q_INVOKABLE void removeRule(const QString &kind, const QString &entity);
    Q_INVOKABLE void setSubscribed(const QString &roomId, bool subscribed);
    Q_INVOKABLE void refreshSubscriptions();
    Q_INVOKABLE bool isSubscribed(const QString &roomId) const;
    /// Ask whether the subscribed lists cover an entity. The answer arrives
    /// on `checkFinished`.
    Q_INVOKABLE void check(const QString &kind, const QString &entity);

Q_SIGNALS:
    void stateChanged();
    void subscriptionsChanged();
    /// A rule write finished. `category` is empty on success.
    void writeFinished(bool ok, const QString &category);
    /// The answer to check(). `detail` carries roomId, ruleEntity, ruleKind
    /// and reason when `matched` is true.
    void checkFinished(const QString &entity, bool matched,
                       const QVariantMap &detail);

private:
    void onRules(quint64 opId, bool ok, const QString &roomId, bool canWrite,
                 bool truncated, const QVariantList &rules);
    void onWritten(quint64 opId, bool ok, const QString &category);
    void onSubscriptions(quint64 opId, bool ok, const QString &category,
                         const QStringList &rooms);
    void onCheck(quint64 opId, const QString &entity, bool matched,
                 const QVariantMap &detail);
    void onLoggedOut();

    MatrixClient *m_client = nullptr;
    PolicyRuleModel m_rules;
    QString m_roomId;
    bool m_canWrite = false;
    bool m_truncated = false;
    QStringList m_subscriptions;
    QString m_lastError;
    quint64 m_nextOpId = 1;
    quint64 m_rulesOp = 0;
    quint64 m_writeOp = 0;
    quint64 m_subsOp = 0;
    quint64 m_checkOp = 0;
};
