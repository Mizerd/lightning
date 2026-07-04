#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <memory>

class SecretStore;

class SettingsManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString homeserverUrl READ homeserverUrl WRITE setHomeserverUrl NOTIFY homeserverUrlChanged)
    Q_PROPERTY(Theme theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized NOTIFY startMinimizedChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY sessionChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString secretBackendName READ secretBackendName NOTIFY secretBackendChanged)
    Q_PROPERTY(bool secretsAreSecure READ secretsAreSecure NOTIFY secretBackendChanged)

public:
    enum Theme {
        SystemTheme = 0,
        LightTheme = 1,
        DarkTheme = 2,
    };
    Q_ENUM(Theme)

    explicit SettingsManager(QObject *parent = nullptr);

    // Inject the process-wide SecretStore. Must be called once, immediately
    // after construction, before any accessToken read/save. When set, any
    // pre-existing plaintext access token in QSettings is migrated into the
    // store and the plaintext key is deleted.
    void setSecretStore(SecretStore *store);
    SecretStore *secretStore() const { return m_secretStore; }

    QString homeserverUrl() const;
    void setHomeserverUrl(const QString &url);

    Theme theme() const;
    void setTheme(Theme t);

    QString language() const;
    void setLanguage(const QString &lang);

    bool startMinimized() const;
    void setStartMinimized(bool v);

    bool notificationsEnabled() const;
    void setNotificationsEnabled(bool v);

    // Session storage.
    //
    // v0.4: accessToken lives in the SecretStore (libsecret when available,
    // insecure QSettings fallback otherwise). Non-secret session metadata
    // (userId, deviceId, homeserverUrl, syncToken) stays in QSettings —
    // syncToken is not a credential but restart-recoverable state.
    bool hasSession() const;
    QString accessToken() const;
    QString userId() const;
    QString deviceId() const;
    QString syncToken() const;

    // True iff the process is using a native, secure secret backend.
    bool secretsAreSecure() const;
    QString secretBackendName() const;

    void saveSession(const QString &homeserverUrl,
                     const QString &userId,
                     const QString &deviceId,
                     const QString &accessToken);
    void setSyncToken(const QString &token);
    void clearSession();

Q_SIGNALS:
    void homeserverUrlChanged();
    void themeChanged();
    void languageChanged();
    void startMinimizedChanged();
    void notificationsEnabledChanged();
    void sessionChanged();
    void secretBackendChanged();

private:
    void migratePlaintextTokenIfPresent();

    std::unique_ptr<QSettings> m_store;
    SecretStore *m_secretStore = nullptr; // not owned; lifetime = process
};
