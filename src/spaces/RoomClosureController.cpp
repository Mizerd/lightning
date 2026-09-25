#include "spaces/RoomClosureController.h"

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QLoggingCategory>
#include <QSet>
#include <QTimer>

Q_LOGGING_CATEGORY(lcRoomClosure, "lightning.roomclosure")

namespace {

const QString kClose = QStringLiteral("close");
const QString kDelete = QStringLiteral("delete");

} // namespace

RoomClosureController::RoomClosureController(QObject *parent)
    : QObject(parent)
    , m_planTimer(new QTimer(this))
{
    m_planTimer->setSingleShot(true);
    connect(m_planTimer, &QTimer::timeout, this,
            &RoomClosureController::onPlanTimedOut);
    m_stepTimer = new QTimer(this);
    m_stepTimer->setSingleShot(true);
    connect(m_stepTimer, &QTimer::timeout, this,
            &RoomClosureController::onStepTimedOut);
}

void RoomClosureController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    forgetAccount();
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::roomClosurePlanReceived, this,
            &RoomClosureController::onPlanReceived);
    connect(m_client, &MatrixClient::roomClosureProgress, this,
            &RoomClosureController::onClosureProgress);
    connect(m_client, &MatrixClient::roomClosureFinished, this,
            &RoomClosureController::onClosureFinished);
    connect(m_client, &MatrixClient::serverAdminStatusReceived, this,
            &RoomClosureController::onAdminStatus);
    connect(m_client, &MatrixClient::adminRoomDeleteProgress, this,
            &RoomClosureController::onDeleteProgress);
    connect(m_client, &MatrixClient::adminRoomDeleteFinished, this,
            &RoomClosureController::onDeleteFinished);
    // A flow, and the administrator answer, belong to one account.
    connect(m_client, &MatrixClient::loggedOut, this,
            &RoomClosureController::forgetAccount);
}

void RoomClosureController::setScopeResolver(
    std::function<QStringList(const QString &)> resolver)
{
    m_scopeResolver = std::move(resolver);
}

void RoomClosureController::setParentResolver(
    std::function<QStringList(const QString &)> resolver)
{
    m_parentResolver = std::move(resolver);
}

void RoomClosureController::setRoomInfoResolver(
    std::function<QVariantMap(const QString &)> resolver)
{
    m_infoResolver = std::move(resolver);
}

void RoomClosureController::resetForAccountChange()
{
    forgetAccount();
}

bool RoomClosureController::accountMoved(const QString &pinned) const
{
    return !m_client || m_client->currentUserId() != pinned;
}

void RoomClosureController::forgetAccount()
{
    m_serverAdmin = QStringLiteral("unknown");
    m_adminOp = 0;
    m_adminUser.clear();
    m_adminUnavailable = false;
    reset();
}

void RoomClosureController::setPhase(const QString &phase)
{
    m_phase = phase;
    Q_EMIT stateChanged();
}

void RoomClosureController::reset()
{
    m_mode.clear();
    m_flowUser.clear();
    m_targetId.clear();
    m_targetName.clear();
    m_targetIsSpace = false;
    m_targetRow.clear();
    m_rooms.clear();
    m_scope.clear();
    m_parentCandidates.clear();
    m_parents.clear();
    m_lockedParents.clear();
    m_leaveHeld.clear();
    m_resultPending = false;
    m_cascade = true;
    m_leaveAfter = true;
    m_unlistFromParents = true;
    m_block = true;
    m_planTruncated = false;
    m_typed.clear();
    m_reason.clear();
    m_notice.clear();
    m_planOp = 0;
    m_planTimer->stop();
    m_stepTimer->stop();
    m_steps.clear();
    m_nextStep = 0;
    m_stepOp = 0;
    m_stepRoom.clear();
    m_succeeded = 0;
    m_partial = 0;
    m_failed = 0;
    m_unknown = 0;
    setPhase(QStringLiteral("idle"));
}

void RoomClosureController::viewClosed()
{
    if (m_phase == QLatin1String("running")) {
        m_resultPending = true;
        return;
    }
    reset();
}

QString RoomClosureController::serverAdmin() const
{
    if (m_serverAdmin != QLatin1String("unknown") && accountMoved(m_adminUser))
        return QStringLiteral("unknown");
    return m_serverAdmin;
}

void RoomClosureController::checkServerAdmin()
{
    // An answer, or a question in flight, about another account is dropped
    // and this account is asked.
    if (m_client && m_serverAdmin != QLatin1String("unknown")
        && accountMoved(m_adminUser)) {
        m_serverAdmin = QStringLiteral("unknown");
        m_adminOp = 0;
        m_adminUnavailable = false;
    }
    if (!m_client || m_serverAdmin == QLatin1String("yes")
        || m_serverAdmin == QLatin1String("checking")) {
        return;
    }
    // A server that said "not an administrator" is believed for the
    // session; one we could not reach is asked again later.
    if (m_serverAdmin == QLatin1String("no")
        && (!m_adminUnavailable || !m_adminAnswered.isValid()
            || m_adminAnswered.elapsed() < m_adminRetryMs)) {
        return;
    }
    m_adminUser = m_client->currentUserId();
    m_adminOp = m_client->requestServerAdminStatus();
    if (m_adminOp != 0) {
        m_serverAdmin = QStringLiteral("checking");
    } else {
        // Not asked at all: try again later rather than believe "no".
        m_serverAdmin = QStringLiteral("no");
        m_adminUnavailable = true;
        m_adminAnswered.start();
    }
    Q_EMIT stateChanged();
}

