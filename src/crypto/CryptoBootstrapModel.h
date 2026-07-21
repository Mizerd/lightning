#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

// v0.7: verified-session crypto-bootstrap status.
//
// After this device is verified by an already trusted session, the Rust
// Matrix SDK automatically requests the missing cross-signing secrets and
// the backup recovery key from that session, enables key backup when the
// secret arrives, downloads every backed-up room key (OneShot), and
// re-decrypts cached history in place. This model only NAMES where that
// SDK-owned process currently is, from the sanitized state events the
// bridge observer forwards (state names and key counts — never key
// material, session ids, or secrets). It cannot mutate crypto state.
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

public:
    enum Phase {
        Idle,                  // no session / state unknown
        Unverified,            // session not yet verified — bootstrap can't run
        WaitingForKeys,        // verified; SDK secret requests are out, waiting
        RestoringHistory,      // backup key arrived; downloading room keys
        Ready,                 // backup enabled; history decryption available
        NoBackupAvailable,     // verified but no recoverable backup exists
        ManualRecoveryRequired // waited past the bound; keys never arrived
    };
    Q_ENUM(Phase)

    explicit CryptoBootstrapModel(QObject *parent = nullptr);

    Phase phase() const { return m_phase; }
    QString statusMessage() const;
    bool active() const;
    bool needsRecoveryKey() const;
    int keysReceived() const { return m_keysReceived; }

    // The automatic request may never be answered (the other device is
    // offline, or served a key that did not match the backup); after this
    // bound the model stops the indefinite "waiting" state and escalates to
    // ManualRecoveryRequired so the UI can offer the recovery key instead of
    // shimmering forever. Test hook keeps it deterministic.
    void setWaitTimeoutMsForTest(int ms) { m_waitTimeoutMs = ms; }

    // One sanitized observer event: kind is verification_state /
    // recovery_state / backup_state / room_keys_received; state is the SDK
    // enum name; count is the imported-key count for room_keys_received.
    void applyEvent(const QString &kind, const QString &state, quint64 count);
    void reset();

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
    Phase m_phase = Idle;
    int m_keysReceived = 0;
    QTimer m_waitTimer;
    int m_waitTimeoutMs = 30000;
};
