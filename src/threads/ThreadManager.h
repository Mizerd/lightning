#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class ThreadManager : public QObject
{
    Q_OBJECT

public:
    explicit ThreadManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    Q_INVOKABLE QStringList threadRootsInRoom(const QString &roomId) const;

    Q_INVOKABLE int threadReplyCount(const QString &roomId,
                                     const QString &rootEventId) const;

    Q_INVOKABLE QVariantList threadSummaries(const QString &roomId) const;

    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          const QString &rootEventId) const;
    Q_INVOKABLE void requestParticipants(const QString &roomId,
                                         const QString &rootEventId);

    // The active room changed. Queued work for other rooms is discarded;
    // in-flight requests keep running, since their answers are keyed by room.
    // Driven by AppController, not QML.
    void setActiveRoom(const QString &roomId);

    int participantFetchesInFlightForTest() const
    { return static_cast<int>(m_participantsInFlight.size()); }
    int participantFetchesQueuedForTest() const
    { return static_cast<int>(m_participantQueue.size()); }

Q_SIGNALS:
    void participantsChanged(const QString &roomId, const QString &rootEventId);

private:
    static constexpr int kParticipantRequestTimeoutMs = 60000;
    static constexpr int kMaxConcurrentParticipantFetches = 4;
    static constexpr int kMaxQueuedParticipantFetches = 64;

    static QString participantKey(const QString &roomId,
                                  const QString &rootEventId);
    static QString roomOfParticipantKey(const QString &key);
    void clearParticipants();
    void pumpParticipantQueue();
    void dispatchParticipants(const QString &key);
    void releaseParticipantSlot(const QString &key, quint64 generation = 0);

    MatrixClient *m_client = nullptr;
    QHash<QString, QVariantList> m_participants;
    QSet<QString> m_participantsInFlight;
    QHash<QString, quint64> m_participantGeneration;
    quint64 m_nextParticipantGeneration = 1;
    QStringList m_participantQueue;
    QSet<QString> m_participantQueued;
    QString m_activeRoomId;
};
