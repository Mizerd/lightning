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
// Undecryptable events remembered per session for a late decryption, and how
// long one stays news: a key that arrives after this re-raises nothing.
constexpr int kMaxLiveUndecryptable = 256;
constexpr qint64 kUndecryptableMemoryMs = 10 * 60 * 1000;
constexpr int kMaxActivationTokens = 16;
// freedesktop NotificationClosed reasons.
constexpr quint32 kClosedExpired = 1;
constexpr quint32 kClosedDismissed = 2;
// Separates the parts of a card group key; never in a room id or event id.
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

// Plain text of an HTML body with every spoiler replaced by `placeholder`.
// A span carrying data-mx-spoiler hides everything up to its matching close,
// nested spans included; an unterminated spoiler hides the rest. Other tags
// are dropped, block breaks become newlines (the caller collapses them).
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

// What a message says, for the one line a card shows: an edit's new text,
// never the "* " fallback, and a spoiler as a placeholder, never its text.
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

// The gates that do not depend on what the message says: a message refused
// by one of these is not news whatever its plaintext turns out to be.
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
    // A room settling after being opened delivers its recent HISTORY as live
    // appends (backlog=false), and the room's backup download decrypts them a
    // few seconds later; a message already on screen at the latest was read.
    // Neither is news once its key arrives, whatever the window is doing by
    // then, so neither may be remembered as a live arrival to decide later.
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
// The Do Not Disturb state is read again at most this often on delivery, for
// a desktop whose change signal is not followed (GNOME's banners setting).
constexpr qint64 kInhibitionRefreshMs = 30000;
// A daemon not up yet (autostart order) or not answering is asked again,
// after NotificationManager::m_capabilityRetryMs, this many times per session.
constexpr int kMaxCapabilityRequests = 4;
// Deliveries parked until the capabilities answer; a burst past this is
// dropped rather than queued without bound.
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

// A variant that may be wrapped once more (the settings portal's deprecated
// Read returns `v` inside `v`).
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
} // namespace

#ifdef HAVE_QT_DBUS
// The notification's app icon and desktop entry; see desktopIdentityFor().
// When no themed icon resolves, the bundled icon is written to the cache once
// and passed as a file:// URI, which the freedesktop spec accepts (source
// runs, AppImages).
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
    // The foreground account's table always exists, so every card's
    // accountKey names a table.
    m_tables.insert(QString(), AccountTable{});
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
    // Spec 1.2: sent just before ActionInvoked, so the raise may take focus
    // on a compositor that enforces xdg-activation.
    bus.connect(kService, kPath, kInterface,
                QStringLiteral("ActivationToken"), this,
                SLOT(onActivationToken(quint32,QString)));
    // KDE's Do Not Disturb (the `Inhibited` property).
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
            [this] { flushAvatarWaits(/*fallbackAll=*/true); });
    m_utdHoldTimer.setSingleShot(true);
    connect(&m_utdHoldTimer, &QTimer::timeout, this,
            &NotificationManager::flushHeldUndecryptable);
    // Re-deliver the ringing card every few seconds so the themed sound
    // repeats.
    m_callRingTimer.setInterval(5000);
    connect(&m_callRingTimer, &QTimer::timeout, this,
            &NotificationManager::onCallRingTick);
    // How long one balloon keeps the screen; see offerBalloon().
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
    // So is a room's recent history re-delivered after the initial sync (its
    // first subscription on this device, or a gap), even with a Notify
    // verdict.
    if (event.backlog)
        return decision;
    // A redacted message has nothing left to say; its text is gone.
    if (event.redacted)
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
    // The account's push rules, as the SDK evaluated them, decide when they
    // came with the event: they carry a mode set on any client, the account
    // default, keywords (with or without a highlight) and the suppress rules
    // for notices and edits, where the local mode knows only what this device
    // chose (issue #15). Muted, above, still silences first.
    const bool rulesKnown =
        event.pushVerdict != TimelineEvent::PushVerdict::Unknown
        && !context.localModeUnsynced;
    // A mention is what the rules call a highlight: they check @room against
    // the sender's power level and honour the user's own Off for @room and
    // for their name. Without them only m.mentions naming this user counts;
    // m.mentions.room is the sender's own claim, with no power-level check
    // here.
    const bool mention = rulesKnown
        ? event.pushHighlight
        : (event.mentionsMe || event.pushHighlight);
    if (rulesKnown) {
        if (event.pushVerdict == TimelineEvent::PushVerdict::Quiet)
            return decision;
    } else {
        // The spec's default suppress rules, which the account's rules would
        // have applied: an edit notifies only when it newly mentions this
        // user, a bot's m.notice never.
        if (event.isEdit && !event.mentionsMe)
            return decision;
        if (event.type == TimelineEvent::Notice)
            return decision;
        if (context.roomMode == MentionsOnly && !mention)
            return decision;
    }
    // A room on screen, focused and at the latest message needs no
    // notification.
    if (context.roomVisibleAtLatest)
        return decision;
    // Same for a room that is still settling after being opened.
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
            const QString text =
                matrix::preview::normalizePreviewText(messageText(event))
                    .left(160);
            // An emote reads as the timeline renders it: "* Alice waves".
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

    // Sound only applies to notifications that passed the checks above; the
    // mode narrows which of them make a sound. With the account's rules known
    // the default mode follows their sound tweak, which is what "Loud" and
    // "Notify" on the rules page set (DMs and mentions are Loud by default);
    // Off and All are this device's overrides.
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
    // The event's name comes from two places that disagree for a sender the
    // member list does not know yet: the SDK timeline leaves it empty, and
    // the background-sync path fills in the bare user id
    // (RustSdkMatrixClient::displayNameFor). Both mean "no name", or one
    // sender would read "@six:example.org in A" in one card and "six in B" in
    // the next (live Windows test, 2026-10-16).
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
    // A room with no open timeline has no SDK aggregate to send through
    // processUpdated(): its edits arrive here raw, and the card showing the
    // original follows them. The edit itself notifies only if decide() says
    // so (one that newly mentions this user), as before.
    if (event.isEdit)
        refreshFromEdit(event, context);
    const Decision decision = decide(event, context);
    // A message the SDK could not decrypt yet usually has its key a moment
    // later. Held briefly, so the card carries the text (or, for a mention in
    // a mentions-only room, exists at all) instead of "Encrypted message".
    // Only live arrivals are remembered: a history backfill decrypting later
    // must not notify.
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
            return; // already held or delivered: never twice
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
                    /*replacement=*/false);
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
                            /*undecryptable=*/true, /*replacement=*/false);
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
    // An edit that arrived undecryptable could not refresh its card then.
    if (event.isEdit)
        refreshFromEdit(event, context);
    const auto live = m_liveUtds.find(event.eventId);
    // Never seen live while undecryptable (history, a previous account), or
    // already decided: nothing to decide again.
    if (live == m_liveUtds.end() || live->state == UtdState::Decided)
        return;
    const LiveUtd seen = live.value();
    // Kept, so neither a second decryption report nor the same event
    // appended again can notify twice.
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
        // Its ciphertext did not notify (to the rules a mention in a
        // mentions-only room is just an encrypted message); the plaintext
        // decides now.
        if (decision.notify)
            deliverDecision(event, context, decision, false, false);
        return;
    }
    // Delivered as "Encrypted message". Not shown yet (waiting for an avatar
    // or the capabilities): it leaves with the text instead, alert and all.
    // Only the account the event arrived for: two live accounts can share a
    // room, and so an event.
    const QString accountKey = context.accountUserId;
    const auto sameEvent = [&event, &accountKey](const Card &card) {
        return card.accountKey == accountKey
            && card.payload.value(QStringLiteral("eventId")).toString()
                == event.eventId;
    };
    qsizetype unshown = m_avatarWaits.removeIf(sameEvent)
        + m_capabilityWaits.removeIf(sameEvent);
    // Or waiting behind its room's card, which is still being raised.
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
        // It was counted once already; it is delivered again, not anew.
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
    // Shown: replaced in place and silently (it has alerted once), or
    // withdrawn if the plaintext would not have notified. A card that has
    // since moved on to a newer message stays as it is.
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
        return; // a newer card is on its way and replaces it anyway
    if (!decision.notify) {
        closeNotification(cardId);
        forgetPayload(cardId);
        return;
    }
    deliverDecision(event, context, decision, false, /*replacement=*/true);
}

