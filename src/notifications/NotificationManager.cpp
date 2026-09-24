#include "notifications/NotificationManager.h"

#include "app/TrayIcon.h"

#include "storage/AppDataPaths.h"
#include "notifications/FallbackAvatar.h"

#include "matrix/EventPreview.h"
// Only for the static inline composite thread-timeline id helpers; no link
// dependency. See routableRoomId().
#include "matrix/MatrixClient.h"
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

// Coarse lifecycle diagnostics only; never logs bodies or message content.
Q_LOGGING_CATEGORY(lcNotify, "matrix.notify")

namespace {
const auto kService = QStringLiteral("org.freedesktop.Notifications");
const auto kPath = QStringLiteral("/org/freedesktop/Notifications");
const auto kInterface = QStringLiteral("org.freedesktop.Notifications");
// Sound burst coalescing window (ms).
constexpr qint64 kSoundCoalesceMs = 1500;
constexpr int kAvatarWaitMs = 1200;
// Edge of the initials disc handed to the daemon; 64 covers the common 48px
// slot without a soft upscale on HiDPI.
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
// The notification's app icon and desktop entry. The fixed name "lightning"
// resolves only for installed packages. Inside a Flatpak the exported entry
// and icon are renamed to FLATPAK_ID. When no themed icon resolves, the
// bundled icon is written to the cache once and passed as a file:// URI,
// which the freedesktop spec accepts (source runs, AppImages).
struct NotificationIdentity {
    QString appIcon;      // theme name or file:// URI
    QString desktopEntry; // desktop file basename, no .desktop suffix
};

QString materializedIconPath()
{
    // Lightning's own cache root, so a portable install does not write to
    // %LOCALAPPDATA%.
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
    // Inline-reply extension; harmless on daemons that never emit it.
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("NotificationReplied"),
                this, SLOT(onNotificationReplied(quint32,QString)));
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(quint32,quint32)));
#endif
    m_avatarWaitTimer.setSingleShot(true);
    m_avatarWaitTimer.setInterval(kAvatarWaitMs);
    connect(&m_avatarWaitTimer, &QTimer::timeout, this,
            [this] { flushAvatarWaits(/*fallbackAll=*/true); });
    // Re-deliver the ringing card every few seconds so the themed sound
    // repeats.
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
    // Initial-sync backlog is history, not fresh activity.
    if (!context.initialSyncComplete)
        return decision;
    if (context.roomMode == Muted)
        return decision;
    // Covers the window before the server applies the ignore.
    if (context.senderIsIgnored)
        return decision;
    // Call rows carry no body, and calls already notify through the ring lane.
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
    // A room on screen, focused and at the latest message needs no
    // notification.
    if (context.roomVisibleAtLatest)
        return decision;
    // Same for a room that is still settling after being opened.
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
            // Never ciphertext or raw JSON.
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
            // Not the multi-line MSC3381 fallback.
            decision.body = matrix::preview::oneLineSummary(event).left(160);
        } else {
            // One compact line: mention markdown reduces to its label, newlines
            // collapse.
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

    // Sound only applies to notifications that passed the checks above; the
    // mode narrows which of them make a sound.
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

// ── Composite timeline ids stop here ───────────────────────────────────────
//
// Thread timelines share the room diff pipeline, so events from a thread
// arrive with the composite id `room + US + "thread" + US + root` in
// TimelineEvent::roomId. A composite in a click payload would navigate to a
// room that does not exist, break inline replies and never match
// closeRoomNotifications(). Real room ids never contain a unit separator, so
// these helpers are no-ops for them.
QString NotificationManager::routableRoomId(const QString &roomId)
{
    return MatrixClient::isThreadTimelineId(roomId)
        ? MatrixClient::threadTimelineRoomId(roomId)
        : roomId;
}

// The root a click needs to open the thread panel; taken from the composite
// when the event does not carry it.
QString NotificationManager::routableThreadRootId(const QString &roomId,
                                                  const QString &threadRootId)
{
    if (!threadRootId.isEmpty() || !MatrixClient::isThreadTimelineId(roomId))
        return threadRootId;
    return MatrixClient::threadTimelineRootId(roomId);
}

void NotificationManager::processEvent(const TimelineEvent &event,
                                       const Context &context)
{
    const Decision decision = decide(event, context);
    if (!decision.notify)
        return;
    QVariantMap payload;
    // Normalised where the payload is built, so every consumer sees a real room
    // id.
    payload.insert(QStringLiteral("roomId"), routableRoomId(event.roomId));
    payload.insert(QStringLiteral("eventId"), event.eventId);
    payload.insert(QStringLiteral("threadRootId"),
                   routableThreadRootId(event.roomId, event.threadRootId));
    // The owning account; actions taken after a switch are checked against it.
    payload.insert(QStringLiteral("accountUserId"), m_accountUserId);
    // Mentions raise the urgency.
    payload.insert(QStringLiteral("mention"),
                   event.mentionsMe || event.mentionsRoom);
    // Private preview keeps the generic app identity: a room avatar or initials
    // disc would disclose the conversation.
    if (context.previewMode == Private) {
        deliver(decision.title, decision.body, payload, decision.playSound);
        return;
    }
    // Draw the initials disc when there is no avatar, as other surfaces do.
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
    payload.insert(QStringLiteral("accountUserId"), m_accountUserId);
    deliver(title, safeBody, payload, false, avatarMxc);
}

void NotificationManager::clearPending()
{
    stopIncomingCall(QString());
    m_pendingPayloads.clear();
    m_payloadOrder.clear();
    m_avatarWaits.clear();
    m_avatarWaitTimer.stop();
    // The tray balloon's payload is a pending click too; clear it so a balloon
    // left on screen cannot route into the previous account's room.
    m_lastFallbackPayload.clear();
}

void NotificationManager::closeRoomNotifications(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    // A delivery still waiting for its avatar has not been shown yet; drop it
    // for a room that has now been read.
    if (!m_avatarWaits.isEmpty()) {
        const auto sameRoom = [&roomId](const WaitingDelivery &waiting) {
            return waiting.payload.value(QStringLiteral("roomId")).toString()
                == roomId;
        };
        // Log the drop (count only) so it is distinguishable from never having
        // raised a notification.
        const qsizetype dropped = m_avatarWaits.removeIf(sameRoom);
        if (dropped > 0) {
            qCInfo(lcNotify) << "dropped" << dropped
                             << "notification(s) parked for an avatar in a "
                                "room that has been read";
            if (m_avatarWaits.isEmpty())
                m_avatarWaitTimer.stop();
        }
    }
    if (m_pendingPayloads.isEmpty())
        return;
    QList<quint32> stale;
    for (auto it = m_pendingPayloads.cbegin(); it != m_pendingPayloads.cend();
         ++it) {
        if (it.value().value(QStringLiteral("roomId")).toString() == roomId)
            stale.append(it.key());
    }
    if (stale.isEmpty())
        return;
    // The ring owns its own lifetime; reading its room must not silence it.
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
    // Re-recording promotes an id to most recent, so a refreshed call card is
    // not evicted first.
    m_payloadOrder.removeOne(id);
    m_payloadOrder.append(id);
    m_pendingPayloads.insert(id, payload);
    // Evict the oldest past the cap so recent notifications stay clickable.
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
    // Bounded: if the avatar stalls, the notification appears with the fallback
    // after a short grace period.
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

// The only place a notification body is escaped. A freedesktop body is
// markup on servers advertising `body-markup` (GNOME, KDE), and its content
// (names, message text) is attacker-chosen: tags would render and unbalanced
// ones make Pango reject the string. Escaping only when markup is rendered
// avoids literal `&amp;` elsewhere. Callers pass raw text, so escaping happens
// exactly once. The summary is plain text everywhere and is never escaped.
// Guarded by HAVE_QT_DBUS: Windows and macOS build without QtDBus.
#ifdef HAVE_QT_DBUS
QString NotificationManager::bodyForServer(QDBusInterface &notifications,
                                           const QString &body)
{
    if (!m_bodyMarkupKnown) {
        const QDBusReply<QStringList> caps =
            notifications.call(QStringLiteral("GetCapabilities"));
        m_bodyMarkup = caps.isValid()
            && caps.value().contains(QStringLiteral("body-markup"));
        // Offer inline reply only when advertised; otherwise it renders as a
        // button that can never produce text.
        m_inlineReply = caps.isValid()
            && caps.value().contains(QStringLiteral("inline-reply"));
        m_bodyMarkupKnown = true;
        qCInfo(lcNotify) << "notification server body-markup =" << m_bodyMarkup
                         << "inline-reply =" << m_inlineReply;
    }
    return m_bodyMarkup ? body.toHtmlEscaped() : body;
}
#endif

void NotificationManager::deliverNow(const QString &title,
                                     const QString &body,
                                     const QVariantMap &payload, bool sound,
                                     const QImage &avatar)
{
#ifdef HAVE_QT_DBUS
    QDBusInterface notifications(kService, kPath, kInterface,
                                 QDBusConnection::sessionBus());
    if (!notifications.isValid()) {
        // No daemon: fall back to the tray balloon.
        if (!deliverThroughTray(title, body, payload, avatar))
            qCInfo(lcNotify) << "notification service unavailable";
        return;
    }
    const NotificationIdentity identity = notificationIdentity();
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), identity.desktopEntry },
    };
    // Mentions are critical (2), which GNOME and KDE do not time out; ordinary
    // traffic stays normal (1).
    const bool mention = payload.value(QStringLiteral("mention")).toBool();
    hints.insert(QStringLiteral("urgency"),
                 QVariant::fromValue(uchar(mention ? 2 : 1)));
    if (!avatar.isNull()) {
        hints.insert(QStringLiteral("image-data"),
                     QVariant::fromValue(notificationImage(avatar)));
    }
    if (sound) {
        // At most one alert per short window.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastSoundMs >= kSoundCoalesceMs) {
            m_lastSoundMs = now;
            // Themed sound played by the notification server; Lightning plays
            // no audio here.
            hints.insert(QStringLiteral("sound-name"),
                         QStringLiteral("message-new-instant"));
        }
    }
    // Escaped iff the daemon renders markup; see bodyForServer(). The title is
    // never escaped.
    const QString safeBody = bodyForServer(notifications, body);

    // Built after bodyForServer(), which caches whether inline reply exists.
    // Mark-as-read and Reply need a real event: generic notices (invites,
    // verification requests) have no eventId and cannot be read or replied to.
    QStringList actions{ QStringLiteral("default"), tr("Open") };
    const bool actionable =
        !payload.value(QStringLiteral("roomId")).toString().isEmpty()
        && !payload.value(QStringLiteral("eventId")).toString().isEmpty();
    if (actionable) {
        actions << QStringLiteral("mark-read") << tr("Mark as read");
        if (m_inlineReply) {
            // The action id is fixed by the extension; the label is ours.
            actions << QStringLiteral("inline-reply") << tr("Reply");
            hints.insert(QStringLiteral("x-kde-reply-placeholder-text"),
                         tr("Reply…"));
        }
    }

    QDBusReply<quint32> reply = notifications.call(
        QStringLiteral("Notify"), QStringLiteral("Lightning"), quint32(0),
        identity.appIcon, title, safeBody, actions, hints, int(-1));
    if (reply.isValid()) {
        recordPayload(reply.value(), payload);
        return;
    }
    // A refusal is not an absence: log the error name only (never title or
    // body) and fall back to the balloon.
    qCWarning(lcNotify) << "notification server refused the notification:"
                        << reply.error().name();
    deliverThroughTray(title, body, payload, avatar);