void RoomClosureController::onAdminStatus(quint64 opId, bool admin,
                                          const QString &detail)
{
    if (opId == 0 || opId != m_adminOp)
        return;
    m_adminOp = 0;
    // An answer about another account says nothing about this one.
    if (accountMoved(m_adminUser)) {
        m_serverAdmin = QStringLiteral("unknown");
        m_adminUser.clear();
        Q_EMIT stateChanged();
        return;
    }
    m_serverAdmin = admin ? QStringLiteral("yes") : QStringLiteral("no");
    m_adminUnavailable = !admin && detail != QLatin1String("not_admin");
    m_adminAnswered.start();
    qCDebug(lcRoomClosure) << "server admin" << admin << detail;
    Q_EMIT stateChanged();
}

QString RoomClosureController::serverName() const
{
    const QString own = m_client ? m_client->currentUserId() : QString();
    const int colon = own.indexOf(QLatin1Char(':'));
    return colon > 0 ? own.mid(colon + 1) : tr("your homeserver");
}

int RoomClosureController::alsoElsewhere(const QString &roomId) const
{
    if (!m_parentResolver)
        return 0;
    const QSet<QString> scope(m_scope.constBegin(), m_scope.constEnd());
    int count = 0;
    for (const QString &parent : m_parentResolver(roomId))
        count += !scope.contains(parent);
    return count;
}

bool RoomClosureController::begin(const QString &roomId, const QString &name,
                                  bool isSpace, const QString &mode)
{
    // Steps already on the server must still be counted: say so rather than
    // start another flow over them.
    if (m_phase == QLatin1String("running")) {
        m_notice = tr("An earlier close or delete is still running and is "
                      "shown here. Start the new one when it has finished.");
        Q_EMIT stateChanged();
        return false;
    }
    // A flow that finished while its dialog was hidden is shown once first.
    if (m_phase == QLatin1String("done") && m_resultPending) {
        m_resultPending = false;
        m_notice = tr("This is how the earlier close or delete ended. Close "
                      "this to start the new one.");
        Q_EMIT stateChanged();
        return false;
    }
    reset();
    if (!m_client || roomId.isEmpty() || (mode != kClose && mode != kDelete))
        return false;
    // Offered only after the server said so, about this account.
    if (mode == kDelete
        && (m_serverAdmin != QLatin1String("yes") || accountMoved(m_adminUser)))
        return false;
    m_flowUser = m_client->currentUserId();
    m_mode = mode;
    m_targetId = roomId;
    m_targetName = name;
    m_targetIsSpace = isSpace;
    QStringList scope = isSpace && m_scopeResolver ? m_scopeResolver(roomId)
                                                   : QStringList{};
    scope.removeAll(roomId);
    scope.prepend(roomId);
    m_scope = scope;
    if (!isSpace && m_parentResolver)
        m_parentCandidates = m_parentResolver(roomId);

    if (mode == kDelete) {
        // The admin API needs no plan: the server decides.
        m_targetRow = deleteRow(roomId);
        m_targetRow.insert(QStringLiteral("isSpace"), isSpace);
        if (!name.isEmpty())
            m_targetRow.insert(QStringLiteral("name"), name);
        for (int i = 1; i < m_scope.size(); ++i)
            m_rooms.append(deleteRow(m_scope.at(i)));
        setPhase(QStringLiteral("ready"));
        return true;
    }

    // The parents are asked about too, only for whether the viewer may
    // change their children; they are not rows.
    QStringList parents;
    for (const QString &parent : m_parentCandidates) {
        if (!m_scope.contains(parent) && !parents.contains(parent))
            parents.append(parent);
    }
    m_planOp = m_client->requestRoomClosurePlan(m_scope, parents);
    if (m_planOp != 0)
        m_planTimer->start(m_planTimeoutMs);
    setPhase(m_planOp != 0 ? QStringLiteral("planning")
                           : QStringLiteral("failed"));
    return m_planOp != 0;
}

