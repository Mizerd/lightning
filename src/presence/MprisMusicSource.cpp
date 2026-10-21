#include "presence/MprisMusicSource.h"

#ifdef HAVE_QT_DBUS

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

namespace {

const QString kPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

// One D-Bus value as a plain Qt value: an object path becomes its string, a
// wrapped variant is unwrapped, and a string array still inside a
// QDBusArgument becomes a QStringList.
QVariant plainValue(const QVariant &v)
{
    if (v.userType() == qMetaTypeId<QDBusVariant>())
        return plainValue(v.value<QDBusVariant>().variant());
    if (v.userType() == qMetaTypeId<QDBusObjectPath>())
        return v.value<QDBusObjectPath>().path();
    if (v.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::ArrayType
            && arg.currentSignature() == QLatin1String("as")) {
            QStringList list;
            arg >> list;
            return list;
        }
        return {};   // nothing else in Metadata is read
    }
    return v;
}

// MPRIS `Metadata` (a{sv}) as a plain map, whichever way QtDBus handed it
// over: still marshalled (signal and GetAll replies) or already a map.
QVariantMap plainMetadata(const QVariant &v)
{
    QVariantMap raw;
    if (v.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentSignature() != QLatin1String("a{sv}"))
            return {};
        arg >> raw;
    } else if (v.userType() == qMetaTypeId<QDBusVariant>()) {
        return plainMetadata(v.value<QDBusVariant>().variant());
    } else {
        raw = v.toMap();
    }
    QVariantMap out;
    for (auto it = raw.cbegin(); it != raw.cend(); ++it)
        out.insert(it.key(), plainValue(it.value()));
    return out;
}

}   // namespace

MprisMusicSource::MprisMusicSource(QObject *parent)
    : MprisMusicSource(QDBusConnection::sessionBus(),
                       QString::fromLatin1(musicdetect::kSpotifyMprisService), parent)
{
}

MprisMusicSource::MprisMusicSource(const QDBusConnection &bus, const QString &service,
                                   QObject *parent)
    : MusicSource(parent)
    , m_bus(bus)
    , m_service(service)
{
    m_poll.setInterval(kPollIntervalMs);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        // Nobody owns the name: there is nothing to ask until an owner
        // appears, which the service watcher reports.
        if (m_hasOwner)
            refresh();
    });
}

MprisMusicSource::~MprisMusicSource()
{
    stop();
}

void MprisMusicSource::start()
{
    if (m_running)
        return;
    m_running = true;
    m_hasOwner = true;
    ++m_generation;
    if (!m_bus.isConnected())
        return;   // no session bus: nothing to watch, and nothing to report
    m_watcher = new QDBusServiceWatcher(m_service, m_bus,
                                        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) {
                if (!m_running)
                    return;
                m_hasOwner = !newOwner.isEmpty();
                if (newOwner.isEmpty()) {
                    // Spotify quit: whatever it was playing has stopped.
                    m_status.clear();
                    m_metadata.clear();
                    recompute();
                } else {
                    refresh();
                }
            });
    m_subscribed = m_bus.connect(m_service, QString::fromLatin1(musicdetect::kMprisObjectPath),
                                 kPropertiesInterface, QStringLiteral("PropertiesChanged"),
                                 this,
                                 SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    m_poll.start();
    refresh();
}

void MprisMusicSource::stop()
{
    if (!m_running)
        return;
    m_running = false;
    ++m_generation;   // a reply already in flight is dropped
    m_poll.stop();
    if (m_subscribed) {
        m_bus.disconnect(m_service, QString::fromLatin1(musicdetect::kMprisObjectPath),
                         kPropertiesInterface, QStringLiteral("PropertiesChanged"), this,
                         SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
        m_subscribed = false;
    }
    delete m_watcher;
    m_watcher = nullptr;
    m_status.clear();
    m_metadata.clear();
    m_now = {};
}

void MprisMusicSource::refresh()
{
    if (!m_running || !m_bus.isConnected())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, QString::fromLatin1(musicdetect::kMprisObjectPath), kPropertiesInterface,
        QStringLiteral("GetAll"));
    call << QString::fromLatin1(musicdetect::kMprisPlayerInterface);
    // Never start Spotify (or anything) through D-Bus activation by asking.
    call.setAutoStartService(false);
    ++m_requests;
    const int generation = m_generation;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *finished) {
                finished->deleteLater();
                if (generation != m_generation || !m_running)
                    return;
                const QDBusPendingReply<QVariantMap> reply = *finished;
                if (reply.isError()) {
                    // Not running (ServiceUnknown), hidden by a sandbox, or
                    // not answering: nothing is known to be playing.
                    if (reply.error().type() == QDBusError::ServiceUnknown
                        || reply.error().name()
                               == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"))
                        m_hasOwner = false;
                    m_status.clear();
                    m_metadata.clear();
                    recompute();
                    return;
                }
                applyProperties(reply.value(), true);
            });
}

void MprisMusicSource::onPropertiesChanged(const QString &interfaceName,
                                           const QVariantMap &changed,
                                           const QStringList &invalidated)
{
    if (!m_running
        || interfaceName != QLatin1String(musicdetect::kMprisPlayerInterface))
        return;
    m_hasOwner = true;   // only an owner can signal
    applyProperties(changed, false);
    if (invalidated.contains(QStringLiteral("Metadata"))
        || invalidated.contains(QStringLiteral("PlaybackStatus")))
        refresh();
}

void MprisMusicSource::applyProperties(const QVariantMap &props, bool replace)
{
    const auto status = props.constFind(QStringLiteral("PlaybackStatus"));
    if (status != props.cend())
        m_status = plainValue(status.value()).toString();
    else if (replace)
        m_status.clear();
    const auto metadata = props.constFind(QStringLiteral("Metadata"));
    if (metadata != props.cend())
        m_metadata = plainMetadata(metadata.value());
    else if (replace)
        m_metadata.clear();
    recompute();
}

void MprisMusicSource::recompute()
{
    const musicdetect::NowPlaying next = musicdetect::fromMpris(m_status, m_metadata);
    if (next == m_now)
        return;
    m_now = next;
    Q_EMIT nowPlayingChanged();
}

#endif   // HAVE_QT_DBUS
