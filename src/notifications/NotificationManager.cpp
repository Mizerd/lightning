#include "notifications/NotificationManager.h"

#include "storage/AppDataPaths.h"
#include "notifications/FallbackAvatar.h"

#include "matrix/EventPreview.h"
#include "matrix/TimelineEvent.h"
#include "models/UserLookup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLoggingCategory>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPixmap>
#include <QStandardPaths>
#include <QUrl>
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
// Edge of the initials disc handed to the notification daemon. Daemons scale
// what they are given; 64 covers the common 48px slot without being a
// noticeably soft upscale on a HiDPI panel.
constexpr int kFallbackAvatarEdge = 64;

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

#ifdef HAVE_QT_DBUS
// The notification's own identity: what the daemon draws in the corner next
// to "Lightning", and which desktop entry it associates the notification
// with. Both were the fixed name "lightning", which resolves ONLY in an
// installed deb/rpm — a dev build, an un-integrated AppImage and a Flatpak
// all fall through to the daemon's generic document glyph (reported from a
// real desktop, with a screenshot of exactly that).
//
// Inside a Flatpak the exported desktop file and icon are renamed to the
// application ID by the packaging manifest, so the name to use is that ID.
// It is read from FLATPAK_ID rather than hard-coded, so the application
// never has to know its own Flatpak identity and cannot drift from it.
//
// When no themed icon resolves under either name, the icon is materialized
// once from the resource the window icon already falls back to and passed
// as a file:// URI, which the freedesktop specification accepts in place of
// a theme name. That is what makes the icon appear for source runs and
// AppImages, where nothing is installed into an icon theme at all.
struct NotificationIdentity {
    QString appIcon;      // theme name or file:// URI
    QString desktopEntry; // desktop file basename, no .desktop suffix
};

QString materializedIconPath()
{
    // Lightning-OWNED cache, so it follows the portable data root when the
    // installation is portable and stays exactly where it was otherwise.
    // QStandardPaths would answer %LOCALAPPDATA% on Windows, which is one of
    // the three places a portable copy must not write to.
    const QString cacheDir = matrix::app_data::cacheRoot();
    if (cacheDir.isEmpty())
        return {};
    QDir().mkpath(cacheDir);
    const QString path = cacheDir + QStringLiteral("/notification-icon.png");
    if (QFileInfo::exists(path))
        return path;
    const QIcon icon(QStringLiteral(
        ":/qt/qml/MatrixClient/data/icons/hicolor/256x256/apps/lightning.png"));
    const QPixmap pixmap = icon.pixmap(256, 256);
    if (pixmap.isNull() || !pixmap.save(path, "PNG"))
        return {};
    return path;
}

NotificationIdentity notificationIdentity()
{
    static const NotificationIdentity identity = [] {
        NotificationIdentity result;
        const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
        result.desktopEntry = flatpakId.isEmpty()
            ? QStringLiteral("lightning") : flatpakId;
        if (QIcon::hasThemeIcon(result.desktopEntry)) {
            result.appIcon = result.desktopEntry;
            return result;
        }
        const QString path = materializedIconPath();
        result.appIcon = path.isEmpty()
            ? result.desktopEntry
            : QUrl::fromLocalFile(path).toString();
        return result;
    }();
    return identity;
}
#endif

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
    // Incoming-call ring: re-deliver (replacing) every few seconds while
    // ringing so the themed call sound repeats.
    m_callRingTimer.setInterval(5000);
    connect(&m_callRingTimer, &QTimer::timeout, this,
            &NotificationManager::onCallRingTick);
}

void NotificationManager::onCallRingTick()
{
    if (QDateTime::currentMSecsSinceEpoch() >= m_callRingDeadlineMs) {
        stopIncomingCall(m_activeCallId);
        return;
    }
    deliverCallNotification();
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
    // A call row is not a message. It carries no body at all (the sentence
    // is built in TimelineModel), and the call itself already notifies
    // through the ring lane — notifying again here would post an EMPTY
    // desktop notification for every call. It used to be suppressed by the
    // StateChange test above, until calls got their own row kind.
    if (event.isVirtual() || event.type == TimelineEvent::StateChange
        || event.type == TimelineEvent::CallEvent)
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
    // Same judgement, one moment earlier: a room being read is a room being
    // read whether or not its view has finished settling.
    if (context.roomHydrating)
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
    // Carried so the delivery can raise the urgency: a message that names
    // the user is the one it is least acceptable to miss.
    payload.insert(QStringLiteral("mention"),
                   event.mentionsMe || event.mentionsRoom);
    // Private preview deliberately keeps the generic app identity: a room or
    // DM avatar would disclose the conversation the user asked to hide —
    // and an initials disc discloses exactly the same thing, so it is
    // withheld on the same branch rather than treated as a lesser hint.
    if (context.previewMode == Private) {
        deliver(decision.title, decision.body, payload, decision.playSound);
        return;
    }
    // An identity with no avatar is not an identity with no picture: every
    // other surface draws its initials disc, and a notification carrying no
    // image at all gets the daemon's generic document glyph instead.
    const QImage fallback = lightning::notifications::fallbackAvatar(
        context.roomName, context.avatarColorKey, kFallbackAvatarEdge,
        context.themeId);
    deliver(decision.title, decision.body, payload, decision.playSound,
            context.avatarMxc, fallback);
}

