#include "notifications/NotificationManager.h"

#include "app/TrayIcon.h"

#include "storage/AppDataPaths.h"
#include "notifications/FallbackAvatar.h"

#include "matrix/EventPreview.h"
#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"
#include "models/UserLookup.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPixmap>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QImage>

#include <algorithm>
#include <utility>

#ifdef HAVE_QT_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
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

Q_LOGGING_CATEGORY(lcNotify, "matrix.notify")

namespace {
const auto kService = QStringLiteral("org.freedesktop.Notifications");
const auto kPath = QStringLiteral("/org/freedesktop/Notifications");
const auto kInterface = QStringLiteral("org.freedesktop.Notifications");
constexpr qint64 kSoundCoalesceMs = 1500;
constexpr int kAvatarWaitMs = 1200;
constexpr int kFallbackAvatarEdge = 64;
constexpr int kMaxLiveUndecryptable = 256;
constexpr qint64 kUndecryptableMemoryMs = 10 * 60 * 1000;
constexpr int kMaxActivationTokens = 16;
constexpr quint32 kClosedExpired = 1;
constexpr quint32 kClosedDismissed = 2;
const QChar kKeySeparator(0x1f);

// First-strong isolation around an untrusted name spliced into a sentence:
// a display name with an RLO, or in another script than the sentence, cannot
// reorder the words around it. PDI also closes any override left open inside.
QString isolate(const QString &text)
{
    return QChar(0x2068) + text + QChar(0x2069);
}

QString messageGroupKey(const QString &roomId, const QString &threadRootId)
{
    return QStringLiteral("m") + kKeySeparator + roomId + kKeySeparator
        + threadRootId;
}

QString genericGroupKey(const QString &key)
{
    return QStringLiteral("g") + kKeySeparator + key;
}

QString decodeHtmlEntities(const QString &text)
{
    static const QRegularExpression entity(
        QStringLiteral("&(#[xX][0-9a-fA-F]{1,6}|#[0-9]{1,7}|[a-zA-Z]{2,8});"));
    QString out;
    out.reserve(text.size());
    qsizetype last = 0;
    auto it = entity.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += QStringView(text).mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString name = match.captured(1);
        if (name.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const uint code = name.startsWith(QLatin1String("#x"),
                                              Qt::CaseInsensitive)
                ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
            if (ok && code > 0 && code <= 0x10FFFF
                && !(code >= 0xD800 && code <= 0xDFFF)) {
                const char32_t c = code;
                out += QString::fromUcs4(&c, 1);
            }
            continue;
        }
        const QString lower = name.toLower();
        if (lower == QLatin1String("amp"))
            out += QLatin1Char('&');
        else if (lower == QLatin1String("lt"))
            out += QLatin1Char('<');
        else if (lower == QLatin1String("gt"))
            out += QLatin1Char('>');
        else if (lower == QLatin1String("quot"))
            out += QLatin1Char('"');
        else if (lower == QLatin1String("apos"))
            out += QLatin1Char('\'');
        else if (lower == QLatin1String("nbsp"))
            out += QLatin1Char(' ');
        else
            out += match.captured(0);
    }
    out += QStringView(text).mid(last);
    return out;
}

QString spoilerSafeText(const QString &html, const QString &placeholder)
{
    const QString body = matrix::preview::stripReplyFallback(html);
    static const QRegularExpression tag(
        QStringLiteral("<(/?)([a-zA-Z][a-zA-Z0-9-]*)([^>]*)>"));
    static const QStringList breaking{
        QStringLiteral("br"), QStringLiteral("p"), QStringLiteral("div"),
        QStringLiteral("li"), QStringLiteral("blockquote"),
        QStringLiteral("tr"), QStringLiteral("h1"), QStringLiteral("h2"),
        QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
        QStringLiteral("h6"), QStringLiteral("pre")
    };
    QString out;
    qsizetype last = 0;
    int spoilerDepth = 0;
    auto it = tag.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (spoilerDepth == 0)
            out += QStringView(body).mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const bool closing = !match.captured(1).isEmpty();
        const QString name = match.captured(2).toLower();
        const QString attributes = match.captured(3);
        if (spoilerDepth > 0) {
            if (name == QLatin1String("span")) {
                if (closing)
                    --spoilerDepth;
                else if (!attributes.trimmed().endsWith(QLatin1Char('/')))
                    ++spoilerDepth;
            }
            continue;
        }
        if (!closing && name == QLatin1String("span")
            && attributes.contains(QLatin1String("data-mx-spoiler"),
                                   Qt::CaseInsensitive)) {
            out += QLatin1Char(' ') + placeholder + QLatin1Char(' ');
            spoilerDepth = 1;
            continue;
        }
        if (breaking.contains(name))
            out += QLatin1Char('\n');
    }
    if (spoilerDepth == 0)
        out += QStringView(body).mid(last);
    return decodeHtmlEntities(out);
}

QString messageText(const TimelineEvent &event)
{
    const bool spoiler = event.formattedBody.contains(
        QLatin1String("data-mx-spoiler"), Qt::CaseInsensitive);
    QString text;
    if (spoiler) {
        text = spoilerSafeText(
            event.formattedBody,
            QCoreApplication::translate("Notifications", "(spoiler)"));
    } else if (event.isEdit
               && (event.editNewBodyAvailable || !event.editNewBody.isEmpty())) {
        return event.editNewBody;
    } else {
        text = event.body;
    }
    if (event.isEdit) {
        const QString trimmed = text.trimmed();
        if (trimmed.startsWith(QLatin1String("* ")))
            text = trimmed.mid(2);
    }
    return text;
}

bool passesContextGates(const TimelineEvent &event,
                        const NotificationManager::Context &context)
{
    if (!context.notificationsEnabled || !context.initialSyncComplete)
        return false;
    if (event.backlog || event.redacted)
        return false;
    if (context.roomMode == NotificationManager::Muted
        || context.senderIsIgnored)
        return false;
    if (event.isVirtual() || event.type == TimelineEvent::StateChange
        || event.type == TimelineEvent::CallEvent)
        return false;
    if (event.isLocalEcho || event.status != TimelineEvent::Sent)
        return false;
    if (!context.selfUserId.isEmpty() && event.sender == context.selfUserId)
        return false;
    if (context.roomHydrating || context.roomVisibleAtLatest)
        return false;
    return true;
}

#ifdef HAVE_QT_DBUS
// Every call to the daemon is asynchronous. A daemon that owns the name but
// never answers held the GUI thread for Qt's 25 s default per call: 75 s for
// one message (introspection, GetCapabilities, Notify; measured).
constexpr int kCapabilitiesTimeoutMs = 3000;
constexpr int kNotifyTimeoutMs = 10000;
constexpr int kPropertyTimeoutMs = 3000;
constexpr qint64 kInhibitionRefreshMs = 30000;
constexpr int kMaxCapabilityRequests = 4;
constexpr int kMaxCapabilityWaits = 32;

QDBusMessage notificationsCall(const QString &method)
{
    return QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
}

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

QVariant unwrapVariant(QVariant value)
{
    while (value.metaType() == QMetaType::fromType<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}

QDBusPendingCallWatcher *readPortalSetting(QObject *parent,
                                           const QString &method)
{
    QDBusMessage read = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"), method);
    read << QStringLiteral("org.gnome.desktop.notifications")
         << QStringLiteral("show-banners");
    return new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(read, kPropertyTimeoutMs),
        parent);
}
#endif
}

#ifdef HAVE_QT_DBUS
QString materializedIconPath()
{
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

NotificationManager::DesktopIdentity notificationIdentity()
{
    static const NotificationManager::DesktopIdentity identity = [] {
        NotificationManager::DesktopIdentity result =
            NotificationManager::desktopIdentityFor(
                qEnvironmentVariable("FLATPAK_ID"),
                qEnvironmentVariable("SNAP"),
                qEnvironmentVariable("SNAP_NAME"));
        if (!result.appIcon.isEmpty())
            return result;
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

// The fixed name "lightning" resolves only for installed packages. Inside a
// Flatpak the exported entry and icon are renamed to FLATPAK_ID. Inside a
// snap the host knows the entry as `<snap>_lightning` (snapd prefixes
// meta/gui/lightning.desktop with the snap name) and cannot resolve a themed
// name looked up inside the snap, so the icon is the file the snap ships in
// meta/gui (build-snap.sh), a path the host can read.
NotificationManager::DesktopIdentity NotificationManager::desktopIdentityFor(
    const QString &flatpakId, const QString &snapDir, const QString &snapName)
{
    DesktopIdentity identity;
    if (!flatpakId.isEmpty()) {
        identity.desktopEntry = flatpakId;
        return identity;
    }
    if (!snapDir.isEmpty() && !snapName.isEmpty()) {
        identity.desktopEntry = snapName + QStringLiteral("_lightning");
        identity.appIcon = QUrl::fromLocalFile(
            snapDir + QStringLiteral("/meta/gui/lightning.png")).toString();
        return identity;
    }
    identity.desktopEntry = QStringLiteral("lightning");
    return identity;
}

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_tables.insert(QString(), AccountTable{});
#ifdef HAVE_QT_DBUS
    qDBusRegisterMetaType<FreedesktopNotificationImage>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kInterface, QStringLiteral("ActionInvoked"),
                this, SLOT(onActionInvoked(quint32,QString)));
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("NotificationReplied"),
                this, SLOT(onNotificationReplied(quint32,QString)));
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(quint32,quint32)));
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("ActivationToken"), this,
                SLOT(onActivationToken(quint32,QString)));
    bus.connect(kService, kPath,
                QStringLiteral("org.freedesktop.DBus.Properties"),
                QStringLiteral("PropertiesChanged"), this,
                SLOT(onDaemonPropertiesChanged(QString,QVariantMap,QStringList)));
    // A restarted daemon hands out ids from scratch: ours would close and
    // route other applications' cards.
    if (bus.isConnected()) {
        auto *watcher = new QDBusServiceWatcher(
            kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                &NotificationManager::onDaemonOwnerChanged);
    }
#endif
    m_avatarWaitTimer.setSingleShot(true);
    m_avatarWaitTimer.setInterval(kAvatarWaitMs);
    connect(&m_avatarWaitTimer, &QTimer::timeout, this,
            [this] { flushAvatarWaits(true); });
    m_utdHoldTimer.setSingleShot(true);
    connect(&m_utdHoldTimer, &QTimer::timeout, this,
            &NotificationManager::flushHeldUndecryptable);
    m_callRingTimer.setInterval(5000);
    connect(&m_callRingTimer, &QTimer::timeout, this,
            &NotificationManager::onCallRingTick);
    m_balloonHold.setSingleShot(true);
    connect(&m_balloonHold, &QTimer::timeout, this,
            &NotificationManager::onBalloonHoldExpired);
}

