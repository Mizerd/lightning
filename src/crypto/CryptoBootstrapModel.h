#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

// v0.7: verified-session crypto-bootstrap status.
//
// After this device is verified by an already trusted session, the Rust
// Matrix SDK requests the missing cross-signing secrets and the backup
// recovery key from that session, enables key backup when the secret
// arrives, downloads every backed-up room key (OneShot), and re-decrypts
// cached history in place. v0.7.2: the Rust-side recovery coordinator now
// actively re-issues standards-based secret requests on a bounded ladder
// and reports each attempt here. This model only NAMES where that process
// currently is, from the sanitized state events the bridge coordinator
// forwards (state names, fixed tokens, and counts — never key material,
// session ids, or secrets). It cannot mutate crypto state.
//
// Isolation: AppController resets the model on login, logout, and account
// switches; the bridge only emits events for the active session handle, so
// a stale observer can never describe the wrong account.
class CryptoBootstrapModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CryptoBootstrapModel is exposed via app.cryptoBootstrap")
    Q_PROPERTY(Phase phase READ phase NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    // True while the status is worth a line in the security UI.
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(int keysReceived READ keysReceived NOTIFY changed)
    // True when the honest resolution is for the user to enter their recovery
    // key/passphrase (the automatic gossip did not deliver a usable backup
    // key, or there is no server-side secret storage to gossip from). The
    // security UI uses this to surface the recovery field and stop implying
    // another device will answer.
    Q_PROPERTY(bool needsRecoveryKey READ needsRecoveryKey NOTIFY changed)
    // v0.7.2 sanitized recovery diagnostics (fixed tokens and counts only).
    // requestState: "" / requested / already_pending / none_missing /
    //               identity_unverified / no_eligible_devices / unavailable
    Q_PROPERTY(QString requestState READ requestState NOTIFY changed)
    Q_PROPERTY(int requestAttempts READ requestAttempts NOTIFY changed)
    Q_PROPERTY(int eligibleDevices READ eligibleDevices NOTIFY changed)
    // ownIdentity: "" / verified / unverified — whether THIS session trusts
    // the account's cross-signing identity (required before a gossiped
    // secret answer can be accepted at all).
    Q_PROPERTY(QString ownIdentity READ ownIdentity NOTIFY changed)
    // crossSigningSecrets: "" / complete / incomplete — private
    // cross-signing key availability on this session.
    Q_PROPERTY(QString crossSigningSecrets READ crossSigningSecrets NOTIFY changed)
    // True when "Request keys again" is a genuinely useful action: the
    // session is verified, trusts the account identity, and secrets are
    // still missing (waiting family or escalated manual state).
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
        // v0.7.1: the coordinator reported that the other device has not
        // answered the outstanding secret requests yet. Still a waiting
        // state; the coordinator keeps re-requesting on its bounded ladder
        // and eventually reports exhaustion, which (or the local backstop
        // timer) promotes this to ManualRecoveryRequired. Appended so the
        // existing enum values stay stable.
        SecretsPending,
        // v0.7.2: an m.secret.send answer was received and the SDK is
        // validating/importing it (backup enablement follows via the
        // backup_state stream when the secret was the backup key).
        SecretReceived,
        // v0.7.2: this session is cross-signed (device verified) but does
        // not itself trust the account identity, so a gossiped answer
        // could not be accepted — verification must be repeated; requesting
        // again would be dishonest.
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

    // The automatic requests may never be answered (the other device is
    // offline, or served a key that did not match the backup); after this
    // bound the model stops the indefinite "waiting" state and escalates to
    // ManualRecoveryRequired so the UI can offer the recovery key instead of
    // shimmering forever. Backstop only — the coordinator's explicit
    // "secrets_pending exhausted" report is the primary escalation. The
    // bound must exceed the coordinator's largest inter-attempt gap so the
    // backstop cannot fire between two genuine request attempts. Test hook
    // keeps it deterministic.
    void setWaitTimeoutMsForTest(int ms) { m_waitTimeoutMs = ms; }

    // One sanitized coordinator event: kind is verification_state /
    // recovery_state / backup_state / backup_exists / backup_download /
    // secrets_pending / room_keys_received / secret_request /
    // secret_response / own_identity / cross_signing_secrets; state is the
    // SDK enum name (or the coordinator's fixed token); count is the
    // imported-key count for room_keys_received and the eligible verified
    // device count for secret_request.
    void applyEvent(const QString &kind, const QString &state, quint64 count);
    void reset();

    // v0.7.2: the user pressed "Request keys again". Clears the escalated
    // manual state so the model honestly re-enters the waiting family while
    // the coordinator runs the new standards-based request, and restarts
    // the backstop bound.
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
    // v0.7 supervisor inputs. backupExists is the homeserver truth from the
    // one-shot fetch_exists_on_server probe (-1 unknown / 0 no / 1 yes) —
    // it distinguishes "there is nothing to restore" from "waiting for the
    // other device". download tracks the explicit per-room key-download
    // pass ("", started, ok, failed).
    int m_backupExists = -1;
    QString m_download;
    // v0.7.1: the coordinator reported unanswered secret requests
    // ("secrets_pending waiting"). Distinguishes the SecretsPending waiting
    // state from plain WaitingForKeys; cleared by any real progress (and by
    // reset).
    bool m_secretsPending = false;
    // v0.7.2: the coordinator exhausted its bounded request ladder
    // ("secrets_pending exhausted") — the primary, explicit escalation to
    // ManualRecoveryRequired.
    bool m_secretsExhausted = false;
    // v0.7.2 coordinator diagnostics.
    QString m_requestState;
    int m_requestAttempts = 0;
    int m_eligibleDevices = 0;
    QString m_ownIdentity;
    QString m_crossSigning;
    bool m_secretReceived = false;
    // v0.7.2: a fresh request round (manual or a new ladder) may leave the
    // sticky ManualRecoveryRequired state exactly once.
    bool m_rearmed = false;
    Phase m_phase = Idle;
    int m_keysReceived = 0;
    QTimer m_waitTimer;
    // v0.7.2: must exceed the Rust coordinator's largest inter-attempt gap
    // (240 s) plus margin; each explicit "requested" attempt restarts it.
    int m_waitTimeoutMs = 420000;
};