QVariantMap RoomClosureController::deleteRow(const QString &roomId) const
{
    const QVariantMap info = m_infoResolver ? m_infoResolver(roomId)
                                            : QVariantMap{};
    const QString name = info.value(QStringLiteral("name")).toString();
    const int elsewhere = alsoElsewhere(roomId);
    return QVariantMap{
        { QStringLiteral("roomId"), roomId },
        { QStringLiteral("name"), name.isEmpty() ? roomId : name },
        { QStringLiteral("isSpace"),
          info.value(QStringLiteral("isSpace")).toBool() },
        { QStringLiteral("eligible"), true },
        { QStringLiteral("reason"), QString() },
        { QStringLiteral("reasonText"), QString() },
        { QStringLiteral("summary"), QString() },
        { QStringLiteral("alsoElsewhere"), elsewhere },
        // A room another Space also lists starts unselected.
        { QStringLiteral("selected"), elsewhere == 0 },
        { QStringLiteral("status"), QString() },
        { QStringLiteral("message"), QString() },
        { QStringLiteral("progress"), QString() },
    };
}

QVariantMap RoomClosureController::rowFromPlan(const QVariantMap &plan) const
{
    // Fails closed: no reason key or no room id is not offered.
    const QString roomId = plan.value(QStringLiteral("roomId")).toString();
    QString reason = plan.value(QStringLiteral("reason")).toString();
    if (!plan.contains(QStringLiteral("reason")) || roomId.isEmpty())
        reason = QStringLiteral("unknown");
    const bool eligible = reason.isEmpty();
    const int elsewhere = alsoElsewhere(roomId);
    const QString name = plan.value(QStringLiteral("name")).toString();
    return QVariantMap{
        { QStringLiteral("roomId"), roomId },
        { QStringLiteral("name"), name.isEmpty() ? roomId : name },
        { QStringLiteral("isSpace"), plan.value(QStringLiteral("isSpace")) },
        { QStringLiteral("eligible"), eligible },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("reasonText"),
          eligible ? QString() : reasonText(reason) },
        { QStringLiteral("summary"),
          eligible ? closeSummary(plan) : QString() },
        { QStringLiteral("alsoElsewhere"), elsewhere },
        { QStringLiteral("selected"), eligible && elsewhere == 0 },
        { QStringLiteral("status"), QString() },
        { QStringLiteral("message"), QString() },
        { QStringLiteral("progress"), QString() },
    };
}

void RoomClosureController::onPlanReceived(quint64 opId, bool truncated,
                                           const QVariantList &rows)
{
    if (opId == 0 || opId != m_planOp)
        return;
    if (accountMoved(m_flowUser)) {
        forgetAccount();
        return;
    }
    m_planOp = 0;
    m_planTimer->stop();
    m_planTruncated = truncated;
    m_targetRow.clear();
    m_rooms.clear();
    m_parents.clear();
    m_lockedParents = m_parentCandidates;
    for (const QVariant &value : rows) {
        const QVariantMap plan = value.toMap();
        const QString planId = plan.value(QStringLiteral("roomId")).toString();
        if (m_parentCandidates.contains(planId)
            && plan.value(QStringLiteral("canEditChildren")).toBool()) {
            m_parents.append(planId);
            m_lockedParents.removeAll(planId);
        }
    }
    for (const QVariant &value : rows) {
        const QVariantMap row = rowFromPlan(value.toMap());
        const QString roomId = row.value(QStringLiteral("roomId")).toString();
        if (roomId == m_targetId) {
            if (m_targetRow.isEmpty())
                m_targetRow = row;
            continue;
        }
        // Only what was asked about.
        if (m_scope.contains(roomId))
            m_rooms.append(row);
    }
    if (m_targetRow.isEmpty()) {
        m_targetRow = rowFromPlan(QVariantMap{
            { QStringLiteral("roomId"), m_targetId },
            { QStringLiteral("name"), m_targetName },
            { QStringLiteral("isSpace"), m_targetIsSpace },
            { QStringLiteral("reason"), QStringLiteral("unknown") },
        });
    }
    qCDebug(lcRoomClosure) << "plan target offered=" << targetEligible()
                           << "rooms=" << m_rooms.size()
                           << "offered=" << eligibleRoomCount()
                           << "truncated=" << truncated;
    setPhase(QStringLiteral("ready"));
}

void RoomClosureController::onPlanTimedOut()
{
    if (m_phase != QLatin1String("planning") || m_planOp == 0)
        return;
    qCWarning(lcRoomClosure) << "plan did not answer in" << m_planTimeoutMs
                             << "ms";
    m_planOp = 0;
    setPhase(QStringLiteral("failed"));
}

bool RoomClosureController::targetEligible() const
{
    return m_targetRow.value(QStringLiteral("eligible")).toBool();
}

int RoomClosureController::eligibleRoomCount() const
{
    int count = 0;
    for (const QVariant &value : m_rooms)
        count += value.toMap().value(QStringLiteral("eligible")).toBool();
    return count;
}

int RoomClosureController::selectedRoomCount() const
{
    int count = 0;
    for (const QVariant &value : m_rooms) {
        const QVariantMap row = value.toMap();
        count += row.value(QStringLiteral("eligible")).toBool()
                 && row.value(QStringLiteral("selected")).toBool();
    }
    return count;
}

int RoomClosureController::uncheckedRoomCount() const
{
    int count = 0;
    for (const QVariant &value : m_rooms) {
        count += value.toMap().value(QStringLiteral("reason")).toString()
                 == QLatin1String("not_checked");
    }
    return count;
}

