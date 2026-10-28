#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <functional>

class MatrixClient;
class RoomListModel;
class RoomInfoController;

// Ephemeral foreground-account selection. One request at a time means room-only
// acknowledgements cannot be mistaken for the result of another batch request.
class RoomBatchController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool selecting READ selecting NOTIFY changed)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY changed)
    Q_PROPERTY(int succeeded READ succeeded NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
    Q_PROPERTY(int pending READ pending NOTIFY changed)
    Q_PROPERTY(QVariantList outcomes READ outcomes NOTIFY changed)
    Q_PROPERTY(bool awaitingLeaveConfirmation READ awaitingLeaveConfirmation NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(bool markReadSupported READ markReadSupported CONSTANT)
public:
    explicit RoomBatchController(QObject *parent = nullptr);
    void configure(MatrixClient *client, RoomListModel *rooms,
                   RoomInfoController *info,
                   std::function<void(const QString &, int)> notificationWrite,
                   std::function<int(const QString &)> notificationRead = {});
    void setAccount(const QString &accountId);
    bool selecting() const { return m_selecting; }
    int selectionCount() const { return m_selected.size(); }
    int succeeded() const;
    int failed() const;
    int pending() const;
    bool busy() const { return !m_queue.isEmpty() || !m_active.isEmpty(); }
    bool canRetry() const { return failed() > 0 && m_action != QLatin1String("leave"); }
    bool markReadSupported() const;
    bool awaitingLeaveConfirmation() const { return m_confirmLeave; }
    QVariantList outcomes() const;
    Q_INVOKABLE bool isSelected(const QString &roomId) const { return m_selected.contains(roomId); }
    Q_INVOKABLE void select(const QString &roomId);
    Q_INVOKABLE void toggle(const QString &roomId);
    Q_INVOKABLE void selectRange(const QStringList &roomIds);
    Q_INVOKABLE void exitSelection();
    Q_INVOKABLE void run(const QString &action, int mode = -1);
    Q_INVOKABLE void retryFailed();
    Q_INVOKABLE void requestLeave();
    Q_INVOKABLE void confirmLeave();
    Q_INVOKABLE void cancelLeave();
Q_SIGNALS:
    void changed();
private:
    void clear();
    void advance();
    void finish(const QString &roomId, bool ok);
    bool validRoom(const QString &roomId) const;
    MatrixClient *m_client = nullptr;
    RoomListModel *m_rooms = nullptr;
    RoomInfoController *m_info = nullptr;
    std::function<void(const QString &, int)> m_notificationWrite;
    std::function<int(const QString &)> m_notificationRead;
    QString m_account;
    quint64 m_generation = 0;
    QMetaObject::Connection m_tagFailureConnection;
    QSet<QString> m_selected;
    QStringList m_order;
    QHash<QString, QString> m_results;
    QHash<QString, QString> m_names;
    QStringList m_queue;
    QString m_active;
    QString m_action;
    int m_mode = -1;
    bool m_selecting = false;
    bool m_confirmLeave = false;
};
