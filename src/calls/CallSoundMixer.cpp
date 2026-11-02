#include "calls/CallSoundMixer.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QTimer>

#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lcCallSound)

namespace {
constexpr int kSampleRate = 48000;
/// A stall check this much later than its interval means the GUI thread was
/// held up, and the outputs (refilled on it) with it: it judges nothing.
constexpr qint64 kLateMarginMs = 400;
constexpr qint64 kProbeFrames = qint64(kSampleRate) * CueEngine::kProbeMs / 1000;
}

CueEngine::CueEngine(QStringList sounds, Hooks hooks, QObject *parent)
    : QObject(parent), m_sounds(std::move(sounds)), m_hooks(std::move(hooks))
{
    m_clock.start();
    connect(&m_stallTimer, &QTimer::timeout, this, &CueEngine::onStallTick);
}

CueEngine::~CueEngine()
{
    stopAll();
}

void CueEngine::preload()
{
    for (const QString &sound : std::as_const(m_sounds))
        decodable(sound);
    laneFor(QString());
    refreshReadiness();
}

void CueEngine::play(const QString &sound, float gain, const QString &wanted)
{
    if (sound == m_loopSound || !m_sounds.contains(sound))
        return;
    Lane *lane = laneFor(wanted);
    if (!lane)
        return;
    const std::optional<callsound::Pcm> cue = pcm(sound, lane->mixer->channels());
    if (!cue)
        return;
    lane->mixer->start(cue->data, gain, false);
    wake(lane);
}

void CueEngine::loop(const QString &sound, float gain, const QString &wanted)
{
    if (sound == m_loopSound) {
        m_loopGain = gain;
        const auto it = m_lanes.find(m_loopLane);
        if (it != m_lanes.end())
            it->second->mixer->setGain(m_loopVoice, gain);
        return;
    }
    stopLoopVoice();
    m_loopSound.clear();
    m_loopDevice.clear();
    if (sound.isEmpty() || !m_sounds.contains(sound))
        return;
    m_loopSound = sound;
    m_loopGain = gain;
    m_loopDevice = wanted;
    if (Lane *lane = laneFor(wanted)) {
        startLoop(lane);
        wake(lane);
    }
}

int CueEngine::reloadUnusable()
{
    m_failed.clear();
    m_stallFailed.clear();
    ++m_retryGeneration;
    m_retryDue = false;
    QSet<QByteArray> before;
    for (const auto &[id, lane] : m_lanes)
        before.insert(id);
    QList<QByteArray> gone;
    for (const auto &[id, lane] : m_lanes) {
        const std::optional<QByteArray> now =
            m_hooks.resolveOutput(QString::fromUtf8(id));
        if (!now || *now != id)
            gone << id;
    }
    for (const QByteArray &id : std::as_const(gone))
        closeLane(id);

    resumeLoop();

    Lane *defaultLane = laneFor(QString());
    QList<QByteArray> stale;
    for (const auto &[id, lane] : m_lanes) {
        if (lane.get() != defaultLane && id != m_loopLane && lane->mixer->idle())
            stale << id;
    }
    for (const QByteArray &id : std::as_const(stale))
        closeLane(id);

    QList<QByteArray> kept;
    for (const auto &[id, lane] : m_lanes) {
        if (before.contains(id))
            kept << id;
    }
    for (const QByteArray &id : std::as_const(kept)) {
        const auto it = m_lanes.find(id);
        if (it == m_lanes.end() || !it->second->output)
            continue;
        Lane *lane = it->second.get();
        lane->unproven = false;
        if (!lane->feed->busy())
            lane->feed->addSilence(kProbeFrames);
        lane->probed = true;
        wake(lane);
    }
    return refreshReadiness();
}

void CueEngine::resumeLoop()
{
    if (m_loopSound.isEmpty())
        return;
    const std::optional<QByteArray> target = m_hooks.resolveOutput(m_loopDevice);
    const bool playing = m_lanes.count(m_loopLane)
        && m_lanes.at(m_loopLane)->mixer->isPlaying(m_loopVoice);
    if (target && (!playing || *target != m_loopLane)) {
        stopLoopVoice();
        if (Lane *lane = laneFor(m_loopDevice)) {
            startLoop(lane);
            wake(lane);
        }
    }
}

void CueEngine::stopAll()
{
    m_loopSound.clear();
    m_loopDevice.clear();
    m_loopLane.clear();
    m_loopVoice = 0;
    QList<QByteArray> ids;
    for (const auto &[id, lane] : m_lanes)
        ids << id;
    for (const QByteArray &id : std::as_const(ids))
        closeLane(id);
    m_stallTimer.stop();
    ++m_retryGeneration;
    m_retryDue = false;
    m_serverLiveSinceMs = -1;
}