void NotificationManager::onCallRingTick()
{
    if (nowMs() >= m_callRingDeadlineMs) {
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
    flushAvatarWaits(false);
}

NotificationManager::Decision
NotificationManager::decide(const TimelineEvent &event, const Context &context)
{
    Decision decision;
    if (!context.notificationsEnabled)
        return decision;
    if (!context.initialSyncComplete)
        return decision;
    if (event.backlog)
        return decision;
    if (event.redacted)
        return decision;
    if (context.roomMode == Muted)
        return decision;
    if (context.senderIsIgnored)
        return decision;
    if (event.isVirtual() || event.type == TimelineEvent::StateChange
        || event.type == TimelineEvent::CallEvent)
        return decision;
    if (event.isLocalEcho || event.status != TimelineEvent::Sent)
        return decision;
    if (!context.selfUserId.isEmpty() && event.sender == context.selfUserId)
        return decision;
    const bool rulesKnown =
        event.pushVerdict != TimelineEvent::PushVerdict::Unknown
        && !context.localModeUnsynced;
    const bool mention = rulesKnown
        ? event.pushHighlight
        : (event.mentionsMe || event.pushHighlight);
    if (rulesKnown) {
        if (event.pushVerdict == TimelineEvent::PushVerdict::Quiet)
            return decision;
    } else {
        if (event.isEdit && !event.mentionsMe)
            return decision;
        if (event.type == TimelineEvent::Notice)
            return decision;
        if (context.roomMode == MentionsOnly && !mention)
            return decision;
    }
    if (context.roomVisibleAtLatest)
        return decision;
    if (context.roomHydrating)
        return decision;

    const QString sender = isolate(senderLabel(event, context));
    const QString room = isolate(context.roomName.isEmpty()
        ? QCoreApplication::translate("Notifications", "Matrix room")
        : context.roomName);

    decision.notify = true;
    decision.mention = mention;
    switch (context.previewMode) {
    case SenderAndMessage: {
        decision.title = context.roomIsDirect
            ? sender
            : QCoreApplication::translate("Notifications", "%1 in %2")
                  .arg(sender, room);
        if (event.undecryptable) {
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
            decision.body = matrix::preview::oneLineSummary(event).left(160);
        } else {
            const QString text =
                matrix::preview::normalizePreviewText(messageText(event))
                    .left(160);
            decision.body = event.type == TimelineEvent::Emote
                ? QCoreApplication::translate("Notifications", "* %1 %2")
                      .arg(sender, text)
                : text;
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

    switch (context.soundMode) {
    case SoundOff:
        decision.playSound = false;
        break;
    case SoundMentionsAndDirect:
        decision.playSound = rulesKnown
            ? event.pushSound
            : (mention || context.roomIsDirect);
        break;
    case SoundAll:
        decision.playSound = true;
        break;
    }
    return decision;
}

QString NotificationManager::senderLabel(const TimelineEvent &event,
                                        const Context &context)
{
    const auto named = [&event](const QString &name) {
        const QString trimmed = name.trimmed();
        return !trimmed.isEmpty() && trimmed != event.sender;
    };
    if (named(event.senderDisplayName))
        return event.senderDisplayName;
    if (named(context.senderMemberName))
        return context.senderMemberName;
    if (named(context.senderProfileName))
        return context.senderProfileName;
    return matrix::user_lookup::localpartOrUserId(event.sender);
}

bool NotificationManager::shouldNotifyInvite(bool initialSyncComplete,
                                             bool alreadyKnown,
                                             bool notificationsEnabled)
{
    return notificationsEnabled && initialSyncComplete && !alreadyKnown;
}

QString NotificationManager::routableRoomId(const QString &roomId)
{
    return MatrixClient::isThreadTimelineId(roomId)
        ? MatrixClient::threadTimelineRoomId(roomId)
        : roomId;
}

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
    if (event.isEdit)
        refreshFromEdit(event, context);
    const Decision decision = decide(event, context);
    if (event.undecryptable && !event.eventId.isEmpty()
        && passesContextGates(event, context)) {
        auto live = m_liveUtds.find(event.eventId);
        if (live == m_liveUtds.end()) {
            m_liveUtds.insert(event.eventId,
                              LiveUtd{ UtdState::Quiet, nowMs() });
            m_liveUtdOrder.append(event.eventId);
            while (m_liveUtdOrder.size() > kMaxLiveUndecryptable)
                m_liveUtds.remove(m_liveUtdOrder.takeFirst());
            live = m_liveUtds.find(event.eventId);
        } else if (live->state != UtdState::Quiet) {
            return;
        }
        if (!decision.notify || live == m_liveUtds.end())
            return;
        if (m_utdHoldMs > 0) {
            live->state = UtdState::Held;
            m_heldUtds.append(
                HeldUtd{ std::make_shared<const TimelineEvent>(event), context,
                         nowMs() + m_utdHoldMs });
            if (!m_utdHoldTimer.isActive())
                m_utdHoldTimer.start(m_utdHoldMs);
            return;
        }
        live->state = UtdState::Delivered;
    }
    if (!decision.notify)
        return;
    deliverDecision(event, context, decision, event.undecryptable,
false);
}

void NotificationManager::flushHeldUndecryptable()
{
    const qint64 now = nowMs();
    QList<HeldUtd> due;
    for (auto it = m_heldUtds.begin(); it != m_heldUtds.end();) {
        if (it->dueMs <= now) {
            due.append(*it);
            it = m_heldUtds.erase(it);
        } else {
            ++it;
        }
    }
    for (const HeldUtd &held : std::as_const(due)) {
        auto live = m_liveUtds.find(held.event->eventId);
        if (live != m_liveUtds.end())
            live->state = UtdState::Delivered;
        const Decision decision = decide(*held.event, held.context);
        if (decision.notify)
            deliverDecision(*held.event, held.context, decision,
true,false);
    }
    if (!m_heldUtds.isEmpty()) {
        qint64 next = m_heldUtds.first().dueMs;
        for (const HeldUtd &held : std::as_const(m_heldUtds))
            next = std::min(next, held.dueMs);
        m_utdHoldTimer.start(int(std::max<qint64>(0, next - nowMs())));
    }
}

void NotificationManager::processDecrypted(const TimelineEvent &event,
                                           const Context &context)
{
    if (event.eventId.isEmpty() || event.undecryptable)
        return;
    if (event.isEdit)
        refreshFromEdit(event, context);
    const auto live = m_liveUtds.find(event.eventId);
    if (live == m_liveUtds.end() || live->state == UtdState::Decided)
        return;
    const LiveUtd seen = live.value();
    live->state = UtdState::Decided;
    if (nowMs() - seen.seenMs > kUndecryptableMemoryMs) {
        qCInfo(lcNotify) << "a message decrypted too late to notify";
        return;
    }
    const Decision decision = decide(event, context);
    if (seen.state == UtdState::Held) {
        m_heldUtds.removeIf([&event](const HeldUtd &held) {
            return held.event->eventId == event.eventId;
        });
        if (m_heldUtds.isEmpty())
            m_utdHoldTimer.stop();
        if (decision.notify)
            deliverDecision(event, context, decision, false, false);
        return;
    }
    if (seen.state == UtdState::Quiet) {
        if (decision.notify)
            deliverDecision(event, context, decision, false, false);
        return;
    }
    const QString accountKey = context.accountUserId;
    const auto sameEvent = [&event, &accountKey](const Card &card) {
        return card.accountKey == accountKey
            && card.payload.value(QStringLiteral("eventId")).toString()
                == event.eventId;
    };
    qsizetype unshown = m_avatarWaits.removeIf(sameEvent)
        + m_capabilityWaits.removeIf(sameEvent)
        + m_hostCardQueue.removeIf([&sameEvent](const DeferredHostCard &w) {
              return sameEvent(w.card);
          });
    const QString groupKey = messageGroupKey(
        routableRoomId(event.roomId),
        routableThreadRootId(event.roomId, event.threadRootId));
    {
        AccountTable &own = table(accountKey);
        const auto waitingGroup = own.groups.find(groupKey);
        if (waitingGroup != own.groups.end() && waitingGroup->next
            && sameEvent(*waitingGroup->next)) {
            waitingGroup->next.reset();
            ++unshown;
        }
    }
    if (unshown > 0) {
        if (m_avatarWaits.isEmpty())
            m_avatarWaitTimer.stop();
        {
            AccountTable &own = table(accountKey);
            const auto counted = own.groups.find(groupKey);
            if (counted != own.groups.end())
                counted->count = std::max(0, counted->count - int(unshown));
        }
        if (decision.notify)
            deliverDecision(event, context, decision, false, false);
        return;
    }
    const AccountTable &own = table(accountKey);
    quint32 cardId = 0;
    for (auto it = own.pendingPayloads.cbegin();
         it != own.pendingPayloads.cend(); ++it) {
        if (it.value().value(QStringLiteral("eventId")).toString()
                == event.eventId
            && it.value().value(QStringLiteral("utd")).toBool()) {
            cardId = it.key();
            break;
        }
    }
    if (cardId == 0)
        return;
    const QString group = own.pendingPayloads.value(cardId)
                              .value(QStringLiteral("group")).toString();
    const auto groupIt = own.groups.constFind(group);
    if (groupIt != own.groups.cend()
        && (groupIt->inFlight || groupIt->id != cardId))
        return;
    if (!decision.notify) {
        closeNotification(cardId);
        forgetPayload(cardId);
        return;
    }
    deliverDecision(event, context, decision, false,true);
}

namespace {
QString bodyForCount(const NotificationManager::Decision &decision,
                     const TimelineEvent &event,
                     const NotificationManager::Context &context, int count)
{
    if (count <= 1)
        return decision.body;
    if (context.previewMode == NotificationManager::SenderOnly
        && !event.mentionsMe) {
        return QCoreApplication::translate("Notifications", "%n new messages",
                                           nullptr, count);
    }
    if (context.previewMode == NotificationManager::Private) {
        return QCoreApplication::translate(
            "Notifications", "%n new Matrix notifications", nullptr, count);
    }
    return decision.body;
}
}

void NotificationManager::processUpdated(const TimelineEvent &event,
                                         const Context &context)
{
    if (event.eventId.isEmpty() || event.isEdit || event.undecryptable)
        return;
    refreshCard(event, context);
}

bool NotificationManager::refreshFromEdit(const TimelineEvent &edit,
                                          const Context &context)
{
    if (!edit.isEdit || edit.replacesEventId.isEmpty() || edit.sender.isEmpty()
        || edit.undecryptable || edit.redacted || !edit.editNewBodyAvailable)
        return false;
    const QString roomId = routableRoomId(edit.roomId);
    AccountTable &own = table(context.accountUserId);
    auto group = own.groups.begin();
    for (; group != own.groups.end(); ++group) {
        if (group->payload.value(QStringLiteral("eventId")).toString()
                == edit.replacesEventId
            && group->payload.value(QStringLiteral("roomId")).toString()
                == roomId)
            break;
    }
    if (group == own.groups.end() || group->withdrawn
        || group->sender != edit.sender)
        return false;
    const qint64 stamp =
        edit.timestamp.isValid() ? edit.timestamp.toMSecsSinceEpoch() : 0;
    if (stamp < group->editStampMs)
        return true;
    group->editStampMs = stamp;
    TimelineEvent current = edit;
    current.eventId = edit.replacesEventId;
    current.threadRootId =
        group->payload.value(QStringLiteral("threadRootId")).toString();
    current.isEdit = false;
    current.replacesEventId.clear();
    current.editNewBody.clear();
    current.editNewBodyAvailable = false;
    current.body = messageText(edit);
    current.formattedBody.clear();
    current.edited = true;
    current.pushVerdict = TimelineEvent::PushVerdict::Unknown;
    current.pushHighlight = false;
    current.mentionsMe = group->payload.value(QStringLiteral("mention")).toBool();
    current.mentionsRoom = false;
    refreshCard(current, context);
    return true;
}

void NotificationManager::refreshCard(const TimelineEvent &event,
                                      const Context &context)
{
    const QString accountKey = context.accountUserId;
    const QString groupKey = messageGroupKey(
        routableRoomId(event.roomId),
        routableThreadRootId(event.roomId, event.threadRootId));
    auto group = table(accountKey).groups.find(groupKey);
    if (group == table(accountKey).groups.end()
        || group->withdrawn
        || group->payload.value(QStringLiteral("eventId")).toString() != event.eventId)
        return;
    if (group->payload.value(QStringLiteral("utd")).toBool())
        return;
    TimelineEvent current = event;
    if (current.pushVerdict == TimelineEvent::PushVerdict::Unknown) {
        current.pushVerdict = TimelineEvent::PushVerdict::Notify;
        current.pushHighlight =
            group->payload.value(QStringLiteral("mention")).toBool();
    }
    current.backlog = false;
    Decision decision = decide(current, context);
    if (!decision.notify) {
        const QString owner = accountKey.isEmpty() ? m_accountUserId : accountKey;
        withdrawEverywhere([&](const QVariantMap &payload) {
            return payload.value(QStringLiteral("eventId")).toString() == event.eventId
                && payload.value(QStringLiteral("accountUserId")).toString() == owner;
        });
        return;
    }
    decision.body = bodyForCount(decision, current, context, group->count);
    if (group->title == decision.title && group->body == decision.body)
        return;
    group->title = decision.title;
    group->body = decision.body;
    const QString shownTitle = m_liveAccountsMode
        ? labelledTitleForRefresh(event, context, decision.title)
        : decision.title;
    const bool privateMode = context.previewMode == Private;
    const auto matches = [&](const Card &card) {
        return card.accountKey == accountKey
            && card.payload.value(QStringLiteral("eventId")).toString() == event.eventId;
    };
    const auto refresh = [&](Card &card, bool avatarResolved) {
        card.title = shownTitle;
        card.body = decision.body;
        if (privateMode) {
            card.avatarMxc.clear();
            card.fallback = QImage();
            card.avatar = QImage();
            return;
        }
        card.fallback = lightning::notifications::fallbackAvatar(
            context.roomName, context.avatarColorKey, kFallbackAvatarEdge,
            context.themeId);
        if (!avatarResolved)
            card.avatarMxc = context.avatarMxc;
        else if (card.avatar.isNull())
            card.avatar = card.fallback;
    };
    bool waiting = false;
    for (auto &card : m_avatarWaits) {
        if (matches(card)) {
            refresh(card,false);
            waiting = true;
        }
    }
    for (auto &card : m_capabilityWaits) {
        if (matches(card)) {
            refresh(card,true);
            waiting = true;
        }
    }
    if (group->next && matches(*group->next)) {
        refresh(*group->next,true);
        waiting = true;
    }
    for (DeferredHostCard &deferred : m_hostCardQueue) {
        if (matches(deferred.card)) {
            refresh(deferred.card,true);
            waiting = true;
        }
    }
    for (auto &balloon : m_balloonQueue) {
        if (balloon.accountKey == accountKey
            && balloon.payload.value(QStringLiteral("eventId")).toString() == event.eventId) {
            balloon.title = shownTitle;
            balloon.body = decision.body;
            if (privateMode)
                balloon.avatar = QImage();
            waiting = true;
        }
    }
    bool balloonShowsIt = false;
    for (auto &entry : m_shownEntries) {
        if (entry.accountKey == accountKey
            && entry.payload.value(QStringLiteral("eventId")).toString() == event.eventId) {
            entry.title = shownTitle;
            entry.body = decision.body;
            if (privateMode)
                entry.avatar = QImage();
            balloonShowsIt = m_shownEntries.size() == 1;
            waiting = true;
        }
    }
    if (balloonShowsIt && balloonGate() && m_balloonHold.isActive()
        && !m_balloonIsCall) {
        Balloon entry = m_shownEntries.first();
        if (deliverThroughTray(entry.title, entry.body, entry.payload,
                               entry.avatar,false,
                               entry.accountKey)) {
            qCInfo(lcNotify) << "tray balloon shown again with an edit";
            m_shownEntries = { std::move(entry) };
        }
    }
    if (!waiting && (group->id != 0 || group->inFlight))
        deliverDecision(event, context, decision, false,true);
}

void NotificationManager::deliverDecision(const TimelineEvent &event,
                                          const Context &context,
                                          const Decision &decision,
                                          bool undecryptable, bool replacement)
{
    const QString roomId = routableRoomId(event.roomId);
    const QString threadRootId =
        routableThreadRootId(event.roomId, event.threadRootId);
    const QString accountKey = context.accountUserId;
    const bool hostCard = m_liveAccountsMode && !accountKey.isEmpty();
    if (m_liveAccountsMode && !replacement
        && mergeIntoEventCard(event, context, roomId, decision))
        return;
    if (!replacement) {
        if (hostCard) {
            m_hostRoomsAwaitingUnread.insert(
                accountKey + kKeySeparator + roomId, nowMs() + m_awaitUnreadMs);
        } else {
            m_roomsAwaitingUnread.insert(roomId, nowMs() + m_awaitUnreadMs);
        }
    }
    const QString groupKey = messageGroupKey(roomId, threadRootId);
    int count = 0;
    bool groupWasEmpty = false;
    {
        CardGroup &group = table(accountKey).groups[groupKey];
        groupWasEmpty = group.count == 0;
        if (!replacement)
            ++group.count;
        count = group.count;
    }
    QString body = bodyForCount(decision, event, context, count);
    if (hostCard && count > 1) {
        body = context.previewMode == Private
            ? QCoreApplication::translate(
                  "Notifications", "%n new Matrix notifications", nullptr, count)
            : QCoreApplication::translate("Notifications", "%n new messages",
                                          nullptr, count);
    }
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), event.eventId);
    payload.insert(QStringLiteral("threadRootId"), threadRootId);
    payload.insert(QStringLiteral("accountUserId"),
                   accountKey.isEmpty() ? m_accountUserId : accountKey);
    payload.insert(QStringLiteral("mention"), decision.mention);
    payload.insert(QStringLiteral("group"), groupKey);
    if (undecryptable)
        payload.insert(QStringLiteral("utd"), true);
    QString title = decision.title;
    QString eventKey;
    QString label;
    if (m_liveAccountsMode) {
        label = accountLabelOr(context.accountLabel, accountOfKey(accountKey));
        QStringList labels{ label };
        if (!event.eventId.isEmpty()) {
            eventKey = roomId + kKeySeparator + event.eventId;
            const auto record = m_eventCards.constFind(eventKey);
            if (record != m_eventCards.cend() && record->ownerKey == accountKey) {
                labels = record->labels;
                if (record->accounts.size() > 1) {
                    payload.insert(QStringLiteral("accountUserIds"),
                                   record->accounts);
                }
            }
        }
        title += accountSuffix(labels);
    }
    auto &currentGroup = table(accountKey).groups[groupKey];
    currentGroup.payload = payload;
    currentGroup.title = decision.title;
    currentGroup.body = body;
    currentGroup.sender = event.sender;
    if (!replacement)
        currentGroup.editStampMs = 0;

    Card card;
    card.title = title;
    card.body = body;
    card.payload = payload;
    card.sound = decision.playSound && !replacement;
    card.replacement = replacement;
    card.accountKey = accountKey;
    if (context.previewMode != Private) {
        card.fallback = lightning::notifications::fallbackAvatar(
            context.roomName, context.avatarColorKey, kFallbackAvatarEdge,
            context.themeId);
        card.avatarMxc = context.avatarMxc;
    }
    if (!eventKey.isEmpty())
        recordEventCard(eventKey, accountKey, groupKey, label, decision.title,
                        card);
    if (hostCard && !replacement) {
        offerHostCard(std::move(card), groupKey, groupWasEmpty);
        return;
    }
    deliver(std::move(card));
}

void NotificationManager::showGeneric(const QString &title,
                                      const QString &safeBody,
                                      const QString &roomId,
                                      const QString &avatarMxc, bool sound,
                                      const QString &key,
                                      const QString &accountUserId)
{
    ++m_genericNoticeCount;
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), QString{});
    payload.insert(QStringLiteral("threadRootId"), QString{});
    payload.insert(QStringLiteral("accountUserId"),
                   accountUserId.isEmpty() ? m_accountUserId : accountUserId);
    if (!key.isEmpty()) {
        const QString groupKey = genericGroupKey(key);
        payload.insert(QStringLiteral("group"), groupKey);
        table(QString()).groups[groupKey].payload = payload;
    }
    Card card;
    card.title = title;
    card.body = safeBody;
    card.payload = payload;
    card.sound = sound;
    card.avatarMxc = avatarMxc;
    deliver(std::move(card));
}

