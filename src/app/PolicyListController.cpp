#include "app/PolicyListController.h"

#include "matrix/MatrixClient.h"

// ── The rule model ─────────────────────────────────────────────────────

int PolicyRuleModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rules.size());
}

QVariant PolicyRuleModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rules.size())
        return {};
    const QVariantMap row = m_rules.at(index.row()).toMap();
    switch (role) {
    case KindRole:           return row.value(QStringLiteral("kind"));
    case EntityRole:         return row.value(QStringLiteral("entity"));
    case RecommendationRole: return row.value(QStringLiteral("recommendation"));
    case IsBanRole:          return row.value(QStringLiteral("isBan"));
    case ReasonRole:         return row.value(QStringLiteral("reason"));
    case StateKeyRole:       return row.value(QStringLiteral("stateKey"));
    default:                 return {};
    }
}

QHash<int, QByteArray> PolicyRuleModel::roleNames() const
{
    return {
        { KindRole,           "kind" },
        { EntityRole,         "entity" },
        { RecommendationRole, "recommendation" },
        { IsBanRole,          "isBan" },
        { ReasonRole,         "reason" },
        { StateKeyRole,       "stateKey" },
    };
}

void PolicyRuleModel::setRules(const QVariantList &rules)
{
    beginResetModel();
    m_rules = rules;
    endResetModel();
    Q_EMIT countChanged();
}

void PolicyRuleModel::clear()
{
    if (m_rules.isEmpty())
        return;
    beginResetModel();
    m_rules.clear();
    endResetModel();
    Q_EMIT countChanged();
}

// ── The controller ─────────────────────────────────────────────────────

PolicyListController::PolicyListController(QObject *parent)
    : QObject(parent)
{
}

void PolicyListController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::policyRulesReceived, this,
                &PolicyListController::onRules);
        connect(m_client, &MatrixClient::policyRuleWritten, this,
                &PolicyListController::onWritten);
        connect(m_client, &MatrixClient::policySubscriptionsReceived, this,
                &PolicyListController::onSubscriptions);
        connect(m_client, &MatrixClient::policyCheckFinished, this,
                &PolicyListController::onCheck);
        // Policy subscriptions are ACCOUNT data. One account's lists must
        // never be shown under the next.
        connect(m_client, &MatrixClient::loggedOut, this,
                &PolicyListController::onLoggedOut);
    }
    onLoggedOut();
    Q_EMIT stateChanged();
}

bool PolicyListController::available() const
{
    return m_client != nullptr && m_client->supportsPolicyLists();
}

void PolicyListController::openRoom(const QString &roomId)
{
    if (!available() || roomId.isEmpty())
        return;
    // The previous room's rules go NOW, not when the answer arrives: leaving
    // them up would show one room's ban list under another room's name.
    m_rules.clear();
    m_roomId = roomId;
    m_canWrite = false;
    m_truncated = false;
    m_lastError.clear();
    m_rulesOp = m_nextOpId++;
    m_client->fetchPolicyRules(roomId, m_rulesOp);
    Q_EMIT stateChanged();
}

void PolicyListController::addRule(const QString &kind, const QString &entity,
                                   const QString &reason)
{
    if (!available() || m_roomId.isEmpty() || m_writeOp != 0)
        return;
    if (entity.trimmed().isEmpty())
        return;
    m_lastError.clear();
    m_writeOp = m_nextOpId++;
    // `m.ban` is the only recommendation the spec defines, and inventing a
    // second one here would publish advice no other tool reads.
    m_client->writePolicyRule(m_roomId, kind, entity.trimmed(),
                              QStringLiteral("m.ban"), reason, m_writeOp);
    Q_EMIT stateChanged();
}

void PolicyListController::removeRule(const QString &kind,
                                      const QString &entity)
{
    if (!available() || m_roomId.isEmpty() || m_writeOp != 0)
        return;
    if (entity.trimmed().isEmpty())
        return;
    m_lastError.clear();
    m_writeOp = m_nextOpId++;
    // An EMPTY recommendation is the removal: the bridge writes an empty
    // state event, which is the Mjolnir convention and the only removal
    // Matrix state has short of a redaction.
    m_client->writePolicyRule(m_roomId, kind, entity.trimmed(), QString(),
                              QString(), m_writeOp);
    Q_EMIT stateChanged();
}