#else
    // No QtDBus (Windows, macOS): the tray balloon carries the notification
    // (a toast on Windows, a user notification on macOS).
    Q_UNUSED(sound);
    if (!deliverThroughTray(title, body, payload, avatar))
        qCInfo(lcNotify) << "native notifications unavailable on this build";
#endif
}

void NotificationManager::setFallbackTray(TrayIcon *tray)
{
    if (m_fallbackTray == tray)
        return;
    if (m_fallbackTray)
        disconnect(m_fallbackTray, nullptr, this, nullptr);
    m_fallbackTray = tray;
    if (m_fallbackTray) {
        connect(m_fallbackTray, &TrayIcon::messageClicked, this,
                &NotificationManager::onFallbackMessageClicked);
    }
}

bool NotificationManager::deliverThroughTray(const QString &title,
                                             const QString &body,
                                             const QVariantMap &payload,
                                             const QImage &avatar)
{
    if (!m_fallbackTray || !m_fallbackTray->showMessage(title, body, avatar))
        return false;
    // One balloon at a time: the latest payload is the one a click resolves to.
    m_lastFallbackPayload = payload;
    if (!payload.value(QStringLiteral("roomId")).toString().isEmpty())
        qCInfo(lcNotify) << "notification delivered through the tray balloon";
    return true;
}

