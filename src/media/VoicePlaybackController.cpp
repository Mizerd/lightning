#include "media/VoicePlaybackController.h"

#include "app/SettingsManager.h"
#include "media/MediaBridge.h"
#include "media/MediaPlaybackController.h"
#include "media/QtAudioGate.h"

#include <QAudioOutput>
#include <QImage>
#include <QLoggingCategory>
#include <QMediaMetaData>
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
// How many audio files' tags and art clipInfo() remembers (RAM only).
constexpr int kMaxClipInfo = 64;
// Tag text is remote input shown as plain text; bound it and drop controls.
constexpr int kMaxTagLength = 200;

QString cleanTag(const QVariant &value)
{
    QString text = value.typeId() == QMetaType::QStringList
        ? value.toStringList().join(QStringLiteral(", "))
        : value.toString();
    QString out;
    out.reserve(qMin<qsizetype>(text.size(), kMaxTagLength));
    for (const QChar c : std::as_const(text)) {
        if (out.size() >= kMaxTagLength)
            break;
        // Line breaks and other controls would break a one-line label.
        out.append(c.category() == QChar::Other_Control
                           || c == QChar::LineSeparator
                           || c == QChar::ParagraphSeparator
                       ? QChar(u' ') : c);
    }
    return out.simplified();
}
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
        connect(m_settings, &SettingsManager::voicePlaybackRateChanged, this,
                [this] {
                    applySettings();
                    if (m_isVoice)
                        Q_EMIT rateChanged();
                });
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
    // A start position not applied yet is where the clip is about to be.
    if (m_pendingSeekMs >= 0)
        return m_pendingSeekMs;
    return (m_player && m_loaded) ? m_player->position() : 0;
}

qreal VoicePlaybackController::rate() const
{
    // Without settings (a bare controller) a voice clip keeps its own too.
    if (!m_isVoice || !m_settings)
        return m_clipRate;
    return m_settings->voicePlaybackRate();
}

QString VoicePlaybackController::trackTitle() const
{
    return m_clipInfo.value(m_mediaKey).value(QStringLiteral("title"))
        .toString();
}

QString VoicePlaybackController::trackArtist() const
{
    return m_clipInfo.value(m_mediaKey).value(QStringLiteral("artist"))
        .toString();
}

QString VoicePlaybackController::artworkSource() const
{
    return m_clipInfo.value(m_mediaKey).value(QStringLiteral("artworkSource"))
        .toString();
}

bool VoicePlaybackController::rowOnScreen() const
{
    return active() && !m_visibleRows.isEmpty();
}

QObject *VoicePlaybackController::visibleRow() const
{
    return rowOnScreen() ? *m_visibleRows.cbegin() : nullptr;
}

void VoicePlaybackController::clearSession()
{
    stop();
    if (m_clipInfo.isEmpty())
        return;
    m_clipInfo.clear();
    m_clipInfoOrder.clear();
    ++m_clipInfoRevision;
    Q_EMIT metadataChanged();
}

QVariantMap VoicePlaybackController::clipInfo(const QString &mediaKey) const
{
    if (mediaKey.isEmpty())
        return {};
    return m_clipInfo.value(mediaKey);
}

void VoicePlaybackController::rememberEventCover(const QString &mediaKey,
                                                 const QString &source)
{
    if (mediaKey.isEmpty()
        || !source.startsWith(QLatin1String("image://lightning-media/")))
        return;
    QVariantMap info = m_clipInfo.value(mediaKey);
    if (!info.value(QStringLiteral("artworkSource")).toString().isEmpty())
        return;
    info.insert(QStringLiteral("artworkSource"), source);
    m_clipInfo.insert(mediaKey, info);
    m_clipInfoOrder.removeAll(mediaKey);
    m_clipInfoOrder.append(mediaKey);
    while (m_clipInfoOrder.size() > kMaxClipInfo)
        m_clipInfo.remove(m_clipInfoOrder.takeFirst());
    ++m_clipInfoRevision;
    Q_EMIT metadataChanged();
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
            [this](bool seekable) {
                Q_EMIT seekableChanged();
                if (seekable)
                    applyPendingSeek();
            });
    connect(m_player.get(), &QMediaPlayer::metaDataChanged, this,
            &VoicePlaybackController::readMetadata);
    // A pause the reader (or another card taking the speaker) makes is not
    // the call's: the end of the call must not undo it. A resume, likewise,
    // ends the call's claim.
    connect(m_player.get(), &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                if (!m_callPausing && state != QMediaPlayer::PausedState)
                    setPausedForCall(false);
            });
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
    if (m_player)
        m_player->setPlaybackRate(rate());
    if (m_settings && m_output)
        m_output->setVolume(float(m_settings->mediaVolume()));
}

