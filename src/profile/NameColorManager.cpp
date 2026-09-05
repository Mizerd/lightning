#include "profile/NameColorManager.h"

#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {
// #rrggbb and nothing else. Rust validates the same shape on the way out of
// the profile field; this is the second gate, on the value that is about to
// reach a QML colour property. Two independent checks on remote text that
// becomes a paint instruction is the right number.
bool isColour(const QString &value)
{
    static const QRegularExpression re(
        QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return re.match(value).hasMatch();
}
} // namespace

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
            return;   // not ours, or an answer for another account's user
        m_inFlight.remove(userId);
        setSupported(supported);
        // "" is a real answer — this user chose no colour — and it is stored
        // as one so the fetch is not repeated on every binding evaluation.
        const QString accepted = isColour(color) ? color.toLower() : QString();
        if (m_colors.value(userId) == accepted && m_colors.contains(userId))
            return;
        m_colors.insert(userId, accepted);
        ++m_revision;
        Q_EMIT revisionChanged();
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
            ++m_revision;
            Q_EMIT revisionChanged();
        }
    });

    // One account's colours must never surface under another's.
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
        // REFRESH, BOUNDED. A colour someone changed while this client ran
        // used to stay cached for the whole session ("others need to restart
        // their client to see the new colour"). Past the interval the next
        // read re-asks — once, never while an ask is in flight — and keeps
        // serving the cached answer; a changed reply bumps the revision.
        m_lastRead.insert(userId, m_clock.elapsed());
        if (m_refreshMs >= 0 && !m_inFlight.contains(userId)
            && m_clock.elapsed() - m_askedAt.value(userId) >= m_refreshMs)
            dispatchFetch(userId);
        return m_colors.value(userId);
    }
    // FIRST CALL DISPATCHES. This is reached from a binding that re-evaluates
    // for every name on screen, so the ask must happen exactly once per user
    // — m_asked is inserted BEFORE the request, not in the reply, or a
    // timeline of thirty messages from one sender dispatches thirty fetches
    // before the first answer lands.
    m_asked.insert(userId);
    m_lastRead.insert(userId, m_clock.elapsed());
    dispatchFetch(userId);
    if (!m_sweep.isActive())
        m_sweep.start();
    return {};
}

void NameColorManager::dispatchFetch(const QString &userId)
{
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
    // Oldest ask first, so a room with more recently read names than the cap
    // rotates through them over successive sweeps rather than starving the
    // same ones every time.
    QList<QPair<qint64, QString>> due;
    for (auto it = m_lastRead.constBegin(); it != m_lastRead.constEnd(); ++it) {
        const QString &user = it.key();
        if (now - it.value() > m_recentReadMs)
            continue;
        if (!m_asked.contains(user) || m_inFlight.contains(user))
            continue;
        if (m_refreshMs < 0 || now - m_askedAt.value(user) < m_refreshMs)
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
    // An empty value is a deliberate CLEAR and is passed through; anything
    // else must be a colour before it is sent.
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