void NotificationManager::onFallbackMessageClicked()
{
    const QVariantMap payload = m_lastFallbackPayload;
    m_lastFallbackPayload.clear();
    emitOpenFor(payload);
}

// Every openRequested goes through here and is normalised again, since
// processEvent() is not the only payload producer. Idempotent.
void NotificationManager::emitOpenFor(const QVariantMap &payload)
{
    // Gate on the payload, not the room id: notices without a room (e.g. a
    // verification request) still raise the window, which Main.qml does before
    // reading the room id. Sign-out clears the whole payload.
    if (payload.isEmpty())
        return;
    const QString roomId =
        routableRoomId(payload.value(QStringLiteral("roomId")).toString());
    Q_EMIT openRequested(
        roomId, payload.value(QStringLiteral("eventId")).toString(),
        routableThreadRootId(
            payload.value(QStringLiteral("roomId")).toString(),
            payload.value(QStringLiteral("threadRootId")).toString()));
}

void NotificationManager::onActionInvoked(quint32 id, const QString &action)
{
    // Only the card currently showing can answer; a stale card from a previous
    // call must not. Compiles without D-Bus but never fires there.
    if (action == QLatin1String("accept")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty()) {
            const QString callId = m_activeCallId;
            // Retire the card before emitting so a racing re-delivery cannot
            // restore it.
            stopIncomingCall(callId);
            Q_EMIT callAcceptRequested(callId);
        }
        return;
    }
    if (action == QLatin1String("decline")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty()) {
            const QString callId = m_activeCallId;
            stopIncomingCall(callId);
            Q_EMIT callDeclineRequested(callId);
        }
        return;
    }
    // Silence is not an answer, so the card stays. CallSoundController decides
    // whether this is still the ringing call. The call id comes from this
    // class, never from the daemon.
    if (action == QLatin1String("silence")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty() && m_activeCallSilenceOffered) {
            Q_EMIT callSilenceRequested(m_activeCallId);
        }
        return;
    }
    if (action == QLatin1String("mark-read")) {
        const QVariantMap payload = m_pendingPayloads.value(id);
        forgetPayload(id);
        if (payload.isEmpty())
            return;
        // A room id, never a timeline id: markRoomRead has no composite
        // entries.
        Q_EMIT markReadRequested(
            payload.value(QStringLiteral("accountUserId")).toString(),
            routableRoomId(payload.value(QStringLiteral("roomId")).toString()),
            payload.value(QStringLiteral("eventId")).toString());
        return;
    }
    // Some daemons also send ActionInvoked for `inline-reply`, with the text
    // following in NotificationReplied, so the payload must survive.
    if (action == QLatin1String("inline-reply"))
        return;
    if (action != QLatin1String("default"))
        return;
    const QVariantMap payload = m_pendingPayloads.value(id);
    forgetPayload(id);
    if (payload.isEmpty())
        return;
    emitOpenFor(payload);
}

