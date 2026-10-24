#include "media/MprisServer.h"

#include "media/VoicePlaybackController.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QRandomGenerator>

#ifdef HAVE_QT_DBUS
#include <QAudioOutput>
#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTimer>
#endif

Q_LOGGING_CATEGORY(lcMpris, "matrix.media.mpris")

QString MprisServer::baseServiceName()
{
    // A Flatpak may own exactly org.mpris.MediaPlayer2.<app id> (and that name
    // plus a suffix) without any finish-arg, and the sandbox sets FLATPAK_ID.
    // Outside it the plain name is kept, so existing setups do not move.
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID").trimmed();
    if (!flatpakId.isEmpty())
        return QStringLiteral("org.mpris.MediaPlayer2.") + flatpakId;
    return QStringLiteral("org.mpris.MediaPlayer2.lightning");
}

QString MprisServer::objectPath()
{
    return QStringLiteral("/org/mpris/MediaPlayer2");
}

#ifdef HAVE_QT_DBUS

namespace {

const QString kPlayerInterface =
    QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kRootInterface = QStringLiteral("org.mpris.MediaPlayer2");
// The speeds the in-app menus offer.
constexpr double kMinRate = 0.5;
constexpr double kMaxRate = 2.0;
// A position report further than this from where steady playback would be is
// a seek (from the app's own scrubber, a row, or a remote Seek), announced
// with Seeked as the spec asks.
constexpr qint64 kSeekToleranceMs = 1500;

QDBusConnection busFor(const QString &name)
{
    return name.isEmpty() ? QDBusConnection::sessionBus()
                          : QDBusConnection(name);
}

} // namespace

// org.mpris.MediaPlayer2: identity and Raise. No Quit (the app has its own
// close and tray rules), no track list, no URIs.
class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    MprisRootAdaptor(QObject *parent, MprisServer *server)
        : QDBusAbstractAdaptor(parent), m_server(server)
    {
        setAutoRelaySignals(false);
    }

    bool canQuit() const { return false; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Lightning"); }
    QString desktopEntry() const
    {
        return QGuiApplication::desktopFileName();
    }
    QStringList supportedUriSchemes() const { return {}; }
    QStringList supportedMimeTypes() const { return {}; }

public Q_SLOTS:
    void Raise()
    {
        if (m_server)
            Q_EMIT m_server->raiseRequested();
    }
    void Quit() {}

private:
    QPointer<MprisServer> m_server;
};

