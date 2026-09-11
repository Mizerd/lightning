#include "calls/CameraPortal.h"

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

/// How long a portal request may stay outstanding. The user is answering a
/// permission dialog, so this is deliberately long; it exists only so a
/// request that will NEVER be answered cannot wedge the camera. Declared
/// outside the D-Bus guard because the constructor arms the timer in every
/// build.
constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kCamera = "org.freedesktop.portal.Camera";
constexpr auto kRequest = "org.freedesktop.portal.Request";

/// The portal wants a caller-unique token per request so it can predict the
/// Request object path. Random rather than sequential, for the reason
/// ScreenCastPortal records: the path is derived from it and a predictable
/// path is guessable by another app on the same bus.
QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}
#endif
} // namespace

#ifdef HAVE_QT_DBUS
/// One step of the handshake: subscribe to the Request's `Response`, deliver
/// it once, unsubscribe. A near-twin of ScreenCastPortal.cpp's `PortalStep`
/// and deliberately NOT shared with it — both are file-local QObjects with
/// their own metaobject, and lifting one into a header to save thirty lines
/// would put a third moc'd type into every target that includes it for
/// nothing. Named differently so the two translation units cannot collide.
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
    /// response: 0 = granted, 1 = the user declined, 2 = ended some other way.
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
    // No portal, and nothing to ask one for: the capture element opens the
    // device itself (ksvideosrc / avfvideosrc) and the OS gates it its own
    // way. Reported false so the direct route stays in charge, which is the
    // route those platforms have always used.
    return false;
#elif defined(HAVE_QT_DBUS)
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
    // Ask for the interface's version. A desktop with no Camera portal
    // answers with an error, which is the honest "not available" — better
    // than assuming presence and failing at the moment the user presses the
    // camera button.
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
    // The property comes back wrapped in a variant by Properties.Get.
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
    // NOTHING TO CLOSE. Unlike ScreenCast there is no session object here, so
    // a cancelled request leaves no compositor state behind; the only thing
    // that outlives it is the fd, and that belongs to whoever received
    // `ready`.
}

void CameraPortal::reset()
{
    m_busy = false;
}

#if !defined(HAVE_QT_DBUS) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void CameraPortal::requestAccess()
{
    // Reported, never silently ignored: a caller that asked for the portal
    // route on a build or a platform that has none must fall back rather than
    // sit waiting for a signal that cannot arrive.
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void CameraPortal::requestAccess()
{
    if (m_busy) {
        // A second request while the dialog is open would raise two.
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

    // ── Step 1: AccessCamera — this is where the portal asks the user ──
    QDBusMessage access = QDBusMessage::createMethodCall(
        kService, kPath, kCamera, QStringLiteral("AccessCamera"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), freshToken());
    access << options;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(access), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale())
                    return;
                if (reply.isError()) {
                    // The error text can name the desktop and paths; only a
                    // category leaves this scope.
                    reset();
                    m_requestTimeout.stop();
                    Q_EMIT failed(QStringLiteral("no_portal"));
                    return;
                }
                auto *step = new CameraPortalStep(this, reply.value().path());
                connect(step, &CameraPortalStep::answered, this,
                        [this, stale](uint response, const QVariantMap &) {
                            if (stale())
                                return;
                            if (response != 0) {
                                // Declined. Not an error.
                                cancel();
                                Q_EMIT cancelled();
                                return;
                            }
                            openRemote();
                        });
            });
}

void CameraPortal::openRemote()
{
    // OpenPipeWireRemote is the step that actually grants access to pixels.
    // Without it there is no remote in which a camera node exists for this
    // process at all — the ScreenCast lane shipped a black share once by
    // treating the node id as sufficient, and here there is not even a node
    // id to be misled by. It is a plain method call, not a Request: the reply
    // carries the fd.
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
                    // The D-Bus error NAME only: a portal error message can
                    // carry a device path.
                    qCWarning(lcCameraPortal)
                        << "camera OpenPipeWireRemote failed error="
                        << (reply.isError() ? reply.error().name()
                                            : QStringLiteral("no_fd"));
                    cancel();
                    Q_EMIT failed(QStringLiteral("no_pipewire_remote"));
                    return;
                }
                // QDBusUnixFileDescriptor closes its descriptor when the last
                // copy dies, so hand over a DUP that outlives it. The
                // receiver owns the result.
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
