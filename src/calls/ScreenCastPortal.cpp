#include "calls/ScreenCastPortal.h"

#include <QLoggingCategory>
#include <QRandomGenerator>

#ifdef HAVE_QT_DBUS
#include <unistd.h>
#endif

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#endif

namespace {
Q_LOGGING_CATEGORY(lcPortal, "lightning.calls.portal")

/// How long a portal request may stay outstanding. The user is picking a
/// window or a monitor by hand, so this is deliberately long; it exists only
/// so a request that will NEVER be answered cannot wedge the feature.
/// Declared outside the D-Bus guard because the constructor arms the timer in
/// every build.
constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequest = "org.freedesktop.portal.Request";

/// The portal wants a caller-unique token for each request so it can predict
/// the Request object path. Random rather than sequential: the path is
/// derived from it, and a predictable path is guessable by another app on the
/// same bus.
QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}
#endif
} // namespace

#ifdef HAVE_QT_DBUS
/// One step of the handshake. Each portal call returns a Request path whose
/// `Response` signal carries the outcome, so every step subscribes, waits,
/// and unsubscribes — there is no polling anywhere in this exchange.
class PortalStep : public QObject
{
    Q_OBJECT
public:
    PortalStep(ScreenCastPortal *owner, const QString &requestPath)
        : QObject(owner)
    {
        QDBusConnection::sessionBus().connect(
            QString(), requestPath, kRequest, QStringLiteral("Response"),
            this, SLOT(onResponse(uint, QVariantMap)));
        m_path = requestPath;
    }
    ~PortalStep() override
    {
        QDBusConnection::sessionBus().disconnect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }

Q_SIGNALS:
    /// response: 0 = success, 1 = user cancelled, 2 = ended some other way.
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

ScreenCastPortal::ScreenCastPortal(QObject *parent) : QObject(parent)
{
    // Single-shot: armed when a request starts, stopped by cancel() and by a
    // granted source. See m_requestTimeout.
    m_requestTimeout.setSingleShot(true);
    m_requestTimeout.setInterval(kRequestTimeoutMs);
    connect(&m_requestTimeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcPortal) << "screen share request timed out; releasing";
        cancel();
        Q_EMIT failed(QStringLiteral("timeout"));
    });
}

ScreenCastPortal::~ScreenCastPortal()
{
    cancel();
}

bool ScreenCastPortal::available()
{
#ifdef HAVE_QT_DBUS
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
    // Ask for the interface's version. A desktop with no ScreenCast portal
    // answers with an error, which is the honest "not available" — better
    // than assuming presence and failing at the moment the user clicks.
    QDBusMessage probe = QDBusMessage::createMethodCall(
        kService, kPath, "org.freedesktop.DBus.Properties",
        QStringLiteral("Get"));
    probe << QString::fromLatin1(kScreenCast) << QStringLiteral("version");
    const QDBusMessage reply =
        QDBusConnection::sessionBus().call(probe, QDBus::Block, 2000);
    return reply.type() == QDBusMessage::ReplyMessage;
#else
    return false;
#endif
}

void ScreenCastPortal::cancel()
{
    ++m_generation;
    m_busy = false;
    m_requestTimeout.stop();
#ifdef HAVE_QT_DBUS
    if (!m_sessionHandle.isEmpty()) {
        // Close the session explicitly. Leaving it open would keep the
        // compositor streaming a surface nobody is consuming.
        QDBusMessage close = QDBusMessage::createMethodCall(
            kService, m_sessionHandle, "org.freedesktop.portal.Session",
            QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(close);
        m_sessionHandle.clear();
    }
#endif
}

void ScreenCastPortal::reset()
{
    m_busy = false;
    m_sessionHandle.clear();
}

#ifndef HAVE_QT_DBUS
void ScreenCastPortal::requestShare(int types)
{
    Q_UNUSED(types);
    // No portal means no screen sharing. Reported, never silently ignored.
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void ScreenCastPortal::requestShare(int types)
{
    if (m_busy) {
        // A second request while a picker is open would open two dialogs and
        // leave one session orphaned.
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

    // ── Step 1: CreateSession ──
    QDBusMessage create = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("CreateSession"));
    QVariantMap createOptions;
    createOptions.insert(QStringLiteral("handle_token"), freshToken());
    createOptions.insert(QStringLiteral("session_handle_token"),
                         freshToken());
    create << createOptions;

    auto *createWatcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(create), this);
    connect(createWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, types, stale](QDBusPendingCallWatcher *watcher) {
                watcher->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                if (stale())
                    return;
                if (reply.isError()) {
                    // The error text can name the desktop and paths; only a
                    // category leaves this scope.
                    reset();
                    Q_EMIT failed(QStringLiteral("no_portal"));
                    return;
                }
                auto *step = new PortalStep(this, reply.value().path());
                connect(step, &PortalStep::answered, this,
                        [this, types, stale](uint response,
                                             const QVariantMap &results) {
                            if (stale())
                                return;
                            if (response != 0) {
                                reset();
                                Q_EMIT cancelled();
                                return;
                            }
                            m_sessionHandle =
                                results.value(QStringLiteral("session_handle"))
                                    .toString();
                            if (m_sessionHandle.isEmpty()) {
                                reset();
                                Q_EMIT failed(QStringLiteral("no_session"));
                                return;
                            }
                            selectSources(types);
                        });
            });
}

