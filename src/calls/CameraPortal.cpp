#include "calls/CameraPortal.h"
#include "calls/PortalRequest.h"

#include <QLoggingCategory>
#include <QRandomGenerator>

#ifdef HAVE_QT_DBUS
#include <unistd.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QVariantMap>
#endif

namespace {
Q_LOGGING_CATEGORY(lcCameraPortal, "lightning.calls.portal")

/// How long a portal request may stay outstanding. Long, because the user is
/// answering a permission dialog; it only stops an unanswered request from
/// wedging the camera. Outside the D-Bus guard because the timer exists in
/// every build.
constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kCamera = "org.freedesktop.portal.Camera";
constexpr auto kRequest = "org.freedesktop.portal.Request";

/// A caller-unique, random token per request (see ScreenCastPortal).
QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}
#endif
} // namespace

#ifdef HAVE_QT_DBUS
/// One step of the handshake: subscribe to the Request's `Response`, deliver
/// it once, unsubscribe. A deliberate near-copy of ScreenCastPortal's
/// PortalStep (file-local moc types); named differently to avoid collisions.
class CameraPortalStep : public QObject
{
    Q_OBJECT
public:
    CameraPortalStep(CameraPortal *owner, const QString &requestPath)
        : QObject(owner), m_path(requestPath)
    {
        QDBusConnection::sessionBus().connect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }
    ~CameraPortalStep() override
    {
        QDBusConnection::sessionBus().disconnect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }

Q_SIGNALS:
    /// response: 0 = granted, 1 = user declined, 2 = ended some other way.
    void answered(uint response, const QVariantMap &results);

private Q_SLOTS:
    void onResponse(uint response, const QVariantMap &results)
    {
        Q_EMIT answered(response, results);
        deleteLater();
    }

private:
    QString m_path;
};
#endif

CameraPortal::CameraPortal(QObject *parent) : QObject(parent)
{
    m_requestTimeout.setSingleShot(true);
    m_requestTimeout.setInterval(kRequestTimeoutMs);
    connect(&m_requestTimeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcCameraPortal) << "camera portal request timed out; "
                                     "releasing";
        cancel();
        Q_EMIT failed(QStringLiteral("timeout"));
    });
}

CameraPortal::~CameraPortal()
{
    cancel();
}

bool CameraPortal::available()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // No portal: the capture element opens the device itself and the OS gates
    // access, so the direct route applies.
    return false;
#elif defined(HAVE_QT_DBUS)
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
    // Probe the interface version; a desktop without the Camera portal
    // answers with an error.
    QDBusMessage probe = QDBusMessage::createMethodCall(
        kService, kPath, "org.freedesktop.DBus.Properties",
        QStringLiteral("Get"));
    probe << QString::fromLatin1(kCamera) << QStringLiteral("version");
    const QDBusMessage reply =
        QDBusConnection::sessionBus().call(probe, QDBus::Block, 2000);
    return reply.type() == QDBusMessage::ReplyMessage;
#else
    return false;
#endif
}

bool CameraPortal::cameraPresent()
{
#if defined(HAVE_QT_DBUS) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
    QDBusMessage probe = QDBusMessage::createMethodCall(
        kService, kPath, "org.freedesktop.DBus.Properties",
        QStringLiteral("Get"));
    probe << QString::fromLatin1(kCamera)
          << QStringLiteral("IsCameraPresent");
    const QDBusMessage reply =
        QDBusConnection::sessionBus().call(probe, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage
        || reply.arguments().isEmpty())
        return false;
    // Properties.Get wraps the value in a variant.
    return reply.arguments().constFirst().value<QDBusVariant>()
        .variant().toBool();
#else
    return false;
#endif
}

void CameraPortal::cancel()
{
    ++m_generation;
    m_busy = false;
    m_requestTimeout.stop();
    // Nothing to close: the Camera portal has no session. The fd, if any,
    // belongs to whoever received `ready`.
}

void CameraPortal::reset()
{
    m_busy = false;
}

#if !defined(HAVE_QT_DBUS) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void CameraPortal::requestAccess()
{
    // Report rather than ignore, so the caller falls back instead of waiting.
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void CameraPortal::requestAccess()
{
    if (m_busy) {
        // A second request would raise a second dialog.
        Q_EMIT failed(QStringLiteral("busy"));
        return;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        Q_EMIT failed(QStringLiteral("no_portal"));
        return;
    }

    m_busy = true;
    m_requestTimeout.start();
    const quint64 generation = ++m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    // Step 1: AccessCamera, where the portal asks the user.
    const QString token = freshToken();
    const auto onAnswer = [this, stale](uint response, const QVariantMap &) {
        if (stale())
            return;
        if (response != 0) {
            // Declined. Not an error.
            cancel();
            Q_EMIT cancelled();
            return;
        }
        openRemote();
    };
    // Subscribed before the call: with the permission already stored there is
    // no dialog, and the grant arrives right behind the reply. See
    // PortalRequest.h.
    auto subscription = portal::subscribeBeforeCall<CameraPortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);

    QDBusMessage access = QDBusMessage::createMethodCall(
        kService, kPath, kCamera, QStringLiteral("AccessCamera"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    access << options;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(access), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale, subscription, onAnswer](
                QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale()) {
                    portal::dropSubscription(subscription);
                    return;
                }
                if (reply.isError()) {
                    // Error text can name the desktop and paths; report a
                    // category only.
                    portal::dropSubscription(subscription);
                    reset();
                    m_requestTimeout.stop();
                    Q_EMIT failed(QStringLiteral("no_portal"));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
            });
}

void CameraPortal::openRemote()
{
    // OpenPipeWireRemote grants actual access to pixels; without it there is
    // no remote containing a camera node for this process. A plain method
    // call; the reply carries the fd.
    const quint64 generation = m_generation;
    QDBusMessage open = QDBusMessage::createMethodCall(
        kService, kPath, kCamera, QStringLiteral("OpenPipeWireRemote"));
    open << QVariantMap();

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(open), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
                if (generation != m_generation)
                    return;
                if (reply.isError() || !reply.value().isValid()) {
                    // Error name only: messages can carry a device path.
                    qCWarning(lcCameraPortal)
                        << "camera OpenPipeWireRemote failed error="
                        << (reply.isError() ? reply.error().name()
                                            : QStringLiteral("no_fd"));
                    cancel();
                    Q_EMIT failed(QStringLiteral("no_pipewire_remote"));
                    return;
                }
                // QDBusUnixFileDescriptor closes its fd with the last copy, so
                // hand over a dup; the receiver owns it.
                const int fd = ::dup(reply.value().fileDescriptor());
                if (fd < 0) {
                    cancel();
                    Q_EMIT failed(QStringLiteral("no_pipewire_remote"));
                    return;
                }
                m_busy = false;
                m_requestTimeout.stop();
                qCInfo(lcCameraPortal) << "camera portal granted; pipewire "
                                          "remote open";
                Q_EMIT ready(fd);
            });
}
#endif

#ifdef HAVE_QT_DBUS
#include "CameraPortal.moc"
#endif