namespace {
// One card per room stands for every message in it: past the first, the body
// says how many (or, with the text shown, stays the latest line). Shared by
// every path that writes a card's body so a refresh cannot erase the count.
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
} // namespace

void NotificationManager::processUpdated(const TimelineEvent &event,
                                         const Context &context)
{
    // Raw m.replace events have not been validated against their target here
    // (processEvent() validates them, see refreshFromEdit()). The SDK's
    // aggregated row is authoritative, including edit removal.
    if (event.eventId.isEmpty() || event.isEdit || event.undecryptable)
        return;
    refreshCard(event, context);
}

bool NotificationManager::refreshFromEdit(const TimelineEvent &edit,
                                          const Context &context)
{
    // What the SDK checks before it aggregates an edit, against what the card
    // knows: the target is the event a live card shows, in the same room; the
    // editor is that event's sender (the spec's rule, so another member's
    // m.replace cannot put words in someone's mouth); m.new_content is
    // present. Undecryptable or redacted edits carry nothing to show.
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
    // Edits of one message are applied newest last; one delivered late (a
    // sliding-sync reset re-delivering) must not bring older text back.
    const qint64 stamp =
        edit.timestamp.isValid() ? edit.timestamp.toMSecsSinceEpoch() : 0;
    if (stamp < group->editStampMs)
        return true;
    group->editStampMs = stamp;
    // The original as edited. Its verdict is the one that raised the card
    // (refreshCard keeps it for an Unknown one); the edit's own is the
    // suppress-edits rule's, which says nothing about the message.
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
        return; // absent, dismissed, or already replaced by a newer message
    // An "Encrypted message" card changes only through processDecrypted(),
    // which owns the UTD transition (hold, count, replace-or-withdraw). The
    // bridge reports the Set before the decryption, so without this the Set
    // would replace the card first and processDecrypted() would then find a
    // replacement in flight and stop, whatever the plaintext's verdict.
    if (group->payload.value(QStringLiteral("utd")).toBool())
        return;
    // The existing card already passed the account's push rules. SDK Set
    // diffs often omit that verdict (e.g. profile/receipt updates), so an
    // UNKNOWN verdict keeps the one that raised the card. A known verdict is
    // authoritative, Quiet included: it withdraws the card below.
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
    // The card the group shows stands for `count` messages: format with that
    // before comparing, or an unchanged Set turns "2 new ..." back into one.
    decision.body = bodyForCount(decision, current, context, group->count);
    if (group->title == decision.title && group->body == decision.body)
        return; // receipts, reactions and profile-only Set diffs
    group->title = decision.title;
    group->body = decision.body;
    // Presentation follows the CURRENT privacy policy, not the one the card
    // was queued under: a Private card carries no room avatar or initials.
    const bool privateMode = context.previewMode == Private;
    const auto matches = [&](const Card &card) {
        return card.accountKey == accountKey
            && card.payload.value(QStringLiteral("eventId")).toString() == event.eventId;
    };
    const auto refresh = [&](Card &card, bool avatarResolved) {
        card.title = decision.title;
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
            refresh(card, /*avatarResolved=*/false);
            waiting = true;
        }
    }
    for (auto &card : m_capabilityWaits) {
        if (matches(card)) {
            refresh(card, /*avatarResolved=*/true);
            waiting = true;
        }
    }
    if (group->next && matches(*group->next)) {
        refresh(*group->next, /*avatarResolved=*/true);
        waiting = true;
    }
    for (auto &balloon : m_balloonQueue) {
        if (balloon.accountKey == accountKey
            && balloon.payload.value(QStringLiteral("eventId")).toString() == event.eventId) {
            balloon.title = decision.title;
            balloon.body = decision.body;
            if (privateMode)
                balloon.avatar = QImage();
            waiting = true;
        }
    }
    // The balloon on screen (the one-balloon gate, Windows) cannot be
    // updated in place, so while it is still the one showing this message it
    // is shown again with the new text, silently. One that has had its time
    // is in the platform's history and stays as it is, and a summary of
    // several rooms carries no message text.
    bool balloonShowsIt = false;
    for (auto &entry : m_shownEntries) {
        if (entry.accountKey == accountKey
            && entry.payload.value(QStringLiteral("eventId")).toString() == event.eventId) {
            entry.title = decision.title;
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
                               entry.avatar, /*platformSound=*/false,
                               entry.accountKey)) {
            qCInfo(lcNotify) << "tray balloon shown again with an edit";
            // noteBalloon() cleared it; it is still the one on screen.
            m_shownEntries = { std::move(entry) };
        }
    }
    if (!waiting && (group->id != 0 || group->inFlight))
        deliverDecision(event, context, decision, false, /*replacement=*/true);
}

