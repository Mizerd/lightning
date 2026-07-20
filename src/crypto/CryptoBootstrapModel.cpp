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
    }
    return "idle";
}
} // namespace

CryptoBootstrapModel::CryptoBootstrapModel(QObject *parent)
    : QObject(parent)
{
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
    case RestoringHistory:
        return tr("Restoring encrypted history from key backup…");
    case Ready:
        return m_keysReceived > 0
            ? tr("Encrypted history restored (%n key(s) received).", nullptr,
                 m_keysReceived)
            : tr("History decryption is ready.");
    case NoBackupAvailable:
        return tr("No key backup is available from your other session. "
                  "Enter your recovery key to unlock older messages.");
    case Idle:
        break;
    }
    return {};
}

bool CryptoBootstrapModel::active() const
{
    return m_phase == WaitingForKeys || m_phase == RestoringHistory
        || m_phase == Ready || m_phase == NoBackupAvailable;
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
            || m_backup == QLatin1String("resuming")) {
            next = RestoringHistory;
        } else if (m_backup == QLatin1String("enabled")) {
            next = Ready;
        } else if (m_recovery == QLatin1String("disabled")) {
            // Verified, but no secret storage exists to gossip a backup key
            // from — only a manually entered recovery key can help.
            next = NoBackupAvailable;
        } else {
            // Secret requests are on their way (recovery unknown or
            // incomplete, backup not yet enabled).
            next = WaitingForKeys;
        }
    }
    if (next == m_phase)
        return;
    qCInfo(lcCryptoBootstrap)
        << "bootstrap phase" << phaseName(m_phase) << "->" << phaseName(next)
        << "keys_received=" << m_keysReceived;
    m_phase = next;
    Q_EMIT changed();
}

void CryptoBootstrapModel::reset()
{
    const bool wasInteresting = m_phase != Idle || m_keysReceived != 0;
    m_verification = QStringLiteral("unknown");
    m_recovery = QStringLiteral("unknown");
    m_backup = QStringLiteral("unknown");
    m_phase = Idle;
    m_keysReceived = 0;
    if (wasInteresting)
        Q_EMIT changed();
}