CueOutput *CueEngine::outputForTest(const QByteArray &id) const
{
    QByteArray key = id;
    if (key.isEmpty()) {
        const std::optional<QByteArray> def = m_hooks.resolveOutput(QString());
        if (!def)
            return nullptr;
        key = *def;
    }
    const auto it = m_lanes.find(key);
    return it == m_lanes.end() ? nullptr : it->second->output.data();
}

QList<QByteArray> CueEngine::outputIdsForTest() const
{
    QList<QByteArray> ids;
    for (const auto &[id, lane] : m_lanes)
        ids << id;
    return ids;
}

int CueEngine::voicesForTest(const QByteArray &id) const
{
    int voices = 0;
    for (const auto &[laneId, lane] : m_lanes) {
        if (id.isEmpty() || id == laneId)
            voices += lane->mixer->voiceCount();
    }
    return voices;
}

CueEngine::Lane *CueEngine::laneFor(const QString &wanted)
{
    const std::optional<QByteArray> id = m_hooks.resolveOutput(wanted);
    if (!id || m_failed.contains(*id))
        return nullptr;
    const auto it = m_lanes.find(*id);
    Lane *lane = it != m_lanes.end() ? it->second.get() : openLane(*id);
    if (lane)
        lane->lastUsed = ++m_useCounter;
    return lane;
}

CueEngine::Lane *CueEngine::openLane(const QByteArray &id,
                                     std::unique_ptr<callsound::CueMixer> carried)
{
    while (int(m_lanes.size()) >= kMaxOutputs) {
        const Lane *victim = nullptr;
        for (const bool requireIdle : {true, false}) {
            for (const auto &[laneId, lane] : m_lanes) {
                if (laneId == m_loopLane || (requireIdle && !lane->mixer->idle()))
                    continue;
                if (!victim || lane->lastUsed < victim->lastUsed)
                    victim = lane.get();
            }
            if (victim)
                break;
        }
        if (!victim)
            break;
        closeLane(victim->id);
    }

    QAudioFormat format;
    format.setSampleFormat(QAudioFormat::Float);
    format.setSampleRate(kSampleRate);
    bool supported = false;
    for (const int channels : {2, 1}) {
        format.setChannelConfig(channels == 1 ? QAudioFormat::ChannelConfigMono
                                              : QAudioFormat::ChannelConfigStereo);
        if (m_hooks.supports(id, format)) {
            supported = true;
            break;
        }
    }
    CueOutput *output = supported ? m_hooks.open(id, format, this) : nullptr;
    if (!output) {
        qCWarning(lcCallSound)
            << "call sound unusable: the output accepts no cue format"
            << (supported ? "(it is gone)" : "(neither stereo nor mono float)");
        m_failed.insert(id);
        refreshReadiness();
        return nullptr;
    }

    auto lane = std::make_unique<Lane>();
    lane->id = id;
    lane->mixer = carried && carried->channels() == format.channelCount()
        ? std::move(carried)
        : std::make_unique<callsound::CueMixer>(format.channelCount());
    lane->idleTimer = std::make_unique<QTimer>();
    lane->idleTimer->setSingleShot(true);
    connect(lane->idleTimer.get(), &QTimer::timeout, this,
            [this, id] { onIdle(id); });
    lane->output = output;
    lane->feed = new callsound::MixerFeed(lane->mixer.get(), output);
    Lane *raw = lane.get();
    m_lanes.emplace(id, std::move(lane));
    ++m_outputsOpened;
    connect(output, &CueOutput::failed, this,
            [this, output] { onOutputFailed(output); });
    output->start(raw->feed);
    if (!m_lanes.count(id))
        return nullptr;
    raw->idleTimer->start(m_idleSuspendMs);
    return raw;
}

void CueEngine::closeLane(const QByteArray &id)
{
    const auto it = m_lanes.find(id);
    if (it == m_lanes.end())
        return;
    std::unique_ptr<Lane> lane = std::move(it->second);
    m_lanes.erase(it);
    if (id == m_loopLane) {
        m_loopLane.clear();
        m_loopVoice = 0;
    }
    lane->idleTimer->stop();
    if (CueOutput *output = lane->output) {
        disconnect(output, nullptr, this, nullptr);
        output->stop();
        output->deleteLater();
    }
}

