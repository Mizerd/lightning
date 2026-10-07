#include "calls/CallSoundPlayer.h"

#include "app/AsyncLogSink.h"
#include "calls/CallSoundMixer.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QMutex>
#include <QSet>
#include <QSoundEffect>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

#if defined(LIGHTNING_HAVE_QT_AUDIO_ROLE)
// Private Qt Multimedia API, for the stream's role (see AudioSinkOutput).
// CMake defines the macro only after compiling against this header.
#include <QtMultimedia/private/qaudiosystem_p.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(lcCallSound)

namespace {

// Rendered by scripts/generate-call-sounds.py, embedded under qrc:/sounds.
const QStringList kSounds = {
    QStringLiteral("join"),         QStringLiteral("leave"),
    QStringLiteral("connected"),    QStringLiteral("ended"),
    QStringLiteral("disconnected"), QStringLiteral("mute"),
    QStringLiteral("unmute"),       QStringLiteral("deafen"),
    QStringLiteral("undeafen"),     QStringLiteral("share-start"),
    QStringLiteral("share-stop"),   QStringLiteral("hand-raised"),
    QStringLiteral("ring"),         QStringLiteral("call-waiting"),
    QStringLiteral("ringback"),     QStringLiteral("message"),
    QStringLiteral("mention"),
};

// Settings sliders are perceptual; QSoundEffect takes linear gain. Map the
// slider onto 40 dB: 100% = 0 dB, 70% = -12 dB, 50% = -20 dB, 0% = silence.
float linearGain(qreal perceptual)
{
    if (!(perceptual > 0.0))
        return 0.0f;
    if (perceptual >= 1.0)
        return 1.0f;
    return float(std::pow(10.0, -40.0 * (1.0 - perceptual) / 20.0));
}

// See the header: before 6.10 an open on a stalled server blocks the thread
// that plays; from 6.10 QSoundEffect's engine must be created on the GUI
// thread.
constexpr bool kEffectsOffTheGuiThread =
    QT_VERSION < QT_VERSION_CHECK(6, 10, 0);

// See the header: on Linux from 6.10 QSoundEffect frees a finished voice on
// the audio backend's real-time thread, and the GUI thread's glib event loop
// then spins for ever on the voice's closed eventfd. The cues are mixed into
// one QAudioSink there. Windows and macOS keep QSoundEffect (validated live
// on Windows; the defect is the eventfd notifier's).
#if defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
constexpr bool kCuesMixed = true;
#else
constexpr bool kCuesMixed = false;
#endif

std::atomic<bool> g_soundThreadStuck{false};
std::atomic<int> g_exitStatus{0};

/// A QCoreApplication post routine, added last, so it runs before Qt
/// Multimedia's own teardown: everything of ours is already destroyed (and
/// flushed) by the time the application object goes.
void endWithoutTeardown()
{
    // _Exit skips the at-exit drain, and the lines just queued are the ones
    // saying why the sound thread is stuck.
    lightning::logging::flushProcessLog(
        lightning::logging::AsyncLogSink::kFatalDrainMs);
    std::fflush(nullptr);
    std::_Exit(g_exitStatus.load());
}

} // namespace

/// Which sounds reached QSoundEffect::Ready; read on the GUI thread, written
/// where the effects live.
struct CallSoundReadiness {
    mutable QMutex mutex;
    QSet<QString> ready;
    /// Liveness of the effects' thread: jobs posted and jobs finished, and
    /// since when the oldest unfinished one has waited (-1: none). A thread
    /// stuck on the sound server finishes nothing, and then no sound can be
    /// relied on however "ready" it once was.
    QElapsedTimer clock;
    quint64 posted = 0;
    quint64 done = 0;
    qint64 waitingSinceMs = -1;
    bool saidStuck = false;
};

namespace {
/// How long a job may wait before the effects' thread counts as stuck.
constexpr qint64 kStuckAfterMs = 2000;
} // namespace

/// Where the cues are voiced. Lives where the effects live (see the header)
/// and is only ever called there.
class CallSoundEffects : public QObject
{
public:
    ~CallSoundEffects() override = default;