// org.mpris.MediaPlayer2.Player over VoicePlaybackController.
class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    MprisPlayerAdaptor(QObject *parent, VoicePlaybackController *player,
                       MprisServer *server, const QString &busName)
        : QDBusAbstractAdaptor(parent), m_player(player), m_server(server),
          m_busName(busName)
    {
        setAutoRelaySignals(false);
        m_changeTimer.setSingleShot(true);
        m_changeTimer.setInterval(0);
        connect(&m_changeTimer, &QTimer::timeout, this,
                &MprisPlayerAdaptor::flushChanges);
        if (!m_player)
            return;
        const auto changed = [this] { m_changeTimer.start(); };
        connect(m_player, &VoicePlaybackController::currentChanged, this,
                [this, changed] {
                    resetSeekTracking();
                    changed();
                });
        for (auto signal : { &VoicePlaybackController::playingChanged,
                             &VoicePlaybackController::fetchStateChanged,
                             &VoicePlaybackController::durationChanged,
                             &VoicePlaybackController::seekableChanged,
                             &VoicePlaybackController::rateChanged,
                             &VoicePlaybackController::upNextChanged,
                             &VoicePlaybackController::metadataChanged,
                             &VoicePlaybackController::loadedChanged })
            connect(m_player, signal, this, changed);
        connect(m_player, &VoicePlaybackController::playingChanged, this,
                &MprisPlayerAdaptor::resetSeekTracking);
        // The output is created on the first play; follow its level then.
        connect(m_player, &VoicePlaybackController::playerChanged, this,
                [this] {
                    if (auto *out = m_player->audioOutput())
                        connect(out, &QAudioOutput::volumeChanged, this,
                                &MprisPlayerAdaptor::noteChanged,
                                Qt::UniqueConnection);
                    noteChanged();
                });
        if (auto *out = m_player->audioOutput())
            connect(out, &QAudioOutput::volumeChanged, this,
                    &MprisPlayerAdaptor::noteChanged, Qt::UniqueConnection);
        connect(m_player, &VoicePlaybackController::positionChanged, this,
                &MprisPlayerAdaptor::onPosition);
        if (m_server)
            connect(m_server, &MprisServer::metadataPolicyChanged, this,
                    &MprisPlayerAdaptor::noteChanged);
        m_last = snapshot();
        resetSeekTracking();
    }

    QString playbackStatus() const
    {
        if (!m_player || !m_player->active())
            return QStringLiteral("Stopped");
        if (m_player->playing())
            return QStringLiteral("Playing");
        return QStringLiteral("Paused");
    }
    double rate() const { return m_player ? m_player->rate() : 1.0; }
    void setRate(double value)
    {
        if (!m_player)
            return;
        // The spec: 0 means pause.
        if (value <= 0.0) {
            m_player->pause();
            return;
        }
        m_player->setRate(qBound(kMinRate, value, kMaxRate));
    }
    QVariantMap metadata() const
    {
        QVariantMap md;
        if (!m_player || !m_player->active())
            return md;
        // A stable, opaque id: a salted hash, never the event id itself.
        const QByteArray digest = QCryptographicHash::hash(
            (m_server ? m_server->trackSalt() : QByteArray())
                + m_player->eventId().toUtf8(),
            QCryptographicHash::Sha256);
        md.insert(QStringLiteral("mpris:trackid"),
                  QVariant::fromValue(QDBusObjectPath(
                      QStringLiteral("/org/lightning/track/t")
                      + QString::fromLatin1(digest.toHex().left(24)))));
        const qint64 duration = m_player->duration();
        if (duration > 0)
            md.insert(QStringLiteral("mpris:length"),
                      qlonglong(duration) * 1000);
        QString title;
        QString artist;
        // The notification preview privacy for this room decides how much
        // a desktop widget (and a lock screen) may show.
        const int level = m_server ? m_server->previewLevelFor(
                                         m_player->roomId())
                                   : 2;
        const QString generic = m_player->isVoice()
            ? MprisServer::tr("Voice message") : MprisServer::tr("Audio");
        if (level >= 2) {
            title = generic;
        } else if (level == 1) {
            title = generic;
            artist = m_player->senderName();
        } else if (m_player->isVoice()) {
            title = generic;
            artist = m_player->senderName();
        } else {
            title = m_player->trackTitle();
            if (title.isEmpty())
                title = m_player->filename();
            if (title.isEmpty())
                title = MprisServer::tr("Audio");
            artist = m_player->trackArtist();
            if (artist.isEmpty())
                artist = m_player->senderName();
        }
        md.insert(QStringLiteral("xesam:title"), title);
        if (!artist.isEmpty())
            md.insert(QStringLiteral("xesam:artist"), QStringList{ artist });
        return md;
    }
    double volume() const { return m_player ? m_player->volume() : 1.0; }
    void setVolume(double value)
    {
        if (m_player)
            m_player->setVolume(qBound(0.0, value, 1.0));
    }
    qlonglong position() const
    {
        return m_player ? qlonglong(m_player->position()) * 1000 : 0;
    }
    double minimumRate() const { return kMinRate; }
    double maximumRate() const { return kMaxRate; }
    bool canGoNext() const { return m_player && m_player->hasNext(); }
    bool canGoPrevious() const { return m_player && m_player->active(); }
    bool canPlay() const { return m_player && m_player->active(); }
    bool canPause() const { return m_player && m_player->active(); }
    bool canSeek() const { return m_player && m_player->seekable(); }
    bool canControl() const { return true; }

public Q_SLOTS:
    void Next()
    {
        if (m_player)
            m_player->playNext();
    }
    void Previous()
    {
        if (m_player)
            m_player->restart();
    }
    void Pause()
    {
        if (m_player)
            m_player->pause();
    }
    void PlayPause()
    {
        if (m_player)
            m_player->togglePlayPause();
    }
    void Stop()
    {
        if (m_player)
            m_player->stop();
    }
    void Play()
    {
        if (m_player && m_player->active() && !m_player->playing())
            m_player->togglePlayPause();
    }
    void Seek(qlonglong offsetUs)
    {
        if (!m_player || !m_player->seekable())
            return;
        const qint64 target = m_player->position() + offsetUs / 1000;
        // The spec: seeking past the end acts as Next.
        if (m_player->duration() > 0 && target > m_player->duration()) {
            if (!m_player->playNext())
                m_player->seek(m_player->duration());
            return;
        }
        m_player->seek(qMax<qint64>(0, target));
    }
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs)
    {
        if (!m_player || !m_player->seekable() || positionUs < 0)
            return;
        // A request for a track that is no longer current is ignored.
        const QVariant current = metadata().value(
            QStringLiteral("mpris:trackid"));
        if (current.value<QDBusObjectPath>().path() != trackId.path())
            return;
        if (m_player->duration() > 0 && positionUs / 1000 > m_player->duration())
            return;
        m_player->seek(positionUs / 1000);
    }
    void OpenUri(const QString &) {}

