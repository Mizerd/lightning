#include "profile/ProfileFieldsManager.h"

#include <QLocale>
#include <QSet>
#include <QTimeZone>

namespace {

const QSet<QByteArray> &knownZones()
{
    static const QSet<QByteArray> zones = [] {
        const QList<QByteArray> ids = QTimeZone::availableTimeZoneIds();
        return QSet<QByteArray>(ids.cbegin(), ids.cend());
    }();
    return zones;
}

}

ProfileFieldsManager::ProfileFieldsManager(QObject *parent)
    : QObject(parent)
{
}

void ProfileFieldsManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clearSession();
    if (m_client) {
        connect(m_client, &MatrixClient::profileFieldsReceived, this,
                &ProfileFieldsManager::handleReceived);
        connect(m_client, &MatrixClient::profileFieldSet, this,
                &ProfileFieldsManager::handleSet);
        // Answers belong to the account that fetched them.
        connect(m_client, &MatrixClient::loggedOut, this,
                &ProfileFieldsManager::clearSession);
    }
    Q_EMIT availableChanged();
}

bool ProfileFieldsManager::available() const
{
    return m_client && m_client->supportsProfileFields();
}

// A remote zone name reaches QTimeZone only after it is found in this list:
// on Linux the zone database is a directory and the name is a path into it.
bool ProfileFieldsManager::isKnownTimezone(const QString &zoneId)
{
    return !zoneId.isEmpty() && knownZones().contains(zoneId.toLatin1());
}

QStringList ProfileFieldsManager::timezones() const
{
    QStringList ids;
    ids.reserve(knownZones().size());
    for (const QByteArray &id : knownZones())
        ids.append(QString::fromLatin1(id));
    ids.sort();
    return ids;
}

QString ProfileFieldsManager::systemTimezone() const
{
    const QString id = QString::fromLatin1(QTimeZone::systemTimeZoneId());
    return isKnownTimezone(id) ? id : QString();
}

QString ProfileFieldsManager::offsetText(int offsetSeconds)
{
    if (offsetSeconds == 0)
        return QStringLiteral("UTC");
    const int minutes = qAbs(offsetSeconds) / 60;
    QString text = QStringLiteral("UTC%1%2")
        .arg(offsetSeconds < 0 ? QChar(0x2212) : QLatin1Char('+'))
        .arg(minutes / 60);
    if (minutes % 60 != 0)
        text += QStringLiteral(":%1").arg(minutes % 60, 2, 10, QLatin1Char('0'));
    return text;
}

QString ProfileFieldsManager::localTimeText(const QString &zoneId,
                                            const QDateTime &when,
                                            const QString &timeFormat)
{
    if (!isKnownTimezone(zoneId) || !when.isValid())
        return {};
    const QTimeZone zone(zoneId.toLatin1());
    if (!zone.isValid())
        return {};
    const QDateTime local = when.toTimeZone(zone);
    return QStringLiteral("%1 (%2)")
        .arg(QLocale().toString(local.time(), timeFormat),
             offsetText(local.offsetFromUtc()));
}

QVariantMap ProfileFieldsManager::fieldsFor(const QString &userId) const
{
    return m_cache.value(userId);
}

QString ProfileFieldsManager::localTimeFor(const QString &userId,
                                           const QString &timeFormat) const
{
    return localTimeText(
        m_cache.value(userId).value(QStringLiteral("timezone")).toString(),
        QDateTime::currentDateTimeUtc(), timeFormat);
}

QString ProfileFieldsManager::ownTimezone() const
{
    return m_client ? fieldsFor(m_client->currentUserId())
                          .value(QStringLiteral("timezone")).toString()
                    : QString();
}

QString ProfileFieldsManager::ownPronouns() const
{
    return m_client ? fieldsFor(m_client->currentUserId())
                          .value(QStringLiteral("pronouns_input")).toString()
                    : QString();
}

void ProfileFieldsManager::request(const QString &userId)
{
    if (!available() || userId.isEmpty())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const auto asked = m_askedAt.constFind(userId);
    if (asked != m_askedAt.constEnd() && now - *asked < kFreshSeconds)
        return;
    if (m_cache.size() >= kMaxCached && !m_cache.contains(userId))
        return;
    m_askedAt.insert(userId, now);
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, userId);
    m_client->fetchProfileFields(userId, opId);
}

void ProfileFieldsManager::requestOwn()
{
    if (m_client)
        request(m_client->currentUserId());
}

void ProfileFieldsManager::setOwnTimezone(const QString &zoneId)
{
    if (!zoneId.isEmpty() && !isKnownTimezone(zoneId)) {
        setLastError(QStringLiteral("invalid_value"));
        return;
    }
    write(QStringLiteral("timezone"), zoneId);
}

void ProfileFieldsManager::setOwnPronouns(const QString &text)
{
    write(QStringLiteral("pronouns"), text.trimmed());
}

void ProfileFieldsManager::write(const QString &name, const QString &value)
{
    if (!available() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setProfileField(name, value, m_pendingWrite);
}

void ProfileFieldsManager::handleReceived(quint64 opId, const QString &userId,
                                          bool ok, const QVariantMap &fields)
{
    const auto it = m_inFlight.constFind(opId);
    if (it == m_inFlight.constEnd() || *it != userId)
        return;
    m_inFlight.erase(it);
    if (!ok) {
        m_askedAt.remove(userId);
        return;
    }

    QVariantMap cleaned = fields;
    const QString zoneKey = QStringLiteral("timezone");
    if (!isKnownTimezone(cleaned.value(zoneKey).toString()))
        cleaned.insert(zoneKey, QString());

    bool changed = !m_cache.contains(userId) || m_cache.value(userId) != cleaned;
    if (m_client && userId == m_client->currentUserId()) {
        const bool tz =
            cleaned.value(QStringLiteral("can_edit_timezone"), true).toBool();
        const bool pronouns =
            cleaned.value(QStringLiteral("can_edit_pronouns"), true).toBool();
        changed = changed || tz != m_canEditTimezone
                  || pronouns != m_canEditPronouns;
        m_canEditTimezone = tz;
        m_canEditPronouns = pronouns;
    }
    m_cache.insert(userId, cleaned);
    if (!changed)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileFieldsManager::handleSet(quint64 opId, bool ok, const QString &name,
                                     const QString &category)
{
    Q_UNUSED(name);
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        if (category == QLatin1String("unsupported")) {
            m_canEditTimezone = false;
            m_canEditPronouns = false;
            ++m_revision;
            Q_EMIT revisionChanged();
        }
        return;
    }
    setLastError({});
    if (m_client) {
        m_askedAt.remove(m_client->currentUserId());
        requestOwn();
    }
}

void ProfileFieldsManager::clearSession()
{
    m_cache.clear();
    m_askedAt.clear();
    m_inFlight.clear();
    m_pendingWrite = 0;
    m_canEditTimezone = true;
    m_canEditPronouns = true;
    m_lastError.clear();
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT busyChanged();
    Q_EMIT lastErrorChanged();
}

void ProfileFieldsManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