    virtual void preload() = 0;
    virtual void play(const QString &sound, float gain,
                      const QString &device) = 0;
    virtual void loop(const QString &sound, float gain,
                      const QString &device) = 0;
    /// After the output list changed: make every unusable sound usable
    /// again and resume a loop that was lost. Returns how many were
    /// (re)loaded.
    virtual int reloadUnusable() = 0;
    virtual void stopAll() = 0;

protected:
    /// The call's chosen speaker (`wanted`, an output id) or the default.
    /// Re-resolved on every cue so a device plugged in mid-session is used;
    /// resolved where the effects live rather than on the GUI thread, since
    /// asking the audio backend is exactly what can hang (Qt < 6.10).
    static QAudioDevice resolveOutput(const QString &wanted)
    {
        QAudioDevice target = QMediaDevices::defaultAudioOutput();
        if (!wanted.isEmpty()) {
            const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
            for (const QAudioDevice &device : outputs) {
                if (QString::fromUtf8(device.id()) == wanted) {
                    target = device;
                    break;
                }
            }
        }
        return target;
    }

    static void setReady(const std::shared_ptr<CallSoundReadiness> &readiness,
                         const QString &sound, bool ready);
};

/// Owns the QSoundEffects (everywhere but Linux with Qt 6.10 and later; see
/// the header).
class QSoundEffectCues final : public CallSoundEffects
{
public:
    explicit QSoundEffectCues(std::shared_ptr<CallSoundReadiness> readiness)
        : m_readiness(std::move(readiness))
    {
    }

    void preload() override
    {
        QElapsedTimer timer;
        timer.start();
        for (const QString &sound : kSounds)
            effect(sound);
        qCInfo(lcCallSound) << "call sounds preloading ms=" << timer.elapsed();
    }

    void play(const QString &sound, float gain, const QString &device) override
    {
        // A one-shot of the looping sound would cut the loop short.
        if (sound == m_loopSound)
            return;
        QSoundEffect *e = effect(sound);
        if (!e)
            return;
        route(e, device);
        e->setLoopCount(1);
        e->setVolume(gain);
        e->play();
    }

    void loop(const QString &sound, float gain, const QString &device) override
    {
        if (sound == m_loopSound) {
            // The controller re-asserts the loop on every state change:
            // adjust the volume, never restart.
            if (QSoundEffect *e = m_effects.value(sound))
                e->setVolume(gain);
            return;
        }
        if (QSoundEffect *old = m_effects.value(m_loopSound))
            old->stop();
        m_loopSound.clear();
        if (sound.isEmpty())
            return;
        QSoundEffect *e = effect(sound);
        if (!e)
            return;
        m_loopSound = sound;
        m_loopGain = gain;
        m_loopDevice = device;
        route(e, device);
        e->setLoopCount(QSoundEffect::Infinite);
        e->setVolume(gain);
        e->play();
    }

    /// After the output list changed. A QSoundEffect whose output went away
    /// sits in Error, and nothing here ever moved it out (setSource() with
    /// the same URL is a no-op), so canPlay() stayed false for the rest of
    /// the session. Each such sound is replaced by a new effect, and
    /// a sound never loaded (no output at startup) is loaded now. A loop that
    /// was lost resumes. Whether an output exists was asked on the GUI
    /// thread (it used to be asked here, from the effects' thread, and a
    /// stale empty answer there would have skipped the reload without a
    /// word). Returns how many were (re)loaded.
    int reloadUnusable() override
    {
        int reloaded = 0;
        bool loopLost = false;
        for (const QString &sound : kSounds) {
            QSoundEffect *old = m_effects.value(sound);
            if (old && old->status() != QSoundEffect::Error)
                continue;
            if (old) {
                // Its status must no longer reach the readiness set: the new
                // effect's does.
                QObject::disconnect(old, nullptr, this, nullptr);
                m_effects.remove(sound);
                old->stop();
                old->deleteLater();
                loopLost = loopLost || sound == m_loopSound;
            }
            effect(sound);
            ++reloaded;
        }
        if (loopLost) {
            if (QSoundEffect *e = m_effects.value(m_loopSound)) {
                route(e, m_loopDevice);
                e->setLoopCount(QSoundEffect::Infinite);
                e->setVolume(m_loopGain);
                // Played once loaded: QSoundEffect queues a play() that
                // arrives while it is still loading.
                e->play();
            }
        }
        return reloaded;
    }