void RoomClosureController::setCascade(bool cascade)
{
    if (m_cascade == cascade || m_phase != QLatin1String("ready"))
        return;
    m_cascade = cascade;
    Q_EMIT stateChanged();
}

void RoomClosureController::setLeaveAfter(bool leave)
{
    if (m_leaveAfter == leave || m_phase != QLatin1String("ready"))
        return;
    m_leaveAfter = leave;
    Q_EMIT stateChanged();
}

QStringList RoomClosureController::lockedParentNames() const
{
    QStringList names;
    for (const QString &id : m_lockedParents) {
        const QVariantMap info = m_infoResolver ? m_infoResolver(id)
                                                : QVariantMap{};
        const QString name = info.value(QStringLiteral("name")).toString();
        names.append(name.isEmpty() ? id : name);
    }
    return names;
}

QStringList RoomClosureController::parentSpaceNames() const
{
    QStringList names;
    for (const QString &id : m_parents) {
        const QVariantMap info = m_infoResolver ? m_infoResolver(id)
                                                : QVariantMap{};
        const QString name = info.value(QStringLiteral("name")).toString();
        names.append(name.isEmpty() ? id : name);
    }
    return names;
}

void RoomClosureController::setUnlistFromParents(bool unlist)
{
    if (m_unlistFromParents == unlist || m_phase != QLatin1String("ready"))
        return;
    m_unlistFromParents = unlist;
    Q_EMIT stateChanged();
}

void RoomClosureController::setBlock(bool block)
{
    if (m_block == block || m_phase != QLatin1String("ready"))
        return;
    m_block = block;
    Q_EMIT stateChanged();
}

void RoomClosureController::setRoomSelected(const QString &roomId,
                                            bool selected)
{
    if (m_phase != QLatin1String("ready"))
        return;
    for (QVariant &value : m_rooms) {
        QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("roomId")).toString() != roomId)
            continue;
        if (!row.value(QStringLiteral("eligible")).toBool()
            || row.value(QStringLiteral("selected")).toBool() == selected) {
            return;
        }
        row.insert(QStringLiteral("selected"), selected);
        value = row;
        Q_EMIT stateChanged();
        return;
    }
}

void RoomClosureController::setAllRoomsSelected(bool selected)
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

bool RoomClosureController::confirmationNameUsable() const
{
    const QString name = m_targetName.trimmed();
    if (name.isEmpty())
        return false;
    // Zero-width, format, control and other characters nobody can type.
    const QList<uint> codepoints = name.toUcs4();
    for (const uint c : codepoints) {
        if (c != ' ' && (!QChar::isPrint(c) || QChar::isSpace(c)))
            return false;
    }
    return true;
}

QString RoomClosureController::confirmationPhrase() const
{
    return confirmationNameUsable() ? m_targetName.trimmed() : m_targetId;
}

void RoomClosureController::setTypedConfirmation(const QString &text)
{
    if (m_typed == text)
        return;
    m_typed = text;
    Q_EMIT stateChanged();
}

bool RoomClosureController::typedConfirmationMatches() const
{
    if (m_mode != kDelete)
        return true;
    const QString typed = m_typed.trimmed();
    if (typed.isEmpty())
        return false;
    // The id always works, whatever the name holds.
    return typed == confirmationPhrase() || typed == m_targetId;
}

bool RoomClosureController::canConfirm() const
{
    if (m_phase != QLatin1String("ready") || !m_client)
        return false;
    if (accountMoved(m_flowUser))
        return false;
    if (m_mode == kDelete) {
        if (m_serverAdmin != QLatin1String("yes")
            || !typedConfirmationMatches()) {
            return false;
        }
    }
    return targetEligible()
           || (m_targetIsSpace && m_cascade && selectedRoomCount() > 0);
}

void RoomClosureController::confirm(const QString &reason)
{
    if (!canConfirm())
        return;
    m_reason = m_mode == kClose ? reason.trimmed() : QString();
    m_steps.clear();
    // Rooms in reverse scope order, so a subspace's rooms go before the
    // subspace, and the target last: a failure part-way leaves the viewer in
    // the Space to retry.
    for (int i = m_rooms.size() - 1; i >= 0; --i) {
        QVariantMap row = m_rooms.at(i).toMap();
        const bool run = m_cascade
                         && row.value(QStringLiteral("eligible")).toBool()
                         && row.value(QStringLiteral("selected")).toBool();
        if (run)
            m_steps.append(row.value(QStringLiteral("roomId")).toString());
        row.insert(QStringLiteral("status"),
                   run ? QStringLiteral("pending") : QStringLiteral("skipped"));
        m_rooms[i] = row;
    }
    if (targetEligible()) {
        m_steps.append(m_targetId);
        m_targetRow.insert(QStringLiteral("status"), QStringLiteral("pending"));
    } else {
        m_targetRow.insert(QStringLiteral("status"), QStringLiteral("skipped"));
    }
    m_nextStep = 0;
    m_succeeded = 0;
    m_partial = 0;
    m_failed = 0;
    m_unknown = 0;
    m_leaveHeld.clear();
    qCInfo(lcRoomClosure) << m_mode << "steps=" << m_steps.size();
    setPhase(QStringLiteral("running"));
    dispatchNext();
}

