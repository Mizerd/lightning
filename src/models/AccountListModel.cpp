#include "models/AccountListModel.h"

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"

#include <algorithm>

bool AccountListModel::Row::operator==(const Row &o) const
{
    return userId == o.userId && displayName == o.displayName && avatarUrl == o.avatarUrl
        && homeserver == o.homeserver && addedAt == o.addedAt && isActive == o.isActive
        && needsSignIn == o.needsSignIn && sessionRevoked == o.sessionRevoked
        && liveState == o.liveState && keepRunning == o.keepRunning && running == o.running
        && lastLiveAt == o.lastLiveAt;
}

AccountListModel::AccountListModel(SettingsManager *settings, AccountManager *accounts,
                                   const AccountActivityModel *activity,
                                   std::function<bool(const QString &uid)> hasRevocationProof,
                                   QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
    , m_accounts(accounts)
    , m_activity(activity)
    , m_hasRevocationProof(std::move(hasRevocationProof))
{
    if (m_settings) {
        // A record changed (a fresh sign-in, a removal): what the keyring
        // answered before may no longer hold.
        connect(m_settings, &SettingsManager::accountsChanged, this, [this] {
            forgetProbes();
            refresh();
        });
        connect(m_settings, &SettingsManager::keepRunningChanged, this, &AccountListModel::refresh);
        // isActive moves with the active account.
        connect(m_settings, &SettingsManager::sessionChanged, this, &AccountListModel::refresh);
    }
    if (m_accounts)
        connect(m_accounts, &AccountManager::sessionRevokedChanged, this, &AccountListModel::refresh);
    // Live states only: a summary (a badge) never changes a row here.
    if (m_activity)
        connect(m_activity, &AccountActivityModel::liveStateChanged, this,
                &AccountListModel::onActivityLiveStateChanged);
    m_rows = buildRows();
}

int AccountListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant AccountListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case UserIdRole:
        return row.userId;
    case DisplayNameRole:
        return row.displayName;
    case AvatarUrlRole:
        return row.avatarUrl;
    case HomeserverRole:
        return row.homeserver;
    case IsActiveRole:
        return row.isActive;
    case NeedsSignInRole:
        return row.needsSignIn;
    case SessionRevokedRole:
        return row.sessionRevoked;
    case LiveStateRole:
        return row.liveState;
    case KeepRunningRole:
        return row.keepRunning;
    case SectionRole:
        return row.running ? QStringLiteral("running") : QStringLiteral("other");
    case LastLiveAtRole:
        return row.lastLiveAt;
    default:
        return {};
    }
}

QHash<int, QByteArray> AccountListModel::roleNames() const
{
    return {
        { UserIdRole, "userId" },
        { DisplayNameRole, "displayName" },
        { AvatarUrlRole, "avatarUrl" },
        { HomeserverRole, "homeserver" },
        { IsActiveRole, "isActive" },
        { NeedsSignInRole, "needsSignIn" },
        { SessionRevokedRole, "sessionRevoked" },
        { LiveStateRole, "liveState" },
        { KeepRunningRole, "keepRunning" },
        { SectionRole, "section" },
        { LastLiveAtRole, "lastLiveAt" },
    };
}

void AccountListModel::setFilter(const QString &filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    refresh();
}

QVector<AccountListModel::Row> AccountListModel::buildRows() const
{
    QVector<Row> rows;
    if (!m_settings)
        return rows;
    const QString active = m_settings->activeAccountUserId();
    const QString needle = m_filter.trimmed();
    const QStringList ids = m_settings->savedAccountUserIds(); // addedAt order
    rows.reserve(ids.size());
    for (const QString &uid : ids) {
        // The settings file only. NEVER AccountManager::accounts(), account()
        // or needsSignIn(): those read the account's access token.
        const QVariantMap record = m_settings->accountRecord(uid);
        Row row;
        row.userId = uid;
        row.displayName = record.value(QStringLiteral("displayName")).toString();
        row.avatarUrl = record.value(QStringLiteral("avatarUrl")).toString();
        row.homeserver = record.value(QStringLiteral("homeserver")).toString();
        row.addedAt = record.value(QStringLiteral("addedAt")).toString();
        if (!needle.isEmpty() && !row.displayName.contains(needle, Qt::CaseInsensitive)
            && !uid.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        row.isActive = !active.isEmpty() && uid == active;
        row.sessionRevoked = m_accounts && m_accounts->sessionRevoked(uid);
        row.liveState = m_activity ? int(m_activity->liveStateOf(uid))
                                   : int(AccountActivityModel::NotLive);
        row.needsSignIn = row.sessionRevoked
            || (m_hasRevocationProof && m_hasRevocationProof(uid))
            || row.liveState == AccountActivityModel::NeedsSignIn
            // The keyring answered (off this thread) and holds no sign-in:
            // what AccountManager::needsSignIn() says today, read here
            // without a GUI-thread keyring call (la1-M L2).
            || m_lostSignIn.value(uid, false);
        row.keepRunning = m_settings->keepRunningFor(uid);
        row.lastLiveAt = m_settings->lastLiveAtFor(uid);
        row.running = row.isActive || row.liveState == AccountActivityModel::Starting
            || row.liveState == AccountActivityModel::Running
            || row.liveState == AccountActivityModel::Stopping;
        rows.append(row);
    }
    // Running first, the active account at its top, then the rest of the
    // running section in saved order (a running host rewrites lastLiveAt every
    // few minutes, and rows must not jump while the menu is open). Then
    // "other", most recently live first. stable_sort keeps saved (addedAt)
    // order for every tie.
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.running != b.running)
            return a.running;
        if (a.running) {
            if (a.isActive != b.isActive)
                return a.isActive;
            return false;
        }
        return a.lastLiveAt > b.lastLiveAt;
    });
    return rows;
}