void NotificationManager::withdrawGeneric(const QString &key)
{
    if (key.isEmpty())
        return;
    const QString groupKey = genericGroupKey(key);
    withdrawWhere(QString(), [&groupKey](const QVariantMap &payload) {
        return payload.value(QStringLiteral("group")).toString() == groupKey;
    });
}

void NotificationManager::withdrawEvent(const QString &eventId)
{
    if (eventId.isEmpty())
        return;
    m_heldUtds.removeIf([&eventId](const HeldUtd &held) {
        return held.event->eventId == eventId;
    });
    if (m_heldUtds.isEmpty())
        m_utdHoldTimer.stop();
    if (m_liveUtds.remove(eventId) > 0)
        m_liveUtdOrder.removeOne(eventId);
    const int withdrawn =
        withdrawEverywhere([&eventId](const QVariantMap &payload) {
            return payload.value(QStringLiteral("eventId")).toString()
                == eventId;
        });
    if (withdrawn > 0)
        qCInfo(lcNotify) << "withdrew a notification for a removed message";
}

void NotificationManager::clearPendingFor(const QString &accountUserId)
{
    const bool foreground = accountUserId.isEmpty();
    forgetLiveAccountsStateFor(accountUserId);
    if (foreground)
        stopIncomingCall(QString());
    AccountTable &own = table(accountUserId);
    const QList<quint32> shown = own.payloadOrder;
    for (const quint32 id : shown) {
        closeNotification(id);
        tombstone(id, own.pendingPayloads.value(id)
                          .value(QStringLiteral("accountUserId")).toString());
    }
    if (!shown.isEmpty())
        qCInfo(lcNotify) << "withdrew" << shown.size()
                         << "notification(s) of the previous session";
    own.pendingPayloads.clear();
    own.payloadOrder.clear();
    own.groups.clear();
    ++own.deliveryGeneration;
    const auto ownCard = [&accountUserId](const Card &card) {
        return card.accountKey == accountUserId;
    };
    m_avatarWaits.removeIf(ownCard);
    if (m_avatarWaits.isEmpty())
        m_avatarWaitTimer.stop();
    if (foreground) {
        m_activationTokens.clear();
        m_capabilityWaits.clear();
        m_heldUtds.clear();
        m_utdHoldTimer.stop();
        m_liveUtds.clear();
        m_liveUtdOrder.clear();
        m_roomsAwaitingUnread.clear();
        m_balloonQueue.clear();
        m_balloonRooms.clear();
        retireBalloonOnScreen();
        return;
    }
    for (const quint32 id : shown)
        m_activationTokens.remove(id);
    m_capabilityWaits.removeIf(ownCard);
    for (auto it = m_heldUtds.begin(); it != m_heldUtds.end();) {
        if (it->context.accountUserId == accountUserId) {
            const QString eventId = it->event->eventId;
            if (m_liveUtds.remove(eventId) > 0)
                m_liveUtdOrder.removeOne(eventId);
            it = m_heldUtds.erase(it);
        } else {
            ++it;
        }
    }
    if (m_heldUtds.isEmpty())
        m_utdHoldTimer.stop();
    const auto ownBalloon = [&accountUserId](const Balloon &balloon) {
        return balloon.accountKey == accountUserId;
    };
    m_balloonQueue.removeIf(ownBalloon);
    m_shownEntries.removeIf(ownBalloon);
    if (m_lastFallbackAccountKey == accountUserId
        && !m_lastFallbackPayload.isEmpty()) {
        const QList<Balloon> rest = std::exchange(m_shownEntries, {});
        retireBalloonOnScreen();
        for (Balloon entry : rest) {
            entry.platformSound = false;
            enqueueBalloon(std::move(entry),true);
        }
        showPendingBalloons();
    }
}

void NotificationManager::observeRoomUnread(const QString &roomId, bool unread)
{
    if (roomId.isEmpty())
        return;
    if (unread) {
        m_roomsAwaitingUnread.remove(roomId);
        return;
    }
    const auto awaiting = m_roomsAwaitingUnread.constFind(roomId);
    if (awaiting != m_roomsAwaitingUnread.cend() && nowMs() < awaiting.value())
        return;
    m_roomsAwaitingUnread.remove(roomId);
    const int withdrawn = withdrawWhere(
        QString(), [&roomId](const QVariantMap &payload) {
            return payload.value(QStringLiteral("roomId")).toString() == roomId
                && !payload.value(QStringLiteral("eventId")).toString()
                        .isEmpty()
                && payload.value(QStringLiteral("threadRootId")).toString()
                        .isEmpty();
        });
    if (withdrawn > 0)
        qCInfo(lcNotify) << "withdrew" << withdrawn
                         << "notification(s) for a room that has been read";
}

namespace {
constexpr int kMaxEventCards = 256;
constexpr int kMaxAccountRecency = 32;
}

void NotificationManager::setLiveAccountsMode(bool on)
{
    if (on == m_liveAccountsMode)
        return;
    m_liveAccountsMode = on;
    if (on) {
        if (!m_hostCardTimer) {
            m_hostCardTimer = std::make_unique<QTimer>();
            m_hostCardTimer->setSingleShot(true);
            connect(m_hostCardTimer.get(), &QTimer::timeout, this,
                    &NotificationManager::flushHostCards);
        }
        noteAccountUsed(m_accountUserId);
        qCInfo(lcNotify) << "live-accounts notifications on";
        return;
    }
    if (!m_hostCardQueue.isEmpty()) {
        qCInfo(lcNotify) << "dropped" << m_hostCardQueue.size()
                         << "background-account notification(s) still "
                            "waiting: live accounts turned off";
    }
    m_hostCardQueue.clear();
    if (m_hostCardTimer)
        m_hostCardTimer->stop();
    m_lastHostNewCardMs = -1;
    m_eventCards.clear();
    m_eventCardOrder.clear();
    m_accountRecency.clear();
    m_hostRoomsAwaitingUnread.clear();
    m_cardGeneration.clear();
    m_hostRoomsCheckedAt.clear();
    m_accountLabelsShown = false;
    qCInfo(lcNotify) << "live-accounts notifications off";
}

void NotificationManager::setAccountLabelsShown(bool on)
{
    m_accountLabelsShown = on;
}

QString NotificationManager::accountLabelOr(const QString &label,
                                            const QString &userId)
{
    const QString trimmed = label.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;
    if (userId.startsWith(QLatin1Char('@'))) {
        const qsizetype colon = userId.indexOf(QLatin1Char(':'));
        const QString local =
            colon > 1 ? userId.mid(1, colon - 1) : userId.mid(1);
        if (!local.isEmpty())
            return local;
    }
    return userId;
}

QString NotificationManager::labelledTitleForRefresh(const TimelineEvent &event,
                                                     const Context &context,
                                                     const QString &baseTitle)
{
    const QString accountKey = context.accountUserId;
    QStringList labels{ accountLabelOr(context.accountLabel, accountOfKey(accountKey)) };
    if (!event.eventId.isEmpty()) {
        const QString eventKey =
            routableRoomId(event.roomId) + kKeySeparator + event.eventId;
        const auto record = m_eventCards.find(eventKey);
        if (record != m_eventCards.end() && record->ownerKey == accountKey) {
            labels = record->labels;
            record->baseTitle = baseTitle;
        }
    }
    return baseTitle + accountSuffix(labels);
}

QString NotificationManager::accountSuffix(const QStringList &labels) const
{
    if (!m_liveAccountsMode || !m_accountLabelsShown || labels.isEmpty())
        return QString();
    QStringList isolated;
    isolated.reserve(labels.size());
    for (const QString &each : labels)
        isolated.append(isolate(each));
    return QStringLiteral(" · ") + isolated.join(QStringLiteral(", "));
}

