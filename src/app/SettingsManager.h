#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <memory>

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

public:
    enum Theme {
        SystemTheme = 0,
        LightTheme = 1,
        DarkTheme = 2,
    };
    Q_ENUM(Theme)

    explicit SettingsManager(QObject *parent = nullptr);

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
    // v0.2 stores these in QSettings in plaintext. This is documented as a
    // known limitation in docs/threat-model.md and surfaced in the Settings
    // screen. TODO(v0.4): move accessToken (and any future key material) into
    // an OS keychain (Secret Service / KWallet on Linux, Keychain on macOS,
    // DPAPI on Windows). SettingsManager should keep only non-secret pointers
    // to those secure stores.
    bool hasSession() const;
    QString accessToken() const;
    QString userId() const;
    QString deviceId() const;
    QString syncToken() const;

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

private:
    std::unique_ptr<QSettings> m_store;
};