void NotificationManager::showGeneric(const QString &title,
                                      const QString &safeBody,
                                      const QString &roomId,
                                      const QString &avatarMxc)
{
    ++m_genericNoticeCount;
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), QString{});
    payload.insert(QStringLiteral("threadRootId"), QString{});
    deliver(title, safeBody, payload, false, avatarMxc);
}

void NotificationManager::clearPending()
{
    stopIncomingCall(QString());
    m_pendingPayloads.clear();
    m_payloadOrder.clear();
    m_avatarWaits.clear();
    m_avatarWaitTimer.stop();
}

void NotificationManager::closeRoomNotifications(const QString &roomId)
{
    if (roomId.isEmpty() || m_pendingPayloads.isEmpty())
        return;
    QList<quint32> stale;
    for (auto it = m_pendingPayloads.cbegin(); it != m_pendingPayloads.cend();
         ++it) {
        if (it.value().value(QStringLiteral("roomId")).toString() == roomId)
            stale.append(it.key());
    }
    if (stale.isEmpty())
        return;
    // The ring is not a message and owns its own lifetime (stopIncomingCall);
    // closing it from here would silence a call the user has not answered
    // just because they read the room it is ringing in.
    stale.removeOne(m_activeCallNotificationId);
    if (stale.isEmpty())
        return;
#ifdef HAVE_QT_DBUS
    QDBusInterface notifications(kService, kPath, kInterface,
                                 QDBusConnection::sessionBus());
    const bool valid = notifications.isValid();
#endif
    for (const quint32 id : std::as_const(stale)) {
#ifdef HAVE_QT_DBUS
        if (valid)
            notifications.call(QStringLiteral("CloseNotification"), id);
#endif
        forgetPayload(id);
    }
    qCInfo(lcNotify) << "withdrew" << stale.size()
                     << "notification(s) for a room that has been read";
}

void NotificationManager::recordPayload(quint32 id, const QVariantMap &payload)
{
    // Re-recording an id promotes it to most-recent: a notification being
    // actively refreshed (the call card's replaces_id re-delivery) must
    // not sit at the FRONT of the eviction queue just because it was first
    // shown long ago (review round 2).
    m_payloadOrder.removeOne(id);
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
                                  const QString &avatarMxc,
                                  const QImage &fallback)
{
    if (!avatarMxc.startsWith(QLatin1String("mxc://")) || !m_avatarImage) {
        deliverNow(title, body, payload, sound, fallback);
        return;
    }
    const QImage cached = m_avatarImage(avatarMxc, /*request=*/true);
    if (!cached.isNull()) {
        deliverNow(title, body, payload, sound, cached);
        return;
    }
    if (m_avatarFailed && m_avatarFailed(avatarMxc)) {
        deliverNow(title, body, payload, sound, fallback);
        return;
    }
    // Keep delivery bounded: if an avatar service stalls, the notification
    // appears with Lightning's normal icon after a short grace period.
    if (m_avatarWaits.size() >= kMaxPendingPayloads) {
        const WaitingDelivery oldest = m_avatarWaits.takeFirst();
        deliverNow(oldest.title, oldest.body, oldest.payload, oldest.sound,
                   oldest.fallback);
    }
    m_avatarWaits.append({ title, body, payload, sound, avatarMxc, fallback });
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
                       waiting.sound,
                       image.isNull() ? waiting.fallback : image);
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
    const NotificationIdentity identity = notificationIdentity();
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), identity.desktopEntry },
    };
    // URGENCY, so a mention does not quietly time out.
    //
    // freedesktop urgency: 0 low, 1 normal, 2 critical. A critical
    // notification is not dismissed on a timer by GNOME or KDE — it waits
    // for the user, which is the correct treatment for a message that named
    // them and the wrong treatment for ordinary traffic, so ordinary traffic
    // stays normal. Reported as messages going unnoticed entirely; a mention
    // that expired while the user was away was indistinguishable from one
    // that never arrived.
    const bool mention = payload.value(QStringLiteral("mention")).toBool();
    hints.insert(QStringLiteral("urgency"),
                 QVariant::fromValue(uchar(mention ? 2 : 1)));
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
        identity.appIcon, title, body, actions, hints, int(-1));
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
    if (action == QLatin1String("decline")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty()) {
            const QString callId = m_activeCallId;
            stopIncomingCall(callId);
            Q_EMIT callDeclineRequested(callId);
        }
        return;
    }
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
    if (id != 0 && id == m_activeCallNotificationId) {
        // The user (or daemon) dismissed the call card: stop re-ringing,
        // but do NOT decline — dismissing a notification is not an answer,
        // and other devices keep ringing.
        m_activeCallNotificationId = 0;
        m_callRingTimer.stop();
    }
    forgetPayload(id);
}