    void stopAll() override
    {
        for (QSoundEffect *e : std::as_const(m_effects))
            e->stop();
    }

private:
    QSoundEffect *effect(const QString &sound)
    {
        if (QSoundEffect *existing = m_effects.value(sound))
            return existing;
        if (!kSounds.contains(sound))
            return nullptr;
        auto *e = new QSoundEffect(this);
        m_effects.insert(sound, e);
        std::shared_ptr<CallSoundReadiness> readiness = m_readiness;
        connect(e, &QSoundEffect::statusChanged, this, [e, sound, readiness] {
            {
                QMutexLocker lock(&readiness->mutex);
                if (e->status() == QSoundEffect::Ready)
                    readiness->ready.insert(sound);
                else
                    readiness->ready.remove(sound);
            }
            // Loud once: an unusable sound is otherwise silent, and the
            // ringer then falls back to the desktop's call sound. (Worded so
            // package validators' "failed to load" scan does not read it as
            // a missing library.)
            if (e->status() == QSoundEffect::Error)
                qCWarning(lcCallSound) << "call sound unusable sound=" << sound;
        });
        e->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(sound)));
        return e;
    }

    static void route(QSoundEffect *e, const QString &wanted)
    {
        const QAudioDevice target = resolveOutput(wanted);
        if (!target.isNull() && e->audioDevice() != target)
            e->setAudioDevice(target);
    }

    std::shared_ptr<CallSoundReadiness> m_readiness;
    QHash<QString, QSoundEffect *> m_effects;
    QString m_loopSound;
    float m_loopGain = 1.0f;
    QString m_loopDevice;
};

/// CueEngine's output in the app: one pull-mode QAudioSink.
class AudioSinkOutput final : public CueOutput
{
public:
    AudioSinkOutput(const QAudioDevice &device, const QAudioFormat &format,
                    QObject *parent)
        : CueOutput(parent), m_sink(new QAudioSink(device, format, this))
    {
        // The ring buffer is refilled on this (the GUI) thread, so it is how
        // long the GUI thread may stall before a loop stutters, and also the
        // most a cue waits behind audio already mixed. See kRingBufferUs.
        m_sink->setBufferSize(format.bytesForDuration(kRingBufferUs));
        // The stream's role, before start() creates the stream, exactly as
        // QSoundEffect's own engine (QRtAudioEngine) does: PipeWire and
        // PulseAudio then tag it media.role "Notification", so the desktop's
        // notification volume still governs the cues. No public API does it.
#if defined(LIGHTNING_HAVE_QT_AUDIO_ROLE)
        // Private ABI, so only with the very Qt it was built against: a
        // runtime update within 6.11.x is not caught by the loader, and
        // setRole() is a virtual call into it.
        if (qstrcmp(qVersion(), QT_VERSION_STR) == 0) {
            if (QPlatformAudioSink *platform = QPlatformAudioSink::get(*m_sink)) {
                platform->setRole(
                    QtMultimediaPrivate::AudioEndpointRole::SoundEffect);
            }
        } else {
            sayNoRole(QStringLiteral("built with Qt " QT_VERSION_STR
                                     ", running Qt %1")
                          .arg(QLatin1String(qVersion())));
        }
#else
        sayNoRole(QStringLiteral("no private Qt Multimedia headers"));
#endif
        connect(m_sink, &QAudioSink::stateChanged, this, [this] { check(); });
    }

    void start(QIODevice *feed) override
    {
        m_sink->start(feed);
        check();
    }
    void suspend() override { m_sink->suspend(); }
    void resume() override { m_sink->resume(); }
    bool suspended() const override
    {
        return m_sink->state() == QAudio::SuspendedState;
    }
    void stop() override { m_sink->reset(); }

private:
    /// Once per process: the cues then carry Qt's default role.
    static void sayNoRole(const QString &why)
    {
        static bool said = false;
        if (!std::exchange(said, true)) {
            qCInfo(lcCallSound).noquote()
                << QStringLiteral("call sounds: media.role not set (%1)").arg(why);
        }
    }

