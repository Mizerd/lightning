// The call stage's VIEW state, in C++, for exactly two reasons.
//
// 1. IT MUST SURVIVE THE LOADER. The stage is hosted by a Loader whose
//    `active` follows "this room's call is the open room's call". Switching
//    room destroys the component and everything declared inside it. Today
//    that is the ONLY thing that recovers from the dead end described below —
//    an accident, not a control. State that belongs to the CALL has to
//    outlive the component that happens to be drawing it.
//
// 2. IT MUST BE TESTABLE WITHOUT A SCENE. Every existing CallStage assertion
//    is a source scan; the component has never been instantiated in a test.
//    Policy that lives in QML expressions cannot be driven, which is the
//    failure §16 records twice (the row window shipped as a permanent no-op;
//    the rail drop could never group). Policy here can be driven directly.
//
// THE DEFECT THIS CLASS IS SHAPED BY.
//
// The stage had `layoutMode`, starting at "auto". `effectiveLayout` returned
// it verbatim whenever it was not "auto", so the automatic
// promote-a-share rule was reachable ONLY from "auto". The single writer of
// anything else was the "Back to grid" button, which wrote "grid". Nothing,
// anywhere, ever wrote "auto" or "spotlight" back.
//
// So one press of "back to grid" made the spotlight unreachable for the rest
// of the call — and because the grid's tiles never asked for a screen track
// at all, the share was not merely un-spotlighted, it was not drawn anywhere.
// That is the maintainer's "if share is closed no way to get it back",
// exactly, and it is a one-way door with no key.
//
// THE INVARIANT THIS CLASS ENFORCES, AND WHICH THE TEST PINS:
//
//   WHILE A SHARE IS LIVE THERE IS ALWAYS A WAY BACK TO IT.
//
// Dismissal applies to the SPOTLIGHT and never to the share's existence.
// `CallShareModel` does not know this class exists and carries no "dismissed"
// role, so a dismissed share is still a row, still a tile in the grid, still
// routable. On top of that, `restorableShareAvailable` is true for as long as
// any live share is dismissed, so the surface always has something to bind a
// "show it again" control to; and a NEW share re-arms the automatic spotlight
// even if the user pressed grid a moment ago, because a fresh share is not
// the thing they waved away.
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

    /// A participant the user chose to enlarge. Empty means nobody.
    Q_PROPERTY(QString pinnedIdentity READ pinnedIdentity
                   NOTIFY pinnedIdentityChanged)
    /// "auto" | "grid" | "spotlight". A PREFERENCE, never a latch: see
    /// setLayoutPreference() and onShareAppeared().
    Q_PROPERTY(QString layoutPreference READ layoutPreference
                   NOTIFY layoutPreferenceChanged)
    /// The share that should be on the spotlight right now: the NEWEST live
    /// share the user has not dismissed, or empty. Derived, never stored, so
    /// it cannot go stale against the share model.
    Q_PROPERTY(QString spotlightShareId READ spotlightShareId
                   NOTIFY spotlightChanged)
    /// True while at least one LIVE share is dismissed. This is the property
    /// a "Show screen share (N)" control binds to, and it is the machine-
    /// checkable half of the invariant above.
    Q_PROPERTY(bool restorableShareAvailable READ restorableShareAvailable
                   NOTIFY spotlightChanged)
    Q_PROPERTY(int dismissedShareCount READ dismissedShareCount
                   NOTIFY spotlightChanged)
    /// The focused surface should fill a whole screen, in its own window —
    /// Discord's "Full Screen" on the focused stream.
    ///
    /// Here rather than in QML for the same two reasons as everything else in
    /// this class, plus a third: a full-screen window with NOTHING in it is
    /// unrecoverable-looking, so the one state that must never happen is
    /// "full screen with no focused surface". That is enforced here — the
    /// flag drops itself whenever the spotlight empties — instead of being
    /// left to a binding somebody can later simplify away.
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)
    // v0.9.0 picture-in-picture: a small always-on-top window carrying the
    // call while the main window is elsewhere.
    //
    // It lives HERE rather than in QML for the same reason fullScreen does —
    // it is stage state the call's lifecycle has to be able to drop — and
    // the two are MUTUALLY EXCLUSIVE by construction. They compete for the
    // same thing: `SfuVideoRouter` holds ONE sink per track and the last
    // attach owns it, so two surfaces rendering the same participant means
    // one of them goes black. Making them exclusive here, rather than hoping
    // no QML site opens both, is what keeps that from being possible.
    Q_PROPERTY(bool pictureInPicture READ pictureInPicture
                   NOTIFY pictureInPictureChanged)
    /// `LIGHTNING_CALL_TRACE` is set, so the call surface may print its
    /// bounded diagnostic lines.
    ///
    /// Here because this is the only object the stage reaches that C++ owns,
    /// and QML cannot read an environment variable. CONSTANT: the variable is
    /// read once at construction — a process does not gain a debug flag
    /// halfway through, and a notifying property would invite one.
    ///
    /// What it may carry is bounded by the same rule as GuiStallTracer:
    /// LITERAL strings, counts and platform/screen NAMES. Never a room, a
    /// user, a track key or anything a peer chose.
    Q_PROPERTY(bool traceEnabled READ traceEnabled CONSTANT)