void NotificationManager::onNotificationReplied(quint32 id, const QString &text)
{
    const QVariantMap payload = m_pendingPayloads.value(id);
    forgetPayload(id);
    if (payload.isEmpty())
        return;
    // Ignore empty submissions.
    const QString body = text.trimmed();
    if (body.isEmpty())
        return;
    // sendThreadReply() needs a room id, not a composite.
    Q_EMIT replyRequested(
        payload.value(QStringLiteral("accountUserId")).toString(),
        routableRoomId(payload.value(QStringLiteral("roomId")).toString()),
        routableThreadRootId(
            payload.value(QStringLiteral("roomId")).toString(),
            payload.value(QStringLiteral("threadRootId")).toString()),
        body);
}

void NotificationManager::onNotificationClosed(quint32 id, quint32 reason)
{
    if (id != 0 && id == m_activeCallNotificationId) {
        // Dismissing the call card stops re-ringing but does not decline; other
        // devices keep ringing.
        m_activeCallNotificationId = 0;
        m_callRingTimer.stop();
    }
    // freedesktop reasons: 1 expired, 2 dismissed, 3 CloseNotification,
    // 4 undefined. Expired popups stay in KDE/GNOME history, so keep their
    // payload for closeRoomNotifications().
    constexpr quint32 kExpired = 1;
    if (reason == kExpired)
        return;
    forgetPayload(id);
}

