#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class MatrixClient;

// Account-scoped saved message references. The account-data event contains
// ONLY room/event/root ids and saved time. Resolved bodies are memory-only and
// discarded on account switch, redaction and reload.
class SavedMessagesController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(QVariantList rooms READ rooms NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool supported READ supported NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool undoAvailable READ undoAvailable NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY changed)
    Q_PROPERTY(QString roomFilter READ roomFilter WRITE setRoomFilter NOTIFY changed)
    Q_PROPERTY(QString typeFilter READ typeFilter WRITE setTypeFilter NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    static const QString &eventType();
    explicit SavedMessagesController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    QVariantList entries() const;
    QVariantList rooms() const;
    int count() const { return m_refs.size(); }
    bool loaded() const { return m_loaded; }
    bool supported() const;
    bool busy() const { return m_busy; }
    bool undoAvailable() const { return m_undoValid && !m_busy; }
    QString error() const { return m_error; }
    QString query() const { return m_query; }
    void setQuery(const QString &value);
    QString roomFilter() const { return m_roomFilter; }
    void setRoomFilter(const QString &value);
    QString typeFilter() const { return m_typeFilter; }
    void setTypeFilter(const QString &value);
    int revision() const { return m_revision; }

    Q_INVOKABLE bool isSaved(const QString &roomId, const QString &eventId) const;
    Q_INVOKABLE void save(const QString &roomId, const QString &eventId,
                          const QString &threadRootId);
    Q_INVOKABLE void remove(const QString &roomId, const QString &eventId);
    Q_INVOKABLE void undoRemove();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void resolveVisible();
    Q_INVOKABLE void open(const QString &roomId, const QString &eventId);
    Q_INVOKABLE void openRoot(const QString &roomId, const QString &eventId);
    Q_INVOKABLE void openRoom(const QString &roomId);

Q_SIGNALS:
    void changed();
    void actionFailed(const QString &message);
    void saveSucceeded();
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);

private:
    struct Ref {
        QString roomId;
        QString eventId;
        QString rootId;
        qint64 savedAtMs = 0;
        QString sender;
        QString preview;
        QString kind;
        bool resolved = false;
        bool available = false;
    };
    static bool validRef(const Ref &ref);
    static QString key(const QString &roomId, const QString &eventId);
    int indexOf(const QString &roomId, const QString &eventId) const;
    void reset();
    void emitChanged();
    void write();
    void sendWrite();
    bool parseRefs(const QVariantMap &content, bool present, QVector<Ref> &refs) const;
    void failWrite(const QString &message);
    void pumpResolve();
    void receive(const QVariantMap &content, bool present);
    void receiveSource(const QString &roomId, const QString &eventId,
                       bool ok, const QString &json);

    MatrixClient *m_client = nullptr;
    QString m_userId;
    QVector<Ref> m_refs;
    QVector<Ref> m_beforeWrite;
    Ref m_undo;
    bool m_undoValid = false;
    bool m_loaded = false;
    bool m_busy = false;
    bool m_saving = false;
    bool m_fetchingForWrite = false;
    int m_pendingReads = 0;
    bool m_reloadOwed = false;
    QString m_error;
    QString m_query;
    QString m_roomFilter;
    QString m_typeFilter;
    int m_revision = 0;
    QSet<QString> m_resolving;
    QSet<QString> m_attempted;
    QSet<QString> m_redacted;
    QTimer m_resolveTimeout;
};