void NotificationManager::deliverDecision(const TimelineEvent &event,
                                          const Context &context,
                                          const Decision &decision,
                                          bool undecryptable, bool replacement)
{
    const QString roomId = routableRoomId(event.roomId);
    const QString threadRootId =
        routableThreadRootId(event.roomId, event.threadRootId);
    // The room list may not show this room unread yet; see observeRoomUnread().
    if (!replacement)
        m_roomsAwaitingUnread.insert(roomId, nowMs() + m_awaitUnreadMs);
    const QString groupKey = messageGroupKey(roomId, threadRootId);
    // The table this card lives in; "" is the foreground account's.
    const QString accountKey = context.accountUserId;
    int count = 0;
    {
        CardGroup &group = table(accountKey).groups[groupKey];
        if (!replacement)
            ++group.count;
        count = group.count;
    }
    // One card per room: past the first message it stands for all of them,
    // a replacement included (it is the same card, not a new message).
    const QString body = bodyForCount(decision, event, context, count);
    QVariantMap payload;
    // Normalised where the payload is built, so every consumer sees a real room
    // id.
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), event.eventId);
    payload.insert(QStringLiteral("threadRootId"), threadRootId);
    // The owning account; actions taken after a switch are checked against
    // it. The foreground's (empty context account) is the one set on this
    // manager, as it always was.
    payload.insert(QStringLiteral("accountUserId"),
                   accountKey.isEmpty() ? m_accountUserId : accountKey);
    // Selects the mention chime.
    payload.insert(QStringLiteral("mention"), decision.mention);
    payload.insert(QStringLiteral("group"), groupKey);
    // A card a late decryption may replace in place.
    if (undecryptable)
        payload.insert(QStringLiteral("utd"), true);
    auto &currentGroup = table(accountKey).groups[groupKey];
    currentGroup.payload = payload;
    currentGroup.title = decision.title;
    currentGroup.body = body;
    currentGroup.sender = event.sender;
    if (!replacement)
        currentGroup.editStampMs = 0;

    Card card;
    card.title = decision.title;
    card.body = body;
    card.payload = payload;
    card.sound = decision.playSound && !replacement;
    card.replacement = replacement;
    card.accountKey = accountKey;
    // Private preview keeps the generic app identity: a room avatar or initials
    // disc would disclose the conversation.
    if (context.previewMode != Private) {
        // Draw the initials disc when there is no avatar, as other surfaces do.
        card.fallback = lightning::notifications::fallbackAvatar(
            context.roomName, context.avatarColorKey, kFallbackAvatarEdge,
            context.themeId);
        card.avatarMxc = context.avatarMxc;
    }
    deliver(std::move(card));
}