    // Measured 2026-10-07 (private PipeWire graph; the ring looping while the
    // GUI thread was blocked once a second): 40 ms stuttered on every 50 ms
    // block, 120 ms on none up to 100 ms and on every 200 ms one (about
    // 95 ms of silence each). The price: a cue that starts while a loop
    // keeps the output fed is heard about 150 ms after play() instead of
    // about 65 (about 45 either way when the output is idle). Qt's default
    // is 250 ms.
    static constexpr qint64 kRingBufferUs = 120000;

    /// The device could not be opened or went away. An underrun is not that
    /// (older Qt reports one when a pull source runs dry).
    void check()
    {
        const QAudio::Error error = m_sink->error();
        if (m_failed
            || (error != QAudio::OpenError && error != QAudio::IOError
                && error != QAudio::FatalError)) {
            return;
        }
        m_failed = true;
        qCWarning(lcCallSound) << "call sounds: the output failed error="
                               << int(error);
        Q_EMIT failed();
    }

    QAudioSink *m_sink;
    bool m_failed = false;
};

/// Mixes the cues into one long-lived QAudioSink (Linux, Qt 6.10 and later;
/// see the header and CallSoundMixer.h). Lives on the GUI thread.
class MixerCues final : public CallSoundEffects
{
public:
    explicit MixerCues(std::shared_ptr<CallSoundReadiness> readiness)
        : m_engine(kSounds, hooks(std::move(readiness)), this)
    {
    }

    void preload() override
    {
        QElapsedTimer timer;
        timer.start();
        m_engine.preload();
        qCInfo(lcCallSound) << "call sounds preloading ms=" << timer.elapsed();
    }
    void play(const QString &sound, float gain, const QString &device) override
    {
        m_engine.play(sound, gain, device);
    }
    void loop(const QString &sound, float gain, const QString &device) override
    {
        m_engine.loop(sound, gain, device);
    }
    int reloadUnusable() override { return m_engine.reloadUnusable(); }
    void stopAll() override { m_engine.stopAll(); }

private:
    static QAudioDevice outputById(const QByteArray &id)
    {
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice &device : outputs) {
            if (device.id() == id)
                return device;
        }
        return {};
    }

    static CueEngine::Hooks hooks(std::shared_ptr<CallSoundReadiness> readiness)
    {
        CueEngine::Hooks h;
        h.resolveOutput = [](const QString &wanted) -> std::optional<QByteArray> {
            const QAudioDevice device = resolveOutput(wanted);
            if (device.isNull())
                return std::nullopt;
            return device.id();
        };
        h.supports = [](const QByteArray &id, const QAudioFormat &format) {
            const QAudioDevice device = outputById(id);
            return !device.isNull() && device.isFormatSupported(format);
        };
        h.open = [](const QByteArray &id, const QAudioFormat &format,
                    QObject *parent) -> CueOutput * {
            const QAudioDevice device = outputById(id);
            if (device.isNull())
                return nullptr;
            return new AudioSinkOutput(device, format, parent);
        };
        h.wav = [](const QString &sound) -> std::optional<QByteArray> {
            QFile file(QStringLiteral(":/sounds/%1.wav").arg(sound));
            if (!file.open(QIODevice::ReadOnly))
                return std::nullopt;
            return file.readAll();
        };
        h.setReady = [readiness](const QString &sound, bool ready) {
            setReady(readiness, sound, ready);
        };
        return h;
    }

    CueEngine m_engine;
};

void CallSoundEffects::setReady(
    const std::shared_ptr<CallSoundReadiness> &readiness, const QString &sound,
    bool ready)
{
    QMutexLocker lock(&readiness->mutex);
    if (ready)
        readiness->ready.insert(sound);
    else
        readiness->ready.remove(sound);
}

const QStringList &CallSoundPlayer::knownSounds()
{
    return kSounds;
}

bool CallSoundPlayer::playsOffTheGuiThread()
{
    return kEffectsOffTheGuiThread;
}

