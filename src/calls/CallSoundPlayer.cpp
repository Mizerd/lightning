#include "calls/CallSoundPlayer.h"

#include "app/AsyncLogSink.h"
#include "calls/CallSoundMixer.h"
#include "calls/CallSoundRebuild.h"
#include "calls/PulseServerProbe.h"

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
#include <QtMultimedia/private/qaudiosystem_p.h>
#endif
#if defined(LIGHTNING_HAVE_QT_AUDIO_BACKEND_NAME)
#include <QtMultimedia/private/qplatformmediaintegration_p.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(lcCallSound)

namespace {

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

void endWithoutTeardown()
{
    lightning::logging::flushProcessLog(
        lightning::logging::AsyncLogSink::kFatalDrainMs);
    std::fflush(nullptr);
    std::_Exit(g_exitStatus.load());
}

}

struct CallSoundReadiness {
    mutable QMutex mutex;
    QSet<QString> ready;
    QElapsedTimer clock;
    quint64 posted = 0;
    quint64 done = 0;
    qint64 waitingSinceMs = -1;
    bool saidStuck = false;
};

namespace {
constexpr qint64 kStuckAfterMs = 2000;
}

class CallSoundEffects : public QObject
{
public:
    ~CallSoundEffects() override = default;

    virtual void preload() = 0;
    virtual void play(const QString &sound, float gain,
                      const QString &device) = 0;
    virtual void loop(const QString &sound, float gain,
                      const QString &device) = 0;
    virtual int reloadUnusable() = 0;
    virtual void stopAll() = 0;

protected:
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

    int reloadUnusable() override
    {
        bool loopLost = false;
        if (QSoundEffect *old = m_effects.value(m_loopSound)) {
            loopLost =
                old->isPlaying() || old->status() == QSoundEffect::Error;
        }
        const int reloaded = callsound::replaceEveryEffect(
            m_effects, kSounds,
            [this](QSoundEffect *old) {
                QObject::disconnect(old, nullptr, this, nullptr);
                old->stop();
                delete old;
            },
            [this](const QString &sound) { return effect(sound) != nullptr; });
        if (loopLost) {
            if (QSoundEffect *e = m_effects.value(m_loopSound)) {
                route(e, m_loopDevice);
                e->setLoopCount(QSoundEffect::Infinite);
                e->setVolume(m_loopGain);
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
            if (e->status() == QSoundEffect::Error)
                qCWarning(lcCallSound) << "call sound unusable sound=" << sound;
        });
        e->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(sound)));
        return e;
    }

    static void route(QSoundEffect *e, const QString &wanted)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QAudioDevice target = chosenOutput(wanted);
        if (e->audioDevice() != target)
            e->setAudioDevice(target);
#else
        const QAudioDevice target = resolveOutput(wanted);
        if (!target.isNull() && e->audioDevice() != target)
            e->setAudioDevice(target);
#endif
    }

    static QAudioDevice chosenOutput(const QString &wanted)
    {
        if (wanted.isEmpty())
            return {};
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice &device : outputs) {
            if (QString::fromUtf8(device.id()) == wanted)
                return device;
        }
        return {};
    }

    std::shared_ptr<CallSoundReadiness> m_readiness;
    QHash<QString, QSoundEffect *> m_effects;
    QString m_loopSound;
    float m_loopGain = 1.0f;
    QString m_loopDevice;
};

class AudioSinkOutput final : public CueOutput
{
public:
    AudioSinkOutput(const QAudioDevice &device, const QAudioFormat &format,
                    QObject *parent)
        : CueOutput(parent), m_sink(new QAudioSink(device, format, this))
    {
        m_sink->setBufferSize(format.bytesForDuration(kRingBufferUs));
#if defined(LIGHTNING_HAVE_QT_AUDIO_ROLE)
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
    static void sayNoRole(const QString &why)
    {
        static bool said = false;
        if (!std::exchange(said, true)) {
            qCInfo(lcCallSound).noquote()
                << QStringLiteral("call sounds: media.role not set (%1)").arg(why);
        }
    }

    static constexpr qint64 kRingBufferUs = 120000;

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
        // A new stream while Qt's PulseAudio backend has no connection
        // aborts the process (CallSoundMixer.h), so unless the backend is
        // known not to be PulseAudio a dead output waits for the server's
        // socket to answer AND for Qt to still be connecting to it
        // (PulseServerProbe.h).
        if (qtAudioBackendMayBePulse()) {
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            h.serverReachable = lightning::calls::PulseGate(
                [] { return lightning::calls::qtPulseThreadRunning(); },
                [] { return lightning::calls::pulseServerReachable(); },
                [clock] { return clock->elapsed(); },
                [] {
                    static bool said = false;
                    if (!std::exchange(said, true)) {
                        qCWarning(lcCallSound)
                            << "call sounds: Qt's PulseAudio connection did not come "
                               "back; call sounds stay off until Lightning restarts";
                    }
                });
        }
        return h;
    }

    static bool qtAudioBackendMayBePulse()
    {
#if defined(LIGHTNING_HAVE_QT_AUDIO_BACKEND_NAME)
        if (qstrcmp(qVersion(), QT_VERSION_STR) == 0) {
            const QString name = QString(QPlatformMediaIntegration::audioBackendName());
            const bool pulse = name == QLatin1String("PulseAudio");
            qCInfo(lcCallSound).noquote()
                << QStringLiteral("call sounds: Qt audio backend %1; a dead output %2")
                       .arg(name, pulse ? QStringLiteral("is reopened once the PulseAudio "
                                                         "server and Qt's connection are back")
                                        : QStringLiteral("needs no server gate"));
            return pulse;
        }
#endif
        qCInfo(lcCallSound) << "call sounds: Qt audio backend not known; a dead output "
                               "is reopened once the PulseAudio server and Qt's "
                               "connection are back";
        return true;
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
    m_anyOutput = [] { return !QMediaDevices::audioOutputs().isEmpty(); };
    m_devices = new QMediaDevices(this);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this,
            &CallSoundPlayer::audioOutputsChanged);
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
    const bool anyOutput = m_anyOutput ? m_anyOutput() : true;
    m_lastReloadAnyOutput = anyOutput;
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
QString cueDevice(const std::function<QString()> &callSpeakerId, bool inCall)
{
    return inCall && callSpeakerId ? callSpeakerId() : QString();
}
}

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
