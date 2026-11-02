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

constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kCamera = "org.freedesktop.portal.Camera";
constexpr auto kRequest = "org.freedesktop.portal.Request";

QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}
#endif
}

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
    return false;
#elif defined(HAVE_QT_DBUS)
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
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
    return reply.arguments().constFirst().value<QDBusVariant>()
        .variant().toBool();
#else
    return false;
#endif
}

QString CameraPortal::failureCategory(const QString &errorName,
                                     bool openingRemote)
{
    if (errorName == QLatin1String("org.freedesktop.portal.Error.NotAllowed")
        || errorName == QLatin1String("org.freedesktop.portal.Error.Cancelled")
        || errorName == QLatin1String("org.freedesktop.DBus.Error.AccessDenied"))
        return QStringLiteral("not_allowed");
    if (errorName == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || errorName == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")
        || errorName
            == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface")
        || errorName == QLatin1String("org.freedesktop.DBus.Error.UnknownObject")
        || errorName
            == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"))
        return QStringLiteral("no_portal");
    return openingRemote ? QStringLiteral("no_pipewire_remote")
                         : QStringLiteral("portal_failed");
}

void CameraPortal::cancel()
{
    ++m_generation;
    m_busy = false;
    m_requestTimeout.stop();
}

void CameraPortal::reset()
{
    m_busy = false;
}

#if !defined(HAVE_QT_DBUS) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void CameraPortal::requestAccess()
{
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void CameraPortal::requestAccess()
{
    if (m_busy) {
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

    const QString token = freshToken();
    const auto onAnswer = [this, stale](uint response, const QVariantMap &) {
        if (stale())
            return;
        if (response != 0) {
            cancel();
            Q_EMIT cancelled();
            return;
        }
        openRemote();
    };
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
                    // Error text can name the desktop and paths; the error
                    // NAME decides between a refusal and no portal at all.
                    const QString category = failureCategory(
                        reply.error().name(),false);
                    qCWarning(lcCameraPortal)
                        << "camera AccessCamera failed error="
                        << reply.error().name() << "category=" << category;
                    portal::dropSubscription(subscription);
                    reset();
                    m_requestTimeout.stop();
                    Q_EMIT failed(category);
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
            });
}

void CameraPortal::openRemote()
{
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
                    qCWarning(lcCameraPortal)
                        << "camera OpenPipeWireRemote failed error="
                        << (reply.isError() ? reply.error().name()
                                            : QStringLiteral("no_fd"));
                    cancel();
                    Q_EMIT failed(
                        reply.isError()
                            ? failureCategory(reply.error().name(),
true)
                            : QStringLiteral("no_pipewire_remote"));
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