void NotificationManager::showGeneric(const QString &title,
                                      const QString &safeBody,
                                      const QString &roomId,
                                      const QString &avatarMxc, bool sound,
                                      const QString &key)
{
    ++m_genericNoticeCount;
    QVariantMap payload;
    payload.insert(QStringLiteral("roomId"), roomId);
    payload.insert(QStringLiteral("eventId"), QString{});
    payload.insert(QStringLiteral("threadRootId"), QString{});
    payload.insert(QStringLiteral("accountUserId"), m_accountUserId);
    // Generic notices are the foreground account's: the "" table.
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
    // A redacted message must not surface later either.
    m_heldUtds.removeIf([&eventId](const HeldUtd &held) {
        return held.event->eventId == eventId;
    });
    if (m_heldUtds.isEmpty())
        m_utdHoldTimer.stop();
    if (m_liveUtds.remove(eventId) > 0)
        m_liveUtdOrder.removeOne(eventId);
    // Every account's: a redaction removes the message for all of them.
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
    // The ring is the foreground account's and process-wide; another
    // account's clear never stops it.
    if (foreground)
        stopIncomingCall(QString());
    // The account's cards (and their previews) go with it. An action on one
    // that still arrives meets its tombstone, and the app's refusal.
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
    // A Notify still in flight belongs to the cleared account; its reply must
    // not record a click payload, and its card is closed on arrival.
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
        // The tray balloon's payload is a pending click too; clear it so a
        // balloon left on screen cannot route into the previous account's
        // room.
        m_balloonQueue.clear();
        m_balloonRooms.clear();
        retireBalloonOnScreen();
        return;
    }
    // Another account: only what is tagged with it. Its withdrawn cards'
    // activation tokens, its undelivered cards, and its held messages, which
    // are forgotten as live arrivals too so a late decryption raises nothing.
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
    // A balloon this account raised cannot route into it any more; a click
    // still raises the window. What it had waiting goes too.
    const auto ownBalloon = [&accountUserId](const Balloon &balloon) {
        return balloon.accountKey == accountUserId;
    };
    m_balloonQueue.removeIf(ownBalloon);
    // Including what a summary on screen stands for, so no break-through
    // re-queues it.
    m_shownEntries.removeIf(ownBalloon);
    if (m_lastFallbackAccountKey == accountUserId
        && !m_lastFallbackPayload.isEmpty()) {
        // Whatever else it stood for waits again, and goes out at once.
        const QList<Balloon> rest = std::exchange(m_shownEntries, {});
        retireBalloonOnScreen();
        for (Balloon entry : rest) {
            entry.platformSound = false;
            enqueueBalloon(std::move(entry), /*older=*/true);
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
    // A message can notify before the room list's unread fields catch up, and
    // that stale "read" withdrew the card it had just raised. Wait until the
    // list has shown the room unread since its latest card, or a few seconds
    // (a room read before the list ever showed it unread).
    const auto awaiting = m_roomsAwaitingUnread.constFind(roomId);
    if (awaiting != m_roomsAwaitingUnread.cend() && nowMs() < awaiting.value())
        return;
    m_roomsAwaitingUnread.remove(roomId);
    // Only the room's own messages: its unread fields never count a thread
    // reply (matrix-sdk with threading support skips them) and say nothing
    // about a notice such as a missed call, so "read" says nothing about
    // those either. The foreground account's room list, so its table.
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

void NotificationManager::closeRoomNotifications(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    m_roomsAwaitingUnread.remove(roomId);
    // A message still held for its key belongs to the room just read. The
    // foreground account's room, so only its messages and its table.
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
    // Every card's accountKey names a table (the constructor makes "", and
    // a card's table is made before the card), so this reaches every wait.
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
    // Deliveries not shown yet: waiting for an avatar or the capabilities.
    const auto cardMatches = [&match, &accountKey](const Card &card) {
        return card.accountKey == accountKey && match(card.payload);
    };
    // And messages waiting behind the balloon on screen: a room that was
    // read, or a card withdrawn, is not news for the summary either.
    const qsizetype dropped = m_avatarWaits.removeIf(cardMatches)
        + m_capabilityWaits.removeIf(cardMatches)
        + m_balloonQueue.removeIf([&match, &accountKey](const Balloon &b) {
              return b.accountKey == accountKey && match(b.payload);
          });
    // What the balloon on screen stands for, too: a break-through would
    // otherwise put a read room back into the summary.
    m_shownEntries.removeIf([&match, &accountKey](const Balloon &b) {
        return b.accountKey == accountKey && match(b.payload);
    });
    if (dropped > 0) {
        // Count only, so it is distinguishable from never having raised one.
        qCInfo(lcNotify) << "dropped" << dropped
                         << "notification(s) not shown yet";
        if (m_avatarWaits.isEmpty())
            m_avatarWaitTimer.stop();
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
    // The ring owns its own lifetime; reading its room must not silence it.
    stale.removeOne(m_activeCallNotificationId);
    for (const quint32 id : std::as_const(stale)) {
        closeNotification(id);
        forgetPayload(id);
    }
    // Cards still being raised are closed when their id arrives; a group with
    // nothing shown is forgotten, so its count starts again.
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
    if (m_tombstones.remove(id) > 0)
        m_tombstoneOrder.removeOne(id);
    // A daemon id names one card, so it is in one table only. Never true
    // with a single account; kept so lookups by id cannot be ambiguous.
    for (auto it = m_tables.begin(); it != m_tables.end(); ++it) {
        if (it.key() != accountKey && it->pendingPayloads.remove(id) > 0)
            it->payloadOrder.removeOne(id);
    }
    AccountTable &own = table(accountKey);
    // Re-recording promotes an id to most recent, so a refreshed call card is
    // not evicted first.
    own.payloadOrder.removeOne(id);
    own.payloadOrder.append(id);
    own.pendingPayloads.insert(id, payload);
    // Evict the table's oldest past the cap so recent notifications stay
    // clickable; another account's cards are not this one's to evict. The
    // evicted card is withdrawn rather than left in the history, where a
    // click would do nothing and no read could ever remove it.
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
        // forgetPayload() only removes what it finds; never loop forever.
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
    // The card is gone, and so is any in-place update still waiting for its
    // avatar or the capabilities: delivering it would raise a new popup.
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
    // The card is gone; the next message raises a new one. One still on its
    // way keeps the group (and its count) for its answer.
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
    // A daemon that no longer had the card it was asked to replace raises a
    // new one; the old id is gone.
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
        // Read or redacted while it was being raised.
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
    // Nothing is waited for.
    bus.send(call);
#else
    Q_UNUSED(id);
#endif
}

void NotificationManager::deliver(Card card)
{
    if (!card.avatarMxc.startsWith(QLatin1String("mxc://")) || !m_avatarImage) {
        card.avatar = card.fallback;
        deliverNow(std::move(card));
        return;
    }
    const QImage cached = m_avatarImage(card.avatarMxc, /*request=*/true);
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
    // Bounded: if the avatar stalls, the notification appears with the fallback
    // after a short grace period. Per account: one busy account flushes its
    // own oldest, never another's.
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
            ? m_avatarImage(waiting.avatarMxc, /*request=*/false) : QImage{};
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
    // Mark-as-read and Reply need a real event: generic notices (invites,
    // verification requests) have no eventId and cannot be read or replied to.
    if (actionable) {
        shape.actions << QStringLiteral("mark-read") << tr("Mark as read");
        if (inlineReplySupported) {
            // The action id is fixed by the extension; the label is ours.
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
    // Asked first, so on one daemon its answer arrives before the
    // capabilities release the first delivery and its chime.
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
        // The daemon changed hands meanwhile; the new one is asked anew.
        if (daemon != m_daemonGeneration)
            return;
        m_capabilityRequestInFlight = false;
        const QDBusPendingReply<QStringList> caps = *watcher;
        if (caps.isError()) {
            // Until an answer: escape, since raw member text must never reach
            // a daemon that renders markup, and offer no inline reply.
            m_bodyMarkup = true;
            m_inlineReply = false;
            // Not up yet, or not answering: ask again later, bounded. Any
            // other error is the daemon's answer.
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
            // Offer inline reply only when advertised; otherwise it renders
            // as a button that can never produce text.
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
            // Not up yet: asked again with the next capability request. Any
            // other error is an answer: plain cards.
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
    // Delivered now, with what is known; a retry only serves later ones.
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
    // Plasma reports ("Plasma", "KDE", its own version); 6.4.0 keeps the
    // actions of an expired card that has any (Globals.qml onExpired).
    if (name != QLatin1String("Plasma") || vendor != QLatin1String("KDE"))
        return false;
    const QStringList parts = version.split(QLatin1Char('.'));
    bool majorOk = false;
    bool minorOk = false;
    const int major = parts.value(0).toInt(&majorOk);
    const int minor = parts.value(1).toInt(&minorOk);
    // An unreadable version is treated as an old one: a resident card on any
    // Plasma only means it stays until it is clicked or read.
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
    // KDE: the daemon's `Inhibited` property; PropertiesChanged follows it.
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
        // A daemon without the property (most) answers with an error.
        if (reply.isError())
            return;
        const QVariant value = unwrapVariant(reply.value().variant());
        if (value.metaType() != QMetaType::fromType<bool>())
            return;
        if (m_daemonInhibited != value.toBool())
            qCInfo(lcNotify) << "desktop do-not-disturb =" << value.toBool();
        m_daemonInhibited = value.toBool();
    });
    // GNOME: its daemon has no such property; the banners setting is read
    // through the settings portal, which also works inside a Flatpak. Only
    // on GNOME, so no other session starts a portal for it.
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
        // A portal older than version 2 has only the deprecated Read.
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
    // A replacement updates a card in place and never raises one: it is valid
    // only while the group it was made for still shows (or is raising) that
    // very event, for the same account.
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
    // Replacements wait too (avatar, capabilities, a Notify in flight). The
    // card may have been dismissed, withdrawn or superseded since: dropping
    // is right, and delivering would raise a NEW popup with no replace id.
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
    // The balloon: no session bus, no daemon, or no QtDBus at all (Windows,
    // macOS: a toast on Windows, a user notification on macOS). It carries
    // the plan's platformSound: on Windows TrayIcon raises the balloon with
    // NIIF_NOSOUND unless the desktop's sound was chosen, since Qt's own
    // balloon always made Windows add its sound on top of (or instead of)
    // ours. A balloon cannot be replaced, so an in-place update is dropped.
    const auto throughTray = [this, mention, roomId, isMessage](const Card &c) {
        if (c.replacement)
            return;
        const SoundPlan plan =
            takeSoundPlan(c.sound, mention, trayCarriesSound());
        if (!offerBalloon(Balloon{ c.title, c.body, c.payload, c.avatar,
                                   plan.platformSound, c.accountKey },
                          /*breakThrough=*/mention || roomId.isEmpty()))
            qCInfo(lcNotify) << "notification service unavailable";
        // Delivered or not, the window can still ask for attention.
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
        // The capabilities decide how the body is escaped, the server
        // information whether it is resident; wait for them.
        if (m_capabilityWaits.size() < kMaxCapabilityWaits)
            m_capabilityWaits.append(std::move(card));
        else
            qCWarning(lcNotify) << "notification dropped: daemon not answering";
        return;
    }
    // Do Not Disturb, re-read now and then for a desktop whose change signal
    // is not followed; the answer serves the next delivery.
    if (!m_inhibitionQueried
        || nowMs() - m_inhibitionQueriedAtMs > kInhibitionRefreshMs) {
        m_inhibitionQueried = true;
        m_inhibitionQueriedAtMs = nowMs();
        queryDesktopInhibition();
    }
    // One card per group: replace it in place, and while its first Notify is
    // in flight hold the newest card for the id rather than raise a second.
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
                // The held one never sounded; the one replacing it carries
                // its alert.
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
    // Lightning's chime, or the themed one, or none; the platform's own sound
    // is suppressed unless it is the one that plays.
    const SoundPlan plan = takeSoundPlan(card.sound && !card.replacement,
                                         mention, /*pathCarriesSound=*/true);
    const QVariantMap soundHintMap = soundHints(plan);
    for (auto it = soundHintMap.cbegin(); it != soundHintMap.cend(); ++it)
        hints.insert(it.key(), it.value());
    if (shape.inlineReply) {
        hints.insert(QStringLiteral("x-kde-reply-placeholder-text"),
                     tr("Reply…"));
    }
    // Escaped iff the daemon renders markup; see bodyForServer(). The title is
    // never escaped.
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
        // The daemon changed hands meanwhile: that id is the old daemon's and
        // may already name another application's card on the new one.
        if (daemon != m_daemonGeneration)
            return;
        if (!takeNotifyReply(card, groupKey, generation,
                             reply.isError()
                                 ? std::nullopt
                                 : std::optional<quint32>(reply.value())))
            return;
        // No daemon, or a refusal: log the error name only (never title or
        // body) and fall back to the balloon.
        if (reply.error().type() == QDBusError::ServiceUnknown)
            qCInfo(lcNotify) << "notification service unavailable";
        else
            qCWarning(lcNotify) << "notification server refused the notification:"
                                << reply.error().name();
        if (!card.replacement
            && !offerBalloon(
                Balloon{ card.title, card.body, card.payload, card.avatar,
                         platformSound, card.accountKey },
                /*breakThrough=*/card.payload.value(QStringLiteral("mention"))
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
        return plan; // silent: nothing of ours, nothing of the platform's
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
        // Themed call sound. Not coalesced with message sounds: the repeat is
        // the ring.
        hints.insert(QStringLiteral("sound-name"),
                     QStringLiteral("phone-incoming-call"));
    } else {
        // A silent card, or Lightning's own ringer sounding: no daemon adds
        // its default sound to it, nor to a redraw after Silence.
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
    // macOS: Qt's NSUserNotification sets no soundName; a Linux tray balloon
    // carries no sound of its own (TrayIcon::showMessage).
    return false;
#endif
}

NotificationManager::SoundPlan NotificationManager::takeSoundPlan(
    bool sound, bool mention, bool pathCarriesSound)
{
    // The call state (a call, a share capturing the output mix) and the
    // desktop's Do Not Disturb hold for both sources.
    const bool allowed = sound && (!m_soundAllowed || m_soundAllowed())
        && !desktopInhibited();
    bool coalesced = false;
    if (allowed) {
        // At most one alert per short window, but a mention breaks through an
        // ordinary burst: only another mention coalesces it.
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
        // The desktop's sound was chosen but this delivery cannot make it (a
        // macOS banner, a Linux tray balloon): ours rather than silence.
        plan = planSound(true, false, SourceLightning);
    }
    if (plan.playOwn) {
        const bool played = m_ownSound && m_ownSound(mention);
        qCInfo(lcNotify) << "notification sound own="
                         << (played ? "played" : "not played");
        // The policy already allowed it (checked above), so a chime that did
        // not play is a fault: sounds not loaded, a wedged sound server. The
        // desktop's sound rather than silence, as the ringer falls back. Only
        // with the policy installed: without it the player's refusal may be
        // the policy itself.
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
        // Gate only: the one on screen goes first, or Windows would queue
        // this one behind it and a click on either would reach us as the
        // same signal, resolved to this payload.
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
    // The latest payload is the one a click resolves to: with the gate,
    // until it is clicked, replaced or has had its time; without it, unless
    // balloons for other rooms are outstanding too. The payload names its
    // account, which the click carries (openRequestedForAccount).
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
    // Read per balloon: the user can change Windows' duration at any time.
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

// A balloon's click (QSystemTrayIcon::messageClicked) carries no id, and
// Windows queues balloons, showing each for its notification duration: a
// click on an older one arrived as "the" click, while the payload we held was
// the newest's, and from the second room on we could only raise the window
// (live Windows test 2026-10-16: 4 of 4 clicks on an older balloon lost, the
// latest one opening Home). So where a balloon can be taken down (Windows),
// at most one is on screen at a time, and a click always means that one.
// What arrives while it is up waits as one entry per room (or thread),
// merged, and goes out as one summary when its time is up. A mention, or a
// notice with no room, breaks through at once, as it breaks through the
// sound burst rule; the balloon it displaces goes back into the summary.
//
// Where nothing can take a balloon down (macOS keeps old notifications in
// Notification Center, a Linux tray has no call for it) a click may come from
// any of them, so every balloon is shown and the click guard counts rooms
// instead (noteBalloon, onFallbackMessageClicked).
bool NotificationManager::offerBalloon(Balloon balloon, bool breakThrough)
{
    if (!balloonGate())
        return deliverThroughTray(balloon.title, balloon.body, balloon.payload,
                                  balloon.avatar, balloon.platformSound,
                                  balloon.accountKey);
    if (m_balloonIsCall && m_balloonHold.isActive())
        breakThrough = false;
    if (!breakThrough && m_balloonHold.isActive()) {
        enqueueBalloon(std::move(balloon), /*older=*/false);
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
    // Shown: what its own room had waiting is covered by it.
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
    // A break-through (or the call's balloon) displaces a balloon nobody has
    // clicked: what it stood for is still news, so it waits in the summary
    // (it has alerted once). Withdrawals and account clears keep
    // m_shownEntries current, so nothing read or signed out comes back.
    if (!balloonGate() || !m_balloonHold.isActive())
        return;
    const QList<Balloon> displaced = std::exchange(m_shownEntries, {});
    for (Balloon entry : displaced) {
        entry.platformSound = false;
        enqueueBalloon(std::move(entry), /*older=*/true);
    }
}

void NotificationManager::enqueueBalloon(Balloon balloon, bool older)
{
    // One entry per account and card group: the newest message stands for
    // the ones before it, and keeps any platform sound none of them made.
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
    // Bounded by rooms rather than traffic; past the cap the oldest room's
    // entry goes (the summary still counts what remains).
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
        // One room: its latest message, as its own card would read, and a
        // click that opens it.
        summary = m_balloonQueue.last();
    } else {
        // Several: a count, which names no room (so it holds under every
        // preview mode), and a click that opens no room.
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
        // The table it was raised from: its accounts' one, or none for a
        // count across accounts (an account clear then drops that account's
        // entries from what it stands for, and leaves the count).
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
        // Kept: the next balloon's time running out tries again.
        qCInfo(lcNotify) << "notification service unavailable";
        return;
    }
    m_shownEntries = std::exchange(m_balloonQueue, {});
}

void NotificationManager::onBalloonHoldExpired()
{
    // Its time is up: Windows has taken it down, so it routes nowhere any
    // more and the next show needs no removal first (one the pointer still
    // rests on is left alone: a removal is for a real break-through). What
    // waited behind it goes out now.
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
    // The platform took the clicked balloon down; the screen is free.
    m_balloonHold.stop();
    m_balloonMayBeVisible = false;
    m_balloonIsCall = false;
    m_shownEntries.clear();
    // Windows toast click reported as doing nothing on the first of three
    // clicks (2026-10-14, cause unknown). Booleans and counts only, so a
    // capture can tell "Qt never delivered the click" (no line) from
    // "delivered with no payload stored" from "routed" without logging a room
    // id.
    qCInfo(lcNotify) << "tray balloon clicked payload="
                     << (!payload.isEmpty()) << "room="
                     << !payload.value(QStringLiteral("roomId")).toString().isEmpty()
                     << "rooms=" << rooms << "pending=" << m_balloonQueue.size();
    if (!balloonGate()) {
        // Qt reports a click on any of the balloons as the same signal. With
        // more than one room outstanding the latest is a guess that is wrong
        // for every other balloon, so the window comes up without choosing
        // a room.
        emitOpenFor(rooms > 1 ? QVariantMap{} : payload);
        return;
    }
    // Only one balloon is ever on screen (offerBalloon), so the click is the
    // one it showed: its room, or no room for a several-room summary. An
    // empty payload (timed out, or signed out) still raises the window: a
    // click that does nothing reads as a broken notification.
    emitOpenFor(payload);
    // The room just opened is not news for the next balloon.
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

// Every openRequested goes through here and is normalised again, since
// processEvent() is not the only payload producer. Idempotent.
void NotificationManager::emitOpenFor(const QVariantMap &payload)
{
    // Notices without a room (e.g. a verification request), and cards whose
    // room is no longer known, still raise the window, which Main.qml does
    // before reading the room id. Sign-out clears the payloads, so nothing
    // routes into the previous account's rooms.
    const QString roomId =
        routableRoomId(payload.value(QStringLiteral("roomId")).toString());
    const QString eventId = payload.value(QStringLiteral("eventId")).toString();
    const QString threadRootId = routableThreadRootId(
        payload.value(QStringLiteral("roomId")).toString(),
        payload.value(QStringLiteral("threadRootId")).toString());
    // The account first, so a listener routing by account has it before the
    // room-only signal that every existing listener follows.
    Q_EMIT openRequestedForAccount(
        payload.value(QStringLiteral("accountUserId")).toString(), roomId,
        eventId, threadRootId);
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
    // Only the card currently showing can answer; a stale card from a previous
    // call must not. Compiles without D-Bus but never fires there.
    if (action == QLatin1String("accept")) {
        if (id != 0 && id == m_activeCallNotificationId
            && !m_activeCallId.isEmpty()) {
            const QString callId = m_activeCallId;
            // Joining raises the window; let it take focus.
            applyActivationToken(id);
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
        const QVariantMap payload = payloadFor(id);
        if (payload.isEmpty()) {
            handleStaleAction(id, action, QString());
            return;
        }
        forgetPayload(id);
        // Resident: the daemon keeps a card it was answered from (R13).
        closeNotification(id);
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
    const QVariantMap payload = payloadFor(id);
    if (payload.isEmpty()) {
        handleStaleAction(id, action, QString());
        return;
    }
    applyActivationToken(id);
    forgetPayload(id);
    // Resident: the daemon keeps a clicked card (R13). Once it has opened the
    // room it is spent, as a non-resident card would be. The call card is not
    // resident; its own close (and what it means) stays the daemon's.
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
    // Resident: the daemon keeps a card replied from (R13).
    closeNotification(id);
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

// A card we raised whose payload is gone: withdrawn at sign-out or an account
// switch (a daemon that keeps it in its history anyway), evicted, or answered
// after a sign-out. Never silent: the user typed a reply or pressed a button.
// An id with no tombstone is not known to be ours at all (the daemon's
// signals can reach every client on the bus), so it is left alone.
void NotificationManager::handleStaleAction(quint32 id, const QString &action,
                                            const QString &text)
{
    const auto tomb = m_tombstones.constFind(id);
    if (tomb == m_tombstones.cend())
        return;
    const QString account = tomb.value();
    m_tombstones.remove(id);
    m_tombstoneOrder.removeOne(id);
    // Ours (the tombstone says so) and resident: answered once, then gone,
    // rather than staying in the history to refuse every later click (R13).
    closeNotification(id);
    const bool reply = action == QLatin1String("inline-reply-text");
    qCInfo(lcNotify) << "action on a withdrawn notification:"
                     << (reply ? QStringLiteral("reply") : action);
    if (action == QLatin1String("default")) {
        // The room it named is not ours to open any more; the window is.
        applyActivationToken(id);
        emitOpenFor({});
        return;
    }
    const QString body = text.trimmed();
    if (reply && body.isEmpty())
        return;
    if (account.isEmpty() || account != m_accountUserId) {
        // Another account's card: the app refuses it and says so.
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
        // The card expired into the history (some daemons time out even a
        // critical card): stop re-posting it, but keep its id so the end of
        // the call withdraws it and its buttons keep answering meanwhile.
        if (reason == kClosedExpired) {
            m_activeCallCardExpired = true;
            m_callRingTimer.stop();
            return;
        }
        // Dismissing the call card stops re-ringing but does not decline; other
        // devices keep ringing. Lightning's own ringer stops with it, as for
        // the in-app Dismiss.
        const QString callId = m_activeCallId;
        m_activeCallNotificationId = 0;
        m_activeCallCardExpired = false;
        m_callRingTimer.stop();
        forgetPayload(id);
        if (reason == kClosedDismissed && !callId.isEmpty())
            Q_EMIT callCardDismissed(callId);
        return;
    }
    // freedesktop reasons: 1 expired, 2 dismissed, 3 CloseNotification,
    // 4 undefined. Expired popups stay in KDE/GNOME history, so keep their
    // payload for closeRoomNotifications().
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
    // Every id the old daemon gave out may name another application's card on
    // the new one: forget them all, and what it said it could do.
    ++m_daemonGeneration;
    // Every account's ids came from the old daemon. The tables stay, with
    // their delivery generations; the daemon generation already rejects the
    // old daemon's replies.
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
    // Deliveries that waited for the old daemon's answer ask the new one.
    if (!m_capabilityWaits.isEmpty())
        requestCapabilities();
#endif
    // A call still ringing gets its card back on the new daemon.
    if (!m_activeCallId.isEmpty())
        deliverCallNotification();
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
    // Redraw only a card that is showing, or one being raised (it is redrawn
    // when its id arrives). A card the user dismissed stays dismissed: a
    // join-state change is no reason to put it back, ringing.
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
    // One ring at a time; a newer call replaces the previous card.
    if (!m_activeCallId.isEmpty() && m_activeCallId != callId)
        stopIncomingCall(m_activeCallId);
    // A new call gets its own balloon; the same call announced again (the
    // stuck-ringer fallback) does not get a second, since a balloon can never
    // be withdrawn.
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
    // The re-post is the ringing sound; stopping the timer stops it. The card
    // is still retired when the call ends.
    m_activeCallSound = false;
    m_activeCallSilenceOffered = false;
    m_callRingTimer.stop();
    // Redraw without the Silence button, but only if the card is still showing
    // (id 0 means it was closed; an expired one is in the history). The tray
    // balloon is latched per call.
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
    // A card still being raised is retired when its id arrives.
    m_callRedrawPending = false;
    m_callWaitsForCapabilities = false;
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
    // One Notify at a time: until it answers, the card's id (replaces_id) is
    // unknown and a second one would raise a duplicate card.
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
        { QStringLiteral("urgency"), quint8(2) }, // critical: a live ring
    };
    const QVariantMap soundHintMap = callSoundHints(m_activeCallSound);
    for (auto it = soundHintMap.cbegin(); it != soundHintMap.cend(); ++it)
        hints.insert(it.key(), it.value());
    // Same single escape as deliverNow(); the body contains member-chosen
    // names.
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
        // The old daemon's id; the owner change already raised the card anew.
        if (daemon != m_daemonGeneration)
            return;
        m_callNotifyInFlight = false;
        const QDBusPendingReply<quint32> reply = *watcher;
        if (callId != m_activeCallId) {
            // Stopped or replaced while it was being raised.
            if (!reply.isError())
                closeNotification(reply.value());
        } else if (!reply.isError()) {
            if (m_activeCallNotificationId != 0
                && reply.value() != m_activeCallNotificationId)
                forgetPayload(m_activeCallNotificationId);
            m_activeCallNotificationId = reply.value();
            m_activeCallCardExpired = false;
            // The call card is the foreground account's: the "" table.
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
        // A redraw asked for meanwhile, or the next call's first card.
        if (std::exchange(m_callRedrawPending, false))
            deliverCallNotification();
    });
    return;
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
    // The account it rings for, so the click names it like a message
    // balloon's does (openRequestedForAccount).
    const QVariantMap payload{
        { QStringLiteral("roomId"), m_activeCallRoomId },
        { QStringLiteral("eventId"), QString() },
        { QStringLiteral("threadRootId"), QString() },
        { QStringLiteral("accountUserId"), m_accountUserId },
    };
    m_lastCallTrayPayload = payload;
    // It takes the screen from any message balloon, which waits in the
    // summary instead of being lost.
    requeueDisplacedBalloon();
    // m_activeCallSound is the caller's "the desktop rings", true only when
    // ringing is on and Lightning's own ringer cannot play (AppController);
    // with our ringer running, or ringing off, the balloon stays silent.
    if (!deliverThroughTray(m_activeCallTitle, m_activeCallBody, payload,
                            QImage(), m_activeCallSound, QString())) {
        qCInfo(lcNotify) << "incoming call could not be announced: no "
                            "notification service and no tray balloon";
        return false;
    }
    // Messages wait behind it, mentions included (offerBalloon).
    m_balloonIsCall = true;
    return true;
}
