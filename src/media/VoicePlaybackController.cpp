#include "media/VoicePlaybackController.h"

#include "app/SettingsManager.h"
#include "media/MediaBridge.h"
#include "media/MediaPlaybackController.h"

#include <QAudioOutput>
#include <QLoggingCategory>
#include <QMediaPlayer>
#include <QUrl>

Q_LOGGING_CATEGORY(lcVoicePlayback, "matrix.media.voiceplayback")

// The audio output the voice player plays through. A QAudioOutput with the one
// extra property MediaVolumeControl.qml writes (`userUnmuted`), so the shared
// volume control works on it exactly as on a QML AudioOutput.
class VoiceAudioOutput : public QAudioOutput
{
    Q_OBJECT
    Q_PROPERTY(bool userUnmuted MEMBER m_userUnmuted NOTIFY userUnmutedChanged)

public:
    explicit VoiceAudioOutput(QObject *parent = nullptr)
        : QAudioOutput(parent) {}

Q_SIGNALS:
    void userUnmutedChanged();

private:
    bool m_userUnmuted = false;
};

namespace {
const QString kIdle = QStringLiteral("idle");
const QString kFetching = QStringLiteral("fetching");
const QString kFailed = QStringLiteral("failed");
} // namespace

VoicePlaybackController::VoicePlaybackController(QObject *parent)
    : QObject(parent)
{
}

VoicePlaybackController::~VoicePlaybackController()
{
    // The player first: it must not deliver state into a half-destroyed
    // controller, and it holds the output.
    if (m_player)
        m_player->disconnect(this);
    m_player.reset();
    m_output.reset();
}

void VoicePlaybackController::setMediaBridge(MediaBridge *bridge)
{
    if (m_bridge == bridge)
        return;
    if (m_bridge)
        m_bridge->disconnect(this);
    m_bridge = bridge;
    if (!m_bridge)
        return;
    connect(m_bridge, &MediaBridge::playableMediaReady, this,
            &VoicePlaybackController::onPlayableReady);
    connect(m_bridge, &MediaBridge::mediaFetchFailed, this,
            [this](const QString &cacheKey, const QString &) {
                onFetchFailed(cacheKey);
            });
}

void VoicePlaybackController::setAudibility(MediaPlaybackController *playback)
{
    if (m_audibility == playback)
        return;
    if (m_audibility)
        m_audibility->disconnect(this);
    m_audibility = playback;
    if (!m_audibility)
        return;
    connect(m_audibility, &MediaPlaybackController::audibleOwnerChanged, this,
            &VoicePlaybackController::onAudibleOwnerChanged);
    connect(m_audibility, &MediaPlaybackController::togglePlayPauseRequested,
            this, &VoicePlaybackController::onToggleRequested);
}

void VoicePlaybackController::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        m_settings->disconnect(this);
    m_settings = settings;
    if (m_settings) {
        // The remembered level and speed apply to this player as to every
        // card. The volume control writes the setting as well as the output,
        // so following the setting never fights the user.
        connect(m_settings, &SettingsManager::mediaVolumeChanged, this,
                &VoicePlaybackController::applySettings);
        connect(m_settings, &SettingsManager::mediaPlaybackRateChanged, this,
                &VoicePlaybackController::applySettings);
    }
    applySettings();
}

void VoicePlaybackController::setRoomNameResolver(RoomNameResolver resolver)
{
    m_roomNameResolver = std::move(resolver);
}

void VoicePlaybackController::setSourceResolverForTest(SourceResolver resolver)
{
    m_testSourceResolver = std::move(resolver);
}

bool VoicePlaybackController::playing() const
{
    return m_player
           && m_player->playbackState() == QMediaPlayer::PlayingState;
}

qint64 VoicePlaybackController::position() const
{
    return (m_player && m_loaded) ? m_player->position() : 0;
}

qint64 VoicePlaybackController::duration() const
{
    const qint64 decoded = (m_player && m_loaded) ? m_player->duration() : 0;
    return decoded > 0 ? decoded : m_declaredDurationMs;
}

bool VoicePlaybackController::seekable() const
{
    return m_player && m_loaded && m_player->isSeekable();
}

QString VoicePlaybackController::ownerKey() const
{
    return m_eventId.isEmpty()
        ? QString()
        : QStringLiteral("voice\u001f") + m_eventId;
}

QString VoicePlaybackController::cacheKey() const
{
    // MediaBridge's playable cache key (mediaCacheKey(key, 0)).
    return QStringLiteral("full:") + m_mediaKey;
}

bool VoicePlaybackController::isCurrent(const QString &eventId) const
{
    return !eventId.isEmpty() && eventId == m_eventId;
}