bool NotificationManager::mergeIntoEventCard(const TimelineEvent &event,
                                             const Context &context,
                                             const QString &roomId,
                                             const Decision &decision)
{
    if (event.eventId.isEmpty())
        return false;
    const QString eventKey = roomId + kKeySeparator + event.eventId;
    const auto record = m_eventCards.find(eventKey);
    if (record == m_eventCards.end())
        return false;
    if (record->ownerKey == context.accountUserId)
        return false;
    const QString eventId = event.eventId;
    const auto sameMessage = [&roomId, &eventId](const QVariantMap &payload) {
        return payload.value(QStringLiteral("eventId")).toString() == eventId
            && routableRoomId(payload.value(QStringLiteral("roomId")).toString())
                == roomId;
    };
    const bool upgrade = decision.mention
        && !record->card.payload.value(QStringLiteral("mention")).toBool();
    DeferredHostCard *waitingHost = nullptr;
    for (DeferredHostCard &waiting : m_hostCardQueue) {
        if (waiting.card.accountKey == record->ownerKey
            && waiting.groupKey == record->groupKey
            && sameMessage(waiting.card.payload)) {
            waitingHost = &waiting;
            break;
        }
    }
    {
        const AccountTable &owner = table(record->ownerKey);
        const auto group = owner.groups.constFind(record->groupKey);
        const bool groupGone = group == owner.groups.cend();
        const bool shown = !groupGone && sameMessage(group->payload);
        if (!waitingHost && !shown && (groupGone || upgrade))
            return false;
    }
    const QString uid = accountOfKey(context.accountUserId);
    if (!record->accounts.contains(uid)) {
        record->accounts.append(uid);
        record->labels.append(accountLabelOr(context.accountLabel, uid));
    }
    const QString title = record->baseTitle + accountSuffix(record->labels);
    const QStringList accounts = record->accounts;
    qCInfo(lcNotify) << "one message, live accounts=" << accounts.size()
                     << ": merged into one card"
                     << (upgrade ? "(now a mention)" : "");
    if (waitingHost) {
        DeferredHostCard &waiting = *waitingHost;
        waiting.card.title = title;
        waiting.card.payload.insert(QStringLiteral("accountUserIds"),
                                    accounts);
        if (upgrade) {
            waiting.card.payload.insert(QStringLiteral("mention"), true);
            waiting.card.sound = waiting.card.sound || decision.playSound;
        }
        record->card = waiting.card;
        table(record->ownerKey).groups[record->groupKey].payload =
            waiting.card.payload;
        return true;
    }
    if (sameMessage(m_lastFallbackPayload))
        m_lastFallbackPayload.insert(QStringLiteral("accountUserIds"), accounts);
    if (upgrade) {
        const QString ownerKey = record->ownerKey;
        m_balloonQueue.removeIf([&ownerKey, &sameMessage](const Balloon &b) {
            return b.accountKey == ownerKey && sameMessage(b.payload);
        });
    }
    for (Balloon &waiting : m_balloonQueue) {
        if (waiting.accountKey == record->ownerKey
            && sameMessage(waiting.payload))
            waiting.payload.insert(QStringLiteral("accountUserIds"), accounts);
    }
    AccountTable &owner = table(record->ownerKey);
    for (auto it = owner.pendingPayloads.begin();
         it != owner.pendingPayloads.end(); ++it) {
        if (sameMessage(it.value()))
            it.value().insert(QStringLiteral("accountUserIds"), accounts);
    }
    const auto group = owner.groups.find(record->groupKey);
    if (group == owner.groups.end() || !sameMessage(group->payload))
        return true;
    Card card = record->card;
    card.title = title;
    card.payload = group->payload;
    card.payload.insert(QStringLiteral("accountUserIds"), accounts);
    if (upgrade) {
        card.payload.insert(QStringLiteral("mention"), true);
        card.sound = decision.playSound;
        card.replacement = false;
    } else {
        card.sound = false;
        card.replacement = true;
    }
    group->payload = card.payload;
    record->card = card;
    deliver(std::move(card));
    return true;
}

void NotificationManager::recordEventCard(const QString &eventKey,
                                          const QString &accountKey,
                                          const QString &groupKey,
                                          const QString &label,
                                          const QString &baseTitle,
                                          const Card &card)
{
    const auto existing = m_eventCards.find(eventKey);
    if (existing != m_eventCards.end()) {
        if (existing->ownerKey == accountKey) {
            existing->groupKey = groupKey;
            existing->baseTitle = baseTitle;
            existing->card = card;
        }
        return;
    }
    EventCard record;
    record.ownerKey = accountKey;
    record.groupKey = groupKey;
    record.accounts = QStringList{ accountOfKey(accountKey) };
    record.labels = QStringList{ label };
    record.baseTitle = baseTitle;
    record.card = card;
    m_eventCards.insert(eventKey, record);
    m_eventCardOrder.append(eventKey);
    while (m_eventCardOrder.size() > kMaxEventCards)
        m_eventCards.remove(m_eventCardOrder.takeFirst());
}

bool NotificationManager::deliveryReplacesInPlace() const
{
#ifdef HAVE_QT_DBUS
    return QDBusConnection::sessionBus().isConnected();
#else
    return false;
#endif
}

void NotificationManager::offerHostCard(Card card, const QString &groupKey,
                                        bool groupWasEmpty)
{
    for (DeferredHostCard &waiting : m_hostCardQueue) {
        if (waiting.card.accountKey == card.accountKey
            && waiting.groupKey == groupKey) {
            card.sound = card.sound || waiting.card.sound;
            waiting.card = std::move(card);
            return;
        }
    }
    if (!groupWasEmpty && deliveryReplacesInPlace()) {
        deliver(std::move(card));
        return;
    }
    const qint64 now = nowMs();
    if (m_hostCardQueue.isEmpty()
        && (m_lastHostNewCardMs < 0
            || now - m_lastHostNewCardMs >= m_hostCardIntervalMs)) {
        m_lastHostNewCardMs = now;
        deliver(std::move(card));
        return;
    }
    noteCardFor(card.accountKey);
    m_hostCardQueue.append(DeferredHostCard{ groupKey, std::move(card) });
    while (m_hostCardQueue.size() > kMaxPendingPayloads) {
        qsizetype drop = 0;
        for (qsizetype i = 0; i < m_hostCardQueue.size(); ++i) {
            if (!m_hostCardQueue.at(i).card.payload
                     .value(QStringLiteral("mention")).toBool()) {
                drop = i;
                break;
            }
        }
        const DeferredHostCard dropped = m_hostCardQueue.takeAt(drop);
        AccountTable &own = table(dropped.card.accountKey);
        const auto group = own.groups.find(dropped.groupKey);
        if (group != own.groups.end() && group->id == 0 && !group->inFlight
            && !group->next)
            own.groups.erase(group);
        qCInfo(lcNotify) << "dropped a waiting background-account "
                            "notification: too many rooms waiting";
    }
    armHostCardTimer();
}

void NotificationManager::armHostCardTimer()
{
    if (!m_hostCardTimer || m_hostCardQueue.isEmpty())
        return;
    const qint64 wait = m_lastHostNewCardMs < 0
        ? 0
        : std::max<qint64>(0, m_lastHostNewCardMs + m_hostCardIntervalMs
                                  - nowMs());
    m_hostCardTimer->start(int(wait));
}

void NotificationManager::flushHostCards()
{
    if (m_hostCardQueue.isEmpty())
        return;
    const qint64 now = nowMs();
    if (m_lastHostNewCardMs >= 0
        && now - m_lastHostNewCardMs < m_hostCardIntervalMs) {
        armHostCardTimer();
        return;
    }
    DeferredHostCard next = m_hostCardQueue.takeFirst();
    m_lastHostNewCardMs = now;
    deliver(std::move(next.card));
    armHostCardTimer();
}

void NotificationManager::noteAccountUsed(const QString &userId)
{
    if (userId.isEmpty())
        return;
    m_accountRecency.removeAll(userId);
    m_accountRecency.prepend(userId);
    while (m_accountRecency.size() > kMaxAccountRecency)
        m_accountRecency.removeLast();
}

QString NotificationManager::openAccountFor(const QVariantMap &payload) const
{
    const QString owner =
        payload.value(QStringLiteral("accountUserId")).toString();
    const QStringList accounts =
        payload.value(QStringLiteral("accountUserIds")).toStringList();
    if (accounts.size() < 2)
        return owner;
    if (!m_accountUserId.isEmpty() && accounts.contains(m_accountUserId))
        return m_accountUserId;
    for (const QString &recent : m_accountRecency) {
        if (accounts.contains(recent))
            return recent;
    }
    return owner;
}

void NotificationManager::forgetLiveAccountsStateFor(const QString &accountKey)
{
    if (m_eventCards.isEmpty() && m_hostCardQueue.isEmpty()
        && m_hostRoomsAwaitingUnread.isEmpty())
        return;
    m_hostCardQueue.removeIf([&accountKey](const DeferredHostCard &waiting) {
        return waiting.card.accountKey == accountKey;
    });
    if (m_hostCardQueue.isEmpty() && m_hostCardTimer)
        m_hostCardTimer->stop();
    if (!accountKey.isEmpty()) {
        const QString prefix = accountKey + kKeySeparator;
        for (auto it = m_hostRoomsAwaitingUnread.begin();
             it != m_hostRoomsAwaitingUnread.end();) {
            if (it.key().startsWith(prefix))
                it = m_hostRoomsAwaitingUnread.erase(it);
            else
                ++it;
        }
        for (auto it = m_hostRoomsCheckedAt.begin(); it != m_hostRoomsCheckedAt.end();) {
            if (it.key().startsWith(prefix))
                it = m_hostRoomsCheckedAt.erase(it);
            else
                ++it;
        }
        m_cardGeneration.remove(accountKey);
    }
    const QString uid = accountOfKey(accountKey);
    for (auto it = m_eventCards.begin(); it != m_eventCards.end();) {
        if (it->ownerKey == accountKey) {
            m_eventCardOrder.removeOne(it.key());
            it = m_eventCards.erase(it);
            continue;
        }
        const qsizetype index = it->accounts.indexOf(uid);
        if (index >= 0) {
            it->accounts.removeAt(index);
            if (index < it->labels.size())
                it->labels.removeAt(index);
        }
        ++it;
    }
}

bool NotificationManager::hasCardsFor(const QString &accountKey) const
{
    const auto own = m_tables.constFind(accountKey);
    if (own != m_tables.cend()
        && (!own->pendingPayloads.isEmpty() || !own->groups.isEmpty()))
        return true;
    const auto ofAccount = [&accountKey](const auto &waiting) {
        return waiting.accountKey == accountKey;
    };
    return std::any_of(m_avatarWaits.cbegin(), m_avatarWaits.cend(), ofAccount)
        || std::any_of(m_capabilityWaits.cbegin(), m_capabilityWaits.cend(),
                       ofAccount)
        || std::any_of(m_balloonQueue.cbegin(), m_balloonQueue.cend(), ofAccount)
        || std::any_of(m_shownEntries.cbegin(), m_shownEntries.cend(), ofAccount)
        || std::any_of(m_hostCardQueue.cbegin(), m_hostCardQueue.cend(),
                       [&accountKey](const DeferredHostCard &waiting) {
                           return waiting.card.accountKey == accountKey;
                       });
}

void NotificationManager::observeRoomUnreadFor(const QString &accountUserId,
                                               const QString &roomId,
                                               bool unread)
{
    if (!m_liveAccountsMode || accountUserId.isEmpty() || roomId.isEmpty())
        return;
    const QString key = accountUserId + kKeySeparator + roomId;
    if (unread) {
        m_hostRoomsAwaitingUnread.remove(key);
        m_hostRoomsCheckedAt.remove(key);
        return;
    }
    const quint64 generation = m_cardGeneration.value(accountUserId);
    const auto checked = m_hostRoomsCheckedAt.constFind(key);
    if (checked != m_hostRoomsCheckedAt.cend() && checked.value() == generation)
        return;
    if (!hasCardsFor(accountUserId)) {
        m_hostRoomsAwaitingUnread.remove(key);
        m_hostRoomsCheckedAt.insert(key, generation);
        return;
    }
    const auto awaiting = m_hostRoomsAwaitingUnread.constFind(key);
    if (awaiting != m_hostRoomsAwaitingUnread.cend()
        && nowMs() < awaiting.value())
        return;
    m_hostRoomsAwaitingUnread.remove(key);
    m_hostRoomsCheckedAt.insert(key, generation);
    ++m_hostUnreadPasses;
    const int withdrawn = withdrawWhere(
        accountUserId, [&roomId](const QVariantMap &payload) {
            return payload.value(QStringLiteral("roomId")).toString() == roomId
                && !payload.value(QStringLiteral("eventId")).toString()
                        .isEmpty()
                && payload.value(QStringLiteral("threadRootId")).toString()
                        .isEmpty();
        });
    if (withdrawn > 0)
        qCInfo(lcNotify) << "withdrew" << withdrawn
                         << "background-account notification(s) for a room "
                            "read elsewhere";
}

