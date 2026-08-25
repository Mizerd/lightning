#include "calls/CallStageState.h"

#include "calls/CallShareModel.h"

namespace {
/// One per participant is the ceiling a call can produce; the extra headroom
/// covers a sharer stopping and restarting inside one call, which mints a new
/// id each time.
constexpr int kMaxDismissedShareIds = 128;
} // namespace

CallStageState::CallStageState(QObject *parent) : QObject(parent) {}

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
    // NEWEST first. CallShareModel keeps arrival order, so the last row is
    // the most recent share — the one a person who just started sharing
    // expects everyone to be looking at.
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
    // The full-screen guard runs whether or not anything else changed: it
    // reads the same two facts, and an early return that skipped it would
    // leave a full-screen window showing a share that had ended.
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
    // Nothing focused, nothing to fill a screen with. Refused rather than
    // stored: the alternative is a black monitor with a control bar on it and
    // no obvious way back.
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

void CallStageState::onShareEnded(const QString &shareId)
{
    m_dismissedShareIds.remove(shareId);
    onSharesChanged();
}

void CallStageState::onShareAppeared(const QString &shareId)
{
    // A NEW share is not the one the user dismissed. Re-arming "auto" here is
    // the deliberate opposite of the old behaviour, where "back to grid"
    // wrote a mode nothing ever wrote back and every later share was
    // therefore silently suppressed for the rest of the call.
    //
    // Note this cannot resurrect a dismissal: dismissed ids are pruned when
    // their share ends, and a restarted share carries a NEW id, so it arrives
    // undismissed by construction.
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
        // Refuse rather than evict. Evicting an arbitrary id would silently
        // un-dismiss a share the user waved away; refusing leaves the newest
        // one on the spotlight, which is visible and recoverable.
        return;
    }
    m_dismissedShareIds.insert(shareId);
    onSharesChanged();
}

void CallStageState::restoreShare(const QString &shareId)
{
    if (!m_dismissedShareIds.remove(shareId))
        return;
    // Restoring is an explicit request to look at it, so the preference must
    // not keep the grid pinned over the top of it.
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
    // Clearing the pin can be the thing that empties the spotlight, and
    // "Back to grid" does exactly that while full screen may be up.
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
        return; // refused, not stored — an unknown mode is how the latch bit
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
    m_pinnedIdentity.clear();
    m_layoutPreference = QStringLiteral("auto");
    m_dismissedShareIds.clear();
    // The call ended. A full-screen window outliving it would cover the
    // desktop with a call that is over.
    m_fullScreen = false;
    if (hadPin)
        Q_EMIT pinnedIdentityChanged();
    if (hadMode)
        Q_EMIT layoutPreferenceChanged();
    if (hadFullScreen)
        Q_EMIT fullScreenChanged();
    onSharesChanged();
}
