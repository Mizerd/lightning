#include "crypto/CryptoBootstrapModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcCryptoBootstrap, "lightning.crypto.bootstrap")

namespace {
const char *phaseName(CryptoBootstrapModel::Phase phase)
{
    switch (phase) {
    case CryptoBootstrapModel::Idle: return "idle";
    case CryptoBootstrapModel::Unverified: return "unverified";
    case CryptoBootstrapModel::WaitingForKeys: return "waiting_for_keys";
    case CryptoBootstrapModel::RestoringHistory: return "restoring_history";
    case CryptoBootstrapModel::Ready: return "ready";
    case CryptoBootstrapModel::NoBackupAvailable: return "no_backup";
    case CryptoBootstrapModel::ManualRecoveryRequired: return "manual_recovery";
    case CryptoBootstrapModel::SecretsPending: return "secrets_pending";
    case CryptoBootstrapModel::SecretReceived: return "secret_received";
    case CryptoBootstrapModel::IdentityIncomplete: return "identity_incomplete";
    }
    return "idle";
}
} // namespace

CryptoBootstrapModel::CryptoBootstrapModel(QObject *parent)
    : QObject(parent)
{
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout,
            this, &CryptoBootstrapModel::onWaitTimeout);
    reset();
}

QString CryptoBootstrapModel::statusMessage() const
{
    switch (m_phase) {
    case Unverified:
        return tr("Verify this session to unlock encrypted history.");
    case WaitingForKeys:
        if (m_requestState == QLatin1String("requested"))
            return tr("Encryption-key request sent to %n verified "
                      "session(s).", nullptr, m_eligibleDevices);
        return tr("Requesting encryption keys from your verified session. "
                  "Approve the request on your other device if it asks.");
    case SecretsPending:
        // Requests are out and unanswered; Lightning keeps re-requesting.
        if (m_requestAttempts > 0)
            return tr("Your verified session has not responded yet. Keep it "
                      "open and connected — Lightning will request the keys "
                      "again automatically.");
        return tr("Your other device has not sent the encryption keys yet. "
                  "Keep it open and unlocked — or verify again to request "
                  "the keys once more.");
    case SecretReceived:
        return tr("Encryption secret received. Preparing backup "
                  "restoration…");
    case IdentityIncomplete:
        // Requesting again cannot help: gossiped answers are accepted only once
        // this session trusts the account identity.
        return tr("This session is trusted by your other device, but its own "
                  "identity check did not complete. Verify this session "
                  "again — or enter your recovery key.");
    case RestoringHistory:
        return tr("Restoring encrypted history from key backup…");
    case Ready:
        return m_keysReceived > 0
            ? tr("Encrypted history restored (%n key(s) received).", nullptr,
                 m_keysReceived)
            : tr("History decryption is ready.");
    case NoBackupAvailable:
        // Without a server-side backup, neither gossip nor a recovery key can
        // restore history; only an exported key file can.
        if (m_backupExists == 0)
            return tr("This account has no encryption key backup on the "
                      "server. Older encrypted messages can only be "
                      "recovered by importing a key file from another "
                      "client.");
        return tr("No key backup is available from your other session. "
                  "Enter your recovery key to unlock older messages.");
    case ManualRecoveryRequired:
        return tr("Your verified session did not send the encryption keys. "
                  "Enter your recovery key or passphrase to restore encrypted "
                  "history.");
    case Idle:
        break;
    }
    return {};
}

bool CryptoBootstrapModel::active() const
{
    return m_phase == WaitingForKeys || m_phase == SecretsPending
        || m_phase == SecretReceived || m_phase == IdentityIncomplete
        || m_phase == RestoringHistory
        || m_phase == Ready || m_phase == NoBackupAvailable
        || m_phase == ManualRecoveryRequired;
}

bool CryptoBootstrapModel::needsRecoveryKey() const
{
    return m_phase == NoBackupAvailable || m_phase == ManualRecoveryRequired
        || m_phase == IdentityIncomplete;
}

bool CryptoBootstrapModel::canRequestKeys() const
{
    if (m_ownIdentity == QLatin1String("unverified"))
        return false;
    return m_phase == WaitingForKeys || m_phase == SecretsPending
        || m_phase == SecretReceived
        || m_phase == ManualRecoveryRequired;
}

