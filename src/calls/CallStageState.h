// The call stage's view state, in C++ because:
//
// 1. It must survive the Loader: switching rooms destroys the stage component,
//    but this state belongs to the call.
// 2. It must be testable without a scene: policy in QML expressions cannot be
//    driven by tests.
//
// Invariant: while a share is live there is always a way back to it.
// Dismissal only affects the spotlight; a dismissed share is still a row, a
// grid tile and routable. restorableShareAvailable stays true while any live
// share is dismissed, and a new share re-arms the automatic spotlight even
// after "back to grid". (Previously "grid" latched and made shares
// unreachable for the rest of the call.)
#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class CallShareModel;

class CallStageState : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallStageState is exposed via app.groupCall.stageState")

    /// A participant the user chose to enlarge; empty means nobody.
    Q_PROPERTY(QString pinnedIdentity READ pinnedIdentity
                   NOTIFY pinnedIdentityChanged)
    /// "auto" | "grid" | "spotlight": a preference, not a latch. See
    /// setLayoutPreference() and onShareAppeared().
    Q_PROPERTY(QString layoutPreference READ layoutPreference
                   NOTIFY layoutPreferenceChanged)
    /// The share for the spotlight: the newest live share not dismissed, or
    /// empty. Derived, so it cannot go stale.
    Q_PROPERTY(QString spotlightShareId READ spotlightShareId
                   NOTIFY spotlightChanged)
    /// True while at least one live share is dismissed; what a "Show screen
    /// share (N)" control binds to.
    Q_PROPERTY(bool restorableShareAvailable READ restorableShareAvailable
                   NOTIFY spotlightChanged)
    Q_PROPERTY(int dismissedShareCount READ dismissedShareCount
                   NOTIFY spotlightChanged)
    /// The focused surface fills a whole screen in its own window. Enforced
    /// here: the flag drops itself whenever nothing is focused, so full screen
    /// is never empty.
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)
    // Picture-in-picture: a small always-on-top window carrying the call.
    // Mutually exclusive with full screen: SfuVideoRouter holds one sink per
    // track and the last attach wins, so two surfaces would leave one black.
    Q_PROPERTY(bool pictureInPicture READ pictureInPicture
                   NOTIFY pictureInPictureChanged)
    /// `LIGHTNING_CALL_TRACE` is set, so the call surface may print its
    /// bounded diagnostic lines (QML cannot read the environment). Read once.
    /// Lines may carry literal strings, counts and platform/screen names only;
    /// never a room, user, track key or anything a peer chose.
    Q_PROPERTY(bool traceEnabled READ traceEnabled CONSTANT)

public:
    explicit CallStageState(QObject *parent = nullptr);

    /// Not owned. Observed to prune dismissals of ended shares and to re-arm on
    /// a new share.
    void setShareModel(CallShareModel *shares);

    QString pinnedIdentity() const { return m_pinnedIdentity; }
    QString layoutPreference() const { return m_layoutPreference; }
    QString spotlightShareId() const;
    bool restorableShareAvailable() const;
    int dismissedShareCount() const;
    bool fullScreen() const { return m_fullScreen; }
    bool traceEnabled() const { return m_traceEnabled; }

    /// Enter or leave full screen. Refuses to enter with nothing focused (no
    /// spotlight share and nobody pinned); the check is here so every caller
    /// inherits it.
    Q_INVOKABLE void setFullScreen(bool fullScreen);
    Q_INVOKABLE void toggleFullScreen();
    bool pictureInPicture() const { return m_pictureInPicture; }
    Q_INVOKABLE void setPictureInPicture(bool pip);
    Q_INVOKABLE void togglePictureInPicture();

    /// Take a share off the spotlight. It remains a row and a grid tile.
    Q_INVOKABLE void dismissShare(const QString &shareId);
    Q_INVOKABLE void restoreShare(const QString &shareId);
    /// Undo every dismissal.
    Q_INVOKABLE void restoreAllShares();
    Q_INVOKABLE bool isShareDismissed(const QString &shareId) const;

    Q_INVOKABLE void pin(const QString &identity);
    Q_INVOKABLE void clearPin();

    /// "auto" | "grid" | "spotlight"; anything else is refused, not stored.
    Q_INVOKABLE void setLayoutPreference(const QString &mode);

    /// Leaving the call: every field belongs to one call.
    void clear();

Q_SIGNALS:
    void pinnedIdentityChanged();
    void layoutPreferenceChanged();
    void spotlightChanged();
    void fullScreenChanged();
    void pictureInPictureChanged();

private Q_SLOTS:
    /// A share ended: drop its dismissal and re-derive the spotlight.
    void onShareEnded(const QString &shareId);
    /// A share started: something new is on offer, so a previous "grid"
    /// choice does not apply to it.
    void onShareAppeared(const QString &shareId);
    void onSharesChanged();

private:
    QPointer<CallShareModel> m_shares;
    QString m_pinnedIdentity;
    QString m_layoutPreference = QStringLiteral("auto");
    /// Bounded, since a hostile peer could restart a share in a loop.
    QSet<QString> m_dismissedShareIds;
    /// Cached so the derived properties notify only on a real change.
    QString m_lastSpotlightShareId;
    bool m_lastRestorable = false;
    bool m_fullScreen = false;
    bool m_pictureInPicture = false;
    /// Read once in the constructor.
    bool m_traceEnabled = false;

    /// Whether full screen has something to show. Not exposed, so no caller
    /// can check and then act separately.
    bool hasFocusedSurface() const;
    /// Drop full screen if its content is gone; called from every path that
    /// can empty the spotlight.
    void enforceFullScreenHasSurface();
};
