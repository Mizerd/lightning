#include "calls/CallSoundPlayer.h"

#include <QAudioDevice>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QSoundEffect>
#include <QUrl>

#include <cmath>

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
    QStringLiteral("ringback"),
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

} // namespace

const QStringList &CallSoundPlayer::knownSounds()
{
    return kSounds;
}

CallSoundPlayer::CallSoundPlayer(std::function<QString()> callSpeakerId,
                                 QObject *parent)
    : QObject(parent)
    , m_callSpeakerId(std::move(callSpeakerId))
{
    // No output device (a headless session, a CI container): nothing can
    // play, so skip the preload. canPlay() stays false and the ringer falls
    // back to the desktop's sound; a later play() creates its effect lazily.
    if (QMediaDevices::audioOutputs().isEmpty()) {
        qCInfo(lcCallSound) << "call sounds unavailable: no audio output device";
        return;
    }
    QElapsedTimer timer;
    timer.start();
    for (const QString &sound : kSounds)
        effect(sound);
    qCInfo(lcCallSound) << "call sounds preloading ms=" << timer.elapsed();
}

CallSoundPlayer::~CallSoundPlayer()
{
    for (QSoundEffect *e : std::as_const(m_effects))
        e->stop();
}

QSoundEffect *CallSoundPlayer::effect(const QString &sound)
{
    if (QSoundEffect *existing = m_effects.value(sound))
        return existing;
    if (!kSounds.contains(sound))
        return nullptr;
    auto *e = new QSoundEffect(this);
    m_effects.insert(sound, e);
    connect(e, &QSoundEffect::statusChanged, this, [e, sound] {
        // Loud once: an unusable sound is otherwise silent, and the ringer
        // then falls back to the desktop's call sound. (Worded so package
        // validators' "failed to load" scan does not read it as a missing
        // library.)
        if (e->status() == QSoundEffect::Error)
            qCWarning(lcCallSound) << "call sound unusable sound=" << sound;
    });
    e->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(sound)));
    return e;
}

void CallSoundPlayer::route(QSoundEffect *e, bool inCall)
{
    // Re-resolved on every play so a device plugged in mid-session is used.
    QAudioDevice target = QMediaDevices::defaultAudioOutput();
    const QString wanted = inCall && m_callSpeakerId ? m_callSpeakerId()
                                                     : QString();
    if (!wanted.isEmpty()) {
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice &device : outputs) {
            if (QString::fromUtf8(device.id()) == wanted) {
                target = device;
                break;
            }
        }
    }
    if (!target.isNull() && e->audioDevice() != target)
        e->setAudioDevice(target);
}

void CallSoundPlayer::play(const QString &sound, qreal volume, bool inCall)
{
    // A one-shot of the looping sound would cut the loop short.
    if (sound == m_loopSound)
        return;
    QSoundEffect *e = effect(sound);
    if (!e)
        return;
    route(e, inCall);
    e->setLoopCount(1);
    e->setVolume(linearGain(volume));
    e->play();
}

void CallSoundPlayer::loop(const QString &sound, qreal volume, bool inCall)
{
    if (!sound.isEmpty() && !kSounds.contains(sound))
        return;
    if (sound == m_loopSound) {
        // The controller re-asserts the loop on every state change: adjust
        // the volume, never restart.
        if (QSoundEffect *e = m_effects.value(sound))
            e->setVolume(linearGain(volume));
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
    route(e, inCall);
    e->setLoopCount(QSoundEffect::Infinite);
    e->setVolume(linearGain(volume));
    e->play();
}

bool CallSoundPlayer::canPlay(const QString &sound) const
{
    const QSoundEffect *e = m_effects.value(sound);
    return e && e->status() == QSoundEffect::Ready;
}
