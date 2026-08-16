#include "notifications/NotificationManager.h"

#include "matrix/EventPreview.h"
#include "matrix/TimelineEvent.h"
#include "models/UserLookup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLoggingCategory>
#include <QImage>

#include <utility>

#ifdef HAVE_QT_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#endif

#ifdef HAVE_QT_DBUS
struct FreedesktopNotificationImage {
    int width = 0;
    int height = 0;
    int rowStride = 0;
    bool hasAlpha = true;
    int bitsPerSample = 8;
    int channels = 4;
    QByteArray data;
};
Q_DECLARE_METATYPE(FreedesktopNotificationImage)

QDBusArgument &operator<<(QDBusArgument &argument,
                          const FreedesktopNotificationImage &image)
{
    argument.beginStructure();
    argument << image.width << image.height << image.rowStride
             << image.hasAlpha << image.bitsPerSample << image.channels
             << image.data;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                FreedesktopNotificationImage &image)
{
    argument.beginStructure();
    argument >> image.width >> image.height >> image.rowStride
             >> image.hasAlpha >> image.bitsPerSample >> image.channels
             >> image.data;
    argument.endStructure();
    return argument;
}
#endif

// Never logs notification bodies or message content — the category exists
// for coarse lifecycle diagnostics only.
Q_LOGGING_CATEGORY(lcNotify, "matrix.notify")

namespace {
const auto kService = QStringLiteral("org.freedesktop.Notifications");
const auto kPath = QStringLiteral("/org/freedesktop/Notifications");
const auto kInterface = QStringLiteral("org.freedesktop.Notifications");
// Notification-sound burst coalescing window (ms).
constexpr qint64 kSoundCoalesceMs = 1500;
constexpr int kAvatarWaitMs = 1200;

#ifdef HAVE_QT_DBUS
FreedesktopNotificationImage notificationImage(const QImage &source)
{
    const int edge = qMin(source.width(), source.height());
    const QRect crop((source.width() - edge) / 2,
                     (source.height() - edge) / 2, edge, edge);
    const QImage image = source.copy(crop)
        .scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGBA8888);
    FreedesktopNotificationImage out;
    out.width = image.width();
    out.height = image.height();
    out.rowStride = image.bytesPerLine();
    out.data = QByteArray(reinterpret_cast<const char *>(image.constBits()),
                          static_cast<qsizetype>(image.sizeInBytes()));
    return out;
}
#endif
} // namespace

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
#ifdef HAVE_QT_DBUS
    qDBusRegisterMetaType<FreedesktopNotificationImage>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kInterface, QStringLiteral("ActionInvoked"),
                this, SLOT(onActionInvoked(quint32,QString)));
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(quint32,quint32)));
#endif
    m_avatarWaitTimer.setSingleShot(true);
    m_avatarWaitTimer.setInterval(kAvatarWaitMs);
    connect(&m_avatarWaitTimer, &QTimer::timeout, this,
            [this] { flushAvatarWaits(/*fallbackAll=*/true); });
}

void NotificationManager::setAvatarProvider(
    std::function<QImage(const QString &, bool)> image,
    std::function<bool(const QString &)> failed)
{
    m_avatarImage = std::move(image);
    m_avatarFailed = std::move(failed);
}

void NotificationManager::avatarCacheChanged()
{
    flushAvatarWaits(/*fallbackAll=*/false);
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
    // An ignored sender must not notify even in the window before the
    // server applies the ignore and stops delivering their events.
    if (context.senderIsIgnored)
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
        ? matrix::user_lookup::localpartOrUserId(event.sender)
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
        } else if (event.type == TimelineEvent::Image
                   || event.type == TimelineEvent::Sticker) {
            decision.body = QCoreApplication::translate(
                "Notifications", "Sent an image");
        } else if (event.type == TimelineEvent::File) {
            decision.body = QCoreApplication::translate(
                "Notifications", "Sent a file");
        } else if (event.type == TimelineEvent::Video) {
            decision.body = QCoreApplication::translate(
                "Notifications", "Sent a video");
        } else if (event.type == TimelineEvent::Audio) {
            decision.body = event.mediaIsVoice
                ? QCoreApplication::translate("Notifications",
                                              "Sent a voice message")
                : QCoreApplication::translate("Notifications",
                                              "Sent an audio file");
        } else if (event.type == TimelineEvent::Poll) {
            // Never the multi-line MSC3381 fallback (question plus every
            // answer) in a desktop notification.
            decision.body = matrix::preview::oneLineSummary(event).left(160);
        } else {
            // Bodies are free-form: mention markdown reduces to its label,
            // newlines collapse — a notification is one compact line.
            decision.body =
                matrix::preview::normalizePreviewText(event.body).left(160);
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

    // Sound rides on the notify decision (so muted / active-room /
    // mentions-only suppression already suppressed it). The mode narrows which
    // eligible notifications also make a sound.
    switch (context.soundMode) {
    case SoundOff:
        decision.playSound = false;
        break;
    case SoundMentionsAndDirect:
        decision.playSound =
            mention || context.roomIsDirect;
        break;
    case SoundAll:
        decision.playSound = true;
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
    // Private preview deliberately keeps the generic app identity: a room or
    // DM avatar would disclose the conversation the user asked to hide.
    deliver(decision.title, decision.body, payload, decision.playSound,
            context.previewMode == Private ? QString() : context.avatarMxc);
}

void NotificationManager::showGeneric(const QString &title,
                                      const QString &safeBody,
                                      const QString &roomId,
                                      const QString &avatarMxc)
{
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), QString{});
    payload.insert(QStringLiteral("threadRootId"), QString{});
    deliver(title, safeBody, payload, false, avatarMxc);
}