QStringList NotificationManager::callActions(bool acceptOffered, bool rtcLane,
                                             bool silenceOffered)
{
    QStringList actions{ QStringLiteral("default"), tr("Open") };
    if (acceptOffered) {
        // The verb follows the lane, as in IncomingCallPrompt: an RTC call is
        // joined, a legacy 1:1 invite is answered.
        actions << QStringLiteral("accept")
                << (rtcLane ? tr("Join") : tr("Answer"));
    }
    // Silence before Decline.
    if (silenceOffered)
        actions << QStringLiteral("silence") << tr("Silence");
    // Decline last, where a mis-aimed click is least likely to hit it.
    actions << QStringLiteral("decline") << tr("Decline");
    return actions;
}

void NotificationManager::setCallAcceptOffered(const QString &callId,
                                               bool offered, bool rtcLane)
{
    if (callId.isEmpty() || m_activeCallId != callId)
        return;
    if (m_activeCallAcceptOffered == offered
        && m_activeCallRtcLane == rtcLane) {
        return;
    }
    m_activeCallAcceptOffered = offered;
    m_activeCallRtcLane = rtcLane;
    // Redraw only; the deadline and ring timer are untouched.
    deliverCallNotification();
}

void NotificationManager::showIncomingCall(const QString &roomId,
                                           const QString &callId,
                                           const QString &title,
                                           const QString &safeBody,
                                           bool sound, int ringSeconds,
                                           bool acceptOffered, bool rtcLane,
                                           bool silenceOffered)
{
    if (callId.isEmpty())
        return;
    // One ring at a time; a newer call replaces the previous card.
    if (!m_activeCallId.isEmpty() && m_activeCallId != callId)
        stopIncomingCall(m_activeCallId);
    m_activeCallId = callId;
    m_activeCallRoomId = roomId;
    m_activeCallTitle = title;
    m_activeCallBody = safeBody;
    m_activeCallSound = sound;
    m_activeCallAcceptOffered = acceptOffered;
    m_activeCallRtcLane = rtcLane;
    m_activeCallSilenceOffered = silenceOffered;
    m_callRingDeadlineMs = QDateTime::currentMSecsSinceEpoch()
        + qint64(qBound(5, ringSeconds, 300)) * 1000;
    // A new call gets its own balloon.
    m_activeCallTrayDelivered = false;
    deliverCallNotification();
    if (sound)
        m_callRingTimer.start();
}

