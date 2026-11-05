#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <functional>

class MatrixClient;
class RoomListModel;

class AutoMarkReadController : public QObject
{
    Q_OBJECT

public:
    explicit AutoMarkReadController(QObject *parent = nullptr);

    void setSources(MatrixClient *client, RoomListModel *rooms);
    void setExcludedRooms(std::function<QStringList()> excluded);
    void setIntervalMinutes(int minutes);
    void setAccount(const QString &userId);

    int intervalMs() const { return m_interval.interval(); }
    bool armed() const { return m_interval.isActive(); }
    bool passRunning() const { return !m_active.isEmpty() || !m_queue.isEmpty(); }
    QString account() const { return m_account; }

    void setIntervalMsForTest(int ms) { m_testIntervalMs = ms; rearm(); }
    void setStepTimeoutMsForTest(int ms) { m_stepTimeout.setInterval(ms); }

Q_SIGNALS:
    void passStarted(int rooms);

private:
    void rearm();
    void startPass();
    void next();
    void finishRoom(const QString &roomId);
    void stopPass();

    QPointer<MatrixClient> m_client;
    QPointer<RoomListModel> m_rooms;
    std::function<QStringList()> m_excluded;
    QString m_account;
    int m_minutes = 0;
    int m_testIntervalMs = 0;
    quint64 m_generation = 0;
    QTimer m_interval;
    QTimer m_stepTimeout;
    QStringList m_queue;
    QString m_active;
    QList<QMetaObject::Connection> m_clientConnections;
};