void CueEngine::onOutputFailed(CueOutput *output)
{
    auto it = m_lanes.begin();
    while (it != m_lanes.end() && it->second->output != output)
        ++it;
    if (it == m_lanes.end())
        return;
    const QByteArray id = it->first;
    std::shared_ptr<Lane> lane(std::move(it->second));
    m_lanes.erase(it);
    lane->idleTimer->stop();
    if (id == m_loopLane) {
        m_loopLane.clear();
        m_loopVoice = 0;
    }
    disconnect(output, nullptr, this, nullptr);
    // This runs inside the output's own signal: stopping it here would
    // destroy the stream whose notifier is still delivering it. The mixer
    // the feed reads stays alive until then.
    QMetaObject::invokeMethod(
        output,
        [output, lane] {
            Q_UNUSED(lane);
            output->stop();
            output->deleteLater();
        },
        Qt::QueuedConnection);
    // Loud once: an unusable sound is otherwise silent, and the ringer then
    // falls back to the desktop's call sound. (Worded so package validators'
    // "failed to load" scan does not read it as a missing library.)
    qCWarning(lcCallSound) << "call sound unusable: the output failed; "
                              "waiting for the audio outputs to change";
    m_failed.insert(id);
    refreshReadiness();
}

void CueEngine::wake(Lane *lane)
{
    if (!lane->output)
        return;
    if (lane->output->suspended())
        lane->output->resume();
    lane->feed->kick();
    lane->idleTimer->start(m_idleSuspendMs);
    watch(lane);
}

void CueEngine::onIdle(const QByteArray &id)
{
    const auto it = m_lanes.find(id);
    if (it == m_lanes.end() || !it->second->output)
        return;
    Lane *lane = it->second.get();
    if (lane->feed->busy()) {
        lane->idleTimer->start(m_idleSuspendMs);
        return;
    }
    if (!lane->output->suspended())
        lane->output->suspend();
}

void CueEngine::watch(Lane *lane)
{
    if (!lane->output || !lane->feed->busy())
        return;
    if (lane->checkedAtMs < 0) {
        lane->takenAtCheck = lane->feed->bytesTaken();
        lane->checkedAtMs = m_clock.elapsed();
    }
    if (!m_stallTimer.isActive()) {
        m_lastStallTickMs = m_clock.elapsed();
        m_stallTimer.start(qBound(10, m_stallMs / 5, 250));
    }
}

void CueEngine::onStallTick()
{
    const qint64 now = m_clock.elapsed();
    const bool late = now - m_lastStallTickMs > m_stallTimer.interval() + kLateMarginMs;
    m_lastStallTickMs = now;
    QList<QByteArray> stalled;
    bool watching = false;
    for (const auto &[id, lane] : m_lanes) {
        if (!lane->output)
            continue;
        const quint64 taken = lane->feed->bytesTaken();
        const bool tookSome = taken != lane->takenAtCheck;
        if (tookSome) {
            lane->dead = false;
            lane->probed = false;
        }
        const quint64 proof =
            quint64(kProbeFrames) * sizeof(float) * lane->mixer->channels();
        if (lane->unproven && taken >= proof)
            lane->unproven = false;
        if (!lane->feed->busy()) {
            lane->takenAtCheck = taken;
            lane->checkedAtMs = -1;
            continue;
        }
        watching = true;
        if (lane->checkedAtMs < 0 || late || tookSome) {
            lane->takenAtCheck = taken;
            lane->checkedAtMs = now;
        } else if (now - lane->checkedAtMs >= m_stallMs) {
            stalled << id;
        }
    }
    const bool waiting = !stalled.isEmpty() || m_retryDue;
    const bool settled = serverSettled(now, waiting, late);
    if (!watching && !m_retryDue)
        m_stallTimer.stop();
    if (m_retryDue && settled) {
        m_retryDue = false;
        for (const QByteArray &id : std::as_const(m_stallFailed))
            m_failed.remove(id);
        m_stallFailed.clear();
        resumeLoop();
        refreshReadiness();
    }
    for (const QByteArray &id : std::as_const(stalled)) {
        const auto it = m_lanes.find(id);
        if (it == m_lanes.end())
            continue;
        Lane *lane = it->second.get();
        if (lane->unproven) {
            giveUpStalled(id);
            continue;
        }
        const bool due =
            lane->probed || now - lane->checkedAtMs >= m_reopenSettleMs;
        if (due && settled) {
            reopenStalled(id);
        } else if (!std::exchange(lane->dead, true)) {
            qCWarning(lcCallSound)
                << "call sounds: an output took no audio for" << m_stallMs
                << "ms with cues waiting (did the sound server restart?); "
                   "reopening it once the server is back";
        }
    }
}

bool CueEngine::serverSettled(qint64 now, bool waiting, bool late)
{
    if (!m_hooks.serverReachable)
        return true;
    if (!waiting || late) {
        m_serverLiveSinceMs = -1;
        m_gatePollMs = m_gatePollMinMs;
        m_nextGatePollMs = -1;
        return false;
    }
    if (m_serverLiveSinceMs < 0 && m_nextGatePollMs >= 0 && now < m_nextGatePollMs)
        return false;
    if (!m_hooks.serverReachable()) {
        m_serverLiveSinceMs = -1;
        m_nextGatePollMs = now + m_gatePollMs;
        m_gatePollMs = qMin(m_gatePollMs * 2, m_gatePollMaxMs);
        return false;
    }
    m_gatePollMs = m_gatePollMinMs;
    m_nextGatePollMs = -1;
    if (m_serverLiveSinceMs < 0)
        m_serverLiveSinceMs = now;
    return now - m_serverLiveSinceMs >= m_serverLiveMs;
}