void RoomClosureController::markRow(const QString &roomId,
                                    const QString &status,
                                    const QString &message)
{
    if (roomId == m_targetId) {
        m_targetRow.insert(QStringLiteral("status"), status);
        m_targetRow.insert(QStringLiteral("message"), message);
        m_targetRow.insert(QStringLiteral("progress"), QString());
        return;
    }
    for (QVariant &value : m_rooms) {
        QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("roomId")).toString() != roomId)
            continue;
        row.insert(QStringLiteral("status"), status);
        row.insert(QStringLiteral("message"), message);
        row.insert(QStringLiteral("progress"), QString());
        value = row;
        return;
    }
}

void RoomClosureController::setRowProgress(const QString &roomId,
                                           const QString &progress)
{
    if (roomId == m_targetId) {
        m_targetRow.insert(QStringLiteral("progress"), progress);
    } else {
        for (QVariant &value : m_rooms) {
            QVariantMap row = value.toMap();
            if (row.value(QStringLiteral("roomId")).toString() != roomId)
                continue;
            row.insert(QStringLiteral("progress"), progress);
            value = row;
            break;
        }
    }
    Q_EMIT stateChanged();
}

void RoomClosureController::dispatchNext()
{
    // Nothing more goes out through a client that now speaks for another
    // account: the remaining steps belong to the one that started them.
    if (accountMoved(m_flowUser)) {
        qCWarning(lcRoomClosure) << "account changed; flow dropped";
        forgetAccount();
        return;
    }
    // One room at a time: each close is many requests of its own.
    while (m_nextStep < m_steps.size()) {
        const QString roomId = m_steps.at(m_nextStep++);
        quint64 opId = 0;
        if (m_client) {
            if (m_mode == kClose) {
                // Out of its Spaces only for a single room; a Space's own
                // rooms keep their place in it.
                const QStringList unlist =
                    roomId == m_targetId && !m_targetIsSpace
                            && m_unlistFromParents
                        ? m_parents
                        : QStringList{};
                // Never leave a Space while anything beneath it is left
                // undone: once out, it cannot be cascaded again.
                bool leave = m_leaveAfter;
                if (leave && anyDescendantUnfinished(roomId)) {
                    leave = false;
                    m_leaveHeld.insert(roomId);
                }
                opId = m_client->closeRoom(roomId, m_reason, leave, unlist);
            } else if (m_mode == kDelete) {
                opId = m_client->adminDeleteRoom(roomId, m_block);
            }
        }
        if (opId == 0) {
            ++m_failed;
            markRow(roomId, QStringLiteral("failed"),
                    tr("The request could not be sent."));
            continue;
        }
        m_stepOp = opId;
        m_stepRoom = roomId;
        m_stepTimer->start(m_stepSilenceMs);
        markRow(roomId, QStringLiteral("running"), QString());
        Q_EMIT stateChanged();
        return;
    }
    m_stepOp = 0;
    m_stepRoom.clear();
    m_stepTimer->stop();
    m_notice.clear();
    qCInfo(lcRoomClosure) << m_mode << "done ok=" << m_succeeded
                          << "partial=" << m_partial << "failed=" << m_failed;
    setPhase(QStringLiteral("done"));
}

void RoomClosureController::stepAnswered(const QString &roomId,
                                         const QString &status,
                                         const QString &message)
{
    m_stepOp = 0;
    m_stepTimer->stop();
    if (status == QLatin1String("ok"))
        ++m_succeeded;
    else if (status == QLatin1String("partial")
             || status == QLatin1String("following"))
        ++m_partial;
    else if (status == QLatin1String("unknown"))
        ++m_unknown;
    else
        ++m_failed;
    markRow(roomId, status, message);
    dispatchNext();
}

void RoomClosureController::onClosureProgress(quint64 opId,
                                              const QString &roomId, int done,
                                              int total)
{
    if (opId == 0 || opId != m_stepOp || roomId != m_stepRoom)
        return;
    m_stepTimer->start(m_stepSilenceMs);
    setRowProgress(roomId, total > 0
                               ? tr("Removing members: %1 of %2")
                                     .arg(done)
                                     .arg(total)
                               : tr("Working…"));
}

bool RoomClosureController::stepWatchdogArmedForTest() const
{
    return m_stepTimer->isActive();
}

int RoomClosureController::stepWatchdogIntervalForTest() const
{
    return m_stepTimer->interval();
}

void RoomClosureController::onStepTimedOut()
{
    if (m_phase != QLatin1String("running") || m_stepOp == 0)
        return;
    qCWarning(lcRoomClosure) << m_mode << "step did not answer in"
                             << m_stepSilenceMs << "ms";
    // A late answer is ignored; the server may still finish it.
    stepAnswered(m_stepRoom, QStringLiteral("unknown"),
                 tr("No answer yet. It may still finish on the server: check "
                    "the room before running this again."));
}

