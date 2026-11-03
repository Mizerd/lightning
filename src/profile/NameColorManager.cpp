#include "profile/NameColorManager.h"

#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {
// #rrggbb only. Rust validates the same shape; this is the second check before
// remote text becomes a QML colour.
bool isColour(const QString &value)
{
    static const QRegularExpression re(
        QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return re.match(value).hasMatch();
}
}

NameColorManager::NameColorManager(QObject *parent) : QObject(parent) {
    m_clock.start();
    m_sweep.setInterval(kSweepMs);
    m_sweep.setSingleShot(false);
    connect(&m_sweep, &QTimer::timeout, this, &NameColorManager::sweepRecentlyRead);
}

void NameColorManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clear();
    if (!m_client) {
        Q_EMIT availableChanged();
        return;
    }

    connect(m_client, &MatrixClient::nameColorReceived, this,
            [this](quint64 opId, const QString &userId, const QString &color,
                   bool supported) {
        const QString expected = m_pending.take(opId);
        if (expected.isEmpty() || expected != userId)
            return;
        m_inFlight.remove(userId);
        m_failures.remove(userId);
        setSupported(supported);
        const QString accepted = isColour(color) ? color.toLower() : QString();
        if (m_colors.value(userId) == accepted && m_colors.contains(userId))
            return;
        m_colors.insert(userId, accepted);
        ++m_revision;
        Q_EMIT revisionChanged();
    });

    connect(m_client, &MatrixClient::nameColorFailed, this,
            [this](quint64 opId, const QString &userId) {
        const QString expected = m_pending.take(opId);
        if (expected.isEmpty() || expected != userId)
            return;
        m_inFlight.remove(userId);
        ++m_failures[userId];
    });

    connect(m_client, &MatrixClient::userProfileChanged, this,
            [this](const QString &userId) {
        if (!m_asked.contains(userId))
            return;
        m_stale.insert(userId);
        const qint64 now = m_clock.elapsed();
        const auto read = m_lastRead.constFind(userId);
        if (read != m_lastRead.constEnd() && now - read.value() <= m_recentReadMs
            && isDue(userId, now))
            dispatchFetch(userId);
    });

    connect(m_client, &MatrixClient::nameColorSet, this,
            [this](quint64 opId, bool ok, const QString &color,
                   const QString &category) {
        if (opId != m_pendingSet)
            return;
        m_pendingSet = 0;
        Q_EMIT busyChanged();
        if (!ok) {
            if (category == QLatin1String("unsupported"))
                setSupported(false);
            setLastError(category.isEmpty() ? QStringLiteral("failed")
                                            : category);
            return;
        }
        setLastError(QString());
        const QString self = m_client ? m_client->currentUserId() : QString();
        if (!self.isEmpty()) {
            m_colors.insert(self, isColour(color) ? color.toLower() : QString());
            m_asked.insert(self);
            m_askedAt.insert(self, m_clock.elapsed());
            ++m_revision;
            Q_EMIT revisionChanged();
        }
    });

    connect(m_client, &MatrixClient::loggedOut, this, [this] { clear(); });
    Q_EMIT availableChanged();
}

bool NameColorManager::available() const
{
    return m_client && m_client->supportsNameColors();
}

QString NameColorManager::ownColor() const
{
    if (!m_client)
        return {};
    return m_colors.value(m_client->currentUserId());
}

QString NameColorManager::colorFor(const QString &userId)
{
    if (userId.isEmpty() || !available() || !m_supported)
        return {};
    if (m_asked.contains(userId)) {
        m_lastRead.insert(userId, m_clock.elapsed());
        if (isDue(userId, m_clock.elapsed()))
            dispatchFetch(userId);
        return m_colors.value(userId);
    }
    m_asked.insert(userId);
    m_lastRead.insert(userId, m_clock.elapsed());
    dispatchFetch(userId);
    if (!m_sweep.isActive())
        m_sweep.start();
    return {};
}

bool NameColorManager::isDue(const QString &userId, qint64 now) const
{
    if (m_inFlight.contains(userId))
        return false;
    const qint64 age = now - m_askedAt.value(userId);
    const int failures = m_failures.value(userId);
    if (failures > 0) {
        const qint64 wait = qMin<qint64>(
            static_cast<qint64>(m_backoffBaseMs) << qMin(failures - 1, 10),
            m_refreshMs >= 0 ? m_refreshMs : Q_INT64_C(1) << 40);
        return age >= wait;
    }
    if (m_stale.contains(userId))
        return age >= m_minGapMs;
    return m_refreshMs >= 0 && age >= m_refreshMs;
}

void NameColorManager::dispatchFetch(const QString &userId)
{
    m_stale.remove(userId);
    m_askedAt.insert(userId, m_clock.elapsed());
    m_inFlight.insert(userId);
    const quint64 op = m_nextOp++;
    m_pending.insert(op, userId);
    m_client->fetchNameColor(userId, op);
}

void NameColorManager::sweepRecentlyRead()
{
    if (!available() || !m_supported)
        return;
    const qint64 now = m_clock.elapsed();
    QList<QPair<qint64, QString>> due;
    for (auto it = m_lastRead.constBegin(); it != m_lastRead.constEnd(); ++it) {
        const QString &user = it.key();
        if (now - it.value() > m_recentReadMs)
            continue;
        if (!m_asked.contains(user) || !isDue(user, now))
            continue;
        due.append(qMakePair(m_askedAt.value(user), user));
    }
    std::sort(due.begin(), due.end());
    int dispatched = 0;
    for (const auto &entry : std::as_const(due)) {
        if (dispatched++ >= kSweepCap)
            break;
        dispatchFetch(entry.second);
    }
}

void NameColorManager::setOwnColor(const QString &value)
{
    if (!available() || m_pendingSet != 0)
        return;
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty() && !isColour(trimmed)) {
        setLastError(QStringLiteral("invalid_colour"));
        return;
    }
    setLastError(QString());
    m_pendingSet = m_nextOp++;
    Q_EMIT busyChanged();
    m_client->setNameColor(trimmed.toLower(), m_pendingSet);
}

void NameColorManager::clear()
{
    m_colors.clear();
    m_asked.clear();
    m_askedAt.clear();
    m_inFlight.clear();
    m_stale.clear();
    m_failures.clear();
    m_pending.clear();
    m_pendingSet = 0;
    m_lastError.clear();
    m_supported = true;
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT busyChanged();
    Q_EMIT lastErrorChanged();
    Q_EMIT supportedChanged();
    m_lastRead.clear();
    m_sweep.stop();
}

void NameColorManager::setSupported(bool supported)
{
    if (m_supported == supported)
        return;
    m_supported = supported;
    Q_EMIT supportedChanged();
}

void NameColorManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
