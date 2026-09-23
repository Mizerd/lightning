#include "calls/CallSoundPlayer.h"

#include <QAudioDevice>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSoundEffect>
#include <QUrl>

#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(lcCallSound)

namespace {

// The bundled set; scripts/generate-call-sounds.py renders every one of
// them and the resource list in CMakeLists.txt embeds them under /sounds.
const QStringList kSounds = {
    QStringLiteral("join"),         QStringLiteral("leave"),
    QStringLiteral("connected"),    QStringLiteral("ended"),
    QStringLiteral("disconnected"), QStringLiteral("mute"),
    QStringLiteral("unmute"),       QStringLiteral("deafen"),
    QStringLiteral("undeafen"),     QStringLiteral("share-start"),
    QStringLiteral("share-stop"),   QStringLiteral("hand-raised"),
    QStringLiteral("ring"),         QStringLiteral("call-waiting"),
    QStringLiteral("ringback"),
};

// The settings sliders are PERCEPTUAL; QSoundEffect's volume is linear
// gain. Map the slider onto a 40 dB range so equal slider steps sound like
// equal steps: 100% is 0 dB, 70% is -12 dB, 50% is -20 dB, 0% is silence.
float linearGain(qreal perceptual)
{
    if (!(perceptual > 0.0))
        return 0.0f;
    if (perceptual >= 1.0)
        return 1.0f;
    return float(std::pow(10.0, -40.0 * (1.0 - perceptual) / 20.0));
}

// How long the destructor waits for the worker to release the audio
// device. A backend that never answers must not hang application exit.
constexpr unsigned long kShutdownWaitMs = 3000;

} // namespace

const QStringList &CallSoundPlayer::knownSounds()
{
    return kSounds;
}

CallSoundPlayer::CallSoundPlayer(std::function<QString()> callSpeakerId)
    : m_callSpeakerId(std::move(callSpeakerId))
{
    m_thread.setObjectName(QStringLiteral("call-sounds"));
    m_worker = new CallSoundWorker(this);
    m_worker->moveToThread(&m_thread);
    m_thread.start();
    QMetaObject::invokeMethod(m_worker, "preload", Qt::QueuedConnection);
}

CallSoundPlayer::~CallSoundPlayer()
{
    CallSoundWorker *worker = m_worker;
    QThread *thread = &m_thread;
    // Release the effects ON their thread, then stop that thread from
    // inside it, so nothing queued before this is lost to quit().
    QMetaObject::invokeMethod(
        worker,
        [worker, thread] {
            worker->shutdown();
            thread->quit();
        },
        Qt::QueuedConnection);
    if (m_thread.wait(kShutdownWaitMs)) {
        delete worker;
    } else {
        // Leaked deliberately: deleting an object whose thread is still
        // running is undefined, and a wedged audio backend at exit is not
        // worth a crash.
        qCWarning(lcCallSound) << "call sound thread did not stop in"
                               << kShutdownWaitMs << "ms";
    }
    m_worker = nullptr;
}

void CallSoundPlayer::play(const QString &sound, qreal volume, bool inCall)
{
    if (!kSounds.contains(sound))
        return;
    const QString device = inCall && m_callSpeakerId ? m_callSpeakerId()
                                                     : QString();
    QMetaObject::invokeMethod(m_worker, "play", Qt::QueuedConnection,
                              Q_ARG(QString, sound),
                              Q_ARG(qreal, qreal(linearGain(volume))),
                              Q_ARG(QString, device));
}

void CallSoundPlayer::loop(const QString &sound, qreal volume, bool inCall)
{
    if (!sound.isEmpty() && !kSounds.contains(sound))
        return;
    const QString device = inCall && m_callSpeakerId ? m_callSpeakerId()
                                                     : QString();
    QMetaObject::invokeMethod(m_worker, "loop", Qt::QueuedConnection,
                              Q_ARG(QString, sound),
                              Q_ARG(qreal, qreal(linearGain(volume))),
                              Q_ARG(QString, device));
}

bool CallSoundPlayer::canPlay(const QString &sound) const
{
    QMutexLocker lock(&m_loadedLock);
    return m_loaded.contains(sound);
}

void CallSoundPlayer::markLoaded(const QString &sound, bool loaded)
{
    QMutexLocker lock(&m_loadedLock);
    if (loaded)
        m_loaded.insert(sound);
    else
        m_loaded.remove(sound);
}

// ── Worker (runs on the player's thread) ───────────────────────────────────

CallSoundWorker::CallSoundWorker(CallSoundPlayer *owner)
    : m_owner(owner)
{
}

QSoundEffect *CallSoundWorker::effect(const QString &sound)
{
    if (QSoundEffect *existing = m_effects.value(sound))
        return existing;
    if (!kSounds.contains(sound))
        return nullptr;
    auto *e = new QSoundEffect(this);
    m_effects.insert(sound, e);
    connect(e, &QSoundEffect::statusChanged, this, [this, e, sound] {
        const QSoundEffect::Status status = e->status();
        if (status == QSoundEffect::Ready) {
            m_owner->markLoaded(sound, true);
        } else if (status == QSoundEffect::Error) {
            m_owner->markLoaded(sound, false);
            // Loud once, because the consequence is silent: the ringer
            // falls back to the desktop's call sound and every other cue
            // simply does not play.
            qCWarning(lcCallSound) << "call sound failed to load sound="
                                   << sound;
        }
    });
    e->setSource(
        QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(sound)));
    return e;
}

void CallSoundWorker::preload()
{
    for (const QString &sound : kSounds)
        effect(sound);
}

void CallSoundWorker::route(QSoundEffect *e, const QString &deviceId)
{
    QAudioDevice target = QMediaDevices::defaultAudioOutput();
    if (!deviceId.isEmpty()) {
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice &device : outputs) {
            if (QString::fromUtf8(device.id()) == deviceId) {
                target = device;
                break;
            }
        }
    }
    if (!target.isNull() && e->audioDevice() != target)
        e->setAudioDevice(target);
}

void CallSoundWorker::play(const QString &sound, qreal volume,
                           const QString &deviceId)
{
    // The loop owns its effect: a one-shot of the same sound would cut the
    // loop short and end it after one pass.
    if (sound == m_loopSound)
        return;
    QSoundEffect *e = effect(sound);
    if (!e)
        return;
    route(e, deviceId);
    e->setLoopCount(1);
    e->setVolume(float(volume));
    e->play();
}

void CallSoundWorker::loop(const QString &sound, qreal volume,
                           const QString &deviceId)
{
    if (sound == m_loopSound) {
        // Same loop: a volume change only, never a restart — the
        // controller re-asserts the loop on every state change.
        if (QSoundEffect *e = m_effects.value(sound))
            e->setVolume(float(volume));
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
    route(e, deviceId);
    e->setLoopCount(QSoundEffect::Infinite);
    e->setVolume(float(volume));
    e->play();
}

void CallSoundWorker::shutdown()
{
    for (QSoundEffect *e : std::as_const(m_effects))
        e->stop();
    qDeleteAll(m_effects);
    m_effects.clear();
    m_loopSound.clear();
}