void CryptoBootstrapModel::onWaitTimeout()
{
    // Backstop; the coordinator's "exhausted" report is primary. Escalate only
    // while still waiting. SecretReceived counts as waiting, so an unrequested
    // m.secret.send cannot park this phase forever.
    if (m_phase != WaitingForKeys && m_phase != SecretsPending
        && m_phase != SecretReceived)
        return;
    qCInfo(lcCryptoBootstrap)
        << "bootstrap phase" << phaseName(m_phase) << "-> manual_recovery"
        << "(automatic key request timed out)";
    m_phase = ManualRecoveryRequired;
    Q_EMIT changed();
}

void CryptoBootstrapModel::applyEvent(const QString &kind,
                                      const QString &state, quint64 count,
                                      quint64 inconclusive)
{
    if (kind == QLatin1String("verification_state")) {
        m_verification = state;
    } else if (kind == QLatin1String("recovery_state")) {
        m_recovery = state;
    } else if (kind == QLatin1String("backup_state")) {
        m_backup = state;
    } else if (kind == QLatin1String("backup_exists")) {
        // Server truth from the one-shot probe.
        m_backupExists = (state == QLatin1String("true")) ? 1 : 0;
    } else if (kind == QLatin1String("backup_download")) {
        // The per-room download pass. Skips are refused here too: recompute()
        // reads anything but "failed" as Ready, so a misrouted skip would
        // report Ready over unrestored history.
        if (state.startsWith(QLatin1String("skipped_"))) {
            qCInfo(lcCryptoBootstrap)
                << "backup download skipped reason=" << state;
            return;
        }
        m_download = state;
    } else if (kind == QLatin1String("backup_download_skipped")) {
        // Why a pass did not run: logged, never stored in m_download, or a
        // skipped room could overwrite another room's "failed" and report Ready
        // over unrestored history.
        qCInfo(lcCryptoBootstrap)
            << "backup download skipped reason=" << state;
        return;
    } else if (kind == QLatin1String("auto_key_recovery")) {
        // The automatic pass for undecryptable rows: an observation, not a
        // download outcome. Both counts are logged; "1 of 32" alone cannot tell
        // missing backup entries from a refusing server.
        qCInfo(lcCryptoBootstrap)
            << "auto key recovery" << state << "sessions=" << count
            << "inconclusive=" << inconclusive;
        return;
    } else if (kind == QLatin1String("secrets_pending")) {
        if (state == QLatin1String("exhausted")) {
            // The coordinator finished its requests with secrets still missing.
            m_secretsExhausted = true;
        } else {
            // Requests are out and unanswered; the coordinator keeps going.
            m_secretsPending = true;
        }
    } else if (kind == QLatin1String("secret_request")) {
        // One request attempt; count is eligible verified sessions.
        m_requestState = state;
        m_eligibleDevices = static_cast<int>(qMin<quint64>(count, 1000));
        // An attempt that still finds secrets missing means a received answer
        // did not complete recovery.
        if (state != QLatin1String("none_missing"))
            m_secretReceived = false;
        if (state == QLatin1String("requested")) {
            m_requestAttempts += 1;
            // A new request restarts the wait and may leave an escalated state
            // once.
            m_secretsExhausted = false;
            m_rearmed = true;
            if (m_phase == WaitingForKeys || m_phase == SecretsPending
                || m_phase == SecretReceived)
                m_waitTimer.start(m_waitTimeoutMs);
        } else if (state == QLatin1String("none_missing")) {
            m_secretsPending = false;
            m_secretsExhausted = false;
        }
    } else if (kind == QLatin1String("secret_response")) {
        // Arrival only; the SDK validates and imports it, and nothing crosses
        // the FFI.
        m_secretReceived = true;
        m_secretsPending = false;
    } else if (kind == QLatin1String("own_identity")) {
        m_ownIdentity = state;
    } else if (kind == QLatin1String("cross_signing_secrets")) {
        m_crossSigning = state;
    } else if (kind == QLatin1String("room_keys_received")) {
        if (count > 0) {
            m_keysReceived += static_cast<int>(
                qMin<quint64>(count, 1000000));
            // Counts refresh the Ready message; the phase comes from the states
            // below.
            Q_EMIT changed();
        }
        return;
    } else {
        return;
    }
    recompute();
}