public:
    explicit CallStageState(QObject *parent = nullptr);

    /// Not owned. The state observes the model to prune dismissals of shares
    /// that ended and to re-arm on a share that started.
    void setShareModel(CallShareModel *shares);

    QString pinnedIdentity() const { return m_pinnedIdentity; }
    QString layoutPreference() const { return m_layoutPreference; }
    QString spotlightShareId() const;
    bool restorableShareAvailable() const;
    int dismissedShareCount() const;
    bool fullScreen() const { return m_fullScreen; }
    bool traceEnabled() const { return m_traceEnabled; }

    /// Go full screen, or come back.
    ///
    /// Refuses to go full screen when there is nothing focused: no share on
    /// the spotlight and nobody pinned. A window filling the monitor with an
    /// empty rectangle is the worst outcome this feature can produce, and the
    /// refusal is here rather than in the caller so every caller inherits it.
    Q_INVOKABLE void setFullScreen(bool fullScreen);
    Q_INVOKABLE void toggleFullScreen();
    bool pictureInPicture() const { return m_pictureInPicture; }
    Q_INVOKABLE void setPictureInPicture(bool pip);
    Q_INVOKABLE void togglePictureInPicture();

    /// Take this share off the spotlight. It REMAINS a row in the share
    /// model and therefore a tile in the grid — "dismissed" has never meant
    /// "gone", and this method is where that distinction is kept.
    Q_INVOKABLE void dismissShare(const QString &shareId);
    Q_INVOKABLE void restoreShare(const QString &shareId);
    /// Undo every dismissal. What the "Show screen share" control calls when
    /// more than one is waiting.
    Q_INVOKABLE void restoreAllShares();
    Q_INVOKABLE bool isShareDismissed(const QString &shareId) const;

    Q_INVOKABLE void pin(const QString &identity);
    Q_INVOKABLE void clearPin();

    /// "auto" | "grid" | "spotlight". Anything else is REFUSED rather than
    /// stored: an unrecognised mode read back through `effectiveLayout` is
    /// how the original latch behaved.
    Q_INVOKABLE void setLayoutPreference(const QString &mode);

    /// Leaving the call. Every field here belongs to one call.
    void clear();

Q_SIGNALS:
    void pinnedIdentityChanged();
    void layoutPreferenceChanged();
    void spotlightChanged();
    void fullScreenChanged();
    void pictureInPictureChanged();

private Q_SLOTS:
    /// A share ended: drop its dismissal so the id cannot be inherited (the
    /// ids are not reused, but an unbounded set of dead ids is still a leak),
    /// and re-derive the spotlight.
    void onShareEnded(const QString &shareId);
    /// A share started. This is what makes "grid" a preference rather than a
    /// latch: something NEW is on offer, and the user's earlier "not that
    /// one" cannot speak for it.
    void onShareAppeared(const QString &shareId);
    void onSharesChanged();

private:
    QPointer<CallShareModel> m_shares;
    QString m_pinnedIdentity;
    QString m_layoutPreference = QStringLiteral("auto");
    /// Bounded: a call cannot have more shares than participants, and an
    /// unbounded set fed by a hostile peer restarting a share in a loop is
    /// not a set we want to hold.
    QSet<QString> m_dismissedShareIds;
    /// Cached only so the derived properties can notify on a real change
    /// rather than on every model poke.
    QString m_lastSpotlightShareId;
    bool m_lastRestorable = false;
    bool m_fullScreen = false;
    bool m_pictureInPicture = false;
    /// Read once, in the constructor. See the property's note.
    bool m_traceEnabled = false;

    /// True when there is something for a full screen to SHOW. Not a
    /// property: it is the guard, and exposing it would invite a caller to
    /// check it and then act, which is the race this avoids by keeping the
    /// check inside the write.
    bool hasFocusedSurface() const;
    /// Drop full screen if the thing it was showing is gone. Called from
    /// every path that can empty the spotlight.
    void enforceFullScreenHasSurface();
};
