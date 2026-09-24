#include "spaces/SpaceModerationController.h"

#include "matrix/MatrixClient.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSpaceModeration, "lightning.spacemoderation")

namespace {

bool isKnownOp(const QString &op)
{
    return op == QLatin1String("kick") || op == QLatin1String("ban")
           || op == QLatin1String("unban");
}

} // namespace

SpaceModerationController::SpaceModerationController(QObject *parent)
    : QObject(parent)
{
}

void SpaceModerationController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    reset();
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::moderationPlanReceived, this,
            &SpaceModerationController::onPlanReceived);
    connect(m_client, &MatrixClient::moderationFinished, this,
            &SpaceModerationController::onModerationFinished);
    // A flow belongs to the account that started it.
    connect(m_client, &MatrixClient::loggedOut, this,
            &SpaceModerationController::reset);
}

void SpaceModerationController::setScopeResolver(
    std::function<QStringList(const QString &)> resolver)
{
    m_scopeResolver = std::move(resolver);
}

void SpaceModerationController::setPhase(const QString &phase)
{
    m_phase = phase;
    Q_EMIT stateChanged();
}

void SpaceModerationController::reset()
{
    m_spaceId.clear();
    m_spaceName.clear();
    m_userId.clear();
    m_displayName.clear();
    m_op.clear();
    m_reason.clear();
    m_spaceRow.clear();
    m_rooms.clear();
    m_cascade = true;
    m_planTruncated = false;
    m_planOp = 0;
    m_steps.clear();
    m_nextStep = 0;
    m_stepOp = 0;
    m_stepRoom.clear();
    m_succeeded = 0;
    m_failed = 0;
    setPhase(QStringLiteral("idle"));
}

void SpaceModerationController::begin(const QString &spaceId,
                                      const QString &spaceName,
                                      const QString &userId,
                                      const QString &displayName,
                                      const QString &op)
{
    // A running flow is finished or reset first: its steps are already on
    // the server and their answers must still be counted.
    if (m_phase == QLatin1String("running"))
        return;
    reset();
    if (!m_client || spaceId.isEmpty() || userId.isEmpty() || !isKnownOp(op))
        return;
    m_spaceId = spaceId;
    m_spaceName = spaceName;
    m_userId = userId;
    m_displayName = displayName;
    m_op = op;
    QStringList scope = m_scopeResolver ? m_scopeResolver(spaceId)
                                        : QStringList{};
    // The Space is always assessed first, even when the resolver omits it.
    scope.removeAll(spaceId);
    scope.prepend(spaceId);
    m_planOp = m_client->requestModerationPlan(scope, userId, op);
    setPhase(m_planOp != 0 ? QStringLiteral("planning")
                           : QStringLiteral("failed"));
}

QVariantMap SpaceModerationController::rowFromPlan(const QVariantMap &plan)
{
    // Fails closed: a row without a reason key or a room id is not offered.
    QString reason = plan.value(QStringLiteral("reason")).toString();
    if (!plan.contains(QStringLiteral("reason"))
        || plan.value(QStringLiteral("roomId")).toString().isEmpty())
        reason = QStringLiteral("unknown");
    const bool eligible = reason.isEmpty();
    return QVariantMap{
        { QStringLiteral("roomId"), plan.value(QStringLiteral("roomId")) },
        { QStringLiteral("name"), plan.value(QStringLiteral("name")) },
        { QStringLiteral("isSpace"), plan.value(QStringLiteral("isSpace")) },
        { QStringLiteral("eligible"), eligible },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("reasonText"),
          eligible ? QString() : reasonText(reason) },
        // Offered rooms start selected: the cascade switch is the one
        // decision, and the list refines it.
        { QStringLiteral("selected"), eligible },
        { QStringLiteral("status"), QString() },
        { QStringLiteral("message"), QString() },
    };
}

