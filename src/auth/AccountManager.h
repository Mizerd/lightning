#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class SettingsManager;

// v0.7: the QML-facing view of the persistent multi-account registry.
// Account records (user id, homeserver, device id, cached display name and
// avatar) live in SettingsManager under accounts/<slug>/; access tokens stay
// in the SecretStore keyed by the full Matrix user id. This class only
// reads/writes metadata — the account-switch *lifecycle* (client handle,
// sync, models) is orchestrated by AppController.
class AccountManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString activeUserId READ activeUserId NOTIFY activeUserIdChanged)
    Q_PROPERTY(QStringList knownUserIds READ knownUserIds NOTIFY accountsChanged)
    Q_PROPERTY(bool hasActiveAccount READ hasActiveAccount NOTIFY activeUserIdChanged)
    // List of maps: {userId, homeserver, displayName, avatarUrl, isActive,
    // needsSignIn}.
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)

public:
    explicit AccountManager(SettingsManager *settings, QObject *parent = nullptr);

    QString activeUserId() const;
    QStringList knownUserIds() const;
    bool hasActiveAccount() const { return !activeUserId().isEmpty(); }
    QVariantList accounts() const;

    Q_INVOKABLE bool hasAccount(const QString &userId) const;
    Q_INVOKABLE QVariantMap account(const QString &userId) const;
    // True when nothing survives that could restore this account locally —
    // no saved record, or no access token. Derived live; never persisted.
    // Returns false when the secret backend cannot be read at all, because
    // "cannot answer" is not "the account is broken".
    Q_INVOKABLE bool needsSignIn(const QString &userId) const;

    void setActiveUser(const QString &userId);
    void clearActiveUser();
    // Cache the account's own profile for the switcher UI.
    void updateProfile(const QString &userId,
                       const QString &displayName,
                       const QString &avatarUrl);
    // Metadata + secrets removal only; AppController wraps this with the
    // client/store lifecycle for a full account removal.
    bool removeAccount(const QString &userId);

Q_SIGNALS:
    void activeUserIdChanged();
    void accountsChanged();

private:
    // True when the SecretStore backend itself cannot answer (no store
    // injected, keyring locked, session bus unavailable).
    bool secretBackendUnavailable() const;

    SettingsManager *m_settings = nullptr; // not owned; outlives this object
};
