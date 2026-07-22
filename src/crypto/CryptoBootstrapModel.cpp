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
        // The coordinator's honest intermediate report: requests are out,
        // the other device has not answered, and Lightning will re-request
        // on its bounded ladder without the user doing anything.
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
        // Requesting again would be dishonest here: a gossiped answer could
        // not be accepted until this session itself trusts the account
        // identity, which only a completed verification (or the recovery
        // key) establishes.
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
        // Server-truth absence is worded honestly: without a backup on the
        // homeserver, neither gossip nor a recovery key can restore
        // history — only a key file exported from another client can.
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
    // Backstop only: the coordinator's explicit "exhausted" report is the
    // primary escalation. Only escalate if we are still idly waiting — any
    // real progress moved us on and stopped the timer. SecretReceived is
    // part of the timed wait: an answer that never turns into backup
    // progress must not disable the escalation (review finding: a stray
    // unrequested m.secret.send could otherwise wedge this phase forever).
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
                                      const QString &state, quint64 count)
{
    if (kind == QLatin1String("verification_state")) {
        m_verification = state;
    } else if (kind == QLatin1String("recovery_state")) {
        m_recovery = state;
    } else if (kind == QLatin1String("backup_state")) {
        m_backup = state;
    } else if (kind == QLatin1String("backup_exists")) {
        // Homeserver truth from the supervisor's one-shot probe.
        m_backupExists = (state == QLatin1String("true")) ? 1 : 0;
    } else if (kind == QLatin1String("backup_download")) {
        // The supervisor's explicit per-room download pass.
        m_download = state;
    } else if (kind == QLatin1String("secrets_pending")) {
        if (state == QLatin1String("exhausted")) {
            // v0.7.2: the coordinator finished its bounded request ladder
            // with secrets still missing — the explicit, honest escalation.
            m_secretsExhausted = true;
        } else {
            // Requests are out and unanswered; the ladder keeps running.
            m_secretsPending = true;
        }
    } else if (kind == QLatin1String("secret_request")) {
        // v0.7.2: one coordinator request attempt. count = eligible
        // verified sessions (never identifiers).
        m_requestState = state;
        m_eligibleDevices = static_cast<int>(qMin<quint64>(count, 1000));
        // Any attempt that still finds secrets missing invalidates a
        // stale "answer received, processing…" state — the received
        // secret evidently did not complete recovery.
        if (state != QLatin1String("none_missing"))
            m_secretReceived = false;
        if (state == QLatin1String("requested")) {
            m_requestAttempts += 1;
            // A genuinely new request restarts the bounded wait and may
            // leave an earlier escalated state once.
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
        // v0.7.2: an m.secret.send answer arrived (arrival only — the SDK
        // validates and imports it; name and value never cross the FFI).
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
            // Counts refresh the Ready message; phase itself is derived
            // from the state trio below.
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
            // The backup key is usable. The supervisor's explicit download
            // pass tells us whether history restoration actually worked —
            // a failed pass escalates honestly instead of claiming Ready.
            next = m_download == QLatin1String("failed")
                ? ManualRecoveryRequired : Ready;
        } else if (m_backupExists == 0) {
            // Server truth: no key backup exists at all — there is nothing
            // for gossip OR a recovery key to restore history from.
            next = NoBackupAvailable;
        } else if (m_recovery == QLatin1String("disabled")) {
            // Verified, but no secret storage exists to gossip a backup key
            // from — only a manually entered recovery key can help.
            next = NoBackupAvailable;
        } else if (m_ownIdentity == QLatin1String("unverified")) {
            // v0.7.2: gossip answers could not be accepted — re-requesting
            // would mislead; a repeated verification (or the recovery key)
            // is the honest remedy.
            next = IdentityIncomplete;
        } else if (m_secretsExhausted) {
            // Exhaustion outranks a received-but-unhelpful answer: the
            // coordinator finished its ladder with secrets still missing,
            // so manual recovery is the honest state (review finding).
            next = ManualRecoveryRequired;
        } else if (m_secretReceived) {
            // v0.7.2: an answer arrived; the SDK is processing it. Backup
            // enablement (or the next request attempt) moves us on.
            next = SecretReceived;
        } else {
            // Secret requests are on their way (recovery unknown or
            // incomplete, backup not yet enabled). The coordinator's
            // secrets_pending report refines the same wait honestly.
            next = m_secretsPending ? SecretsPending : WaitingForKeys;
        }
    }
    // Once escalated to manual recovery, a no-progress event must not bounce
    // back to the waiting spinner — only real backup progress or a genuinely
    // new request round (m_rearmed) promotes it.
    if (m_phase == ManualRecoveryRequired
        && (next == WaitingForKeys || next == SecretsPending
            || next == SecretReceived)
        && !m_rearmed)
        next = ManualRecoveryRequired;
    m_rearmed = false;
    // SecretReceived stays inside the TIMED waiting family: the backstop
    // must be able to escalate an answer that never turns into backup
    // progress (review finding — a stray m.secret.send could otherwise
    // park the model forever with the timer stopped).
    const bool wasWaiting = m_phase == WaitingForKeys
        || m_phase == SecretsPending || m_phase == SecretReceived;
    const bool nextWaiting = next == WaitingForKeys
        || next == SecretsPending || next == SecretReceived;
    // Any non-waiting outcome consumes the pending report, so a later
    // re-entry into the waiting phase starts from the plain copy again.
    if (!nextWaiting) {
        m_secretsPending = false;
        // Leaving the waiting family also consumes the arrival flag.
        m_secretReceived = false;
    }
    if (next == m_phase)
        return;
    // Arm the bounded wait when ENTERING the waiting family; keep it running
    // across the WaitingForKeys -> SecretsPending -> SecretReceived
    // refinements (same wait, same escalation bound); cancel it on any real
    // progress or terminal state so a late timeout can never override them.
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
    // The user explicitly asked the coordinator for a new request round.
    // Leave the escalated state honestly (the coordinator's events will
    // refine or re-escalate) and restart the backstop bound.
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
