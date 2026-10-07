// Replacing every cue's sound effect after an audio output change, in the one
// order that can actually reach a new output stream.
//
// Why the order matters: from Qt 6.10, QSoundEffect does not own its output.
// Every effect asks a process-wide pool for an engine keyed on the output's
// ID and the sample format, held by weak reference
// (QSoundEffectPrivateWithPlayer::getEngineFor, qtmultimedia 6.11), and the
// engine opens ONE QAudioSink when it is created. When that sink's endpoint
// is invalidated (WASAPI AUDCLNT_E_DEVICE_INVALIDATED: measured on Windows
// 2026-10-07, an RDP audio endpoint gone for 28 s) the sink stops with
// IOError and the engine never restarts it, but nothing tells the effects:
// they stay Ready and play into it. An endpoint that returns under the same
// ID maps to the same pool key, so a NEW effect created while any old effect
// still holds that engine is handed the dead one. The old cues were replaced
// one at a time, each old effect only deleteLater()'d and its successor
// created at once (and, its sample already decoded, attached to an engine at
// once): every replacement inherited the dead engine, which logged
// "reloaded= 17" and stayed silent until a restart emptied the pool.
//
// So every old effect is destroyed, now, before the first new one exists.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <utility>

namespace callsound {

/// Replaces every effect in `effects` (sound name -> effect). `destroy` must
/// free an effect immediately (never deleteLater()); `create(sound)` builds
/// the new one, inserts it into `effects` and returns whether it did. All old
/// effects are destroyed before the first create. Returns how many were
/// created.
template <typename Effect, typename Destroy, typename Create>
int replaceEveryEffect(QHash<QString, Effect *> &effects,
                       const QStringList &sounds, Destroy &&destroy,
                       Create &&create)
{
    QHash<QString, Effect *> retired;
    retired.swap(effects);
    for (Effect *effect : std::as_const(retired))
        destroy(effect);
    retired.clear();
    int created = 0;
    for (const QString &sound : sounds) {
        if (create(sound))
            ++created;
    }
    return created;
}

} // namespace callsound