void VoicePlaybackController::ensurePlayer()
{
    if (m_player)
        return;
    m_output = std::make_unique<VoiceAudioOutput>();
    m_player = std::make_unique<QMediaPlayer>();
    m_player->setAudioOutput(m_output.get());
    connect(m_player.get(), &QMediaPlayer::playbackStateChanged, this,
            &VoicePlaybackController::playingChanged);
    connect(m_player.get(), &QMediaPlayer::positionChanged, this,
            &VoicePlaybackController::positionChanged);
    connect(m_player.get(), &QMediaPlayer::durationChanged, this,
            &VoicePlaybackController::durationChanged);
    connect(m_player.get(), &QMediaPlayer::seekableChanged, this,
            &VoicePlaybackController::seekableChanged);
    connect(m_player.get(), &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                onMediaStatus(int(status));
            });
    connect(m_player.get(), &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &) {
                if (error == QMediaPlayer::NoError || !active())
                    return;
                qCWarning(lcVoicePlayback) << "player error" << error;
                setFetchState(kFailed);
            });
    applySettings();
    Q_EMIT playerChanged();
}

void VoicePlaybackController::applySettings()
{
    if (!m_settings)
        return;
    if (m_output)
        m_output->setVolume(float(m_settings->mediaVolume()));
    if (m_player)
        m_player->setPlaybackRate(m_settings->mediaPlaybackRate());
}

void VoicePlaybackController::play(const QString &eventId,
                                   const QString &roomId,
                                   const QString &mediaKey,
                                   const QVariantMap &meta)
{
    if (eventId.isEmpty() || mediaKey.isEmpty())
        return;
    if (isCurrent(eventId) && mediaKey == m_mediaKey) {
        if (m_fetchState == kFailed) {
            if (m_bridge)
                m_bridge->retry(cacheKey());
            setLoaded(false);
            startOrFetch();
        } else {
            resume();
        }
        return;
    }

    // One clip at a time: the previous one is unloaded, not merely paused.
    stop();

    m_eventId = eventId;
    m_roomId = roomId;
    m_mediaKey = mediaKey;
    m_senderName = meta.value(QStringLiteral("senderName")).toString();
    m_senderId = meta.value(QStringLiteral("senderId")).toString();
    m_senderAvatarMxc =
        meta.value(QStringLiteral("senderAvatarMxc")).toString();
    m_filename = meta.value(QStringLiteral("filename")).toString();
    m_isVoice = meta.value(QStringLiteral("isVoice")).toBool();
    m_declaredDurationMs =
        qMax<qint64>(0, meta.value(QStringLiteral("durationMs")).toLongLong());
    m_threadRootId = meta.value(QStringLiteral("threadRootId")).toString();
    m_roomName = meta.value(QStringLiteral("roomName")).toString();
    if (m_roomName.isEmpty() && m_roomNameResolver)
        m_roomName = m_roomNameResolver(roomId);
    qCDebug(lcVoicePlayback) << "play; voice:" << m_isVoice;
    Q_EMIT currentChanged();
    Q_EMIT durationChanged();
    Q_EMIT positionChanged();
    startOrFetch();
}

void VoicePlaybackController::toggle(const QString &eventId,
                                     const QString &roomId,
                                     const QString &mediaKey,
                                     const QVariantMap &meta)
{
    if (isCurrent(eventId) && playing()) {
        pause();
        return;
    }
    play(eventId, roomId, mediaKey, meta);
}

void VoicePlaybackController::togglePlayPause()
{
    if (!active())
        return;
    if (playing())
        pause();
    else if (m_fetchState == kFailed)
        play(m_eventId, m_roomId, m_mediaKey, {});
    else
        resume();
}

void VoicePlaybackController::pause()
{
    if (m_player && playing())
        m_player->pause();
}

void VoicePlaybackController::resume()
{
    if (!active())
        return;
    if (!m_loaded) {
        // Still fetching: playback starts when the file arrives.
        if (m_fetchState != kFetching)
            startOrFetch();
        return;
    }
    if (m_audibility)
        m_audibility->acquire(ownerKey());
    m_player->play();
}

void VoicePlaybackController::seek(qint64 positionMs)
{
    if (!m_player || !m_loaded)
        return;
    const qint64 total = duration();
    qint64 target = qMax<qint64>(0, positionMs);
    if (total > 0)
        target = qMin(target, total);
    m_player->setPosition(target);
}

void VoicePlaybackController::stop()
{
    if (!active() && !m_player)
        return;
    const bool wasActive = active();
    const QString owner = ownerKey();
    if (m_player) {
        m_player->stop();
        // Releases the decrypted temp file's handle.
        m_player->setSource(QUrl());
    }
    unpin();
    cancelFetch();
    if (m_audibility && !owner.isEmpty())
        m_audibility->release(owner);
    setLoaded(false);
    setFetchState(kIdle);
    m_eventId.clear();
    m_roomId.clear();
    m_roomName.clear();
    m_threadRootId.clear();
    m_senderName.clear();
    m_senderId.clear();
    m_senderAvatarMxc.clear();
    m_mediaKey.clear();
    m_filename.clear();
    m_isVoice = false;
    m_declaredDurationMs = 0;
    if (wasActive) {
        qCDebug(lcVoicePlayback) << "stopped";
        Q_EMIT currentChanged();
        Q_EMIT positionChanged();
        Q_EMIT durationChanged();
        Q_EMIT playingChanged();
    }
}

