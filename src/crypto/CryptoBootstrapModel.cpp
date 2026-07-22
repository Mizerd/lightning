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
        return tr("Requesting encryption keys from your verified session. "
                  "Approve the request on your other device if it asks.");
    case SecretsPending:
        // The Rust watchdog's honest intermediate report: the request went
        // out once, the other device has not answered. Distinct copy from
        // WaitingForKeys so users know what is actually happening.
        return tr("Your other device has not sent the encryption keys yet. "
                  "Keep it open and unlocked — or verify again to request "
                  "the keys once more.");
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
        || m_phase == RestoringHistory
        || m_phase == Ready || m_phase == NoBackupAvailable
        || m_phase == ManualRecoveryRequired;
}

bool CryptoBootstrapModel::needsRecoveryKey() const
{
    return m_phase == NoBackupAvailable || m_phase == ManualRecoveryRequired;
}

void CryptoBootstrapModel::onWaitTimeout()
{
    // Only escalate if we are still idly waiting — any real progress moved us
    // on and stopped the timer. SecretsPending is the same wait, just with
    // the watchdog's honest explanation, so it escalates on the same bound.
    if (m_phase != WaitingForKeys && m_phase != SecretsPending)
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
        // v0.7.1: the supervisor's 90 s watchdog — the other device has not
        // answered the fire-once secret request. Only refines the waiting
        // phase; recompute() clears it again on any real progress.
        m_secretsPending = true;
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
        } else {
            // Secret requests are on their way (recovery unknown or
            // incomplete, backup not yet enabled). The Rust watchdog's
            // secrets_pending report refines the same wait honestly.
            next = m_secretsPending ? SecretsPending : WaitingForKeys;
        }
    }
    // Once escalated to manual recovery, a no-progress event must not bounce
    // back to the waiting spinner — only real backup progress promotes it.
    if (m_phase == ManualRecoveryRequired
        && (next == WaitingForKeys || next == SecretsPending))
        next = ManualRecoveryRequired;
    const bool wasWaiting =
        m_phase == WaitingForKeys || m_phase == SecretsPending;
    const bool nextWaiting =
        next == WaitingForKeys || next == SecretsPending;
    // Any non-waiting outcome consumes the watchdog report, so a later
    // re-entry into the waiting phase starts from the plain copy again.
    if (!nextWaiting)
        m_secretsPending = false;
    if (next == m_phase)
        return;
    // Arm the bounded wait when ENTERING the waiting family; keep it running
    // across the WaitingForKeys -> SecretsPending refinement (same wait,
    // same escalation bound); cancel it on any real progress or terminal
    // state so a late timeout can never override them.
    if (nextWaiting && !wasWaiting)
        m_waitTimer.start(m_waitTimeoutMs);
    else if (!nextWaiting)
        m_waitTimer.stop();
    qCInfo(lcCryptoBootstrap)
        << "bootstrap phase" << phaseName(m_phase) << "->" << phaseName(next)
        << "keys_received=" << m_keysReceived;
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
    m_phase = Idle;
    m_keysReceived = 0;
    if (wasInteresting)
        Q_EMIT changed();
}