void AccountListModel::refresh()
{
    QVector<Row> next = buildRows();
    if (next == m_rows) {
        requestProbes();
        return;
    }
    bool sameOrder = next.size() == m_rows.size();
    for (int i = 0; sameOrder && i < next.size(); ++i)
        sameOrder = next.at(i).userId == m_rows.at(i).userId;
    if (sameOrder) {
        for (int i = 0; i < next.size(); ++i) {
            if (next.at(i) == m_rows.at(i))
                continue;
            m_rows[i] = next.at(i);
            const QModelIndex idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx);
        }
    } else {
        // Rows moved, appeared or went: a structural change, which happens on
        // a user action or a lifecycle edge, never on a badge update.
        beginResetModel();
        m_rows = std::move(next);
        endResetModel();
    }
    Q_EMIT rowsChanged();
    requestProbes();
}

void AccountListModel::setSignInProbe(SignInProbe probe)
{
    m_signInProbe = std::move(probe);
    forgetProbes();
    requestProbes();
}

void AccountListModel::forgetProbes()
{
    ++m_probeGeneration;
    m_lostSignIn.clear();
    m_probing.clear();
}

void AccountListModel::requestProbes()
{
    if (!m_signInProbe)
        return;
    // Chosen first: a probe that answered inside the call would refresh the
    // rows being walked.
    QStringList ask;
    for (const Row &row : std::as_const(m_rows)) {
        if (m_probing.size() + ask.size() >= kMaxProbesInFlight)
            break;
        // A running or active account says so itself (its host or the
        // foreground read the keyring); a row already marked needs nothing.
        if (row.isActive || row.running || row.needsSignIn
            || m_lostSignIn.contains(row.userId) || m_probing.contains(row.userId))
            continue;
        ask.append(row.userId);
    }
    for (const QString &uid : std::as_const(ask)) {
        m_probing.insert(uid);
        const quint64 generation = m_probeGeneration;
        QPointer<AccountListModel> self(this);
        m_signInProbe(uid, [self, uid, generation](bool lost) {
            if (!self || generation != self->m_probeGeneration)
                return;
            self->m_probing.remove(uid);
            self->m_lostSignIn.insert(uid, lost);
            // Re-reads the rows (a lost sign-in shows its chip) and asks
            // about the next ones.
            self->refresh();
        });
    }
}

void AccountListModel::onActivityLiveStateChanged(const QString &userId)
{
    Q_UNUSED(userId);
    refresh();
}

QVariantMap AccountListModel::mapOf(const Row &row)
{
    QVariantMap m;
    // Today's AccountManager::accounts() keys, same spelling...
    m.insert(QStringLiteral("userId"), row.userId);
    m.insert(QStringLiteral("homeserver"), row.homeserver);
    m.insert(QStringLiteral("displayName"), row.displayName);
    m.insert(QStringLiteral("avatarUrl"), row.avatarUrl);
    m.insert(QStringLiteral("isActive"), row.isActive);
    m.insert(QStringLiteral("needsSignIn"), row.needsSignIn);
    m.insert(QStringLiteral("sessionRevoked"), row.sessionRevoked);
    // ...plus the Labs roles.
    m.insert(QStringLiteral("liveState"), row.liveState);
    m.insert(QStringLiteral("keepRunning"), row.keepRunning);
    m.insert(QStringLiteral("section"),
             row.running ? QStringLiteral("running") : QStringLiteral("other"));
    m.insert(QStringLiteral("lastLiveAt"), row.lastLiveAt);
    return m;
}

QVariantList AccountListModel::rowsSnapshot() const
{
    QVariantList out;
    out.reserve(m_rows.size());
    for (const Row &row : m_rows)
        out.append(mapOf(row));
    return out;
}