QStringList RoomClosureController::descendantsOf(const QString &spaceId) const
{
    // Parent links within the scope only, cycle-safe.
    QHash<QString, QStringList> children;
    if (m_parentResolver) {
        for (const QString &roomId : m_scope) {
            for (const QString &parent : m_parentResolver(roomId)) {
                if (parent != roomId && m_scope.contains(parent))
                    children[parent].append(roomId);
            }
        }
    }
    QStringList out;
    QSet<QString> seen{ spaceId };
    QStringList queue{ spaceId };
    while (!queue.isEmpty()) {
        const QString next = queue.takeFirst();
        for (const QString &child : children.value(next)) {
            if (seen.contains(child))
                continue;
            seen.insert(child);
            out.append(child);
            queue.append(child);
        }
    }
    // The target owns every room in its scope, linked or not.
    if (spaceId == m_targetId) {
        for (int i = 1; i < m_scope.size(); ++i) {
            if (!seen.contains(m_scope.at(i)))
                out.append(m_scope.at(i));
        }
    }
    return out;
}

bool RoomClosureController::anyDescendantUnfinished(const QString &spaceId) const
{
    const QStringList below = descendantsOf(spaceId);
    QSet<QString> withRow;
    for (const QVariant &value : m_rooms) {
        const QVariantMap row = value.toMap();
        const QString roomId = row.value(QStringLiteral("roomId")).toString();
        withRow.insert(roomId);
        if (!below.contains(roomId))
            continue;
        // Not done yet counts too: a room listed under two subspaces runs
        // under only one of them.
        const QString status = row.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("partial")
            || status == QLatin1String("failed")
            || status == QLatin1String("unknown")
            || status == QLatin1String("pending")
            || status == QLatin1String("running")) {
            return true;
        }
        // Rooms the plan could not assess may still need closing.
        const QString reason = row.value(QStringLiteral("reason")).toString();
        if (reason == QLatin1String("unknown")
            || reason == QLatin1String("not_checked")) {
            return true;
        }
    }
    // A room beneath it with no row at all (past the plan's cap, or dropped
    // by the backend) was never assessed or closed.
    for (const QString &roomId : below) {
        if (!withRow.contains(roomId))
            return true;
    }
    return false;
}

void RoomClosureController::onClosureFinished(quint64 opId,
                                              const QString &roomId,
                                              const QVariantMap &result)
{
    if (opId == 0 || opId != m_stepOp || roomId != m_stepRoom
        || m_mode != kClose) {
        return;
    }
    const QString outcome = result.value(QStringLiteral("outcome")).toString();
    // A join-rule change that timed out may have landed: not known.
    const bool unanswered =
        result.value(QStringLiteral("category")).toString()
        == QLatin1String("unknown");
    const QString status = outcome == QLatin1String("closed")
                               ? QStringLiteral("ok")
                           : outcome == QLatin1String("partial")
                               ? QStringLiteral("partial")
                           : unanswered ? QStringLiteral("unknown")
                                        : QStringLiteral("failed");
    QString message = closeResultText(result);
    if (m_leaveHeld.contains(roomId))
        message += tr(" · you stayed: rooms inside it are not fully closed");
    stepAnswered(roomId, status, message);
}

void RoomClosureController::onDeleteProgress(quint64 opId,
                                             const QString &roomId,
                                             const QString &status)
{
    if (opId == 0 || opId != m_stepOp || roomId != m_stepRoom)
        return;
    if (status == QLatin1String("scheduled"))
        setRowProgress(roomId, tr("Queued on the server"));
    else if (status == QLatin1String("active"))
        setRowProgress(roomId, tr("The server is deleting it"));
}

void RoomClosureController::onDeleteFinished(quint64 opId,
                                             const QString &roomId,
                                             const QVariantMap &result)
{
    if (opId == 0 || opId != m_stepOp || roomId != m_stepRoom
        || m_mode != kDelete) {
        return;
    }
    const QString status = result.value(QStringLiteral("status")).toString();
    QString rowStatus = QStringLiteral("failed");
    if (result.value(QStringLiteral("ok")).toBool()
        && status == QLatin1String("complete")) {
        rowStatus = QStringLiteral("ok");
    } else if (status == QLatin1String("following")) {
        rowStatus = QStringLiteral("following");
    } else if (status == QLatin1String("unknown")) {
        rowStatus = QStringLiteral("unknown");
    }
    stepAnswered(roomId, rowStatus, deleteResultText(result));
}