void VoicePlaybackController::play(const QString &eventId,
                                   const QString &roomId,
                                   const QString &mediaKey,
                                   const QVariantMap &meta)
{
    if (eventId.isEmpty() || mediaKey.isEmpty())
        return;
    if (isCurrent(eventId) && mediaKey == m_mediaKey) {
        const qint64 startAt =
            meta.value(QStringLiteral("startAtMs"), -1).toLongLong();
        if (startAt >= 0)
            seek(startAt);
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

    // Starting the head of the run keeps the rest of it (the chain, or the
    // bar's Next); anything else starts a new one, resolved by AppController.
    QVariantList carried;
    if (!m_upNext.isEmpty()
        && m_upNext.constFirst().toMap().value(QStringLiteral("eventId"))
               .toString() == eventId)
        carried = m_upNext.mid(1);

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
    m_waveform = meta.value(QStringLiteral("waveform")).toList();
    // Bounded like the ingest's (at most 96 buckets).
    if (m_waveform.size() > 96)
        m_waveform = m_waveform.mid(0, 96);
    m_declaredDurationMs =
        qMax<qint64>(0, meta.value(QStringLiteral("durationMs")).toLongLong());
    m_threadRootId = meta.value(QStringLiteral("threadRootId")).toString();
    m_roomName = meta.value(QStringLiteral("roomName")).toString();
    if (m_roomName.isEmpty() && m_roomNameResolver)
        m_roomName = m_roomNameResolver(roomId);
    const qint64 startAt =
        meta.value(QStringLiteral("startAtMs"), -1).toLongLong();
    m_pendingSeekMs = startAt > 0 ? startAt : -1;
    m_upNext = carried;
    m_clipRate = 1.0;
    qCDebug(lcVoicePlayback) << "play; voice:" << m_isVoice;
    Q_EMIT currentChanged();
    Q_EMIT durationChanged();
    Q_EMIT positionChanged();
    Q_EMIT rateChanged();
    Q_EMIT metadataChanged();
    if (!m_upNext.isEmpty())
        Q_EMIT upNextChanged();
    applySettings();
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
    dropWaitingStart();
    if (m_player && playing()) {
        m_player->pause();
        return;
    }
    // Still fetching: it would start by itself once the file arrives, in the
    // middle of whatever asked for the pause (a recording, a call). It stays
    // the current clip, and Play fetches it again.
    if (active() && !m_loaded && m_fetchState == kFetching) {
        cancelFetch();
        setFetchState(kIdle);
    }
}

void VoicePlaybackController::resume()
{
    if (!active())
        return;
    // The reader (or the end of the call) resumed it: the call's claim ends.
    setPausedForCall(false);
    if (!m_loaded) {
        // Still fetching: playback starts when the file arrives.
        if (m_fetchState != kFetching)
            startOrFetch();
        return;
    }
    if (m_audibility)
        m_audibility->acquire(ownerKey());
    playGated();
}

void VoicePlaybackController::seek(qint64 positionMs)
{
    if (!active())
        return;
    const qint64 total = duration();
    qint64 target = qMax<qint64>(0, positionMs);
    if (total > 0)
        target = qMin(target, total);
    if (!m_player || !m_loaded || !m_player->isSeekable()) {
        // Still fetching or opening: start there once the file is open.
        m_pendingSeekMs = target;
        Q_EMIT positionChanged();
        return;
    }
    m_pendingSeekMs = -1;
    m_player->setPosition(target);
}

void VoicePlaybackController::applyPendingSeek()
{
    if (m_pendingSeekMs < 0 || !m_player || !m_loaded
        || !m_player->isSeekable())
        return;
    // Only once the backend has the media open: a position set while it is
    // still loading is dropped by the FFmpeg backend (measured live: an Opus
    // clip asked to start at 0:50 started at 0:00, while a WAV in the tests
    // did not show it).
    const auto status = m_player->mediaStatus();
    if (status != QMediaPlayer::LoadedMedia
        && status != QMediaPlayer::BufferingMedia
        && status != QMediaPlayer::BufferedMedia)
        return;
    const qint64 target = m_pendingSeekMs;
    m_pendingSeekMs = -1;
    m_player->setPosition(target);
    Q_EMIT positionChanged();
}

bool VoicePlaybackController::playNext()
{
    if (m_upNext.isEmpty())
        return false;
    const QVariantMap next = m_upNext.constFirst().toMap();
    const QString eventId = next.value(QStringLiteral("eventId")).toString();
    const QString mediaKey = next.value(QStringLiteral("mediaKey")).toString();
    const QString roomId = next.value(QStringLiteral("roomId")).toString();
    if (eventId.isEmpty() || mediaKey.isEmpty())
        return false;
    QVariantMap meta = next;
    if (meta.value(QStringLiteral("roomName")).toString().isEmpty())
        meta.insert(QStringLiteral("roomName"), m_roomName);
    play(eventId, roomId.isEmpty() ? m_roomId : roomId, mediaKey, meta);
    return true;
}

void VoicePlaybackController::restart()
{
    if (!active())
        return;
    seek(0);
    if (!playing())
        resume();
}

void VoicePlaybackController::setRate(qreal rate)
{
    if (!(rate >= 0.25) || !(rate <= 4.0))
        return;
    if ((!active() || m_isVoice) && m_settings) {
        // A voice message, or no clip: the remembered voice speed.
        m_settings->setVoicePlaybackRate(rate);
        return;
    }
    if (!active())
        return;
    if (qFuzzyCompare(m_clipRate + 1.0, rate + 1.0))
        return;
    m_clipRate = rate;
    applySettings();
    Q_EMIT rateChanged();
}

qreal VoicePlaybackController::volume() const
{
    if (m_settings)
        return m_settings->mediaVolume();
    return m_output ? qreal(m_output->volume()) : 1.0;
}

void VoicePlaybackController::setVolume(qreal volume)
{
    const qreal v = qBound<qreal>(0.0, volume, 1.0);
    if (m_settings)
        m_settings->setMediaVolume(v); // applySettings() follows it
    else if (m_output)
        m_output->setVolume(float(v));
}

void VoicePlaybackController::setUpNext(const QVariantList &queue)
{
    if (!active() || m_upNext == queue)
        return;
    m_upNext = queue;
    Q_EMIT upNextChanged();
}

void VoicePlaybackController::setPausedForCall(bool paused)
{
    if (m_pausedForCall == paused)
        return;
    m_pausedForCall = paused;
    Q_EMIT pausedForCallChanged();
}

void VoicePlaybackController::pauseForCall()
{
    if (!active())
        return;
    // Only a clip that was going to make sound has anything to resume: one
    // playing, one still fetching, or one whose start waits at the audio gate
    // (pause() drops that start, so it neither begins mid-call nor loses its
    // resume; resumeAfterCall() goes through the gate again).
    const bool wasRunning = playing()
        || (!m_loaded && m_fetchState == kFetching)
        || m_gateWaiting;
    m_callPausing = true;
    pause();
    m_callPausing = false;
    if (wasRunning)
        setPausedForCall(true);
}

void VoicePlaybackController::resumeAfterCall()
{
    if (!m_pausedForCall)
        return;
    setPausedForCall(false);
    // Not over something else the reader started meanwhile (a video took
    // the speaker): that one keeps it.
    const QString owner = m_audibility ? m_audibility->audibleOwner()
                                       : QString();
    if (active() && !playing() && (owner.isEmpty() || owner == ownerKey()))
        resume();
}

void VoicePlaybackController::reportRowVisible(QObject *row,
                                               const QString &eventId,
                                               bool visible)
{
    if (!row)
        return;
    const bool before = rowOnScreen();
    if (visible && active() && isCurrent(eventId)) {
        if (!m_visibleRows.contains(row)) {
            m_visibleRows.insert(row);
            connect(row, &QObject::destroyed, this,
                    &VoicePlaybackController::onRowDestroyed,
                    Qt::UniqueConnection);
        }
    } else {
        m_visibleRows.remove(row);
    }
    if (rowOnScreen() != before)
        Q_EMIT rowOnScreenChanged();
}

void VoicePlaybackController::onRowDestroyed(QObject *row)
{
    const bool before = rowOnScreen();
    m_visibleRows.remove(row);
    if (rowOnScreen() != before)
        Q_EMIT rowOnScreenChanged();
}

void VoicePlaybackController::readMetadata()
{
    if (!m_player || !active() || m_isVoice || m_mediaKey.isEmpty())
        return;
    const QMediaMetaData md = m_player->metaData();
    const QString title = cleanTag(md.value(QMediaMetaData::Title));
    QString artist = cleanTag(md.value(QMediaMetaData::ContributingArtist));
    if (artist.isEmpty())
        artist = cleanTag(md.value(QMediaMetaData::AlbumArtist));
    QString art;
    QVariant image = md.value(QMediaMetaData::CoverArtImage);
    if (!image.isValid() || image.value<QImage>().isNull())
        image = md.value(QMediaMetaData::ThumbnailImage);
    if (m_bridge && image.isValid())
        art = m_bridge->audioArtworkSource(m_mediaKey, image);
    QVariantMap info = m_clipInfo.value(m_mediaKey);
    // A later, emptier report (backends deliver tags in steps) never erases
    // what an earlier one found.
    if (!title.isEmpty())
        info.insert(QStringLiteral("title"), title);
    if (!artist.isEmpty())
        info.insert(QStringLiteral("artist"), artist);
    if (!art.isEmpty())
        info.insert(QStringLiteral("artworkSource"), art);
    if (info == m_clipInfo.value(m_mediaKey))
        return;
    m_clipInfo.insert(m_mediaKey, info);
    m_clipInfoOrder.removeAll(m_mediaKey);
    m_clipInfoOrder.append(m_mediaKey);
    while (m_clipInfoOrder.size() > kMaxClipInfo)
        m_clipInfo.remove(m_clipInfoOrder.takeFirst());
    ++m_clipInfoRevision;
    Q_EMIT metadataChanged();
}

void VoicePlaybackController::stop()
{
    if (!active() && !m_player)
        return;
    dropWaitingStart();
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
    m_waveform.clear();
    m_declaredDurationMs = 0;
    m_pendingSeekMs = -1;
    m_clipRate = 1.0;
    const bool hadQueue = !m_upNext.isEmpty();
    m_upNext.clear();
    const bool hadRows = !m_visibleRows.isEmpty();
    m_visibleRows.clear();
    setPausedForCall(false);
    if (wasActive) {
        qCDebug(lcVoicePlayback) << "stopped";
        Q_EMIT currentChanged();
        Q_EMIT positionChanged();
        Q_EMIT durationChanged();
        Q_EMIT playingChanged();
        Q_EMIT rateChanged();
        Q_EMIT metadataChanged();
    }
    if (hadQueue)
        Q_EMIT upNextChanged();
    if (hadRows)
        Q_EMIT rowOnScreenChanged();
}

void VoicePlaybackController::reclaimAudibility()
{
    // Paused as well as playing: a clip paused before the switch is still
    // the one Space should resume (design-media U5). Never one still being
    // fetched or failed, which has nothing to toggle.
    if (m_audibility && active() && m_loaded)
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
    playGated();
    // A file already open (cached) may be seekable at once.
    applyPendingSeek();
}

void VoicePlaybackController::setAudioGateForTest(
    lightning::media::QtAudioGate *gate)
{
    m_audioGate = gate;
}

void VoicePlaybackController::playGated()
{
    // QMediaPlayer::play() opens the audio stream, which aborts the process
    // while Qt's PulseAudio backend has no connection (QtAudioGate.h). Held
    // until it has one; the newest request wins, and a pause or stop in the
    // meantime cancels it.
    using lightning::media::QtAudioGate;
    QtAudioGate *gate = m_audioGate ? m_audioGate.data() : QtAudioGate::current();
    const quint64 tag = ++m_playTag;
    if (!gate) {
        ++m_playerStarts;
        m_player->play();
        return;
    }
    gate->cancel(this);
    // Waiting at the gate is "running" to a call that pauses the clip
    // (pauseForCall()); whenReady() may also start it at once.
    m_gateWaiting = true;
    gate->whenReady(
        this,
        [this, tag] {
            if (tag != m_playTag || !m_player || !m_loaded)
                return;
            m_gateWaiting = false;
            ++m_playerStarts;
            m_player->play();
        },
        [this, tag] {
            if (tag != m_playTag || !active())
                return;
            m_gateWaiting = false;
            qCWarning(lcVoicePlayback)
                << "the sound server did not come back; playback not started";
            setFetchState(kFailed);
        });
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
    if (owner.isEmpty() || owner == ownerKey())
        return;
    // A start still waiting at the audio gate lost the speaker too: it must
    // not begin later over the card that took it.
    dropWaitingStart();
    if (playing())
        m_player->pause();
}

void VoicePlaybackController::dropWaitingStart()
{
    ++m_playTag;
    m_gateWaiting = false;
    using lightning::media::QtAudioGate;
    if (QtAudioGate *gate =
            m_audioGate ? m_audioGate.data() : QtAudioGate::current())
        gate->cancel(this);
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
        const QString room = m_roomId;
        const QString thread = m_threadRootId;
        const bool voice = m_isVoice;
        QMetaObject::invokeMethod(this, [this, finished, room, thread, voice] {
            if (m_eventId != finished)
                return;
            // The run as it stands now, before stop() forgets it.
            const QVariantList queue = m_upNext;
            stop();
            Q_EMIT clipFinished(finished, room, thread, voice, queue);
        }, Qt::QueuedConnection);
        return;
    }
    if ((status == int(QMediaPlayer::LoadedMedia)
         || status == int(QMediaPlayer::BufferedMedia)) && active())
        applyPendingSeek();
    if (status == int(QMediaPlayer::InvalidMedia) && active())
        setFetchState(kFailed);
}

#include "VoicePlaybackController.moc"