void VoicePlaybackController::reclaimAudibility()
{
    if (m_audibility && playing())
        m_audibility->acquire(ownerKey());
}

void VoicePlaybackController::handleRedaction(const QString &roomId,
                                              const QString &eventId)
{
    if (active() && isCurrent(eventId)
        && (roomId.isEmpty() || roomId == m_roomId)) {
        qCInfo(lcVoicePlayback) << "playing event was redacted; stopping";
        stop();
    }
}

void VoicePlaybackController::startOrFetch()
{
    QString url;
    if (m_testSourceResolver)
        url = m_testSourceResolver(m_mediaKey);
    else if (m_bridge)
        url = m_bridge->playableSource(m_mediaKey);
    if (!url.isEmpty()) {
        startSource(url);
        return;
    }
    if (!m_testSourceResolver && !m_bridge) {
        setFetchState(kFailed);
        return;
    }
    // MediaBridge writes the file on a worker thread; playableMediaReady
    // brings us back. A fetch it refuses outright reports failure there too.
    m_fetchingKey = m_mediaKey;
    setFetchState(kFetching);
}

void VoicePlaybackController::startSource(const QString &url)
{
    ensurePlayer();
    m_fetchingKey.clear();
    setFetchState(kIdle);
    m_player->setSource(QUrl(url));
    // The player holds the materialised file open; the LRU must not delete it
    // under a seek or a replay.
    if (m_pinnedKey != m_mediaKey) {
        unpin();
        if (m_bridge) {
            m_bridge->pinPlayable(m_mediaKey);
            m_pinnedKey = m_mediaKey;
        }
    }
    setLoaded(true);
    if (m_audibility)
        m_audibility->acquire(ownerKey());
    m_player->play();
}

void VoicePlaybackController::setFetchState(const QString &state)
{
    if (m_fetchState == state)
        return;
    m_fetchState = state;
    Q_EMIT fetchStateChanged();
}

void VoicePlaybackController::setLoaded(bool loaded)
{
    if (m_loaded == loaded)
        return;
    m_loaded = loaded;
    Q_EMIT loadedChanged();
    Q_EMIT seekableChanged();
    Q_EMIT durationChanged();
    Q_EMIT positionChanged();
}

void VoicePlaybackController::cancelFetch()
{
    if (m_fetchingKey.isEmpty())
        return;
    if (m_bridge)
        m_bridge->cancelPlayable(m_fetchingKey);
    m_fetchingKey.clear();
}

void VoicePlaybackController::unpin()
{
    if (m_pinnedKey.isEmpty())
        return;
    if (m_bridge)
        m_bridge->unpinPlayable(m_pinnedKey);
    m_pinnedKey.clear();
}

void VoicePlaybackController::onAudibleOwnerChanged()
{
    // Another card (a video) took the speaker. An empty owner is a stopAll()
    // (room switch): that is AppController's to resolve, not a competitor.
    if (!m_audibility)
        return;
    const QString owner = m_audibility->audibleOwner();
    if (!owner.isEmpty() && owner != ownerKey() && playing())
        m_player->pause();
}

void VoicePlaybackController::onToggleRequested(const QString &ownerKey)
{
    if (active() && ownerKey == this->ownerKey())
        togglePlayPause();
}

void VoicePlaybackController::onPlayableReady(const QString &cacheKey)
{
    if (m_fetchState != kFetching || cacheKey != this->cacheKey())
        return;
    const QString url = m_testSourceResolver
        ? m_testSourceResolver(m_mediaKey)
        : (m_bridge ? m_bridge->playableSource(m_mediaKey) : QString());
    if (url.isEmpty())
        return;
    startSource(url);
}

void VoicePlaybackController::onFetchFailed(const QString &cacheKey)
{
    if (m_fetchState != kFetching || cacheKey != this->cacheKey())
        return;
    m_fetchingKey.clear(); // the fetch is over; nothing to cancel
    setFetchState(kFailed);
}

void VoicePlaybackController::onMediaStatus(int status)
{
    if (status == int(QMediaPlayer::EndOfMedia) && active()) {
        // The clip finished: clear the bar and return the row to idle. Queued,
        // so the source is not reset from inside the player's own emission;
        // keyed, so a clip started meanwhile is not the one stopped.
        const QString finished = m_eventId;
        QMetaObject::invokeMethod(this, [this, finished] {
            if (m_eventId == finished)
                stop();
        }, Qt::QueuedConnection);
        return;
    }
    if (status == int(QMediaPlayer::InvalidMedia) && active())
        setFetchState(kFailed);
}

#include "VoicePlaybackController.moc"
