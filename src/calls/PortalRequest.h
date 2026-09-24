#pragma once

// Subscribing to an xdg-desktop-portal Request before making the call that
// creates it. Shared by CameraPortal and ScreenCastPortal.
//
// A portal method returns a Request path, and the answer arrives later as its
// `Response` signal. Subscribing only after handling the reply loses a
// Response that arrives immediately (QtDBus drops signals with no receiver):
// with a stored camera permission, AccessCamera's Response arrives about
// 0.4 ms after its reply, and the camera never started.
//
// The portal spec makes the path predictable:
// /org/freedesktop/portal/desktop/request/SENDER/TOKEN, where SENDER is the
// caller's unique bus name without the leading ':' and with '.' replaced by
// '_', and TOKEN is the caller's `handle_token`. So subscribe first, then
// call. Portals older than 0.9 return some other path, which
// reconcileRequestPath() then follows (as racy as before).

#include <QPointer>
#include <QString>

namespace portal {

/// The Request path a portal will use for `token` and the caller's unique bus
/// name (":1.189"). Empty when either input is empty; the caller then
/// subscribes after the reply.
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
/// since steps delete themselves) and its path, kept separately because a
/// missing step means "already answered", not "never subscribed".
template <typename Step>
struct Subscription
{
    QPointer<Step> step;
    QString path;
};

/// Subscribe a new `Step` (a QObject that subscribes to `Response` on the
/// given path and emits `answered`) to the predicted path, before the call.
/// Empty when nothing can be predicted.
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

/// Called once the reply names the real Request path. If it matches the
/// prediction, the subscription is already in place (and may have answered);
/// otherwise (portals older than 0.9) follow the returned path.
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
