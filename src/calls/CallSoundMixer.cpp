#include "calls/CallSoundMixer.h"

#include <QLoggingCategory>
#include <QMetaObject>

Q_DECLARE_LOGGING_CATEGORY(lcCallSound)

namespace {
/// Every bundled cue's rate (scripts/generate-call-sounds.py).
constexpr int kSampleRate = 48000;
} // namespace

CueEngine::CueEngine(QStringList sounds, Hooks hooks, QObject *parent)
    : QObject(parent), m_sounds(std::move(sounds)), m_hooks(std::move(hooks))
{
}

CueEngine::~CueEngine()
{
    stopAll();
}

void CueEngine::preload()
{
    for (const QString &sound : std::as_const(m_sounds))
        decoded(sound);
    laneFor(QString());
    refreshReadiness();
}

void CueEngine::play(const QString &sound, float gain, const QString &wanted)
{
    // A one-shot of the looping sound would double it.
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
        // The controller re-asserts the loop on every state change: adjust
        // the volume, never restart, never move.
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
    // Kept even when there is no output now: it starts when there is one.
    if (Lane *lane = laneFor(wanted)) {
        startLoop(lane);
        wake(lane);
    }
}

int CueEngine::reloadUnusable()
{
    m_failed.clear();
    // Outputs whose device is gone. (A device that exists resolves to
    // itself.)
    QList<QByteArray> gone;
    for (const auto &[id, lane] : m_lanes) {
        const std::optional<QByteArray> now =
            m_hooks.resolveOutput(QString::fromUtf8(id));
        if (!now || *now != id)
            gone << id;
    }
    for (const QByteArray &id : std::as_const(gone))
        closeLane(id);

    // The loop goes to (or resumes on) the device it resolves to now.
    if (!m_loopSound.isEmpty()) {
        const std::optional<QByteArray> target =
            m_hooks.resolveOutput(m_loopDevice);
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

    // The default device's output, as at preload; an idle output no cue
    // goes to by default any more is closed (the normal teardown).
    Lane *defaultLane = laneFor(QString());
    QList<QByteArray> stale;
    for (const auto &[id, lane] : m_lanes) {
        if (lane.get() != defaultLane && id != m_loopLane && lane->mixer->idle())
            stale << id;
    }
    for (const QByteArray &id : std::as_const(stale))
        closeLane(id);
    return refreshReadiness();
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

CueEngine::Lane *CueEngine::openLane(const QByteArray &id)
{
    // Bounded: close the output used longest ago, one with nothing playing
    // if there is one, never the loop's.
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

    // The cues' own format (48 kHz float), in stereo, else mono: what
    // QSoundEffect's sample cache fell back to as well. Asked of this
    // device, not of the default one.
    QAudioFormat format;
    format.setSampleFormat(QAudioFormat::Float);
    format.setSampleRate(kSampleRate);
    bool supported = false;
    for (const int channels : {2, 1}) {
        // Sets the channel count too (and setChannelCount() would clear it).
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
    lane->mixer = std::make_unique<callsound::CueMixer>(format.channelCount());
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
        return nullptr; // failed while starting
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
        // The loop starts again on the next output.
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
            Q_UNUSED(lane); // kept alive until here
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
}

void CueEngine::onIdle(const QByteArray &id)
{
    const auto it = m_lanes.find(id);
    if (it == m_lanes.end() || !it->second->output)
        return;
    Lane *lane = it->second.get();
    if (!lane->mixer->idle()) {
        lane->idleTimer->start(m_idleSuspendMs);
        return;
    }
    // Nothing to play: stop the stream running, keep it open.
    if (!lane->output->suspended())
        lane->output->suspend();
}

std::optional<callsound::Pcm> CueEngine::decoded(const QString &sound)
{
    const auto known = m_decoded.constFind(sound);
    if (known != m_decoded.cend())
        return known.value();
    if (m_undecodable.contains(sound))
        return std::nullopt;
    const std::optional<QByteArray> wav = m_hooks.wav(sound);
    std::optional<callsound::Pcm> pcm =
        wav ? callsound::decodeWav(*wav) : std::nullopt;
    // One rate for every voice: the mixer does not resample.
    if (pcm && pcm->format.sampleRate() != kSampleRate)
        pcm.reset();
    if (!pcm) {
        m_undecodable.insert(sound);
        qCWarning(lcCallSound) << "call sound unusable sound=" << sound
                               << "(not bundled as 16-bit 48 kHz PCM)";
        return std::nullopt;
    }
    m_decoded.insert(sound, *pcm);
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
        const bool playable = outputUsable && decoded(sound).has_value();
        ready += playable ? 1 : 0;
        if (m_hooks.setReady)
            m_hooks.setReady(sound, playable);
    }
    return ready;
}
