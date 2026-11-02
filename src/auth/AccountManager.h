#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class SettingsManager;

class AccountManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString activeUserId READ activeUserId NOTIFY activeUserIdChanged)
    Q_PROPERTY(QStringList knownUserIds READ knownUserIds NOTIFY accountsChanged)
    Q_PROPERTY(bool hasActiveAccount READ hasActiveAccount NOTIFY activeUserIdChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)

public:
    explicit AccountManager(SettingsManager *settings, QObject *parent = nullptr);

    QString activeUserId() const;
    QStringList knownUserIds() const;
    bool hasActiveAccount() const { return !activeUserId().isEmpty(); }
    QVariantList accounts() const;

    Q_INVOKABLE bool hasAccount(const QString &userId) const;
    Q_INVOKABLE QVariantMap account(const QString &userId) const;
    Q_INVOKABLE bool needsSignIn(const QString &userId) const;

    void setActiveUser(const QString &userId);
    void clearActiveUser();
    // The server revoked this account's session during this run. Held in
    // memory only: the next restore asks the server again. Cleared when the
    // account signs in again or is removed.
    void markSessionRevoked(const QString &userId);
    void clearSessionRevoked(const QString &userId);
    bool sessionRevoked(const QString &userId) const;
    void updateProfile(const QString &userId,
                       const QString &displayName,
                       const QString &avatarUrl);
    bool removeAccount(const QString &userId);

Q_SIGNALS:
    void activeUserIdChanged();
    void accountsChanged();
    void sessionRevokedChanged(const QString &userId);

private:
    bool secretBackendUnavailable() const;

    SettingsManager *m_settings = nullptr;
    QStringList m_revokedSessions;
};