QString NotificationManager::eventCardTitleForTest(const QString &roomId,
                                                   const QString &eventId) const
{
    const auto record =
        m_eventCards.constFind(roomId + kKeySeparator + eventId);
    if (record == m_eventCards.cend())
        return QString();
    return record->baseTitle + accountSuffix(record->labels);
}

QStringList NotificationManager::eventCardAccountsForTest(
    const QString &roomId, const QString &eventId) const
{
    return m_eventCards.value(roomId + kKeySeparator + eventId).accounts;
}

void NotificationManager::closeRoomNotifications(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    m_roomsAwaitingUnread.remove(roomId);
    m_heldUtds.removeIf([&roomId](const HeldUtd &held) {
        return held.context.accountUserId.isEmpty()
            && routableRoomId(held.event->roomId) == roomId;
    });
    if (m_heldUtds.isEmpty())
        m_utdHoldTimer.stop();
    const int withdrawn = withdrawWhere(
        QString(), [&roomId](const QVariantMap &payload) {
            return payload.value(QStringLiteral("roomId")).toString()
                == roomId;
        });
    if (withdrawn > 0)
        qCInfo(lcNotify) << "withdrew" << withdrawn
                         << "notification(s) for a room that has been read";
}

void NotificationManager::closeThreadNotifications(const QString &roomId,
                                                   const QString &rootEventId)
{
    if (roomId.isEmpty() || rootEventId.isEmpty())
        return;
    m_heldUtds.removeIf([&roomId, &rootEventId](const HeldUtd &held) {
        return held.context.accountUserId.isEmpty()
            && routableRoomId(held.event->roomId) == roomId
            && routableThreadRootId(held.event->roomId,
                                    held.event->threadRootId)
                == rootEventId;
    });
    if (m_heldUtds.isEmpty())
        m_utdHoldTimer.stop();
    const int withdrawn = withdrawWhere(
        QString(), [&roomId, &rootEventId](const QVariantMap &payload) {
            return payload.value(QStringLiteral("roomId")).toString() == roomId
                && payload.value(QStringLiteral("threadRootId")).toString()
                    == rootEventId;
        });
    if (withdrawn > 0)
        qCInfo(lcNotify) << "withdrew" << withdrawn
                         << "notification(s) for a thread that has been read";
}

int NotificationManager::withdrawEverywhere(
    const std::function<bool(const QVariantMap &)> &match)
{
    const QList<QString> keys = m_tables.keys();
    int withdrawn = 0;
    for (const QString &key : keys)
        withdrawn += withdrawWhere(key, match);
    return withdrawn;
}

int NotificationManager::withdrawWhere(
    const QString &accountKey,
    const std::function<bool(const QVariantMap &)> &match)
{
    const auto cardMatches = [&match, &accountKey](const Card &card) {
        return card.accountKey == accountKey && match(card.payload);
    };
    const qsizetype dropped = m_avatarWaits.removeIf(cardMatches)
        + m_capabilityWaits.removeIf(cardMatches)
        + m_balloonQueue.removeIf([&match, &accountKey](const Balloon &b) {
              return b.accountKey == accountKey && match(b.payload);
          })
        + m_hostCardQueue.removeIf(
              [&cardMatches](const DeferredHostCard &waiting) {
                  return cardMatches(waiting.card);
              });
    m_shownEntries.removeIf([&match, &accountKey](const Balloon &b) {
        return b.accountKey == accountKey && match(b.payload);
    });
    if (dropped > 0) {
        qCInfo(lcNotify) << "dropped" << dropped
                         << "notification(s) not shown yet";
        if (m_avatarWaits.isEmpty())
            m_avatarWaitTimer.stop();
        if (m_hostCardQueue.isEmpty() && m_hostCardTimer)
            m_hostCardTimer->stop();
    }
    QList<quint32> stale;
    {
        const AccountTable &own = table(accountKey);
        for (auto it = own.pendingPayloads.cbegin();
             it != own.pendingPayloads.cend(); ++it) {
            if (match(it.value()))
                stale.append(it.key());
        }
    }
    stale.removeOne(m_activeCallNotificationId);
    for (const quint32 id : std::as_const(stale)) {
        closeNotification(id);
        forgetPayload(id);
    }
    QHash<QString, CardGroup> &groups = table(accountKey).groups;
    for (auto it = groups.begin(); it != groups.end();) {
        if (it->next && match(it->next->payload))
            it->next.reset();
        if (!match(it->payload)) {
            ++it;
        } else if (it->inFlight) {
            it->withdrawn = true;
            ++it;
        } else if (it->id == 0) {
            it = groups.erase(it);
        } else {
            ++it;
        }
    }
    return int(stale.size());
}

void NotificationManager::recordPayload(const QString &accountKey, quint32 id,
                                        const QVariantMap &payload)
{
    noteCardFor(accountKey);
    if (m_tombstones.remove(id) > 0)
        m_tombstoneOrder.removeOne(id);
    for (auto it = m_tables.begin(); it != m_tables.end(); ++it) {
        if (it.key() != accountKey && it->pendingPayloads.remove(id) > 0)
            it->payloadOrder.removeOne(id);
    }
    AccountTable &own = table(accountKey);
    own.payloadOrder.removeOne(id);
    own.payloadOrder.append(id);
    own.pendingPayloads.insert(id, payload);
    while (own.payloadOrder.size() > kMaxPendingPayloads) {
        qsizetype index = 0;
        if (own.payloadOrder.first() == m_activeCallNotificationId
            && own.payloadOrder.size() > 1)
            index = 1;
        const quint32 oldest = own.payloadOrder.at(index);
        const QString account = own.pendingPayloads.value(oldest)
                                    .value(QStringLiteral("accountUserId"))
                                    .toString();
        closeNotification(oldest);
        forgetPayload(oldest);
        own.payloadOrder.removeOne(oldest);
        tombstone(oldest, account);
    }
}

NotificationManager::AccountTable *NotificationManager::tableHolding(quint32 id)
{
    for (auto it = m_tables.begin(); it != m_tables.end(); ++it) {
        if (it->pendingPayloads.contains(id))
            return &it.value();
    }
    return nullptr;
}

QVariantMap NotificationManager::payloadFor(quint32 id) const
{
    for (auto it = m_tables.cbegin(); it != m_tables.cend(); ++it) {
        const auto found = it->pendingPayloads.constFind(id);
        if (found != it->pendingPayloads.cend())
            return found.value();
    }
    return {};
}

int NotificationManager::pendingPayloadCountForTest(
    const QString &accountUserId) const
{
    const auto it = m_tables.constFind(accountUserId);
    return it == m_tables.cend() ? 0 : int(it->pendingPayloads.size());
}

quint64 NotificationManager::deliveryGenerationForTest(
    const QString &accountUserId) const
{
    const auto it = m_tables.constFind(accountUserId);
    return it == m_tables.cend() ? 0 : it->deliveryGeneration;
}

int NotificationManager::avatarWaitCount(const QString &accountKey) const
{
    return int(std::count_if(m_avatarWaits.cbegin(), m_avatarWaits.cend(),
                             [&accountKey](const Card &card) {
                                 return card.accountKey == accountKey;
                             }));
}

const NotificationManager::Card *
NotificationManager::foregroundAvatarWait(int index) const
{
    if (index < 0)
        return nullptr;
    for (const Card &card : m_avatarWaits) {
        if (!card.accountKey.isEmpty())
            continue;
        if (index-- == 0)
            return &card;
    }
    return nullptr;
}

void NotificationManager::forgetPayload(quint32 id)
{
    AccountTable *own = tableHolding(id);
    if (!own)
        return;
    const auto it = own->pendingPayloads.constFind(id);
    const QString groupKey = it->value(QStringLiteral("group")).toString();
    own->pendingPayloads.erase(it);
    own->payloadOrder.removeOne(id);
    m_activationTokens.remove(id);
    if (groupKey.isEmpty())
        return;
    const auto group = own->groups.find(groupKey);
    if (group == own->groups.end() || group->id != id)
        return;
    QString ownerKey;
    for (auto t = m_tables.cbegin(); t != m_tables.cend(); ++t) {
        if (&t.value() == own)
            ownerKey = t.key();
    }
    const auto isReplacementHere = [&groupKey, &ownerKey](const Card &card) {
        return card.replacement && card.accountKey == ownerKey
            && card.payload.value(QStringLiteral("group")).toString() == groupKey;
    };
    m_avatarWaits.removeIf(isReplacementHere);
    m_capabilityWaits.removeIf(isReplacementHere);
    if (m_avatarWaits.isEmpty())
        m_avatarWaitTimer.stop();
    if (group->next && group->next->replacement)
        group->next.reset();
    if (group->inFlight || group->next)
        group->id = 0;
    else
        own->groups.erase(group);
}

bool NotificationManager::takeNotifyReply(const Card &card,
                                          const QString &groupKey,
                                          quint64 generation,
                                          std::optional<quint32> id)
{
    // Its account was cleared meanwhile (sign-out, a switch, a removal): the
    // card is not that table's to route any more, and its preview must not
    // outlive the account either. Only the card's own table's clear counts.
    if (generation != table(card.accountKey).deliveryGeneration) {
        if (id) {
            closeNotification(*id);
            tombstone(*id, card.payload.value(QStringLiteral("accountUserId"))
                               .toString());
        }
        return false;
    }
    if (!groupKey.isEmpty()) {
        QHash<QString, CardGroup> &groups = table(card.accountKey).groups;
        const auto group = groups.find(groupKey);
        if (group != groups.end())
            group->inFlight = false;
    }
    if (!id)
        return true;
    const quint32 shown = *id;
    quint32 previous = 0;
    if (!groupKey.isEmpty()) {
        QHash<QString, CardGroup> &groups = table(card.accountKey).groups;
        auto group = groups.find(groupKey);
        if (group == groups.end())
            group = groups.insert(groupKey, CardGroup{});
        previous = std::exchange(group->id, shown);
    }
    if (previous != 0 && previous != shown)
        forgetPayload(previous);
    recordPayload(card.accountKey, shown, card.payload);
    if (groupKey.isEmpty())
        return false;
    QHash<QString, CardGroup> &groups = table(card.accountKey).groups;
    const auto group = groups.find(groupKey);
    if (group == groups.end())
        return false;
    if (group->withdrawn) {
        group->next.reset();
        closeNotification(shown);
        forgetPayload(shown);
        return false;
    }
    if (group->next) {
        Card next = std::move(*group->next);
        group->next.reset();
        deliverNow(std::move(next));
    }
    return false;
}

void NotificationManager::notifyReplyForTest(const QString &accountUserId,
                                             quint64 generation, quint32 id,
                                             const QVariantMap &payload)
{
    Card card;
    card.payload = payload;
    card.accountKey = accountUserId;
    takeNotifyReply(card, payload.value(QStringLiteral("group")).toString(),
                    generation, id);
}

void NotificationManager::tombstone(quint32 id, const QString &accountUserId)
{
    if (id == 0)
        return;
    if (!m_tombstones.contains(id))
        m_tombstoneOrder.append(id);
    m_tombstones.insert(id, accountUserId);
    while (m_tombstoneOrder.size() > kMaxTombstones)
        m_tombstones.remove(m_tombstoneOrder.takeFirst());
}

void NotificationManager::closeNotification(quint32 id)
{
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (id == 0 || !bus.isConnected())
        return;
    QDBusMessage call = notificationsCall(QStringLiteral("CloseNotification"));
    call << id;
    bus.send(call);
#else
    Q_UNUSED(id);
#endif
}

void NotificationManager::deliver(Card card)
{
    noteCardFor(card.accountKey);
    if (!card.avatarMxc.startsWith(QLatin1String("mxc://")) || !m_avatarImage) {
        card.avatar = card.fallback;
        deliverNow(std::move(card));
        return;
    }
    const QImage cached = m_avatarImage(card.avatarMxc,true);
    if (!cached.isNull()) {
        card.avatar = cached;
        deliverNow(std::move(card));
        return;
    }
    if (m_avatarFailed && m_avatarFailed(card.avatarMxc)) {
        card.avatar = card.fallback;
        deliverNow(std::move(card));
        return;
    }
    if (avatarWaitCount(card.accountKey) >= kMaxPendingPayloads) {
        const auto oldestIt = std::find_if(
            m_avatarWaits.begin(), m_avatarWaits.end(),
            [&card](const Card &waiting) {
                return waiting.accountKey == card.accountKey;
            });
        Card oldest = std::move(*oldestIt);
        m_avatarWaits.erase(oldestIt);
        oldest.avatar = oldest.fallback;
        deliverNow(std::move(oldest));
    }
    m_avatarWaits.append(std::move(card));
    if (!m_avatarWaitTimer.isActive())
        m_avatarWaitTimer.start();
}

