#include "notifications/NotificationManager.h"

#include "matrix/TimelineEvent.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

// Never logs notification bodies or message content — the category exists
// for coarse lifecycle diagnostics only.
Q_LOGGING_CATEGORY(lcNotify, "matrix.notify")

namespace {
const auto kService = QStringLiteral("org.freedesktop.Notifications");
const auto kPath = QStringLiteral("/org/freedesktop/Notifications");
const auto kInterface = QStringLiteral("org.freedesktop.Notifications");
} // namespace

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kInterface, QStringLiteral("ActionInvoked"),
                this, SLOT(onActionInvoked(quint32,QString)));
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(quint32,quint32)));
#endif
}

NotificationManager::Decision
NotificationManager::decide(const TimelineEvent &event, const Context &context)
{
    Decision decision;
    if (!context.notificationsEnabled)
        return decision;
    // Backlog applied during the initial sync is pre-existing history, never
    // fresh activity — suppress it so a restart does not re-notify every
    // already-seen message.
    if (!context.initialSyncComplete)
        return decision;
    if (context.roomMode == Muted)
        return decision;
    if (event.isVirtual() || event.type == TimelineEvent::StateChange)
        return decision;
    if (event.isLocalEcho || event.status != TimelineEvent::Sent)
        return decision;
    if (!context.selfUserId.isEmpty() && event.sender == context.selfUserId)
        return decision;
    const bool mention = event.mentionsMe || event.mentionsRoom;
    if (context.roomMode == MentionsOnly && !mention)
        return decision;
    // A room that is on screen, focused, and following the latest message
    // needs no notification; a scrolled-away or unfocused room still does.
    if (context.roomVisibleAtLatest)
        return decision;

    const QString sender = event.senderDisplayName.isEmpty()
        ? event.sender
        : event.senderDisplayName;
    const QString room = context.roomName.isEmpty()
        ? QCoreApplication::translate("Notifications", "Matrix room")
        : context.roomName;

    decision.notify = true;
    switch (context.previewMode) {
    case SenderAndMessage: {
        decision.title = context.roomIsDirect
            ? sender
            : QCoreApplication::translate("Notifications", "%1 in %2")
                  .arg(sender, room);
        if (event.undecryptable) {
            // Never ciphertext, never raw JSON — a generic placeholder.
            decision.body = QCoreApplication::translate(
                "Notifications", "Encrypted message");
        } else if (event.type == TimelineEvent::Image) {
            decision.body = QCoreApplication::translate(
                "Notifications", "Sent an image");
        } else if (event.type == TimelineEvent::File) {
            decision.body = QCoreApplication::translate(
                "Notifications", "Sent a file");
        } else {
            decision.body = event.body.left(160);
        }
        break;
    }
    case SenderOnly:
        decision.title = room;
        decision.body = event.mentionsMe
            ? QCoreApplication::translate("Notifications",
                                          "%1 mentioned you").arg(sender)
            : QCoreApplication::translate("Notifications",
                                          "New message from %1").arg(sender);
        break;
    case Private:
        decision.title = QStringLiteral("Lightning");
        decision.body = QCoreApplication::translate(
            "Notifications", "New Matrix notification");
        break;
    }
    return decision;
}

bool NotificationManager::shouldNotifyInvite(bool initialSyncComplete,
                                             bool alreadyKnown,
                                             bool notificationsEnabled)
{
    return notificationsEnabled && initialSyncComplete && !alreadyKnown;
}

void NotificationManager::processEvent(const TimelineEvent &event,
                                       const Context &context)
{
    const Decision decision = decide(event, context);
    if (!decision.notify)
        return;
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), event.roomId);
    payload.insert(QStringLiteral("eventId"), event.eventId);
    payload.insert(QStringLiteral("threadRootId"), event.threadRootId);
    deliver(decision.title, decision.body, payload);
}

void NotificationManager::showGeneric(const QString &title,
                                      const QString &safeBody,
                                      const QString &roomId)
{
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), QString{});
    payload.insert(QStringLiteral("threadRootId"), QString{});
    deliver(title, safeBody, payload);
}

void NotificationManager::clearPending()
{
    m_pendingPayloads.clear();
    m_payloadOrder.clear();
}

void NotificationManager::recordPayload(quint32 id, const QVariantMap &payload)
{
    if (!m_pendingPayloads.contains(id))
        m_payloadOrder.append(id);
    m_pendingPayloads.insert(id, payload);
    // Evict the oldest entries once over the cap so the most recent
    // notifications stay clickable (the 0.6.0 code cleared the whole map,
    // silently breaking click routing for every still-visible notification).
    while (m_payloadOrder.size() > kMaxPendingPayloads) {
        const quint32 oldest = m_payloadOrder.takeFirst();
        m_pendingPayloads.remove(oldest);
    }
}

void NotificationManager::forgetPayload(quint32 id)
{
    if (m_pendingPayloads.remove(id) > 0)
        m_payloadOrder.removeOne(id);
}

void NotificationManager::deliver(const QString &title, const QString &body,
                                  const QVariantMap &payload)
{
#ifdef HAVE_QT_DBUS
    QDBusInterface notifications(kService, kPath, kInterface,
                                 QDBusConnection::sessionBus());
    if (!notifications.isValid()) {
        qCInfo(lcNotify) << "notification service unavailable";
        return;
    }
    const QStringList actions{ QStringLiteral("default"), tr("Open") };
    const QVariantMap hints{
        { QStringLiteral("desktop-entry"), QStringLiteral("lightning") },
    };
    QDBusReply<quint32> reply = notifications.call(
        QStringLiteral("Notify"), QStringLiteral("Lightning"), quint32(0),
        QStringLiteral("lightning"), title, body, actions, hints, int(-1));
    if (reply.isValid())
        recordPayload(reply.value(), payload);
#else
    Q_UNUSED(title);
    Q_UNUSED(body);
    Q_UNUSED(payload);
    qCInfo(lcNotify) << "native notifications unavailable on this build";
#endif
}

void NotificationManager::onActionInvoked(quint32 id, const QString &action)
{
    if (action != QLatin1String("default"))
        return;
    const QVariantMap payload = m_pendingPayloads.value(id);
    forgetPayload(id);
    if (payload.isEmpty())
        return;
    Q_EMIT openRequested(payload.value(QStringLiteral("roomId")).toString(),
                         payload.value(QStringLiteral("eventId")).toString(),
                         payload.value(QStringLiteral("threadRootId"))
                             .toString());
}

void NotificationManager::onNotificationClosed(quint32 id, quint32 reason)
{
    Q_UNUSED(reason);
    forgetPayload(id);
}
