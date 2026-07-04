#pragma once

#include <QObject>
#include <QString>

class MatrixClient;

class AuthManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isLoggingIn READ isLoggingIn NOTIFY isLoggingInChanged)
    Q_PROPERTY(bool isLoggedIn READ isLoggedIn NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit AuthManager(MatrixClient *client, QObject *parent = nullptr);

    bool isLoggingIn() const { return m_loggingIn; }
    bool isLoggedIn() const;
    QString currentUserId() const;
    QString lastError() const { return m_lastError; }

    Q_INVOKABLE void login(const QString &homeserver,
                           const QString &user,
                           const QString &password);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void restoreSession();

Q_SIGNALS:
    void isLoggingInChanged();
    void isLoggedInChanged();
    void lastErrorChanged();
    void loginSucceeded();
    void loginFailed(const QString &reason);
    void loggedOut();

private:
    void setLoggingIn(bool v);
    void setLastError(const QString &err);

    MatrixClient *m_client = nullptr;
    bool m_loggingIn = false;
    QString m_lastError;
};
