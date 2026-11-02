#include "calls/ScreenCastPortal.h"
#include "calls/PortalRequest.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QScreen>
#include <QWindow>

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

constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequest = "org.freedesktop.portal.Request";

QString errorCategory(const QDBusError &error, const QString &fallback)
{
    const QString message = error.message();
    if (message.contains(QLatin1String("X11"), Qt::CaseSensitive)
        && (message.contains(QLatin1String("not available"),
                             Qt::CaseInsensitive)
            || message.contains(QLatin1String("not supported"),
                                Qt::CaseInsensitive)
            || message.contains(QLatin1String("unsupported"),
                                Qt::CaseInsensitive)))
        return QStringLiteral("x11_unsupported");
    return fallback;
}

/// A caller-unique token per request, from which the portal derives the
/// Request path. Random, so the path is not guessable by other apps on the
/// bus.
QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}
#endif
}

#ifdef HAVE_QT_DBUS
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
    m_requestTimeout.setSingleShot(true);
    m_requestTimeout.setInterval(kRequestTimeoutMs);
    connect(&m_requestTimeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcPortal) << "screen share request timed out; releasing";
        cancel();
        Q_EMIT failed(QStringLiteral("timeout"));
    });
    m_answerWatchdog.setSingleShot(true);
    connect(&m_answerWatchdog, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcPortal)
            << "screen share portal gave no answer within the caller's bound; "
               "abandoning it";
        cancel();
        Q_EMIT failed(QStringLiteral("no_answer"));
    });
}

void ScreenCastPortal::armAnswerWatchdog(int ms)
{
    if (ms > 0)
        m_answerWatchdog.start(ms);
    else
        m_answerWatchdog.stop();
}

ScreenCastPortal::~ScreenCastPortal()
{
    cancel();
}

bool ScreenCastPortal::available()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return QGuiApplication::screens().size() > 0;
#elif defined(HAVE_QT_DBUS)
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
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
    m_answerWatchdog.stop();
#ifdef HAVE_QT_DBUS
    if (!m_sessionHandle.isEmpty()) {
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
    m_answerWatchdog.stop();
    m_sessionHandle.clear();
}

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void ScreenCastPortal::requestShare(int types, int preparationBoundMs,
                                    int pickerBoundMs)
{
    Q_UNUSED(types);
    Q_UNUSED(preparationBoundMs);
    Q_UNUSED(pickerBoundMs);
    if (m_busy) {
        Q_EMIT failed(QStringLiteral("busy"));
        return;
    }
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        Q_EMIT failed(QStringLiteral("no_screen"));
        return;
    }
    const QWindow *window = QGuiApplication::focusWindow();
    const QScreen *screen = window ? window->screen() : nullptr;
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const int index = screen ? int(screens.indexOf(screen)) : 0;
    Q_EMIT ready(static_cast<unsigned>(index < 0 ? 0 : index), -1);
}
#elif !defined(HAVE_QT_DBUS)
void ScreenCastPortal::requestShare(int types, int preparationBoundMs,
                                    int pickerBoundMs)
{
    Q_UNUSED(types);
    Q_UNUSED(preparationBoundMs);
    Q_UNUSED(pickerBoundMs);
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void ScreenCastPortal::requestShare(int types, int preparationBoundMs,
                                    int pickerBoundMs)
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
    m_pickerBoundMs = pickerBoundMs;
    armAnswerWatchdog(preparationBoundMs);
    const quint64 generation = ++m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    QDBusMessage create = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("CreateSession"));
    const QString token = freshToken();
    QVariantMap createOptions;
    createOptions.insert(QStringLiteral("handle_token"), token);
    createOptions.insert(QStringLiteral("session_handle_token"),
                         freshToken());
    create << createOptions;

    const auto onAnswer = [this, types, stale](uint response,
                                               const QVariantMap &results) {
        if (stale())
            return;
        if (response != 0) {
            qCWarning(lcPortal) << "screen share session refused response="
                                << response;
            reset();
            Q_EMIT failed(QStringLiteral("session_refused"));
            return;
        }
        m_sessionHandle =
            results.value(QStringLiteral("session_handle")).toString();
        if (m_sessionHandle.isEmpty()) {
            reset();
            Q_EMIT failed(QStringLiteral("no_session"));
            return;
        }
        selectSources(types);
    };
    auto subscription = portal::subscribeBeforeCall<PortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);

    auto *createWatcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(create), this);
    connect(createWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale, subscription, onAnswer](
                QDBusPendingCallWatcher *watcher) mutable {
                watcher->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                if (stale()) {
                    portal::dropSubscription(subscription);
                    return;
                }
                if (reply.isError()) {
                    portal::dropSubscription(subscription);
                    reset();
                    Q_EMIT failed(errorCategory(reply.error(),
                                                QStringLiteral("no_portal")));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
            });
}

void ScreenCastPortal::selectSources(int types)
{
    const quint64 generation = m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    QDBusMessage select = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("SelectSources"));
    const QString token = freshToken();
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("types"), static_cast<uint>(types));
    options.insert(QStringLiteral("multiple"), false);
    options.insert(QStringLiteral("cursor_mode"), 2u);
    select << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << options;

    const auto onAnswer = [this, stale](uint response, const QVariantMap &) {
        if (stale())
            return;
        if (response == 1) {
            cancel();
            Q_EMIT cancelled();
            return;
        }
        if (response != 0) {
            qCWarning(lcPortal) << "screen share source selection refused "
                                   "response=" << response;
            cancel();
            Q_EMIT failed(QStringLiteral("session_refused"));
            return;
        }
        startSession();
    };
    auto subscription = portal::subscribeBeforeCall<PortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(select), this);
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
                    portal::dropSubscription(subscription);
                    cancel();
                    Q_EMIT failed(errorCategory(
                        reply.error(), QStringLiteral("select_failed")));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
            });
}

void ScreenCastPortal::startSession()
{
    const quint64 generation = m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    armAnswerWatchdog(m_pickerBoundMs);
    QDBusMessage start = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("Start"));
    const QString token = freshToken();
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    start << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
          << QString() << options;

    const auto onAnswer = [this, stale](uint response,
                                        const QVariantMap &results) {
        if (stale())
            return;
        if (response != 0) {
            cancel();
            Q_EMIT cancelled();
            return;
        }
        handleStreams(results);
    };
    auto subscription = portal::subscribeBeforeCall<PortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(start), this);
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
                    portal::dropSubscription(subscription);
                    cancel();
                    Q_EMIT failed(errorCategory(
                        reply.error(), QStringLiteral("start_failed")));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
            });
}

void ScreenCastPortal::handleStreams(const QVariantMap &results)
{
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
    qCInfo(lcPortal) << "screen share source selected";
    openRemote(nodeId);
}

void ScreenCastPortal::openRemote(unsigned nodeId)
{
    // OpenPipeWireRemote grants actual access: the node id alone names a node
    // in a remote we were never given, and the pipeline then runs without
    // producing frames. A plain method call; the reply carries the fd.
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
                    qCWarning(lcPortal)
                        << "OpenPipeWireRemote failed error="
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
                m_answerWatchdog.stop();
                Q_EMIT ready(nodeId, fd);
            });
}
#endif

#ifdef HAVE_QT_DBUS
#include "ScreenCastPortal.moc"
#endif
