#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class SettingsManager;

// QML-facing view of the multi-account registry. Records live in
// SettingsManager under accounts/<slug>/, tokens in the SecretStore. This class
// handles metadata only; AppController owns the account-switch lifecycle.
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
    // True when nothing could restore this account locally (no record or no
    // token). False when the secret backend cannot be read at all.
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