const char *CallSoundPlayer::cueBackend()
{
    return kCuesMixed ? "QAudioSink mixer" : "QSoundEffect";
}

bool CallSoundPlayer::soundThreadStuck()
{
    return g_soundThreadStuck.load();
}

void CallSoundPlayer::setExitStatus(int status)
{
    g_exitStatus.store(status);
}

CallSoundPlayer::CallSoundPlayer(std::function<QString()> callSpeakerId,
                                 QObject *parent)
    : QObject(parent)
    , m_callSpeakerId(std::move(callSpeakerId))
    , m_readiness(std::make_shared<CallSoundReadiness>())
{
    startEffects(kEffectsOffTheGuiThread);
    // Hotplug: an output that went away and came back (or one that appears
    // for the first time) makes the unusable cues loadable again. A
    // notification from Qt's backend, never a poll. A fallback only: on
    // Windows 2026-10-07 this path never reloaded a cue, and AppController
    // drives the reload through followOutputChanges().
    m_anyOutput = [] { return !QMediaDevices::audioOutputs().isEmpty(); };
    m_devices = new QMediaDevices(this);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this,
            &CallSoundPlayer::audioOutputsChanged);
    // No output device (a headless session, a CI container): nothing can
    // play, so skip the preload. canPlay() stays false and the ringer falls
    // back to the desktop's sound; a later play() creates its effect lazily.
    if (QMediaDevices::audioOutputs().isEmpty()) {
        qCInfo(lcCallSound) << "call sounds unavailable: no audio output device";
        return;
    }
    post([](CallSoundEffects *effects) { effects->preload(); });
}

CallSoundPlayer::~CallSoundPlayer()
{
    if (!m_thread) {
        m_effects->stopAll();
        delete m_effects;
        return;
    }
    post([](CallSoundEffects *effects) { effects->stopAll(); });
    // Deleted on its own thread: a finishing QThread still runs the deferred
    // deletes posted to it.
    m_effects->deleteLater();
    m_thread->quit();
    // Bounded: a sound server that stopped answering can hold the effects'
    // thread for its 30 s timeout, and quitting the app must not wait for
    // it. A running QThread must not be destroyed, so it is left behind.
    if (!m_thread->wait(2000)) {
        qCWarning(lcCallSound)
            << "call sounds: the sound thread is stuck on the sound server; "
               "leaving it behind, and the process will end without Qt's "
               "audio teardown";
        g_soundThreadStuck.store(true);
        if (QCoreApplication::instance())
            qAddPostRoutine(endWithoutTeardown);
        return;
    }
    delete m_thread;
}

CallSoundPlayer::CallSoundPlayer(ForTest, bool offTheGuiThread)
    : QObject(nullptr)
    , m_readiness(std::make_shared<CallSoundReadiness>())
{
    // No preload: a test must never create a sound effect on a thread Qt
    // 6.10+ does not allow; see the header.
    m_forTest = true;
    startEffects(offTheGuiThread);
}

void CallSoundPlayer::startEffects(bool offTheGuiThread)
{
    m_outputChangeTimer = new QTimer(this);
    m_outputChangeTimer->setSingleShot(true);
    connect(m_outputChangeTimer, &QTimer::timeout, this,
            &CallSoundPlayer::reloadAfterOutputChange);
    m_readiness->clock.start();
    if constexpr (kCuesMixed)
        m_effects = new MixerCues(m_readiness);
    else
        m_effects = new QSoundEffectCues(m_readiness);
    if (offTheGuiThread) {
        m_thread = new QThread();
        m_thread->setObjectName(QStringLiteral("call-sounds"));
        m_effects->moveToThread(m_thread);
        m_thread->start();
    }
}

void CallSoundPlayer::post(std::function<void(CallSoundEffects *)> job)
{
    CallSoundEffects *effects = m_effects;
    if (!m_thread) {
        job(effects);
        return;
    }
    std::shared_ptr<CallSoundReadiness> readiness = m_readiness;
    {
        QMutexLocker lock(&readiness->mutex);
        if (readiness->posted == readiness->done)
            readiness->waitingSinceMs = readiness->clock.elapsed();
        ++readiness->posted;
    }
    QMetaObject::invokeMethod(
        effects,
        [effects, readiness, job = std::move(job)] {
            job(effects);
            // Acknowledged: the next job, if any, starts waiting now.
            QMutexLocker lock(&readiness->mutex);
            ++readiness->done;
            readiness->waitingSinceMs = readiness->posted == readiness->done
                ? -1
                : readiness->clock.elapsed();
        },
        Qt::QueuedConnection);
}

