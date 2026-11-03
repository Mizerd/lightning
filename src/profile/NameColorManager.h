#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

class NameColorManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString ownColor READ ownColor NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit NameColorManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    QString ownColor() const;
    bool busy() const { return m_pendingSet != 0; }
    QString lastError() const { return m_lastError; }

    Q_INVOKABLE QString colorFor(const QString &userId);
    Q_INVOKABLE void setOwnColor(const QString &value);
    void setRefreshIntervalForTest(int ms) { m_refreshMs = ms; }
    void setMinRefetchGapForTest(int ms) { m_minGapMs = ms; }
    void setBackoffBaseForTest(int ms) { m_backoffBaseMs = ms; }
    void sweepForTest() { sweepRecentlyRead(); }
    void setRecentReadWindowForTest(int ms) { m_recentReadMs = ms; }
    void clear();

Q_SIGNALS:
    void availableChanged();
    void supportedChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void setSupported(bool supported);
    void setLastError(const QString &error);
    void dispatchFetch(const QString &userId);
    void sweepRecentlyRead();
    bool isDue(const QString &userId, qint64 now) const;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_colors;
    QSet<QString> m_asked;
    QHash<quint64, QString> m_pending;
    QHash<QString, qint64> m_askedAt;
    QSet<QString> m_inFlight;
    QElapsedTimer m_clock;
    int m_refreshMs = 5 * 60 * 1000;
    int m_minGapMs = 10 * 1000;
    int m_backoffBaseMs = 30 * 1000;
    QSet<QString> m_stale;
    QHash<QString, int> m_failures;
    QHash<QString, qint64> m_lastRead;
    int m_recentReadMs = 5 * 60 * 1000;
    static constexpr int kSweepMs = 60 * 1000;
    static constexpr int kSweepCap = 24;
    QTimer m_sweep;
    quint64 m_nextOp = 1;
    quint64 m_pendingSet = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
