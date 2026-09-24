#include "calls/CallStageState.h"

#include <QtGlobal>

#include "calls/CallShareModel.h"

namespace {
/// One per participant is the natural ceiling; headroom covers sharers
/// restarting within a call (each restart mints a new id).
constexpr int kMaxDismissedShareIds = 128;
} // namespace

CallStageState::CallStageState(QObject *parent) : QObject(parent)
{
    // Read once: the property is CONSTANT. Any non-empty value enables it,
    // like the other opt-in traces.
    m_traceEnabled = !qEnvironmentVariableIsEmpty("LIGHTNING_CALL_TRACE");
}

void CallStageState::setShareModel(CallShareModel *shares)
{
    if (m_shares == shares)
        return;
    if (m_shares)
        disconnect(m_shares, nullptr, this, nullptr);
    m_shares = shares;
    if (m_shares) {
        connect(m_shares, &CallShareModel::shareEnded, this,
                &CallStageState::onShareEnded);
        connect(m_shares, &CallShareModel::shareAppeared, this,
                &CallStageState::onShareAppeared);
        connect(m_shares, &CallShareModel::countChanged, this,
                &CallStageState::onSharesChanged);
    }
    onSharesChanged();
}

QString CallStageState::spotlightShareId() const
{
    if (m_shares.isNull())
        return {};
    // Newest first: rows are in arrival order, so the last row is the most
    // recent share.
    const QStringList ids = m_shares->shareIds();
    for (int i = ids.size() - 1; i >= 0; --i) {
        if (!m_dismissedShareIds.contains(ids.at(i)))
            return ids.at(i);
    }
    return {};
}

bool CallStageState::restorableShareAvailable() const
{
    if (m_shares.isNull())
        return false;
    const QStringList ids = m_shares->shareIds();
    for (const QString &id : ids) {
        if (m_dismissedShareIds.contains(id))
            return true;
    }
    return false;
}

int CallStageState::dismissedShareCount() const
{
    if (m_shares.isNull())
        return 0;
    int n = 0;
    const QStringList ids = m_shares->shareIds();
    for (const QString &id : ids) {
        if (m_dismissedShareIds.contains(id))
            ++n;
    }
    return n;
}

void CallStageState::onSharesChanged()
{
    const QString spotlight = spotlightShareId();
    const bool restorable = restorableShareAvailable();
    // Always run the full-screen guard, even when nothing else changed.
    enforceFullScreenHasSurface();
    if (spotlight == m_lastSpotlightShareId && restorable == m_lastRestorable)
        return;
    m_lastSpotlightShareId = spotlight;
    m_lastRestorable = restorable;
    Q_EMIT spotlightChanged();
}

bool CallStageState::hasFocusedSurface() const
{
    return !spotlightShareId().isEmpty() || !m_pinnedIdentity.isEmpty();
}

void CallStageState::enforceFullScreenHasSurface()
{
    if (!m_fullScreen || hasFocusedSurface())
        return;
    m_fullScreen = false;
    Q_EMIT fullScreenChanged();
}

void CallStageState::setFullScreen(bool fullScreen)
{
    // Refused with nothing focused: a full-screen black window is the worst
    // outcome.
    if (fullScreen && !hasFocusedSurface())
        return;
    if (m_fullScreen == fullScreen)
        return;
    m_fullScreen = fullScreen;
    Q_EMIT fullScreenChanged();
}

void CallStageState::toggleFullScreen()
{
    setFullScreen(!m_fullScreen);
}

void CallStageState::setPictureInPicture(bool pip)
{
    if (m_pictureInPicture == pip)
        return;
    m_pictureInPicture = pip;
    // Entering PiP leaves full screen: both render the focused surface, and
    // the video router gives one sink to whoever attached last, so one would
    // go black. Deliberately one-directional (full screen already needs a
    // focused surface), so the two flags cannot oscillate.
    if (pip && m_fullScreen) {
        m_fullScreen = false;
        Q_EMIT fullScreenChanged();
    }
    Q_EMIT pictureInPictureChanged();
}

void CallStageState::togglePictureInPicture()
{
    setPictureInPicture(!m_pictureInPicture);
}

void CallStageState::onShareEnded(const QString &shareId)
{
    m_dismissedShareIds.remove(shareId);
    onSharesChanged();
}

void CallStageState::onShareAppeared(const QString &shareId)
{
    // A new share is not the one the user dismissed, so re-arm "auto". This
    // cannot resurrect a dismissal: ended shares are pruned and a restart has
    // a new id.
    Q_UNUSED(shareId);
    if (m_layoutPreference != QLatin1String("auto")) {
        m_layoutPreference = QStringLiteral("auto");
        Q_EMIT layoutPreferenceChanged();
    }
    onSharesChanged();
}

void CallStageState::dismissShare(const QString &shareId)
{
    if (shareId.isEmpty() || m_dismissedShareIds.contains(shareId))
        return;
    if (m_dismissedShareIds.size() >= kMaxDismissedShareIds) {
        // Refuse rather than evict: evicting would silently un-dismiss a
        // share.
        return;
    }
    m_dismissedShareIds.insert(shareId);
    onSharesChanged();
}

void CallStageState::restoreShare(const QString &shareId)
{
    if (!m_dismissedShareIds.remove(shareId))
        return;
    // Restoring asks to see the share, so a "grid" preference must not hide
    // it.
    if (m_layoutPreference == QLatin1String("grid")) {
        m_layoutPreference = QStringLiteral("auto");
        Q_EMIT layoutPreferenceChanged();
    }
    onSharesChanged();
}

void CallStageState::restoreAllShares()
{
    if (m_dismissedShareIds.isEmpty())
        return;
    m_dismissedShareIds.clear();
    if (m_layoutPreference == QLatin1String("grid")) {
        m_layoutPreference = QStringLiteral("auto");
        Q_EMIT layoutPreferenceChanged();
    }
    onSharesChanged();
}

bool CallStageState::isShareDismissed(const QString &shareId) const
{
    return m_dismissedShareIds.contains(shareId);
}

void CallStageState::pin(const QString &identity)
{
    if (m_pinnedIdentity == identity)
        return;
    m_pinnedIdentity = identity;
    Q_EMIT pinnedIdentityChanged();
    // Clearing the pin can empty the spotlight while full screen is up.
    enforceFullScreenHasSurface();
}

void CallStageState::clearPin()
{
    pin(QString());
}

void CallStageState::setLayoutPreference(const QString &mode)
{
    if (mode != QLatin1String("auto") && mode != QLatin1String("grid")
        && mode != QLatin1String("spotlight")) {
        return; // unknown modes are refused, never stored
    }
    if (m_layoutPreference == mode)
        return;
    m_layoutPreference = mode;
    Q_EMIT layoutPreferenceChanged();
}

void CallStageState::clear()
{
    const bool hadPin = !m_pinnedIdentity.isEmpty();
    const bool hadMode = m_layoutPreference != QLatin1String("auto");
    const bool hadFullScreen = m_fullScreen;
    const bool hadPip = m_pictureInPicture;
    m_pinnedIdentity.clear();
    m_layoutPreference = QStringLiteral("auto");
    m_dismissedShareIds.clear();
    // The call ended; a full-screen window must not outlive it.
    m_fullScreen = false;
    // Nor a floating call window.
    m_pictureInPicture = false;
    if (hadPip)
        Q_EMIT pictureInPictureChanged();
    if (hadPin)
        Q_EMIT pinnedIdentityChanged();
    if (hadMode)
        Q_EMIT layoutPreferenceChanged();
    if (hadFullScreen)
        Q_EMIT fullScreenChanged();
    onSharesChanged();
}