Q_SIGNALS:
    void Seeked(qlonglong positionUs);

private:
    void noteChanged() { m_changeTimer.start(); }

    QVariantMap snapshot() const
    {
        return {
            { QStringLiteral("PlaybackStatus"), playbackStatus() },
            { QStringLiteral("Rate"), rate() },
            { QStringLiteral("Metadata"), metadata() },
            { QStringLiteral("Volume"), volume() },
            { QStringLiteral("CanGoNext"), canGoNext() },
            { QStringLiteral("CanGoPrevious"), canGoPrevious() },
            { QStringLiteral("CanPlay"), canPlay() },
            { QStringLiteral("CanPause"), canPause() },
            { QStringLiteral("CanSeek"), canSeek() },
        };
    }

    void flushChanges()
    {
        const QVariantMap now = snapshot();
        QVariantMap changed;
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
            if (m_last.value(it.key()) != it.value())
                changed.insert(it.key(), it.value());
        }
        m_last = now;
        if (changed.isEmpty())
            return;
        QDBusMessage signal = QDBusMessage::createSignal(
            MprisServer::objectPath(),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << kPlayerInterface << changed << QStringList();
        busFor(m_busName).send(signal);
    }

    void resetSeekTracking()
    {
        m_lastPositionMs = m_player ? m_player->position() : 0;
        m_sincePosition.restart();
    }

    void onPosition()
    {
        if (!m_player || !m_player->active())
            return;
        const qint64 pos = m_player->position();
        const qint64 elapsed = m_player->playing()
            ? qint64(double(m_sincePosition.elapsed()) * m_player->rate())
            : 0;
        const qint64 expected = m_lastPositionMs + elapsed;
        m_lastPositionMs = pos;
        m_sincePosition.restart();
        if (qAbs(pos - expected) > kSeekToleranceMs)
            Q_EMIT Seeked(qlonglong(pos) * 1000);
    }

    QPointer<VoicePlaybackController> m_player;
    QPointer<MprisServer> m_server;
    QString m_busName;
    QTimer m_changeTimer;
    QVariantMap m_last;
    qint64 m_lastPositionMs = 0;
    QElapsedTimer m_sincePosition;
};

MprisServer::MprisServer(VoicePlaybackController *player, QObject *parent)
    : QObject(parent), m_player(player)
{
    m_salt = QByteArray::number(QRandomGenerator::system()->generate64(), 16);
    if (!m_player)
        return;
    // Debounced: a chain advance is stop() then play() in one turn, and the
    // name must not drop off the bus (and the desktop widget flicker) between.
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, &MprisServer::sync);
    connect(m_player, &VoicePlaybackController::currentChanged, this,
            [this, timer] {
                // Joining the bus is not worth a wait; leaving it is.
                if (m_player && m_player->active() && m_serviceName.isEmpty()
                    && !m_exported)
                    sync();
                else
                    timer->start();
            });
}

MprisServer::~MprisServer()
{
    unregisterFromBus();
}

void MprisServer::setBusConnectionName(const QString &busName)
{
    m_busName = busName;
}

void MprisServer::setPreviewPolicy(PreviewPolicy policy)
{
    m_policy = std::move(policy);
    Q_EMIT metadataPolicyChanged();
}

void MprisServer::refreshMetadata()
{
    Q_EMIT metadataPolicyChanged();
}

int MprisServer::previewLevelFor(const QString &roomId) const
{
    // No policy (a bare server): the most private level.
    if (!m_policy)
        return 2;
    const int level = m_policy(roomId);
    return (level >= 0 && level <= 2) ? level : 2;
}

void MprisServer::sync()
{
    // Opted out (the test suites): never on the bus.
    const bool disabled =
        qEnvironmentVariableIntValue("LIGHTNING_DISABLE_MPRIS") != 0;
    const bool wanted = !disabled && m_player && m_player->active();
    if (wanted && !m_exported)
        registerOnBus();
    else if (!wanted && m_exported)
        unregisterFromBus();
}