void NotificationManager::silenceIncomingCall(const QString &callId)
{
    if (callId.isEmpty() || m_activeCallId != callId)
        return;
    if (!m_activeCallSound && !m_activeCallSilenceOffered)
        return;
    // The re-post is the ringing sound; stopping the timer stops it. The card
    // is still retired when the call ends.
    m_activeCallSound = false;
    m_activeCallSilenceOffered = false;
    m_callRingTimer.stop();
    // Redraw without the Silence button, but only if the card is still showing
    // (id 0 means it was closed). The tray balloon is latched per call.
    if (m_activeCallNotificationId != 0)
        deliverCallNotification();
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
    // Reset with the rest of the call state; these have caller-side defaults.
    m_activeCallAcceptOffered = false;
    m_activeCallRtcLane = false;
    m_activeCallSilenceOffered = false;
    m_activeCallTrayDelivered = false;
    m_activeCallId.clear();
    m_activeCallRoomId.clear();
    m_activeCallTitle.clear();
    m_activeCallBody.clear();
    m_activeCallSound = false;
}

void NotificationManager::deliverCallNotification()
{
    if (m_activeCallId.isEmpty())
        return;
#ifdef HAVE_QT_DBUS
    QDBusInterface notifications(kService, kPath, kInterface,
                                 QDBusConnection::sessionBus());
    if (!notifications.isValid()) {
        qCInfo(lcNotify) << "notification service unavailable";
        deliverCallThroughTray();
        return;
    }
    const QStringList actions = callActions(m_activeCallAcceptOffered,
                                            m_activeCallRtcLane,
                                            m_activeCallSilenceOffered);
    const NotificationIdentity identity = notificationIdentity();
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), identity.desktopEntry },
        { QStringLiteral("urgency"), quint8(2) }, // critical: a live ring
    };
    if (m_activeCallSound) {
        // Themed call sound. Not coalesced with message sounds: the repeat is
        // the ring.
        hints.insert(QStringLiteral("sound-name"),
                     QStringLiteral("phone-incoming-call"));
    }
    // Same single escape as deliverNow(); the body contains member-chosen
    // names.
    const QDBusReply<quint32> reply = notifications.call(
        QStringLiteral("Notify"), QStringLiteral("Lightning"),
        m_activeCallNotificationId, identity.appIcon, m_activeCallTitle,
        bodyForServer(notifications, m_activeCallBody), actions, hints,
        int(-1));
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
        return;
    }
    qCWarning(lcNotify) << "notification server refused the call card:"
                        << reply.error().name();
#endif
    // No freedesktop card (no daemon, or no QtDBus on Windows/macOS): announce
    // the call through the tray balloon. A balloon has no buttons, so the click
    // routes to the room, where IncomingCallPrompt offers Join/Answer/Decline.
    deliverCallThroughTray();
}

// One balloon per call, not per ring tick: a balloon cannot replace itself,
// so repeating it would raise a new toast every 5 s. Latched on the attempt:
// a missing tray icon here means notifications are off, not a transient
// failure worth retrying.
bool NotificationManager::deliverCallThroughTray()
{
    if (m_activeCallTrayDelivered)
        return false;
    m_activeCallTrayDelivered = true;
    ++m_callTrayAttempts;
    const QVariantMap payload{
        { QStringLiteral("roomId"), m_activeCallRoomId },
        { QStringLiteral("eventId"), QString() },
        { QStringLiteral("threadRootId"), QString() },
    };
    m_lastCallTrayPayload = payload;
    if (!deliverThroughTray(m_activeCallTitle, m_activeCallBody, payload,
                            QImage())) {
        qCInfo(lcNotify) << "incoming call could not be announced: no "
                            "notification service and no tray balloon";
        return false;
    }
    return true;
}