void SpaceModerationController::onPlanReceived(quint64 opId,
                                               const QString &userId,
                                               const QString &op,
                                               bool truncated,
                                               const QVariantList &rooms)
{
    if (opId == 0 || opId != m_planOp || userId != m_userId || op != m_op)
        return;
    m_planOp = 0;
    m_planTruncated = truncated;
    m_spaceRow.clear();
    m_rooms.clear();
    for (const QVariant &value : rooms) {
        const QVariantMap row = rowFromPlan(value.toMap());
        const QString roomId = row.value(QStringLiteral("roomId")).toString();
        if (roomId == m_spaceId) {
            if (m_spaceRow.isEmpty())
                m_spaceRow = row;
            continue;
        }
        m_rooms.append(row);
    }
    if (m_spaceRow.isEmpty()) {
        // The backend did not answer for the Space itself: treat it as not
        // offered rather than guessing.
        m_spaceRow = rowFromPlan(QVariantMap{
            { QStringLiteral("roomId"), m_spaceId },
            { QStringLiteral("name"), m_spaceName },
            { QStringLiteral("isSpace"), true },
            { QStringLiteral("reason"), QStringLiteral("unknown") },
        });
    }
    qCDebug(lcSpaceModeration) << "plan op=" << m_op
                               << "spaceOffered=" << spaceEligible()
                               << "rooms=" << m_rooms.size()
                               << "offered=" << eligibleRoomCount()
                               << "truncated=" << truncated;
    setPhase(QStringLiteral("ready"));
}

bool SpaceModerationController::spaceSkipped() const
{
    return !m_spaceRow.isEmpty() && !spaceEligible();
}

bool SpaceModerationController::spaceEligible() const
{
    return m_spaceRow.value(QStringLiteral("eligible")).toBool();
}

int SpaceModerationController::eligibleRoomCount() const
{
    int count = 0;
    for (const QVariant &value : m_rooms)
        count += value.toMap().value(QStringLiteral("eligible")).toBool();
    return count;
}

int SpaceModerationController::selectedRoomCount() const
{
    int count = 0;
    for (const QVariant &value : m_rooms) {
        const QVariantMap row = value.toMap();
        count += row.value(QStringLiteral("eligible")).toBool()
                 && row.value(QStringLiteral("selected")).toBool();
    }
    return count;
}

int SpaceModerationController::skippedRoomCount() const
{
    return m_rooms.size() - eligibleRoomCount();
}

void SpaceModerationController::setCascade(bool cascade)
{
    if (m_cascade == cascade || m_phase != QLatin1String("ready"))
        return;
    m_cascade = cascade;
    Q_EMIT stateChanged();
}

void SpaceModerationController::setRoomSelected(const QString &roomId,
                                                bool selected)
{
    if (m_phase != QLatin1String("ready"))
        return;
    for (QVariant &value : m_rooms) {
        QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("roomId")).toString() != roomId)
            continue;
        if (!row.value(QStringLiteral("eligible")).toBool())
            return;
        if (row.value(QStringLiteral("selected")).toBool() == selected)
            return;
        row.insert(QStringLiteral("selected"), selected);
        value = row;
        Q_EMIT stateChanged();
        return;
    }
}

void SpaceModerationController::setAllRoomsSelected(bool selected)
{
    if (m_phase != QLatin1String("ready"))
        return;
    bool changed = false;
    for (QVariant &value : m_rooms) {
        QVariantMap row = value.toMap();
        if (!row.value(QStringLiteral("eligible")).toBool()
            || row.value(QStringLiteral("selected")).toBool() == selected) {
            continue;
        }
        row.insert(QStringLiteral("selected"), selected);
        value = row;
        changed = true;
    }
    if (changed)
        Q_EMIT stateChanged();
}

bool SpaceModerationController::canConfirm() const
{
    if (m_phase != QLatin1String("ready") || !m_client)
        return false;
    return spaceEligible() || (m_cascade && selectedRoomCount() > 0);
}