void CryptoBootstrapModel::recompute()
{
    Phase next = Idle;
    if (m_verification == QLatin1String("unverified")) {
        next = Unverified;
    } else if (m_verification == QLatin1String("verified")) {
        if (m_backup == QLatin1String("downloading")
            || m_backup == QLatin1String("enabling")
            || m_backup == QLatin1String("resuming")
            || m_download == QLatin1String("started")) {
            next = RestoringHistory;
        } else if (m_backup == QLatin1String("enabled")) {
            // The backup key is usable; a failed download pass escalates
            // instead of claiming Ready.
            next = m_download == QLatin1String("failed")
                ? ManualRecoveryRequired : Ready;
        } else if (m_backupExists == 0) {
            // No backup exists on the server: nothing to restore from.
            next = NoBackupAvailable;
        } else if (m_recovery == QLatin1String("disabled")) {
            // No secret storage to gossip from; only a recovery key can help.
            next = NoBackupAvailable;
        } else if (m_ownIdentity == QLatin1String("unverified")) {
            // Gossiped answers cannot be accepted; re-verification or the
            // recovery key is the remedy.
            next = IdentityIncomplete;
        } else if (m_secretsExhausted) {
            // Exhaustion outranks a received but unhelpful answer.
            next = ManualRecoveryRequired;
        } else if (m_secretReceived) {
            // An answer arrived; backup enablement or the next attempt moves
            // on.
            next = SecretReceived;
        } else {
            // Secret requests are out; secrets_pending refines the wait.
            next = m_secretsPending ? SecretsPending : WaitingForKeys;
        }
    }
    // Once escalated, only real backup progress or a new request round
    // (m_rearmed) leaves manual recovery.
    if (m_phase == ManualRecoveryRequired
        && (next == WaitingForKeys || next == SecretsPending
            || next == SecretReceived)
        && !m_rearmed)
        next = ManualRecoveryRequired;
    m_rearmed = false;
    // SecretReceived stays inside the timed wait so the backstop can still
    // escalate it.
    const bool wasWaiting = m_phase == WaitingForKeys
        || m_phase == SecretsPending || m_phase == SecretReceived;
    const bool nextWaiting = next == WaitingForKeys
        || next == SecretsPending || next == SecretReceived;
    // Leaving the wait consumes the pending report.
    if (!nextWaiting) {
        m_secretsPending = false;
        // Leaving the waiting family also consumes the arrival flag.
        m_secretReceived = false;
    }
    if (next == m_phase)
        return;
    // Arm the bound on entering the wait, keep it across its refinements, and
    // cancel it on progress or a terminal state.
    if (nextWaiting && !wasWaiting)
        m_waitTimer.start(m_waitTimeoutMs);
    else if (!nextWaiting)
        m_waitTimer.stop();
    qCInfo(lcCryptoBootstrap)
        << "bootstrap phase" << phaseName(m_phase) << "->" << phaseName(next)
        << "keys_received=" << m_keysReceived
        << "request_attempts=" << m_requestAttempts;
    m_phase = next;
    Q_EMIT changed();
}

void CryptoBootstrapModel::reset()
{
    m_waitTimer.stop();
    const bool wasInteresting = m_phase != Idle || m_keysReceived != 0;
    m_verification = QStringLiteral("unknown");
    m_recovery = QStringLiteral("unknown");
    m_backup = QStringLiteral("unknown");
    m_backupExists = -1;
    m_download.clear();
    m_secretsPending = false;
    m_secretsExhausted = false;
    m_requestState.clear();
    m_requestAttempts = 0;
    m_eligibleDevices = 0;
    m_ownIdentity.clear();
    m_crossSigning.clear();
    m_secretReceived = false;
    m_rearmed = false;
    m_phase = Idle;
    m_keysReceived = 0;
    if (wasInteresting)
        Q_EMIT changed();
}

void CryptoBootstrapModel::rearmAfterManualRequest()
{
    // The user asked for a new request round: leave the escalated state and
    // restart the backstop.
    if (m_phase != ManualRecoveryRequired && m_phase != WaitingForKeys
        && m_phase != SecretsPending && m_phase != SecretReceived)
        return;
    m_secretsExhausted = false;
    m_secretsPending = false;
    m_secretReceived = false;
    m_rearmed = true;
    qCInfo(lcCryptoBootstrap)
        << "bootstrap manual re-request from phase" << phaseName(m_phase);
    recompute();
    if (m_phase == WaitingForKeys || m_phase == SecretsPending)
        m_waitTimer.start(m_waitTimeoutMs);
}