void NotificationManager::clearPending()
{
    m_pendingPayloads.clear();
    m_payloadOrder.clear();
    m_avatarWaits.clear();
    m_avatarWaitTimer.stop();
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
                                  const QVariantMap &payload, bool sound,
                                  const QString &avatarMxc)
{
    if (!avatarMxc.startsWith(QLatin1String("mxc://")) || !m_avatarImage) {
        deliverNow(title, body, payload, sound, {});
        return;
    }
    const QImage cached = m_avatarImage(avatarMxc, /*request=*/true);
    if (!cached.isNull()) {
        deliverNow(title, body, payload, sound, cached);
        return;
    }
    if (m_avatarFailed && m_avatarFailed(avatarMxc)) {
        deliverNow(title, body, payload, sound, {});
        return;
    }
    // Keep delivery bounded: if an avatar service stalls, the notification
    // appears with Lightning's normal icon after a short grace period.
    if (m_avatarWaits.size() >= kMaxPendingPayloads) {
        const WaitingDelivery oldest = m_avatarWaits.takeFirst();
        deliverNow(oldest.title, oldest.body, oldest.payload, oldest.sound, {});
    }
    m_avatarWaits.append({ title, body, payload, sound, avatarMxc });
    if (!m_avatarWaitTimer.isActive())
        m_avatarWaitTimer.start();
}

void NotificationManager::flushAvatarWaits(bool fallbackAll)
{
    QList<WaitingDelivery> remaining;
    for (const WaitingDelivery &waiting : std::as_const(m_avatarWaits)) {
        const QImage image = m_avatarImage
            ? m_avatarImage(waiting.avatarMxc, /*request=*/false) : QImage{};
        const bool failed = !m_avatarImage
            || (m_avatarFailed && m_avatarFailed(waiting.avatarMxc));
        if (!image.isNull() || failed || fallbackAll) {
            deliverNow(waiting.title, waiting.body, waiting.payload,
                       waiting.sound, image);
        } else {
            remaining.append(waiting);
        }
    }
    m_avatarWaits = remaining;
    if (!m_avatarWaits.isEmpty() && !m_avatarWaitTimer.isActive())
        m_avatarWaitTimer.start();
}

void NotificationManager::deliverNow(const QString &title,
                                     const QString &body,
                                     const QVariantMap &payload, bool sound,
                                     const QImage &avatar)
{
#ifdef HAVE_QT_DBUS
    QDBusInterface notifications(kService, kPath, kInterface,
                                 QDBusConnection::sessionBus());
    if (!notifications.isValid()) {
        qCInfo(lcNotify) << "notification service unavailable";
        return;
    }
    const QStringList actions{ QStringLiteral("default"), tr("Open") };
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), QStringLiteral("lightning") },
    };
    if (!avatar.isNull()) {
        hints.insert(QStringLiteral("image-data"),
                     QVariant::fromValue(notificationImage(avatar)));
    }
    if (sound) {
        // Coalesce bursts: at most one alert per short window.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastSoundMs >= kSoundCoalesceMs) {
            m_lastSoundMs = now;
            // Themed freedesktop sound name — the notification server plays
            // it; Lightning bundles no audio and touches no audio device.
            hints.insert(QStringLiteral("sound-name"),
                         QStringLiteral("message-new-instant"));
        }
    }
    QDBusReply<quint32> reply = notifications.call(
        QStringLiteral("Notify"), QStringLiteral("Lightning"), quint32(0),
        QStringLiteral("lightning"), title, body, actions, hints, int(-1));
    if (reply.isValid())
        recordPayload(reply.value(), payload);
#else
    Q_UNUSED(title);
    Q_UNUSED(body);
    Q_UNUSED(payload);
    Q_UNUSED(sound);
    Q_UNUSED(avatar);
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
