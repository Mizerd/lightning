#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// v0.1: tracks the single active account identity. Structure is ready for
// multi-account (v0.5) — each account eventually gets its own MatrixClient
// instance and its own SQLite/session bundle.
class AccountManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString activeUserId READ activeUserId NOTIFY activeUserIdChanged)
    Q_PROPERTY(QStringList knownUserIds READ knownUserIds NOTIFY knownUserIdsChanged)
    Q_PROPERTY(bool hasActiveAccount READ hasActiveAccount NOTIFY activeUserIdChanged)

public:
    explicit AccountManager(QObject *parent = nullptr);

    QString activeUserId() const { return m_activeUserId; }
    QStringList knownUserIds() const { return m_knownUserIds; }
    bool hasActiveAccount() const { return !m_activeUserId.isEmpty(); }

    void setActiveUser(const QString &userId);
    void clearActiveUser();

    Q_INVOKABLE void addAccount(const QString &userId);
    Q_INVOKABLE void removeAccount(const QString &userId);
    Q_INVOKABLE void switchTo(const QString &userId);

Q_SIGNALS:
    void activeUserIdChanged();
    void knownUserIdsChanged();

private:
    QString m_activeUserId;
    QStringList m_knownUserIds;
};