void SpaceModerationController::confirm(const QString &reason)
{
    if (!canConfirm())
        return;
    m_reason = reason.trimmed();
    m_steps.clear();
    // Only rows the plan offered are ever dispatched.
    if (spaceEligible()) {
        m_steps.append(m_spaceId);
        m_spaceRow.insert(QStringLiteral("status"), QStringLiteral("pending"));
    } else {
        m_spaceRow.insert(QStringLiteral("status"), QStringLiteral("skipped"));
    }
    for (QVariant &value : m_rooms) {
        QVariantMap row = value.toMap();
        const bool run = m_cascade
                         && row.value(QStringLiteral("eligible")).toBool()
                         && row.value(QStringLiteral("selected")).toBool();
        if (run)
            m_steps.append(row.value(QStringLiteral("roomId")).toString());
        row.insert(QStringLiteral("status"),
                   run ? QStringLiteral("pending") : QStringLiteral("skipped"));
        value = row;
    }
    m_nextStep = 0;
    m_succeeded = 0;
    m_failed = 0;
    setPhase(QStringLiteral("running"));
    dispatchNext();
}

void SpaceModerationController::markRow(const QString &roomId,
                                        const QString &status,
                                        const QString &message)
{
    if (roomId == m_spaceId) {
        m_spaceRow.insert(QStringLiteral("status"), status);
        m_spaceRow.insert(QStringLiteral("message"), message);
        return;
    }
    for (QVariant &value : m_rooms) {
        QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("roomId")).toString() != roomId)
            continue;
        row.insert(QStringLiteral("status"), status);
        row.insert(QStringLiteral("message"), message);
        value = row;
        return;
    }
}

void SpaceModerationController::dispatchNext()
{
    // One step at a time: a Space with many rooms would otherwise meet the
    // server's rate limit on its own requests.
    while (m_nextStep < m_steps.size()) {
        const QString roomId = m_steps.at(m_nextStep++);
        quint64 opId = 0;
        if (m_client) {
            if (m_op == QLatin1String("kick"))
                opId = m_client->kickUser(roomId, m_userId, m_reason);
            else if (m_op == QLatin1String("ban"))
                opId = m_client->banUser(roomId, m_userId, m_reason);
            else if (m_op == QLatin1String("unban"))
                opId = m_client->unbanUser(roomId, m_userId, m_reason);
        }
        if (opId == 0) {
            ++m_failed;
            markRow(roomId, QStringLiteral("failed"),
                    tr("The request could not be sent."));
            continue;
        }
        m_stepOp = opId;
        m_stepRoom = roomId;
        markRow(roomId, QStringLiteral("running"), QString());
        Q_EMIT stateChanged();
        return;
    }
    m_stepOp = 0;
    m_stepRoom.clear();
    qCDebug(lcSpaceModeration) << "done op=" << m_op << "ok=" << m_succeeded
                               << "failed=" << m_failed;
    setPhase(QStringLiteral("done"));
    Q_EMIT finished(m_spaceId, m_userId, m_op, m_succeeded, m_failed);
}

void SpaceModerationController::onModerationFinished(quint64 opId,
                                                     const QString &roomId,
                                                     const QString &userId,
                                                     const QString &op,
                                                     bool ok,
                                                     const QString &category)
{
    Q_UNUSED(op);
    if (opId == 0 || opId != m_stepOp || roomId != m_stepRoom
        || userId != m_userId) {
        return;
    }
    m_stepOp = 0;
    if (ok) {
        ++m_succeeded;
        markRow(roomId, QStringLiteral("ok"), QString());
    } else {
        ++m_failed;
        markRow(roomId, QStringLiteral("failed"), failureText(category));
    }
    dispatchNext();
}

QString SpaceModerationController::reasonText(const QString &reason)
{
    if (reason == QLatin1String("not_a_member"))
        return tr("Not a member here");
    if (reason == QLatin1String("already_banned"))
        return tr("Already banned here");
    if (reason == QLatin1String("not_banned"))
        return tr("Not banned here");
    if (reason == QLatin1String("no_permission"))
        return tr("You don't have permission here");
    if (reason == QLatin1String("outranked"))
        return tr("Their role here is not below yours");
    if (reason == QLatin1String("not_joined"))
        return tr("You are not in this room");
    if (reason == QLatin1String("unknown"))
        return tr("Their membership here could not be checked");
    return tr("Not available here");
}