void MprisServer::registerOnBus()
{
    QDBusConnection bus = busFor(m_busName);
    if (!bus.isConnected()) {
        qCDebug(lcMpris) << "no session bus; media keys not offered";
        return;
    }
    m_exported = new QObject(this);
    new MprisRootAdaptor(m_exported, this);
    new MprisPlayerAdaptor(m_exported, m_player, this, m_busName);
    if (!bus.registerObject(objectPath(), m_exported,
                            QDBusConnection::ExportAdaptors)) {
        qCWarning(lcMpris) << "could not export the MPRIS object";
        delete m_exported;
        m_exported = nullptr;
        return;
    }
    // Asynchronous: a stuck bus must never stall the GUI thread on the first
    // Play (RequestName is a round trip to the daemon).
    requestName(baseServiceName(), true);
}

void MprisServer::requestName(const QString &name, bool fallbackToInstance)
{
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("RequestName"));
    // DBUS_NAME_FLAG_DO_NOT_QUEUE: owned now, or not at all.
    call << name << uint(4);
    const QPointer<QObject> exported = m_exported;
    auto *watcher = new QDBusPendingCallWatcher(
        busFor(m_busName).asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, exported, name, fallbackToInstance](
                QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        // 1: primary owner; 4: already the owner.
        const bool owned =
            !reply.isError() && (reply.value() == 1 || reply.value() == 4);
        // Unregistered (or registered afresh) while the answer was on its way.
        if (!exported || exported != m_exported) {
            if (!owned)
                return;
            if (!m_exported) {
                // Nobody wants the name any more: give it back, or the
                // desktop would keep a player that is not there.
                QDBusMessage release = QDBusMessage::createMethodCall(
                    QStringLiteral("org.freedesktop.DBus"),
                    QStringLiteral("/org/freedesktop/DBus"),
                    QStringLiteral("org.freedesktop.DBus"),
                    QStringLiteral("ReleaseName"));
                release << name;
                busFor(m_busName).send(release);
            } else if (m_serviceName.isEmpty()) {
                // Registered afresh meanwhile: the name serves that one.
                m_serviceName = name;
                Q_EMIT registrationChanged();
            }
            return;
        }
        if (owned) {
            m_serviceName = name;
            qCInfo(lcMpris) << "media keys offered as" << name;
            Q_EMIT registrationChanged();
            return;
        }
        if (fallbackToInstance) {
            // Another Lightning (a second profile) holds the plain name: the
            // spec's per-instance suffix.
            requestName(baseServiceName() + QStringLiteral(".instance")
                            + QString::number(
                                QCoreApplication::applicationPid()),
                        false);
            return;
        }
        qCWarning(lcMpris) << "could not take an MPRIS name";
    });
}

void MprisServer::unregisterFromBus()
{
    if (!m_exported)
        return;
    QDBusConnection bus = busFor(m_busName);
    if (bus.isConnected()) {
        if (!m_serviceName.isEmpty()) {
            QDBusMessage call = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.DBus"),
                QStringLiteral("/org/freedesktop/DBus"),
                QStringLiteral("org.freedesktop.DBus"),
                QStringLiteral("ReleaseName"));
            call << m_serviceName;
            // Fire and forget: nothing to do with the answer.
            bus.send(call);
        }
        bus.unregisterObject(objectPath());
    }
    delete m_exported;
    m_exported = nullptr;
    const bool had = !m_serviceName.isEmpty();
    m_serviceName.clear();
    if (had)
        Q_EMIT registrationChanged();
}

#include "MprisServer.moc"

#else // !HAVE_QT_DBUS

MprisServer::MprisServer(VoicePlaybackController *player, QObject *parent)
    : QObject(parent), m_player(player)
{
}

MprisServer::~MprisServer() = default;

void MprisServer::setBusConnectionName(const QString &busName)
{
    m_busName = busName;
}

void MprisServer::setPreviewPolicy(PreviewPolicy policy)
{
    m_policy = std::move(policy);
}
void MprisServer::refreshMetadata() {}
int MprisServer::previewLevelFor(const QString &) const { return 2; }
void MprisServer::sync() {}
void MprisServer::registerOnBus() {}
void MprisServer::requestName(const QString &, bool) {}
void MprisServer::unregisterFromBus() {}

#endif // HAVE_QT_DBUS
