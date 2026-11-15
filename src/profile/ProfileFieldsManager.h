#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include "matrix/MatrixClient.h"

// Time zone (MSC4175), pronouns (MSC4247) and the display-only Sable fields;
// see rust/src/profilefields.rs for the keys.
class ProfileFieldsManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString ownTimezone READ ownTimezone NOTIFY revisionChanged)
    Q_PROPERTY(QString ownPronouns READ ownPronouns NOTIFY revisionChanged)
    Q_PROPERTY(bool canEditTimezone READ canEditTimezone NOTIFY revisionChanged)
    Q_PROPERTY(bool canEditPronouns READ canEditPronouns NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QStringList timezones READ timezones CONSTANT)
    Q_PROPERTY(bool timezonesAvailable READ timezonesAvailable CONSTANT)
    Q_PROPERTY(QString systemTimezone READ systemTimezone CONSTANT)

public:
    explicit ProfileFieldsManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    int revision() const { return m_revision; }
    bool busy() const { return m_pendingWrite != 0; }
    QString lastError() const { return m_lastError; }
    QString ownTimezone() const;
    QString ownPronouns() const;
    bool canEditTimezone() const { return m_canEditTimezone; }
    bool canEditPronouns() const { return m_canEditPronouns; }
    QStringList timezones() const;
    bool timezonesAvailable() const { return ianaZoneCount() > 0; }
    QString systemTimezone() const;

    Q_INVOKABLE QVariantMap fieldsFor(const QString &userId) const;
    // "14:32 (UTC+2)" for the user's zone now, empty when they published none.
    Q_INVOKABLE QString localTimeFor(const QString &userId,
                                     const QString &timeFormat) const;
    Q_INVOKABLE void request(const QString &userId);
    Q_INVOKABLE void requestOwn();
    Q_INVOKABLE void setOwnTimezone(const QString &zoneId);
    Q_INVOKABLE void setOwnPronouns(const QString &text);

    // The ids that can be published: Qt also lists fixed offsets such as
    // "UTC+05:30", which are not IANA names and which the bridge refuses.
    static QStringList writableZones(const QList<QByteArray> &ids);
    // Region/City names. Without a zone database Qt still lists UTC.
    static int ianaZoneCount(const QStringList &zones);
    static int ianaZoneCount();
    static bool isKnownTimezone(const QString &zoneId);
    static QString offsetText(int offsetSeconds);
    static QString localTimeText(const QString &zoneId, const QDateTime &when,
                                 const QString &timeFormat);

Q_SIGNALS:
    void availableChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void handleReceived(quint64 opId, const QString &userId, bool ok,
                        const QVariantMap &fields);
    void handleSet(quint64 opId, bool ok, const QString &name,
                   const QString &category);
    void write(const QString &name, const QString &value);
    void clearSession();
    void setLastError(const QString &error);
    void makeRoomFor(const QString &userId);

    static constexpr int kMaxCached = 256;
    // MSC4175 recommends caching a zone for 12 to 24 hours.
    static constexpr qint64 kFreshSeconds = 12 * 60 * 60;

    MatrixClient *m_client = nullptr;
    QHash<QString, QVariantMap> m_cache;
    QHash<QString, qint64> m_askedAt;
    QStringList m_order;
    QHash<quint64, QString> m_inFlight;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    int m_revision = 0;
    bool m_canEditTimezone = true;
    bool m_canEditPronouns = true;
    QString m_lastError;
};
