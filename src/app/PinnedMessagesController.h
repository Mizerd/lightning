#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class PinnedMessagesController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY stateChanged)
    Q_PROPERTY(bool canPin READ canPin NOTIFY stateChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // Bumped on every state change. Reference it in bindings that call
    // isPinned()/canTogglePin(), which QML cannot track on their own.
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)
    Q_PROPERTY(QStringList ids READ ids NOTIFY stateChanged)

public:
    explicit PinnedMessagesController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setRoomId(const QString &roomId);

    QString roomId() const { return m_roomId; }
    bool supported() const;
    bool loading() const { return m_fetchOp != 0; }
    QVariantList entries() const { return m_entries; }
    int total() const { return m_total; }
    bool truncated() const { return m_truncated; }
    bool canPin() const { return m_canPin; }
    bool pending() const { return m_writeOp != 0; }
    QString error() const { return m_error; }
    int revision() const { return m_revision; }
    QStringList ids() const { return m_ids; }

    Q_INVOKABLE bool isPinned(const QString &eventId) const;
    Q_INVOKABLE bool canTogglePin(const QString &eventId, bool pin) const;
    Q_INVOKABLE void pin(const QString &eventId);
    Q_INVOKABLE void unpin(const QString &eventId);
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void roomIdChanged();
    void supportedChanged();
    void stateChanged();
    void pinActionFinished(const QString &roomId, const QString &eventId,
                           bool pin, bool ok, const QString &message);

private:
    void emitStateChanged();
    void fetch(bool allowRemote);
    void clearSnapshot();
    void setPinned(const QString &eventId, bool pin);

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_fetchOp = 0;
    bool m_refreshOwed = false;
    quint64 m_writeOp = 0;
    QString m_writeRoomId;
    QString m_writeEventId;
    bool m_writePin = false;
    QVariantList m_entries;
    QStringList m_ids;
    QSet<QString> m_idSet;
    int m_total = 0;
    bool m_truncated = false;
    bool m_canPin = false;
    int m_revision = 0;
    QString m_error;
    QSet<QString> m_remoteProbed;
    QTimer m_memberRefetch;
};
