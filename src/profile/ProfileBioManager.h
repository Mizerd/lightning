#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

class ProfileBioManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString ownBio READ ownBio NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(int maxLength READ maxLength CONSTANT)

public:
    explicit ProfileBioManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    bool busy() const { return m_pendingWrite != 0; }
    QString lastError() const { return m_lastError; }
    QString ownBio() const;
    // Mirrors MAX_BIO_CHARS in rust/src/bio.rs. MSC4440 sets no limit and lists
    // unbounded bios as a security consideration.
    static constexpr int kMaxLength = 2048;
    int maxLength() const { return kMaxLength; }

    Q_INVOKABLE QString bioFor(const QString &userId) const;
    Q_INVOKABLE void request(const QString &userId);
    Q_INVOKABLE void refresh(const QString &userId);
    Q_INVOKABLE void setOwnBio(const QString &text);
    Q_INVOKABLE void clearOwnBio();

Q_SIGNALS:
    void availableChanged();
    void supportedChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void handleReceived(quint64 opId, const QString &userId, const QString &bio,
                        bool supported);
    void handleSet(quint64 opId, bool ok, const QString &bio,
                   const QString &category);
    void clearSession();
    void setLastError(const QString &error);
    void cache(const QString &userId, const QString &bio);

    static constexpr int kMaxCached = 256;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_cache;
    QSet<QString> m_asked;
    QHash<quint64, QString> m_inFlight;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