QString RoomClosureController::closeResultText(const QVariantMap &result)
{
    const QString outcome = result.value(QStringLiteral("outcome")).toString();
    const QString category = result.value(QStringLiteral("category")).toString();
    if (outcome != QLatin1String("closed")
        && outcome != QLatin1String("partial")) {
        if (category == QLatin1String("forbidden"))
            return tr("The server refused to make it invite-only. Nothing "
                      "was changed.");
        if (category == QLatin1String("unknown"))
            return tr("No answer came back when making it invite-only, so it "
                      "may or may not have changed. Nothing else was done.");
        return tr("It could not be made invite-only. Nothing was changed.");
    }
    QStringList parts;
    const int removed = result.value(QStringLiteral("removed")).toInt();
    const int removeFailed = result.value(QStringLiteral("removeFailed")).toInt();
    const int notAttempted = result.value(QStringLiteral("notAttempted")).toInt();
    const int staying = result.value(QStringLiteral("staying")).toInt();
    const int unlistFailed = result.value(QStringLiteral("unlistFailed")).toInt();
    const QString directory =
        result.value(QStringLiteral("directory")).toString();
    parts.append(tr("Invite-only"));
    if (removed > 0)
        parts.append(tr("%1 removed").arg(removed));
    if (removeFailed > 0)
        parts.append(tr("%1 could not be removed").arg(removeFailed));
    if (notAttempted > 0)
        parts.append(tr("%1 not reached; run it again").arg(notAttempted));
    if (!result.value(QStringLiteral("membersRead")).toBool())
        parts.append(tr("its members could not be read"));
    if (staying > 0) {
        // Absent means an older answer: say nothing about why.
        const bool canKick = !result.contains(QStringLiteral("canKick"))
                             || result.value(QStringLiteral("canKick")).toBool();
        parts.append(canKick
                         ? tr("%1 stay (role not below yours)").arg(staying)
                         : tr("%1 stay (you can't remove members here)")
                               .arg(staying));
    }
    if (directory == QLatin1String("failed")
        || directory == QLatin1String("unknown"))
        parts.append(tr("may still be in the room directory"));
    if (unlistFailed > 0)
        parts.append(tr("still listed in %1 space(s)").arg(unlistFailed));
    if (result.value(QStringLiteral("left")).toBool())
        parts.append(tr("you left"));
    else if (outcome == QLatin1String("partial"))
        parts.append(tr("you stayed so you can run it again"));
    else if (!category.isEmpty())
        parts.append(tr("leaving failed"));
    return parts.join(QStringLiteral(" · "));
}

QString RoomClosureController::deleteResultText(const QVariantMap &result)
{
    const QString status = result.value(QStringLiteral("status")).toString();
    const QString category = result.value(QStringLiteral("category")).toString();
    if (status == QLatin1String("complete")
        && result.value(QStringLiteral("ok")).toBool()) {
        const int removed = result.value(QStringLiteral("removed")).toInt();
        const int failed = result.value(QStringLiteral("failedToRemove")).toInt();
        if (failed > 0)
            return tr("Deleted from the server · %1 removed · %2 could not be "
                      "removed").arg(removed).arg(failed);
        return tr("Deleted from the server · %1 removed").arg(removed);
    }
    if (status == QLatin1String("following"))
        return tr("The server is still deleting it. Check again later.");
    if (status == QLatin1String("unknown")
        || category == QLatin1String("unknown"))
        return tr("Whether the server deleted it is not known: no clear "
                  "answer came back. Check the room before trying again.");
    if (category == QLatin1String("invalid"))
        return tr("The server refused the request. A delete of this room may "
                  "already be running.");
    if (category == QLatin1String("unauthorized"))
        return tr("The server did not accept this session. Sign in again.");
    if (category == QLatin1String("forbidden"))
        return tr("The server refused. This account may no longer be an "
                  "administrator.");
    if (category == QLatin1String("unrecognized"))
        return tr("This server does not offer the delete it was asked for.");
    if (category == QLatin1String("server"))
        return tr("The server reported that the delete failed.");
    if (category == QLatin1String("rate_limited"))
        return tr("The server is rate limiting. Try again shortly.");
    return tr("Failed. Check your connection and retry.");
}

QString RoomClosureController::reasonText(const QString &reason)
{
    if (reason == QLatin1String("not_joined"))
        return tr("You are not in this room");
    if (reason == QLatin1String("no_permission"))
        return tr("You can't change who may join");
    if (reason == QLatin1String("unknown"))
        return tr("Its members could not be checked");
    if (reason == QLatin1String("not_checked"))
        return tr("Not checked in time; left unchanged");
    return tr("Not available here");
}

QString RoomClosureController::closeSummary(const QVariantMap &planRow)
{
    const int removable = planRow.value(QStringLiteral("removable")).toInt();
    const int staying = planRow.value(QStringLiteral("staying")).toInt();
    const QStringList names =
        planRow.value(QStringLiteral("stayingNames")).toStringList();
    const bool canKick = planRow.value(QStringLiteral("canKick"), true).toBool();
    const QString readable =
        planRow.value(QStringLiteral("worldReadable")).toBool()
            ? tr(" · its history stays readable by anyone")
            : QString();
    if (removable == 0 && staying == 0)
        return tr("Only you are here") + readable;
    if (!canKick)
        return tr("You can't remove members here: all %1 stay").arg(staying)
               + readable;
    QString text = removable > 0 ? tr("Removes %1").arg(removable)
                                 : tr("Removes nobody");
    if (staying > 0) {
        QString who = names.join(QStringLiteral(", "));
        if (staying > names.size() && !names.isEmpty())
            who = tr("%1 and %2 more")
                      .arg(who, QString::number(staying - names.size()));
        text += who.isEmpty()
                    ? tr(" · %1 stay").arg(staying)
                    : tr(" · %1 stay: %2").arg(QString::number(staying), who);
    }
    return text + readable;
}