void PolicyListController::setSubscribed(const QString &roomId, bool subscribed)
{
    if (!available() || roomId.isEmpty() || m_subsOp != 0)
        return;
    m_subsOp = m_nextOpId++;
    m_client->setPolicySubscribed(roomId, subscribed, m_subsOp);
    Q_EMIT stateChanged();
}

void PolicyListController::refreshSubscriptions()
{
    if (!available() || m_subsOp != 0)
        return;
    m_subsOp = m_nextOpId++;
    m_client->fetchPolicySubscriptions(m_subsOp);
    Q_EMIT stateChanged();
}

bool PolicyListController::isSubscribed(const QString &roomId) const
{
    return !roomId.isEmpty() && m_subscriptions.contains(roomId);
}

void PolicyListController::check(const QString &kind, const QString &entity)
{
    if (!available() || entity.trimmed().isEmpty())
        return;
    m_checkOp = m_nextOpId++;
    m_client->checkPolicyEntity(kind, entity.trimmed(), m_checkOp);
}

void PolicyListController::onRules(quint64 opId, bool ok, const QString &roomId,
                                   bool canWrite, bool truncated,
                                   const QVariantList &rules)
{
    // ONLY the latest read is applied. A slow read of one room must not
    // overwrite a faster read of the room the user has since moved to — and
    // the room id is checked as well as the op, because the two disagreeing
    // is the shape of that bug.
    if (opId == 0 || opId != m_rulesOp)
        return;
    m_rulesOp = 0;
    if (roomId != m_roomId)
        return;
    if (!ok) {
        m_lastError = tr("That room's rules could not be read.");
        Q_EMIT stateChanged();
        return;
    }
    m_canWrite = canWrite;
    m_truncated = truncated;
    m_rules.setRules(rules);
    Q_EMIT stateChanged();
}

void PolicyListController::onWritten(quint64 opId, bool ok,
                                     const QString &category)
{
    if (opId == 0 || opId != m_writeOp)
        return;
    m_writeOp = 0;
    if (!ok) {
        m_lastError = category == QLatin1String("forbidden")
            ? tr("You do not have permission to publish rules in this room.")
            : tr("That rule could not be published.");
    } else {
        m_lastError.clear();
        // Nothing is applied optimistically: re-read, so the list shows what
        // the room actually holds rather than what we asked for.
        if (!m_roomId.isEmpty()) {
            m_rulesOp = m_nextOpId++;
            m_client->fetchPolicyRules(m_roomId, m_rulesOp);
        }
    }
    Q_EMIT stateChanged();
    Q_EMIT writeFinished(ok, category);
}

void PolicyListController::onSubscriptions(quint64 opId, bool ok,
                                           const QString &category,
                                           const QStringList &rooms)
{
    if (opId == 0 || opId != m_subsOp)
        return;
    m_subsOp = 0;
    if (!ok) {
        m_lastError = category == QLatin1String("too_many")
            ? tr("You are already following as many lists as Lightning "
                 "supports.")
            : tr("That list could not be followed.");
        Q_EMIT stateChanged();
        return;
    }
    m_lastError.clear();
    if (m_subscriptions != rooms) {
        m_subscriptions = rooms;
        Q_EMIT subscriptionsChanged();
    }
    Q_EMIT stateChanged();
}

void PolicyListController::onCheck(quint64 opId, const QString &entity,
                                   bool matched, const QVariantMap &detail)
{
    if (opId == 0 || opId != m_checkOp)
        return;
    m_checkOp = 0;
    Q_EMIT checkFinished(entity, matched, detail);
}

void PolicyListController::onLoggedOut()
{
    m_rules.clear();
    m_roomId.clear();
    m_canWrite = false;
    m_truncated = false;
    m_lastError.clear();
    m_rulesOp = 0;
    m_writeOp = 0;
    m_subsOp = 0;
    m_checkOp = 0;
    if (!m_subscriptions.isEmpty()) {
        m_subscriptions.clear();
        Q_EMIT subscriptionsChanged();
    }
    Q_EMIT stateChanged();
}
