#pragma once

// Subscribing to an xdg-desktop-portal Request BEFORE making the call that
// creates it. Shared by CameraPortal and ScreenCastPortal so the two cannot
// drift apart again.
//
// WHY. A portal method that asks something of the user returns the object path
// of a Request, and the answer arrives later as that Request's `Response`
// signal. Both portals here used to subscribe to `Response` only once the
// method's REPLY had been handled — which is a trip through Qt's event loop,
// while QtDBus's own thread keeps reading the socket and drops any signal that
// has no receiver yet. That is harmless when a dialog stands in between (the
// user takes seconds to answer), and fatal when the portal answers at once:
// measured 2026-09-23 on Fedora 44 / KDE from inside the published Flathub
// build, with the camera permission already stored, `AccessCamera`'s Response
// arrived 0.4 ms after its reply. The grant was lost, the camera never
// started, and nothing logged — the 120 s timeout was the only way out.
// ScreenCast was spared only because its answers go through the desktop's
// portal backend and arrive later; nothing guaranteed that.
//
// THE CURE IS THE ONE THE PORTAL SPEC PRESCRIBES: the Request path is
// PREDICTABLE — /org/freedesktop/portal/desktop/request/SENDER/TOKEN, where
// SENDER is the caller's unique bus name without its leading ':' and with
// every '.' turned into '_', and TOKEN is the `handle_token` the caller chose.
// So subscribe to that path first, then call. Portals older than 0.9 returned
// some other path; `reconcileRequestPath` follows the returned one in that case,
// which is exactly the old behaviour and only as racy as it always was.

#include <QPointer>
#include <QString>

namespace portal {

/// The object path a portal will give the Request created with `token`, for a
/// caller whose unique bus name is `uniqueName` (":1.189"). Empty when either
/// input is — the caller then has nothing to predict and falls back to
/// subscribing after the reply.
inline QString predictedRequestPath(QString uniqueName, const QString &token)
{
    if (uniqueName.isEmpty() || token.isEmpty())
        return {};
    if (uniqueName.startsWith(QLatin1Char(':')))
        uniqueName.remove(0, 1);
    uniqueName.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
        .arg(uniqueName, token);
}

/// A subscription made before the call: the step (null once it has answered,
/// since a step deletes itself on its Response) and the path it was made on.
/// The path is kept SEPARATELY because "the step is gone" means "it already
/// answered", not "there was no subscription".
template <typename Step>
struct Subscription
{
    QPointer<Step> step;
    QString path;
};

/// Subscribe a new `Step` (a QObject whose constructor subscribes to
/// `Response` on the path it is given, and which emits `answered`) to the
/// predicted path, BEFORE the call is made. Empty when there is no prediction
/// to make.
template <typename Step, typename Owner, typename OnAnswer>
Subscription<Step> subscribeBeforeCall(Owner *owner, const QString &uniqueName,
                                       const QString &token, OnAnswer onAnswer)
{
    Subscription<Step> sub;
    sub.path = predictedRequestPath(uniqueName, token);
    if (sub.path.isEmpty())
        return sub;
    sub.step = new Step(owner, sub.path);
    QObject::connect(sub.step.data(), &Step::answered, owner, onAnswer);
    return sub;
}

/// Once the reply names the real Request path. When it is the predicted one
/// there is nothing to do: the subscription is in place and may even have
/// answered already. Otherwise the portal used a path we did not predict
/// (older than 0.9), so drop the guess and follow the returned path.
template <typename Step, typename Owner, typename OnAnswer>
void reconcileRequestPath(Owner *owner, Subscription<Step> &sub,
                          const QString &actualPath, OnAnswer onAnswer)
{
    if (!sub.path.isEmpty() && sub.path == actualPath)
        return;
    if (sub.step)
        sub.step->deleteLater();
    sub.step.clear();
    sub.path = actualPath;
    if (actualPath.isEmpty())
        return;
    sub.step = new Step(owner, actualPath);
    QObject::connect(sub.step.data(), &Step::answered, owner, onAnswer);
}

/// Drop a subscription whose call failed, so no step outlives the request.
template <typename Step>
void dropSubscription(Subscription<Step> &sub)
{
    if (sub.step)
        sub.step->deleteLater();
    sub.step.clear();
}

} // namespace portal
