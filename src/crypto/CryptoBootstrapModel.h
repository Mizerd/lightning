#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

class CryptoBootstrapModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CryptoBootstrapModel is exposed via app.cryptoBootstrap")
    Q_PROPERTY(Phase phase READ phase NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(int keysReceived READ keysReceived NOTIFY changed)
    Q_PROPERTY(bool needsRecoveryKey READ needsRecoveryKey NOTIFY changed)
    Q_PROPERTY(QString requestState READ requestState NOTIFY changed)
    Q_PROPERTY(int requestAttempts READ requestAttempts NOTIFY changed)
    Q_PROPERTY(int eligibleDevices READ eligibleDevices NOTIFY changed)
    Q_PROPERTY(QString ownIdentity READ ownIdentity NOTIFY changed)
    Q_PROPERTY(QString crossSigningSecrets READ crossSigningSecrets NOTIFY changed)
    Q_PROPERTY(bool canRequestKeys READ canRequestKeys NOTIFY changed)

public:
    enum Phase {
        Idle,
        Unverified,
        WaitingForKeys,
        RestoringHistory,
        Ready,
        NoBackupAvailable,
        ManualRecoveryRequired,
        SecretsPending,
        SecretReceived,
        // Device cross-signed, but this session does not trust the account
        // identity, so gossiped answers cannot be accepted. Verification must
        // be repeated; requesting again would not help.
        IdentityIncomplete
    };
    Q_ENUM(Phase)

    explicit CryptoBootstrapModel(QObject *parent = nullptr);

    Phase phase() const { return m_phase; }
    QString statusMessage() const;
    bool active() const;
    bool needsRecoveryKey() const;
    int keysReceived() const { return m_keysReceived; }
    QString requestState() const { return m_requestState; }
    int requestAttempts() const { return m_requestAttempts; }
    int eligibleDevices() const { return m_eligibleDevices; }
    QString ownIdentity() const { return m_ownIdentity; }
    QString crossSigningSecrets() const { return m_crossSigning; }
    bool canRequestKeys() const;

    void setWaitTimeoutMsForTest(int ms) { m_waitTimeoutMs = ms; }

    void applyEvent(const QString &kind, const QString &state, quint64 count,
                    quint64 inconclusive = 0);
    void reset();

    void rearmAfterManualRequest();

Q_SIGNALS:
    void changed();

private:
    void recompute();
    void onWaitTimeout();

    QString m_verification;
    QString m_recovery;
    QString m_backup;
    int m_backupExists = -1;
    QString m_download;
    bool m_secretsPending = false;
    bool m_secretsExhausted = false;
    QString m_requestState;
    int m_requestAttempts = 0;
    int m_eligibleDevices = 0;
    QString m_ownIdentity;
    QString m_crossSigning;
    bool m_secretReceived = false;
    bool m_rearmed = false;
    Phase m_phase = Idle;
    int m_keysReceived = 0;
    QTimer m_waitTimer;
    // Must exceed the coordinator's largest gap between attempts (240 s) plus
    // margin; each "requested" attempt restarts it.
    int m_waitTimeoutMs = 420000;
};