void NotificationManager::flushAvatarWaits(bool fallbackAll)
{
    QList<Card> ready;
    QList<Card> remaining;
    for (const Card &waiting : std::as_const(m_avatarWaits)) {
        const QImage image = m_avatarImage
            ? m_avatarImage(waiting.avatarMxc,false) : QImage{};
        const bool failed = !m_avatarImage
            || (m_avatarFailed && m_avatarFailed(waiting.avatarMxc));
        if (!image.isNull() || failed || fallbackAll) {
            Card card = waiting;
            card.avatar = image.isNull() ? waiting.fallback : image;
            ready.append(std::move(card));
        } else {
            remaining.append(waiting);
        }
    }
    m_avatarWaits = remaining;
    for (Card &card : ready)
        deliverNow(std::move(card));
    if (!m_avatarWaits.isEmpty() && !m_avatarWaitTimer.isActive())
        m_avatarWaitTimer.start();
}

NotificationManager::CardShape NotificationManager::messageCardShape(
    bool actionable, bool inlineReplySupported)
{
    CardShape shape;
    shape.actions = QStringList{ QStringLiteral("default"), tr("Open") };
    if (actionable) {
        shape.actions << QStringLiteral("mark-read") << tr("Mark as read");
        if (inlineReplySupported) {
            shape.actions << QStringLiteral("inline-reply") << tr("Reply");
            shape.inlineReply = true;
        }
    }
    shape.urgency = 1;
    return shape;
}

// The only place a notification body is escaped. A freedesktop body is
// markup on servers advertising `body-markup` (GNOME, KDE), and its content
// (names, message text) is attacker-chosen: tags would render and unbalanced
// ones make Pango reject the string. Escaping only when markup is rendered
// avoids literal `&amp;` elsewhere. Callers pass raw text, so escaping happens
// exactly once. The summary is plain text everywhere and is never escaped.
// Guarded by HAVE_QT_DBUS: Windows and macOS build without QtDBus.
#ifdef HAVE_QT_DBUS
QString NotificationManager::bodyForServer(const QString &body) const
{
    return m_bodyMarkup ? body.toHtmlEscaped() : body;
}

bool NotificationManager::requestCapabilities()
{
    if (m_capabilityRequestInFlight)
        return true;
    if (nowMs() < m_capabilityRetryAtMs)
        return false;
    if (!m_inhibitionQueried) {
        m_inhibitionQueried = true;
        m_inhibitionQueriedAtMs = nowMs();
        queryDesktopInhibition();
    }
    m_capabilityRequestInFlight = true;
    ++m_capabilityRequests;
    if (!m_serverInfoKnown)
        requestServerInformation();
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(
            notificationsCall(QStringLiteral("GetCapabilities")),
            kCapabilitiesTimeoutMs),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, daemon = m_daemonGeneration] {
        watcher->deleteLater();
        if (daemon != m_daemonGeneration)
            return;
        m_capabilityRequestInFlight = false;
        const QDBusPendingReply<QStringList> caps = *watcher;
        if (caps.isError()) {
            m_bodyMarkup = true;
            m_inlineReply = false;
            const QDBusError::ErrorType type = caps.error().type();
            const bool transient = type == QDBusError::ServiceUnknown
                || type == QDBusError::NoReply || type == QDBusError::Timeout
                || type == QDBusError::TimedOut
                || type == QDBusError::Disconnected
                || type == QDBusError::NoServer;
            if (transient && m_capabilityRequests < kMaxCapabilityRequests)
                m_capabilityRetryAtMs = nowMs() + m_capabilityRetryMs;
            else
                m_bodyMarkupKnown = true;
            qCWarning(lcNotify) << "notification server capabilities unknown:"
                                << caps.error().name()
                                << "asking again:" << !m_bodyMarkupKnown;
        } else {
            m_bodyMarkup = caps.value().contains(QStringLiteral("body-markup"));
            m_inlineReply =
                caps.value().contains(QStringLiteral("inline-reply"));
            qCInfo(lcNotify) << "notification server body-markup ="
                             << m_bodyMarkup << "inline-reply ="
                             << m_inlineReply;
        }
        if (!caps.isError())
            m_bodyMarkupKnown = true;
        releaseCapabilityWaits();
    });
    return true;
}

void NotificationManager::requestServerInformation()
{
    if (m_serverInfoInFlight)
        return;
    m_serverInfoInFlight = true;
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(
            notificationsCall(QStringLiteral("GetServerInformation")),
            kCapabilitiesTimeoutMs),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, daemon = m_daemonGeneration] {
        watcher->deleteLater();
        if (daemon != m_daemonGeneration)
            return;
        m_serverInfoInFlight = false;
        const QDBusPendingReply<QString, QString, QString, QString> info = *watcher;
        if (info.isError()) {
            const QDBusError::ErrorType type = info.error().type();
            m_serverInfoKnown = !(type == QDBusError::ServiceUnknown
                                  || type == QDBusError::NoReply
                                  || type == QDBusError::Timeout
                                  || type == QDBusError::TimedOut
                                  || type == QDBusError::Disconnected
                                  || type == QDBusError::NoServer);
            m_residentCards = false;
        } else {
            m_serverInfoKnown = true;
            m_residentCards = residentHintWanted(info.argumentAt<0>(),
                                                 info.argumentAt<1>(),
                                                 info.argumentAt<2>());
            qCInfo(lcNotify) << "notification server" << info.argumentAt<0>()
                             << info.argumentAt<2>()
                             << "resident cards =" << m_residentCards;
        }
        releaseCapabilityWaits();
    });
}

void NotificationManager::releaseCapabilityWaits()
{
    if (m_capabilityRequestInFlight || m_serverInfoInFlight)
        return;
    const QList<Card> waiting = std::exchange(m_capabilityWaits, {});
    for (const Card &wait : waiting)
        deliverNow(wait);
    if (std::exchange(m_callWaitsForCapabilities, false))
        deliverCallNotification();
}
#endif

bool NotificationManager::residentHintWanted(const QString &name,
                                             const QString &vendor,
                                             const QString &version)
{
    if (name != QLatin1String("Plasma") || vendor != QLatin1String("KDE"))
        return false;
    const QStringList parts = version.split(QLatin1Char('.'));
    bool majorOk = false;
    bool minorOk = false;
    const int major = parts.value(0).toInt(&majorOk);
    const int minor = parts.value(1).toInt(&minorOk);
    if (!majorOk || !minorOk)
        return true;
    return major < 6 || (major == 6 && minor < 4);
}

void NotificationManager::queryDesktopInhibition()
{
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QDBusMessage get = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    get << kInterface << QStringLiteral("Inhibited");
    auto *watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(get, kPropertyTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, daemon = m_daemonGeneration] {
        watcher->deleteLater();
        if (daemon != m_daemonGeneration)
            return;
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        if (reply.isError())
            return;
        const QVariant value = unwrapVariant(reply.value().variant());
        if (value.metaType() != QMetaType::fromType<bool>())
            return;
        if (m_daemonInhibited != value.toBool())
            qCInfo(lcNotify) << "desktop do-not-disturb =" << value.toBool();
        m_daemonInhibited = value.toBool();
    });
    if (!qEnvironmentVariable("XDG_CURRENT_DESKTOP")
             .contains(QLatin1String("GNOME"), Qt::CaseInsensitive))
        return;
    const auto apply = [this](const QDBusPendingReply<QDBusVariant> &reply) {
        if (reply.isError())
            return;
        const QVariant value = unwrapVariant(reply.value().variant());
        if (value.metaType() == QMetaType::fromType<bool>())
            m_bannersOff = !value.toBool();
    };
    auto *portal = readPortalSetting(this, QStringLiteral("ReadOne"));
    connect(portal, &QDBusPendingCallWatcher::finished, this,
            [this, portal, apply] {
        portal->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *portal;
        if (!reply.isError()
            || reply.error().type() != QDBusError::UnknownMethod) {
            apply(reply);
            return;
        }
        auto *legacy = readPortalSetting(this, QStringLiteral("Read"));
        connect(legacy, &QDBusPendingCallWatcher::finished, this,
                [legacy, apply] {
            legacy->deleteLater();
            apply(*legacy);
        });
    });
#endif
}

bool NotificationManager::replacementStillOwned(const Card &card) const
{
    const auto own = m_tables.constFind(card.accountKey);
    if (own == m_tables.cend())
        return false;
    const auto group = own->groups.constFind(
        card.payload.value(QStringLiteral("group")).toString());
    return group != own->groups.cend() && !group->withdrawn
        && (group->id != 0 || group->inFlight)
        && group->payload.value(QStringLiteral("eventId")).toString()
            == card.payload.value(QStringLiteral("eventId")).toString();
}

void NotificationManager::deliverNow(Card card)
{
    noteCardFor(card.accountKey);
    if (card.replacement && !replacementStillOwned(card)) {
        ++m_droppedReplacements;
        qCInfo(lcNotify) << "dropped a replacement for a card that is gone";
        return;
    }
    const bool mention = card.payload.value(QStringLiteral("mention")).toBool();
    const QString roomId =
        card.payload.value(QStringLiteral("roomId")).toString();
    const bool isMessage =
        !card.payload.value(QStringLiteral("eventId")).toString().isEmpty();
    const auto throughTray = [this, mention, roomId, isMessage](const Card &c) {
        if (c.replacement)
            return;
        const SoundPlan plan =
            takeSoundPlan(c.sound, mention, trayCarriesSound());
        if (!offerBalloon(Balloon{ c.title, c.body, c.payload, c.avatar,
                                   plan.platformSound, c.accountKey },
mention || roomId.isEmpty()))
            qCInfo(lcNotify) << "notification service unavailable";
        if (isMessage)
            Q_EMIT messageDelivered(roomId, mention);
    };
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        throughTray(card);
        return;
    }
    if ((!m_bodyMarkupKnown && requestCapabilities()) || m_serverInfoInFlight) {
        if (m_capabilityWaits.size() < kMaxCapabilityWaits)
            m_capabilityWaits.append(std::move(card));
        else
            qCWarning(lcNotify) << "notification dropped: daemon not answering";
        return;
    }
    if (!m_inhibitionQueried
        || nowMs() - m_inhibitionQueriedAtMs > kInhibitionRefreshMs) {
        m_inhibitionQueried = true;
        m_inhibitionQueriedAtMs = nowMs();
        queryDesktopInhibition();
    }
    const QString groupKey =
        card.payload.value(QStringLiteral("group")).toString();
    quint32 replacesId = 0;
    if (!groupKey.isEmpty()) {
        QHash<QString, CardGroup> &groups = table(card.accountKey).groups;
        auto group = groups.find(groupKey);
        if (group == groups.end())
            group = groups.insert(groupKey, CardGroup{});
        if (group->inFlight) {
            if (group->next) {
                card.sound = card.sound || group->next->sound;
                card.replacement = card.replacement && group->next->replacement;
            }
            group->next = std::move(card);
            return;
        }
        replacesId = group->id;
        group->inFlight = true;
        group->withdrawn = false;
        group->payload = card.payload;
    }
    const NotificationManager::DesktopIdentity identity = notificationIdentity();
    const CardShape shape =
        messageCardShape(!roomId.isEmpty() && isMessage, m_inlineReply);
    // `resident` (R13), Plasma before 6.4 only (residentHintWanted()): a card
    // must still open its room when clicked in the history after its popup
    // timed out. That Plasma expires a non-resident popup (NotificationClosed
    // reason 1) and strips its actions (libnotificationmanager: "it cannot
    // have any actions unless it is resident"), so the history entry did
    // nothing; a resident one is only marked expired and keeps them. The
    // daemon then no longer removes a clicked card, so every action handled
    // in onActionInvoked() closes it (a no-op where it is already gone).
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), identity.desktopEntry },
        { QStringLiteral("urgency"), QVariant::fromValue(shape.urgency) },
    };
    if (m_residentCards)
        hints.insert(QStringLiteral("resident"), true);
    if (!card.avatar.isNull()) {
        hints.insert(QStringLiteral("image-data"),
                     QVariant::fromValue(notificationImage(card.avatar)));
    }
    const SoundPlan plan = takeSoundPlan(card.sound && !card.replacement,
                                         mention,true);
    const QVariantMap soundHintMap = soundHints(plan);
    for (auto it = soundHintMap.cbegin(); it != soundHintMap.cend(); ++it)
        hints.insert(it.key(), it.value());
    if (shape.inlineReply) {
        hints.insert(QStringLiteral("x-kde-reply-placeholder-text"),
                     tr("Reply…"));
    }
    const QString safeBody = bodyForServer(card.body);

    QDBusMessage call = notificationsCall(QStringLiteral("Notify"));
    call << QStringLiteral("Lightning") << replacesId << identity.appIcon
         << card.title << safeBody << shape.actions << hints << int(-1);
    auto *watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(call, kNotifyTimeoutMs), this);
    if (isMessage && !card.replacement)
        Q_EMIT messageDelivered(roomId, mention);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, card, groupKey, throughTray,
             platformSound = plan.platformSound,
             generation = table(card.accountKey).deliveryGeneration,
             daemon = m_daemonGeneration] {
        watcher->deleteLater();
        ++m_notifyReplies;
        const QDBusPendingReply<quint32> reply = *watcher;
        if (daemon != m_daemonGeneration)
            return;
        if (!takeNotifyReply(card, groupKey, generation,
                             reply.isError()
                                 ? std::nullopt
                                 : std::optional<quint32>(reply.value())))
            return;
        if (reply.error().type() == QDBusError::ServiceUnknown)
            qCInfo(lcNotify) << "notification service unavailable";
        else
            qCWarning(lcNotify) << "notification server refused the notification:"
                                << reply.error().name();
        if (!card.replacement
            && !offerBalloon(
                Balloon{ card.title, card.body, card.payload, card.avatar,
                         platformSound, card.accountKey },
card.payload.value(QStringLiteral("mention"))
                        .toBool()
                    || card.payload.value(QStringLiteral("roomId")).toString()
                           .isEmpty()))
            qCInfo(lcNotify) << "notification service unavailable";
        if (!groupKey.isEmpty()) {
            QHash<QString, CardGroup> &groups = table(card.accountKey).groups;
            const auto group = groups.find(groupKey);
            if (group != groups.end() && group->next) {
                Card next = std::move(*group->next);
                group->next.reset();
                throughTray(next);
            }
        }
    });