void ScreenCastPortal::selectSources(int types)
{
    const quint64 generation = m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    // ── Step 2: SelectSources ──
    // `multiple: false` deliberately: one publisher track per share keeps the
    // mapping from a picked source to a published track unambiguous.
    QDBusMessage select = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("SelectSources"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), freshToken());
    options.insert(QStringLiteral("types"), static_cast<uint>(types));
    options.insert(QStringLiteral("multiple"), false);
    // 1 = hidden, 2 = embedded, 4 = metadata. Embedded draws the cursor into
    // the stream, which is what a viewer expects when someone points at
    // something; metadata would need the receiver to composite it.
    options.insert(QStringLiteral("cursor_mode"), 2u);
    select << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << options;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(select), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale())
                    return;
                if (reply.isError()) {
                    cancel();
                    Q_EMIT failed(QStringLiteral("select_failed"));
                    return;
                }
                auto *step = new PortalStep(this, reply.value().path());
                connect(step, &PortalStep::answered, this,
                        [this, stale](uint response, const QVariantMap &) {
                            if (stale())
                                return;
                            if (response != 0) {
                                cancel();
                                Q_EMIT cancelled();
                                return;
                            }
                            startSession();
                        });
            });
}

void ScreenCastPortal::startSession()
{
    const quint64 generation = m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    // ── Step 3: Start — this is where the portal shows its picker ──
    QDBusMessage start = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("Start"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), freshToken());
    // Empty parent window: Qt has no portable handle to hand over here, and
    // the portal then presents its dialog unparented rather than not at all.
    start << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
          << QString() << options;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(start), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale())
                    return;
                if (reply.isError()) {
                    cancel();
                    Q_EMIT failed(QStringLiteral("start_failed"));
                    return;
                }
                auto *step = new PortalStep(this, reply.value().path());
                connect(step, &PortalStep::answered, this,
                        [this, stale](uint response,
                                      const QVariantMap &results) {
                            if (stale())
                                return;
                            if (response != 0) {
                                // The user pressed Cancel in the picker.
                                cancel();
                                Q_EMIT cancelled();
                                return;
                            }
                            handleStreams(results);
                        });
            });
}

void ScreenCastPortal::handleStreams(const QVariantMap &results)
{
    // `streams` is a(ua{sv}): (node_id, properties). We asked for one source,
    // so the first entry is the answer; taking the first is also what makes
    // "do not publish a different monitor than the user selected" hold — the
    // portal returns exactly what was chosen and nothing else.
    const QVariant streams = results.value(QStringLiteral("streams"));
    const QDBusArgument argument = streams.value<QDBusArgument>();
    if (argument.currentType() != QDBusArgument::ArrayType) {
        cancel();
        Q_EMIT failed(QStringLiteral("no_stream"));
        return;
    }
    uint nodeId = 0;
    bool found = false;
    argument.beginArray();
    while (!argument.atEnd()) {
        argument.beginStructure();
        uint id = 0;
        argument >> id;
        QVariantMap properties;
        argument >> properties;
        argument.endStructure();
        if (!found) {
            nodeId = id;
            found = true;
        }
    }
    argument.endArray();

    if (!found) {
        cancel();
        Q_EMIT failed(QStringLiteral("no_stream"));
        return;
    }
    // The SESSION stays open: closing it would stop the stream we just
    // obtained. It is closed by cancel(), which the caller invokes when the
    // share stops.
    qCInfo(lcPortal) << "screen share source selected";
    openRemote(nodeId);
}

void ScreenCastPortal::openRemote(unsigned nodeId)
{
    // OpenPipeWireRemote is the step that actually grants access. The node id
    // on its own names a node in a remote we were never given, and the
    // resulting pipeline reports no error and produces no frames — a black
    // share, which is exactly what was reported. Firefox, Chromium and OBS
    // all take this fd; so do we.
    //
    // It is a plain method call, not a Request: the reply carries the fd.
    const quint64 generation = m_generation;
    QDBusMessage open = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("OpenPipeWireRemote"));
    open << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
         << QVariantMap();

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(open), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, nodeId](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
                if (generation != m_generation)
                    return;
                if (reply.isError() || !reply.value().isValid()) {
                    // The D-Bus error NAME only: a portal error message can
                    // carry a window title or a path.
                    qCWarning(lcPortal)
                        << "OpenPipeWireRemote failed error="
                        << (reply.isError() ? reply.error().name()
                                            : QStringLiteral("no_fd"));
                    cancel();
                    Q_EMIT failed(QStringLiteral("no_pipewire_remote"));
                    return;
                }
                // QDBusUnixFileDescriptor closes its descriptor when the
                // last copy dies, so hand over a DUP that outlives it. The
                // receiver owns the result.
                const int fd = ::dup(reply.value().fileDescriptor());
                if (fd < 0) {
                    cancel();
                    Q_EMIT failed(QStringLiteral("no_pipewire_remote"));
                    return;
                }
                m_busy = false;
                m_requestTimeout.stop();
                Q_EMIT ready(nodeId, fd);
            });
}
#endif

#ifdef HAVE_QT_DBUS
#include "ScreenCastPortal.moc"
#endif