void CueEngine::reopenStalled(const QByteArray &id)
{
    const auto it = m_lanes.find(id);
    if (it == m_lanes.end())
        return;
    qCWarning(lcCallSound)
        << "call sounds: reopening an output that took no audio for"
        << m_clock.elapsed() - it->second->checkedAtMs << "ms with cues waiting";
    std::unique_ptr<callsound::CueMixer> mixer = std::move(it->second->mixer);
    const int channels = mixer->channels();
    const bool loopHere = id == m_loopLane;
    const int loopVoice = m_loopVoice;
    closeLane(id);
    mixer->rewindOneShots();
    ++m_outputsReopened;
    Lane *fresh = openLane(id, std::move(mixer));
    if (!fresh)
        return;
    fresh->unproven = true;
    if (loopHere) {
        if (fresh->mixer->channels() == channels && fresh->mixer->isPlaying(loopVoice)) {
            m_loopLane = id;
            m_loopVoice = loopVoice;
        } else {
            startLoop(fresh);
        }
    }
    fresh->feed->addSilence(kProbeFrames);
    wake(fresh);
}

void CueEngine::giveUpStalled(const QByteArray &id)
{
    qCWarning(lcCallSound)
        << "call sound unusable: an output took no audio even after it was "
           "reopened; trying again in"
        << m_stallRetryMs << "ms (once the server is back) or when the audio "
                             "outputs change";
    closeLane(id);
    m_failed.insert(id);
    m_stallFailed.insert(id);
    refreshReadiness();
    QTimer::singleShot(m_stallRetryMs, this, [this, generation = m_retryGeneration] {
        if (generation != m_retryGeneration || m_stallFailed.isEmpty())
            return;
        m_retryDue = true;
        if (!m_stallTimer.isActive()) {
            m_lastStallTickMs = m_clock.elapsed();
            m_stallTimer.start(qBound(10, m_stallMs / 5, 250));
        }
    });
}

bool CueEngine::decodable(const QString &sound)
{
    return m_decodable.contains(sound) || decoded(sound).has_value();
}

std::optional<callsound::Pcm> CueEngine::decoded(const QString &sound)
{
    if (m_undecodable.contains(sound))
        return std::nullopt;
    const std::optional<QByteArray> wav = m_hooks.wav(sound);
    std::optional<callsound::Pcm> pcm =
        wav ? callsound::decodeWav(*wav) : std::nullopt;
    if (pcm && pcm->format.sampleRate() != kSampleRate)
        pcm.reset();
    if (!pcm) {
        m_undecodable.insert(sound);
        qCWarning(lcCallSound) << "call sound unusable sound=" << sound
                               << "(not bundled as 16-bit 48 kHz PCM)";
        return std::nullopt;
    }
    m_decodable.insert(sound);
    return pcm;
}

std::optional<callsound::Pcm> CueEngine::pcm(const QString &sound, int channels)
{
    QHash<QString, callsound::Pcm> &cache = m_converted[channels];
    if (const auto it = cache.constFind(sound); it != cache.cend())
        return it.value();
    const std::optional<callsound::Pcm> raw = decoded(sound);
    if (!raw)
        return std::nullopt;
    const callsound::Pcm converted = callsound::withChannels(*raw, channels);
    cache.insert(sound, converted);
    return converted;
}

void CueEngine::startLoop(Lane *lane)
{
    const std::optional<callsound::Pcm> cue =
        pcm(m_loopSound, lane->mixer->channels());
    m_loopVoice = cue ? lane->mixer->start(cue->data, m_loopGain, true) : 0;
    m_loopLane = m_loopVoice ? lane->id : QByteArray();
}

void CueEngine::stopLoopVoice()
{
    const auto it = m_lanes.find(m_loopLane);
    if (it != m_lanes.end())
        it->second->mixer->stop(m_loopVoice);
    m_loopLane.clear();
    m_loopVoice = 0;
}

int CueEngine::refreshReadiness()
{
    const std::optional<QByteArray> def = m_hooks.resolveOutput(QString());
    const bool outputUsable = def && !m_failed.contains(*def);
    int ready = 0;
    for (const QString &sound : std::as_const(m_sounds)) {
        const bool playable = outputUsable && decodable(sound);
        ready += playable ? 1 : 0;
        if (m_hooks.setReady)
            m_hooks.setReady(sound, playable);
    }
    return ready;
}