#else
    throughTray(card);
#endif
}

NotificationManager::SoundPlan NotificationManager::planSound(
    bool allowed, bool coalesced, SoundSource source)
{
    SoundPlan plan;
    if (!allowed || coalesced)
        return plan;
    if (source == SourceSystem) {
        plan.platformSound = true;
        plan.suppressPlatform = false;
    } else {
        plan.playOwn = true;
    }
    return plan;
}

QVariantMap NotificationManager::soundHints(const SoundPlan &plan)
{
    QVariantMap hints;
    if (plan.suppressPlatform)
        hints.insert(QStringLiteral("suppress-sound"), true);
    if (plan.platformSound)
        hints.insert(QStringLiteral("sound-name"),
                     QStringLiteral("message-new-instant"));
    return hints;
}

QVariantMap NotificationManager::callSoundHints(bool sound)
{
    QVariantMap hints;
    if (sound) {
        hints.insert(QStringLiteral("sound-name"),
                     QStringLiteral("phone-incoming-call"));
    } else {
        hints.insert(QStringLiteral("suppress-sound"), true);
    }
    return hints;
}

void NotificationManager::setSoundSource(int source)
{
    m_soundSource = source == SourceSystem ? SourceSystem : SourceLightning;
}

void NotificationManager::setOwnSoundPlayer(
    std::function<bool(bool mention)> player)
{
    m_ownSound = std::move(player);
}

void NotificationManager::setSoundAllowedCheck(std::function<bool()> check)
{
    m_soundAllowed = std::move(check);
}

