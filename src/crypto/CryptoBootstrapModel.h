#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

// Verified-session crypto-bootstrap status.
//
// Once a trusted session verifies this device, the Rust SDK requests the
// missing cross-signing secrets and the backup recovery key, enables backup
// when it arrives, downloads backed-up room keys and re-decrypts history. The
// Rust coordinator re-issues secret requests on a bounded schedule. This model
// only names where that process is, from sanitized events (state names, fixed
// tokens, counts; never key material or session ids). It cannot mutate crypto
// state.
//
// AppController resets it on login, logout and account switch, and the bridge
// emits only for the active session.
class CryptoBootstrapModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CryptoBootstrapModel is exposed via app.cryptoBootstrap")
    Q_PROPERTY(Phase phase READ phase NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    // True while the status deserves a line in the security UI.
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(int keysReceived READ keysReceived NOTIFY changed)
    // The user needs to enter the recovery key/passphrase: gossip did not
    // deliver a usable backup key, or there is no secret storage to gossip
    // from. The UI then shows the recovery field instead of implying another
    // device will answer.
    Q_PROPERTY(bool needsRecoveryKey READ needsRecoveryKey NOTIFY changed)
    // Sanitized recovery diagnostics (fixed tokens and counts only).
    // requestState: "" / requested / already_pending / none_missing /
    //               identity_unverified / no_eligible_devices / unavailable
    Q_PROPERTY(QString requestState READ requestState NOTIFY changed)
    Q_PROPERTY(int requestAttempts READ requestAttempts NOTIFY changed)
    Q_PROPERTY(int eligibleDevices READ eligibleDevices NOTIFY changed)
    // ownIdentity: "" / verified / unverified. Whether this session trusts the
    // account's cross-signing identity, required to accept a gossiped secret.
    Q_PROPERTY(QString ownIdentity READ ownIdentity NOTIFY changed)
    // crossSigningSecrets: "" / complete / incomplete.
    Q_PROPERTY(QString crossSigningSecrets READ crossSigningSecrets NOTIFY changed)
    // "Request keys again" is useful: verified, identity trusted, secrets still
    // missing.
    Q_PROPERTY(bool canRequestKeys READ canRequestKeys NOTIFY changed)

public:
    enum Phase {
        Idle,                  // no session / state unknown
        Unverified,            // session not yet verified — bootstrap can't run
        WaitingForKeys,        // verified; SDK secret requests are out, waiting
        RestoringHistory,      // backup key arrived; downloading room keys
        Ready,                 // backup enabled; history decryption available
        NoBackupAvailable,     // verified but no recoverable backup exists
        ManualRecoveryRequired, // request ladder exhausted; keys never arrived
        // Secret requests are out and unanswered; still waiting. Escalates to
        // ManualRecoveryRequired on the coordinator's exhaustion report or the
        // local backstop. Appended to keep existing values stable.
        SecretsPending,
        // An m.secret.send answer arrived and the SDK is importing it.
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

    // Backstop for requests that are never answered: escalates to
    // ManualRecoveryRequired so the UI offers the recovery key. The
    // coordinator's "exhausted" report is the primary escalation; this bound
    // must exceed its largest gap between attempts.
    void setWaitTimeoutMsForTest(int ms) { m_waitTimeoutMs = ms; }

    // One sanitized coordinator event. kind: verification_state /
    // recovery_state / backup_state / backup_exists / backup_download /
    // secrets_pending / room_keys_received / secret_request / secret_response /
    // own_identity / cross_signing_secrets. state: SDK enum name or fixed
    // token. count: imported keys (room_keys_received) or eligible verified
    // devices (secret_request).
    void applyEvent(const QString &kind, const QString &state, quint64 count,
                    quint64 inconclusive = 0);
    void reset();

    // "Request keys again": leaves the manual state, re-enters waiting while
    // the coordinator runs a new request, and restarts the backstop.
    void rearmAfterManualRequest();

Q_SIGNALS:
    void changed();

private:
    void recompute();
    void onWaitTimeout();

    QString m_verification; // unknown / verified / unverified
    QString m_recovery;     // unknown / enabled / disabled / incomplete
    QString m_backup;       // unknown / creating / enabling / resuming /
                            // enabled / downloading / disabling
    // backupExists: server truth from fetch_exists_on_server (-1 unknown / 0 no
    // / 1 yes), separating "nothing to restore" from "waiting". download: the
    // per-room key-download pass ("", started, ok, failed).
    int m_backupExists = -1;
    QString m_download;
    // Coordinator reported unanswered requests; cleared by progress or reset.
    bool m_secretsPending = false;
    // Coordinator exhausted its requests: the primary escalation.
    bool m_secretsExhausted = false;
    // Coordinator diagnostics.
    QString m_requestState;
    int m_requestAttempts = 0;
    int m_eligibleDevices = 0;
    QString m_ownIdentity;
    QString m_crossSigning;
    bool m_secretReceived = false;
    // A new request round may leave ManualRecoveryRequired once.
    bool m_rearmed = false;
    Phase m_phase = Idle;
    int m_keysReceived = 0;
    QTimer m_waitTimer;
    // Must exceed the coordinator's largest gap between attempts (240 s) plus
    // margin; each "requested" attempt restarts it.
    int m_waitTimeoutMs = 420000;
};