void NotificationManager::showIncomingCall(const QString &roomId,
                                           const QString &callId,
                                           const QString &title,
                                           const QString &safeBody,
                                           bool sound, int ringSeconds)
{
    if (callId.isEmpty())
        return;
    // One ring at a time — a newer call replaces the previous card.
    if (!m_activeCallId.isEmpty() && m_activeCallId != callId)
        stopIncomingCall(m_activeCallId);
    m_activeCallId = callId;
    m_activeCallRoomId = roomId;
    m_activeCallTitle = title;
    m_activeCallBody = safeBody;
    m_activeCallSound = sound;
    m_callRingDeadlineMs = QDateTime::currentMSecsSinceEpoch()
        + qint64(qBound(5, ringSeconds, 300)) * 1000;
    deliverCallNotification();
    if (sound)
        m_callRingTimer.start();
}

void NotificationManager::stopIncomingCall(const QString &callId)
{
    if (m_activeCallId.isEmpty()
        || (!callId.isEmpty() && callId != m_activeCallId))
        return;
    m_callRingTimer.stop();
#ifdef HAVE_QT_DBUS
    if (m_activeCallNotificationId != 0) {
        QDBusInterface notifications(kService, kPath, kInterface,
                                     QDBusConnection::sessionBus());
        if (notifications.isValid())
            notifications.call(QStringLiteral("CloseNotification"),
                               m_activeCallNotificationId);
        forgetPayload(m_activeCallNotificationId);
    }
#endif
    m_activeCallNotificationId = 0;
    m_activeCallId.clear();
    m_activeCallRoomId.clear();
    m_activeCallTitle.clear();
    m_activeCallBody.clear();
    m_activeCallSound = false;
}

void NotificationManager::deliverCallNotification()
{
#ifdef HAVE_QT_DBUS
    if (m_activeCallId.isEmpty())
        return;
    QDBusInterface notifications(kService, kPath, kInterface,
                                 QDBusConnection::sessionBus());
    if (!notifications.isValid()) {
        qCInfo(lcNotify) << "notification service unavailable";
        return;
    }
    const QStringList actions{ QStringLiteral("default"), tr("Open"),
                               QStringLiteral("decline"), tr("Decline") };
    const NotificationIdentity identity = notificationIdentity();
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), identity.desktopEntry },
        { QStringLiteral("urgency"), quint8(2) }, // critical: a live ring
    };
    if (m_activeCallSound) {
        // Themed freedesktop call sound — the notification server plays
        // it; Lightning bundles no audio. Deliberately NOT coalesced with
        // message sounds: the repeat IS the ring.
        hints.insert(QStringLiteral("sound-name"),
                     QStringLiteral("phone-incoming-call"));
    }
    const QDBusReply<quint32> reply = notifications.call(
        QStringLiteral("Notify"), QStringLiteral("Lightning"),
        m_activeCallNotificationId, identity.appIcon, m_activeCallTitle,
        m_activeCallBody, actions, hints, int(-1));
    if (reply.isValid()) {
        if (m_activeCallNotificationId != 0
            && reply.value() != m_activeCallNotificationId)
            forgetPayload(m_activeCallNotificationId);
        m_activeCallNotificationId = reply.value();
        recordPayload(m_activeCallNotificationId,
                      QVariantMap{
                          { QStringLiteral("roomId"), m_activeCallRoomId },
                          { QStringLiteral("eventId"), QString() },
                          { QStringLiteral("threadRootId"), QString() },
                      });
    }
#endif
}