QString RoomClosureController::title() const
{
    const QString what = m_targetName.isEmpty()
                             ? (m_targetIsSpace ? tr("this space")
                                                : tr("this room"))
                             : m_targetName;
    const bool withRooms = m_targetIsSpace && m_cascade
                           && selectedRoomCount() > 0;
    if (m_mode == kDelete) {
        if (withRooms)
            return tr("Delete %1 and %2 rooms from %3?")
                .arg(what, QString::number(selectedRoomCount()), serverName());
        return tr("Delete %1 from %2?").arg(what, serverName());
    }
    if (m_mode != kClose)
        return QString();
    if (withRooms)
        return tr("Close %1 and its rooms?").arg(what);
    return tr("Close %1?").arg(what);
}

QString RoomClosureController::consequenceText() const
{
    if (m_mode == kDelete) {
        const QString server = serverName();
        const QString what =
            m_targetIsSpace && m_cascade && selectedRoomCount() > 0
                ? tr("the space and the %1 rooms selected below")
                      .arg(selectedRoomCount())
                : tr("it");
        return tr("This deletes %2 from %1, your homeserver: everyone on %1 is "
                  "removed and %1's copy of the history is purged. It cannot "
                  "be undone. Members on other servers keep the rooms and "
                  "their history; for them they carry on without %1's users.")
            .arg(server, what);
    }
    if (m_mode != kClose)
        return QString();
    const QString what = m_targetIsSpace
                             ? tr("the space and each room you select")
                             : tr("the room");
    const QString first =
        m_leaveAfter
            ? tr("Closing makes %1 invite-only, takes it out of the public "
                 "room directory, removes the members you are allowed to "
                 "remove (where you may remove people, everyone whose role "
                 "is below yours), and then you leave.")
                  .arg(what)
            : tr("Closing makes %1 invite-only, takes it out of the public "
                 "room directory and removes the members you are allowed to "
                 "remove (where you may remove people, everyone whose role "
                 "is below yours). You stay.")
                  .arg(what);
    return first + QLatin1Char(' ')
           + tr("This does not delete anything: the history stays on every "
                "server that took part, people keep whatever their apps "
                "already downloaded, and a room whose history anyone can "
                "read stays readable. Everyone you cannot remove stays, and "
                "anyone whose role is not below yours can open it again.");
}

QString RoomClosureController::cascadeLabel() const
{
    if (m_mode == kDelete)
        return tr("Also delete rooms in this space");
    if (m_mode == kClose)
        return tr("Also close rooms in this space");
    return QString();
}

QString RoomClosureController::confirmLabel() const
{
    if (m_mode == kDelete)
        return tr("Delete from server");
    if (m_mode == kClose)
        return m_targetIsSpace ? tr("Close space") : tr("Close room");
    return QString();
}

QString RoomClosureController::statusText() const
{
    if (m_phase == QLatin1String("planning"))
        return tr("Checking what closing would do… This takes at most half "
                  "a minute.");
    if (m_phase == QLatin1String("failed"))
        return tr("Lightning could not check these rooms. Nothing was "
                  "changed.");
    if (m_phase == QLatin1String("ready")) {
        if (m_mode == kDelete && !typedConfirmationMatches())
            return tr("Type %1 to confirm.").arg(confirmationPhrase());
        if (!canConfirm())
            return tr("There is nothing here you can close: see the reasons "
                      "below.");
        if (uncheckedRoomCount() > 0)
            return tr("%1 of the rooms could not be checked in time. They are "
                      "not offered and will be left unchanged.")
                .arg(uncheckedRoomCount());
        return QString();
    }
    if (m_phase == QLatin1String("running"))
        return tr("Working: %1 of %2. You can hide this; it keeps going.")
            .arg(m_succeeded + m_partial + m_failed + m_unknown + 1)
            .arg(m_steps.size());
    if (m_phase == QLatin1String("done")) {
        if (m_mode == kDelete)
            return tr("Done: %1 deleted, %2 still being deleted by the server, "
                      "%3 failed, %4 not known yet.")
                .arg(m_succeeded)
                .arg(m_partial)
                .arg(m_failed)
                .arg(m_unknown);
        if (m_partial == 0 && m_failed == 0 && m_unknown == 0)
            return tr("Done: %1 closed.").arg(m_succeeded);
        return tr("Done: %1 closed, %2 partly closed, %3 failed, %4 not known "
                  "yet. The rows below say what is left.")
            .arg(m_succeeded)
            .arg(m_partial)
            .arg(m_failed)
            .arg(m_unknown);
    }
    return QString();
}