bool NotificationManager::trayCarriesSound() const
{
    if (m_trayCarriesSoundOverride >= 0)
        return m_trayCarriesSoundOverride == 1;
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

NotificationManager::SoundPlan NotificationManager::takeSoundPlan(
    bool sound, bool mention, bool pathCarriesSound)
{
    const bool allowed = sound && (!m_soundAllowed || m_soundAllowed())
        && !desktopInhibited();
    bool coalesced = false;
    if (allowed) {
        const qint64 now = nowMs();
        coalesced = mention
            ? m_mentionSoundPlayed
                  && now - m_lastMentionSoundMs < kSoundCoalesceMs
            : m_soundPlayed && now - m_lastSoundMs < kSoundCoalesceMs;
        if (!coalesced) {
            m_lastSoundMs = now;
            m_soundPlayed = true;
            if (mention) {
                m_lastMentionSoundMs = now;
                m_mentionSoundPlayed = true;
            }
        }
    }
    SoundPlan plan = planSound(allowed, coalesced, m_soundSource);
    if (plan.platformSound && !pathCarriesSound) {
        plan = planSound(true, false, SourceLightning);
    }
    if (plan.playOwn) {
        const bool played = m_ownSound && m_ownSound(mention);
        qCInfo(lcNotify) << "notification sound own="
                         << (played ? "played" : "not played");
        if (!played && m_soundAllowed && pathCarriesSound) {
            plan.playOwn = false;
            plan.platformSound = true;
            plan.suppressPlatform = false;
            qCInfo(lcNotify) << "notification sound fell back to the desktop's";
        }
    }
    return plan;
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
                                             const QImage &avatar,
                                             bool platformSound,
                                             const QString &accountKey)
{
    ++m_trayAttempts;
    m_lastTrayPlatformSound = platformSound;
    m_lastTrayTitle = title;
    m_lastTrayBody = body;
    m_lastTrayPayload = payload;
    if (m_fallbackTray) {
        if (m_balloonMayBeVisible && balloonGate())
            m_fallbackTray->hideMessage();
        if (!m_fallbackTray->showMessage(title, body, avatar, platformSound))
            return false;
    } else if (!m_trayShowsForTest) {
        return false;
    }
    noteBalloon(accountKey, payload);
    if (!payload.value(QStringLiteral("roomId")).toString().isEmpty())
        qCInfo(lcNotify) << "notification delivered through the tray balloon";
    return true;
}

void NotificationManager::noteBalloon(const QString &accountKey,
                                      const QVariantMap &payload)
{
    m_lastFallbackPayload = payload;
    m_lastFallbackAccountKey = accountKey;
    m_balloonIsCall = false;
    m_shownEntries.clear();
    if (!balloonGate()) {
        m_balloonRooms.insert(
            routableRoomId(payload.value(QStringLiteral("roomId")).toString()));
        return;
    }
    m_balloonMayBeVisible = true;
    m_balloonHold.start(std::max(0, m_balloonHoldOverrideMs >= 0
                                        ? m_balloonHoldOverrideMs
                                        : TrayIcon::balloonHoldMs()));
}

bool NotificationManager::balloonGate() const
{
    if (m_balloonWithdrawableOverride >= 0)
        return m_balloonWithdrawableOverride == 1;
    return TrayIcon::canWithdrawBalloon();
}

void NotificationManager::deliverThroughTrayForTest(
    const QString &accountUserId, const QVariantMap &payload)
{
    const bool shows = std::exchange(m_trayShowsForTest, true);
    offerBalloon(Balloon{ QString(), QString(), payload, QImage(), false,
                          accountUserId },
                 payload.value(QStringLiteral("mention")).toBool()
                     || payload.value(QStringLiteral("roomId")).toString()
                            .isEmpty());
    m_trayShowsForTest = shows;
}

int NotificationManager::pendingBalloonMessagesForTest() const
{
    int messages = 0;
    for (const Balloon &balloon : m_balloonQueue)
        messages += balloon.messages;
    return messages;
}

bool NotificationManager::offerBalloon(Balloon balloon, bool breakThrough)
{
    if (!balloonGate())
        return deliverThroughTray(balloon.title, balloon.body, balloon.payload,
                                  balloon.avatar, balloon.platformSound,
                                  balloon.accountKey);
    if (m_balloonIsCall && m_balloonHold.isActive())
        breakThrough = false;
    if (!breakThrough && m_balloonHold.isActive()) {
        enqueueBalloon(std::move(balloon),false);
        return true;
    }
    const QString roomId = routableRoomId(
        balloon.payload.value(QStringLiteral("roomId")).toString());
    const QString accountKey = balloon.accountKey;
    requeueDisplacedBalloon();
    if (!deliverThroughTray(balloon.title, balloon.body, balloon.payload,
                            balloon.avatar, balloon.platformSound,
                            balloon.accountKey))
        return false;
    if (!roomId.isEmpty()) {
        m_balloonQueue.removeIf([&](const Balloon &waiting) {
            return waiting.accountKey == accountKey
                && routableRoomId(waiting.payload
                                      .value(QStringLiteral("roomId"))
                                      .toString())
                    == roomId;
        });
    }
    m_shownEntries = { std::move(balloon) };
    return true;
}

void NotificationManager::requeueDisplacedBalloon()
{
    if (!balloonGate() || !m_balloonHold.isActive())
        return;
    const QList<Balloon> displaced = std::exchange(m_shownEntries, {});
    for (Balloon entry : displaced) {
        entry.platformSound = false;
        enqueueBalloon(std::move(entry),true);
    }
}

void NotificationManager::enqueueBalloon(Balloon balloon, bool older)
{
    const auto groupOf = [](const Balloon &b) {
        const QString group = b.payload.value(QStringLiteral("group")).toString();
        return group.isEmpty()
            ? routableRoomId(b.payload.value(QStringLiteral("roomId")).toString())
            : group;
    };
    const QString group = groupOf(balloon);
    for (qsizetype i = 0; i < m_balloonQueue.size(); ++i) {
        Balloon &waiting = m_balloonQueue[i];
        if (waiting.accountKey != balloon.accountKey || groupOf(waiting) != group)
            continue;
        if (older) {
            waiting.messages += balloon.messages;
            return;
        }
        balloon.messages += waiting.messages;
        balloon.platformSound = balloon.platformSound || waiting.platformSound;
        m_balloonQueue.removeAt(i);
        break;
    }
    if (older)
        m_balloonQueue.prepend(std::move(balloon));
    else
        m_balloonQueue.append(std::move(balloon));
    while (m_balloonQueue.size() > kMaxPendingPayloads)
        m_balloonQueue.removeFirst();
}

void NotificationManager::showPendingBalloons()
{
    if (m_balloonQueue.isEmpty())
        return;
    QSet<QString> rooms;
    QSet<QString> accounts;
    int messages = 0;
    bool platformSound = false;
    for (const Balloon &balloon : std::as_const(m_balloonQueue)) {
        rooms.insert(balloon.accountKey + QChar(0x1f)
                     + routableRoomId(balloon.payload
                                          .value(QStringLiteral("roomId"))
                                          .toString()));
        accounts.insert(balloon.payload.value(QStringLiteral("accountUserId"))
                            .toString());
        messages += balloon.messages;
        platformSound = platformSound || balloon.platformSound;
    }
    Balloon summary;
    if (rooms.size() == 1) {
        summary = m_balloonQueue.last();
    } else {
        summary.title = QStringLiteral("Lightning");
        summary.body =
            QCoreApplication::translate("Notifications", "%n new messages",
                                        nullptr, messages)
            + QLatin1Char(' ')
            + QCoreApplication::translate("Notifications", "in %n rooms",
                                          nullptr, int(rooms.size()));
        summary.payload = QVariantMap{
            { QStringLiteral("roomId"), QString() },
            { QStringLiteral("eventId"), QString() },
            { QStringLiteral("threadRootId"), QString() },
            { QStringLiteral("accountUserId"),
              accounts.size() == 1 ? *accounts.cbegin() : QString() },
        };
        QSet<QString> keys;
        for (const Balloon &balloon : std::as_const(m_balloonQueue))
            keys.insert(balloon.accountKey);
        summary.accountKey = keys.size() == 1 ? *keys.cbegin() : QString();
    }
    summary.platformSound = platformSound;
    qCInfo(lcNotify) << "tray balloon summary messages=" << messages
                     << "rooms=" << rooms.size();
    if (!deliverThroughTray(summary.title, summary.body, summary.payload,
                            summary.avatar, summary.platformSound,
                            summary.accountKey)) {
        qCInfo(lcNotify) << "notification service unavailable";
        return;
    }
    m_shownEntries = std::exchange(m_balloonQueue, {});
}

void NotificationManager::onBalloonHoldExpired()
{
    m_lastFallbackPayload.clear();
    m_lastFallbackAccountKey.clear();
    m_balloonMayBeVisible = false;
    m_shownEntries.clear();
    showPendingBalloons();
}

void NotificationManager::retireBalloonOnScreen()
{
    m_lastFallbackPayload.clear();
    m_lastFallbackAccountKey.clear();
    m_balloonHold.stop();
    m_balloonIsCall = false;
    m_shownEntries.clear();
    if (std::exchange(m_balloonMayBeVisible, false) && m_fallbackTray)
        m_fallbackTray->hideMessage();
}

void NotificationManager::onFallbackMessageClicked()
{
    const QVariantMap payload = std::exchange(m_lastFallbackPayload, {});
    const QString accountKey = std::exchange(m_lastFallbackAccountKey, {});
    const qsizetype rooms = m_balloonRooms.size();
    m_balloonRooms.clear();
    m_balloonHold.stop();
    m_balloonMayBeVisible = false;
    m_balloonIsCall = false;
    m_shownEntries.clear();
    qCInfo(lcNotify) << "tray balloon clicked payload="
                     << (!payload.isEmpty()) << "room="
                     << !payload.value(QStringLiteral("roomId")).toString().isEmpty()
                     << "rooms=" << rooms << "pending=" << m_balloonQueue.size();
    if (!balloonGate()) {
        emitOpenFor(rooms > 1 ? QVariantMap{} : payload);
        return;
    }
    emitOpenFor(payload);
    const QString roomId =
        routableRoomId(payload.value(QStringLiteral("roomId")).toString());
    if (!roomId.isEmpty()) {
        m_balloonQueue.removeIf([&](const Balloon &waiting) {
            return waiting.accountKey == accountKey
                && routableRoomId(waiting.payload
                                      .value(QStringLiteral("roomId"))
                                      .toString())
                    == roomId;
        });
    }
    showPendingBalloons();
}

void NotificationManager::emitOpenFor(const QVariantMap &payload)
{
    const QString roomId =
        routableRoomId(payload.value(QStringLiteral("roomId")).toString());
    const QString eventId = payload.value(QStringLiteral("eventId")).toString();
    const QString threadRootId = routableThreadRootId(
        payload.value(QStringLiteral("roomId")).toString(),
        payload.value(QStringLiteral("threadRootId")).toString());
    Q_EMIT openRequestedForAccount(
        m_liveAccountsMode
            ? openAccountFor(payload)
            : payload.value(QStringLiteral("accountUserId")).toString(),
        roomId, eventId, threadRootId);
    Q_EMIT openRequested(roomId, eventId, threadRootId);
}

void NotificationManager::onActivationToken(quint32 id, const QString &token)
{
    // Only for our cards; the signal can reach every listener on the bus.
    if (id == 0 || token.isEmpty())
        return;
    if (!tableHolding(id) && !m_tombstones.contains(id)
        && id != m_activeCallNotificationId)
        return;
    m_activationTokens.insert(id, token);
    while (m_activationTokens.size() > kMaxActivationTokens)
        m_activationTokens.erase(m_activationTokens.begin());
}

void NotificationManager::applyActivationToken(quint32 id)
{
    const QString token = m_activationTokens.take(id);
    m_lastClickHadToken = !token.isEmpty();
    if (token.isEmpty())
        return;
    // As for a second launch (main.cpp): Qt's Wayland shell integration reads
    // and clears the variable when the window next activates.
    if (m_assumeWayland
        || QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}

void NotificationManager::onActionInvoked(quint32 id, const QString &action)
{
    if (action == QLatin1String("accept")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty()) {
            const QString callId = m_activeCallId;
            applyActivationToken(id);
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
    if (action == QLatin1String("silence")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty() && m_activeCallSilenceOffered) {
            Q_EMIT callSilenceRequested(m_activeCallId);
        }
        return;
    }
    if (action == QLatin1String("mark-read")) {
        const QVariantMap payload = payloadFor(id);
        if (payload.isEmpty()) {
            handleStaleAction(id, action, QString());
            return;
        }
        forgetPayload(id);
        closeNotification(id);
        Q_EMIT markReadRequested(
            payload.value(QStringLiteral("accountUserId")).toString(),
            routableRoomId(payload.value(QStringLiteral("roomId")).toString()),
            payload.value(QStringLiteral("eventId")).toString());
        return;
    }
    if (action == QLatin1String("inline-reply"))
        return;
    if (action != QLatin1String("default"))
        return;
    const QVariantMap payload = payloadFor(id);
    if (payload.isEmpty()) {
        handleStaleAction(id, action, QString());
        return;
    }
    applyActivationToken(id);
    forgetPayload(id);
    if (id != m_activeCallNotificationId)
        closeNotification(id);
    emitOpenFor(payload);
}

void NotificationManager::onNotificationReplied(quint32 id, const QString &text)
{
    const QVariantMap payload = payloadFor(id);
    if (payload.isEmpty()) {
        handleStaleAction(id, QStringLiteral("inline-reply-text"), text);
        return;
    }
    forgetPayload(id);
    closeNotification(id);
    const QString body = text.trimmed();
    if (body.isEmpty())
        return;
    Q_EMIT replyRequested(
        payload.value(QStringLiteral("accountUserId")).toString(),
        routableRoomId(payload.value(QStringLiteral("roomId")).toString()),
        routableThreadRootId(
            payload.value(QStringLiteral("roomId")).toString(),
            payload.value(QStringLiteral("threadRootId")).toString()),
        body);
}

void NotificationManager::handleStaleAction(quint32 id, const QString &action,
                                            const QString &text)
{
    const auto tomb = m_tombstones.constFind(id);
    if (tomb == m_tombstones.cend())
        return;
    const QString account = tomb.value();
    m_tombstones.remove(id);
    m_tombstoneOrder.removeOne(id);
    closeNotification(id);
    const bool reply = action == QLatin1String("inline-reply-text");
    qCInfo(lcNotify) << "action on a withdrawn notification:"
                     << (reply ? QStringLiteral("reply") : action);
    if (action == QLatin1String("default")) {
        applyActivationToken(id);
        emitOpenFor({});
        return;
    }
    const QString body = text.trimmed();
    if (reply && body.isEmpty())
        return;
    if (account.isEmpty() || account != m_accountUserId) {
        if (reply)
            Q_EMIT replyRequested(account, QString(), QString(), body);
        else
            Q_EMIT markReadRequested(account, QString(), QString());
        return;
    }
    showStaleActionNotice();
}

void NotificationManager::showStaleActionNotice()
{
    showGeneric(tr("Lightning"),
                tr("That notification is out of date, so nothing was sent. "
                   "Open Lightning and try again from there."));
}

void NotificationManager::onNotificationClosed(quint32 id, quint32 reason)
{
    if (id != 0 && id == m_activeCallNotificationId) {
        if (reason == kClosedExpired) {
            m_activeCallCardExpired = true;
            m_callRingTimer.stop();
            return;
        }
        const QString callId = m_activeCallId;
        m_activeCallNotificationId = 0;
        m_activeCallCardExpired = false;
        m_callRingTimer.stop();
        forgetPayload(id);
        if (reason == kClosedDismissed && !callId.isEmpty())
            Q_EMIT callCardDismissed(callId);
        return;
    }
    if (reason == kClosedExpired)
        return;
    forgetPayload(id);
    if (m_tombstones.remove(id) > 0)
        m_tombstoneOrder.removeOne(id);
}

void NotificationManager::onDaemonPropertiesChanged(
    const QString &interfaceName, const QVariantMap &changed,
    const QStringList &invalidated)
{
    if (interfaceName != kInterface)
        return;
    const auto inhibited = changed.constFind(QStringLiteral("Inhibited"));
    if (inhibited != changed.cend()) {
        if (m_daemonInhibited != inhibited->toBool())
            qCInfo(lcNotify) << "desktop do-not-disturb ="
                             << inhibited->toBool();
        m_daemonInhibited = inhibited->toBool();
    } else if (invalidated.contains(QStringLiteral("Inhibited"))) {
        queryDesktopInhibition();
    }
}

void NotificationManager::onDaemonOwnerChanged(const QString &service,
                                               const QString &oldOwner,
                                               const QString &newOwner)
{
    Q_UNUSED(service);
    Q_UNUSED(oldOwner);
    qCInfo(lcNotify) << "notification service changed hands; running:"
                     << !newOwner.isEmpty();
    ++m_daemonGeneration;
    for (auto it = m_tables.begin(); it != m_tables.end(); ++it) {
        it->pendingPayloads.clear();
        it->payloadOrder.clear();
        it->groups.clear();
    }
    m_tombstones.clear();
    m_tombstoneOrder.clear();
    m_activationTokens.clear();
    m_activeCallNotificationId = 0;
    m_activeCallCardExpired = false;
    m_callNotifyInFlight = false;
    m_callRedrawPending = false;
    m_bodyMarkup = false;
    m_bodyMarkupKnown = false;
    m_inlineReply = false;
    m_capabilityRequestInFlight = false;
    m_capabilityRequests = 0;
    m_capabilityRetryAtMs = 0;
    m_serverInfoInFlight = false;
    m_serverInfoKnown = false;
    m_residentCards = false;
    m_daemonInhibited = false;
    m_inhibitionQueried = false;
    if (newOwner.isEmpty())
        return;
#ifdef HAVE_QT_DBUS
    if (!m_capabilityWaits.isEmpty())
        requestCapabilities();
#endif
    if (!m_activeCallId.isEmpty())
        deliverCallNotification();
}

QStringList NotificationManager::callActions(bool acceptOffered, bool rtcLane,
                                             bool silenceOffered)
{
    QStringList actions{ QStringLiteral("default"), tr("Open") };
    if (acceptOffered) {
        actions << QStringLiteral("accept")
                << (rtcLane ? tr("Join") : tr("Answer"));
    }
    if (silenceOffered)
        actions << QStringLiteral("silence") << tr("Silence");
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
    const bool showing =
        m_activeCallNotificationId != 0 && !m_activeCallCardExpired;
    if (showing || m_callNotifyInFlight)
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
    if (!m_activeCallId.isEmpty() && m_activeCallId != callId)
        stopIncomingCall(m_activeCallId);
    if (m_activeCallId != callId)
        m_activeCallTrayDelivered = false;
    m_activeCallId = callId;
    m_activeCallRoomId = roomId;
    m_activeCallTitle = title;
    m_activeCallBody = safeBody;
    m_activeCallSound = sound;
    m_activeCallAcceptOffered = acceptOffered;
    m_activeCallRtcLane = rtcLane;
    m_activeCallSilenceOffered = silenceOffered;
    m_activeCallCardExpired = false;
    m_callRingDeadlineMs = nowMs() + qint64(qBound(5, ringSeconds, 300)) * 1000;
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
    m_activeCallSound = false;
    m_activeCallSilenceOffered = false;
    m_callRingTimer.stop();
    if (m_activeCallNotificationId != 0 && !m_activeCallCardExpired)
        deliverCallNotification();
}

void NotificationManager::stopIncomingCall(const QString &callId)
{
    if (m_activeCallId.isEmpty()
        || (!callId.isEmpty() && callId != m_activeCallId))
        return;
    m_callRingTimer.stop();
    if (m_activeCallNotificationId != 0) {
        closeNotification(m_activeCallNotificationId);
        forgetPayload(m_activeCallNotificationId);
    }
    m_activeCallNotificationId = 0;
    m_activeCallCardExpired = false;
    m_callRedrawPending = false;
    m_callWaitsForCapabilities = false;
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
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCInfo(lcNotify) << "notification service unavailable";
        deliverCallThroughTray();
        return;
    }
    if (!m_bodyMarkupKnown && requestCapabilities()) {
        m_callWaitsForCapabilities = true;
        return;
    }
    if (m_callNotifyInFlight) {
        m_callRedrawPending = true;
        return;
    }
    const QStringList actions = callActions(m_activeCallAcceptOffered,
                                            m_activeCallRtcLane,
                                            m_activeCallSilenceOffered);
    const NotificationManager::DesktopIdentity identity = notificationIdentity();
    QVariantMap hints{
        { QStringLiteral("desktop-entry"), identity.desktopEntry },
        { QStringLiteral("urgency"), quint8(2) },
    };
    const QVariantMap soundHintMap = callSoundHints(m_activeCallSound);
    for (auto it = soundHintMap.cbegin(); it != soundHintMap.cend(); ++it)
        hints.insert(it.key(), it.value());
    QDBusMessage call = notificationsCall(QStringLiteral("Notify"));
    call << QStringLiteral("Lightning") << m_activeCallNotificationId
         << identity.appIcon << m_activeCallTitle
         << bodyForServer(m_activeCallBody) << actions << hints << int(-1);
    m_callNotifyInFlight = true;
    auto *watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(call, kNotifyTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, callId = m_activeCallId,
             daemon = m_daemonGeneration] {
        watcher->deleteLater();
        ++m_notifyReplies;
        if (daemon != m_daemonGeneration)
            return;
        m_callNotifyInFlight = false;
        const QDBusPendingReply<quint32> reply = *watcher;
        if (callId != m_activeCallId) {
            if (!reply.isError())
                closeNotification(reply.value());
        } else if (!reply.isError()) {
            if (m_activeCallNotificationId != 0
                && reply.value() != m_activeCallNotificationId)
                forgetPayload(m_activeCallNotificationId);
            m_activeCallNotificationId = reply.value();
            m_activeCallCardExpired = false;
            recordPayload(QString(), m_activeCallNotificationId,
                          QVariantMap{
                              { QStringLiteral("roomId"), m_activeCallRoomId },
                              { QStringLiteral("eventId"), QString() },
                              { QStringLiteral("threadRootId"), QString() },
                              { QStringLiteral("accountUserId"),
                                m_accountUserId },
                          });
        } else {
            if (reply.error().type() == QDBusError::ServiceUnknown)
                qCInfo(lcNotify) << "notification service unavailable";
            else
                qCWarning(lcNotify) << "notification server refused the call card:"
                                    << reply.error().name();
            m_callRedrawPending = false;
            deliverCallThroughTray();
            return;
        }
        if (std::exchange(m_callRedrawPending, false))
            deliverCallNotification();
    });
    return;
#endif
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
        { QStringLiteral("accountUserId"), m_accountUserId },
    };
    m_lastCallTrayPayload = payload;
    requeueDisplacedBalloon();
    if (!deliverThroughTray(m_activeCallTitle, m_activeCallBody, payload,
                            QImage(), m_activeCallSound, QString())) {
        qCInfo(lcNotify) << "incoming call could not be announced: no "
                            "notification service and no tray balloon";
        return false;
    }
    m_balloonIsCall = true;
    return true;
}