QString SpaceModerationController::failureText(const QString &category)
{
    if (category == QLatin1String("forbidden"))
        return tr("The server refused. You may not have permission here.");
    if (category == QLatin1String("rate_limited"))
        return tr("The server is rate limiting. Try again shortly.");
    return tr("Failed. Check your connection and retry.");
}

QString SpaceModerationController::title() const
{
    const QString who = m_displayName.isEmpty() ? m_userId : m_displayName;
    const QString where = m_spaceName.isEmpty() ? tr("this space")
                                                : m_spaceName;
    // The plan refused the Space itself: only its rooms can change.
    if (spaceSkipped()) {
        if (m_op == QLatin1String("kick"))
            return tr("Kick %1 from rooms in %2?").arg(who, where);
        if (m_op == QLatin1String("ban"))
            return tr("Ban %1 from rooms in %2?").arg(who, where);
        if (m_op == QLatin1String("unban"))
            return tr("Unban %1 in rooms of %2?").arg(who, where);
    }
    if (m_op == QLatin1String("kick"))
        return tr("Kick %1 from %2?").arg(who, where);
    if (m_op == QLatin1String("ban"))
        return tr("Ban %1 from %2?").arg(who, where);
    if (m_op == QLatin1String("unban"))
        return tr("Unban %1 in %2?").arg(who, where);
    return QString();
}

QString SpaceModerationController::consequenceText() const
{
    // Each names what happens to the Space and, separately, to its rooms,
    // because a Space membership does not carry into the rooms.
    if (spaceSkipped()) {
        return tr("The space itself is unchanged (%1). Only the rooms selected "
                  "below are affected.")
            .arg(m_spaceRow.value(QStringLiteral("reasonText")).toString());
    }
    if (m_op == QLatin1String("kick"))
        return tr("They are removed from the space and can join again if "
                  "invited, or if the space is public. Kicking from the "
                  "space alone does not remove them from its rooms.");
    if (m_op == QLatin1String("ban"))
        return tr("They are removed from the space and cannot join it again "
                  "until someone unbans them. Banning from the space alone "
                  "does not remove them from its rooms.");
    if (m_op == QLatin1String("unban"))
        return tr("They can join the space again if invited, or if the "
                  "space is public. They are not invited back.");
    return QString();
}

QString SpaceModerationController::cascadeLabel() const
{
    if (m_op == QLatin1String("kick"))
        return tr("Also kick them from rooms in this space");
    if (m_op == QLatin1String("ban"))
        return tr("Also ban them from rooms in this space");
    if (m_op == QLatin1String("unban"))
        return tr("Also unban them in rooms in this space");
    return QString();
}

QString SpaceModerationController::confirmLabel() const
{
    if (m_op == QLatin1String("kick"))
        return tr("Kick");
    if (m_op == QLatin1String("ban"))
        return tr("Ban");
    if (m_op == QLatin1String("unban"))
        return tr("Unban");
    return QString();
}

QString SpaceModerationController::statusText() const
{
    if (m_phase == QLatin1String("planning"))
        return tr("Checking where you can do this…");
    if (m_phase == QLatin1String("failed"))
        return tr("Lightning could not check this space's rooms. Nothing "
                  "was changed.");
    if (m_phase == QLatin1String("ready") && !canConfirm())
        return tr("There is nowhere you can do this: see the reasons "
                  "below.");
    if (m_phase == QLatin1String("running"))
        return tr("Working: %1 of %2")
            .arg(m_succeeded + m_failed + 1)
            .arg(m_steps.size());
    if (m_phase == QLatin1String("done")) {
        if (m_failed == 0)
            return tr("Done: %1 succeeded.").arg(m_succeeded);
        return tr("Done with errors: %1 succeeded, %2 failed. The failed "
                  "rooms are marked below.")
            .arg(m_succeeded)
            .arg(m_failed);
    }
    return QString();
}