void CallSoundPlayer::postForTest(std::function<void()> job)
{
    post([job = std::move(job)](CallSoundEffects *) { job(); });
}

void CallSoundPlayer::audioOutputsChanged()
{
    // Trailing edge: wait for the changes to go quiet, but answer a burst
    // that never does once, after the longest wait.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_outputChangeBurstStartMs < 0)
        m_outputChangeBurstStartMs = now;
    const qint64 left = qMax<qint64>(
        0, m_outputChangeMaxWaitMs - (now - m_outputChangeBurstStartMs));
    m_outputChangeTimer->start(
        static_cast<int>(qMin<qint64>(m_outputChangeQuietMs, left)));
}

void CallSoundPlayer::reloadAfterOutputChange()
{
    m_outputChangeBurstStartMs = -1;
    ++m_soundReloads;
    // Asked here, on the GUI thread, from whatever answered the signal.
    const bool anyOutput = m_anyOutput ? m_anyOutput() : true;
    m_lastReloadAnyOutput = anyOutput;
    // Said every time, so a log tells "never told" from "told, nothing to
    // do" (the first Windows run could not).
    qCInfo(lcCallSound) << "call sounds: the audio outputs changed (any="
                        << anyOutput << ")";
    if (m_forTest || !anyOutput)
        return;
    post([](CallSoundEffects *effects) {
        const int reloaded = effects->reloadUnusable();
        qCInfo(lcCallSound)
            << "call sounds re-initialised after an audio output change "
               "reloaded="
            << reloaded;
    });
}

void CallSoundPlayer::markReadyForTest(const QString &sound)
{
    QMutexLocker lock(&m_readiness->mutex);
    m_readiness->ready.insert(sound);
}

namespace {
/// The call's chosen speaker id while in a call, else "" for the default.
/// Read here, on the GUI thread, where the settings live.
QString cueDevice(const std::function<QString()> &callSpeakerId, bool inCall)
{
    return inCall && callSpeakerId ? callSpeakerId() : QString();
}
} // namespace

void CallSoundPlayer::play(const QString &sound, qreal volume, bool inCall)
{
    if (sound == m_loopSound || !kSounds.contains(sound))
        return;
    const float gain = linearGain(volume);
    const QString device = cueDevice(m_callSpeakerId, inCall);
    post([sound, gain, device](CallSoundEffects *effects) {
        effects->play(sound, gain, device);
    });
}

void CallSoundPlayer::loop(const QString &sound, qreal volume, bool inCall)
{
    if (!sound.isEmpty() && !kSounds.contains(sound))
        return;
    m_loopSound = sound;
    const float gain = linearGain(volume);
    const QString device = cueDevice(m_callSpeakerId, inCall);
    post([sound, gain, device](CallSoundEffects *effects) {
        effects->loop(sound, gain, device);
    });
}

bool CallSoundPlayer::canPlay(const QString &sound) const
{
    QMutexLocker lock(&m_readiness->mutex);
    // A thread that has not finished a job for a while cannot play anything,
    // loaded or not. Saying so lets the caller fall back: the ringer then
    // uses the desktop's call sound instead of ringing silently.
    const qint64 waiting = m_readiness->waitingSinceMs;
    if (waiting >= 0 && m_readiness->clock.elapsed() - waiting > kStuckAfterMs) {
        if (!m_readiness->saidStuck) {
            m_readiness->saidStuck = true;
            qCWarning(lcCallSound)
                << "call sounds: the sound thread has not answered for"
                << kStuckAfterMs << "ms (a stalled sound server); reporting "
                   "no sound playable, so the desktop's call sound is used";
        }
        return false;
    }
    m_readiness->saidStuck = false;
    return m_readiness->ready.contains(sound);
}
