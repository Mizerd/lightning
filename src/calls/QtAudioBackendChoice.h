// Which Qt Multimedia audio backend Lightning asks for (open item
// 2026-10-07, "an orphaned PipeWire sink stream can still spin the GUI
// thread"). Header-only and pure where it can be, so the rule is testable
// without Qt Multimedia, a sandbox or a sound server.
//
// Qt 6.11's QPlatformAudioDevices::create() (qplatformaudiodevices.cpp) uses
// its native PipeWire backend when QT_AUDIO_BACKEND is UNSET or exactly
// "pipewire"; any other value, the empty string included, selects the
// PulseAudio backend. That backend's sink has no self-reference: a stream
// whose device goes away is destroyed by its owner on the application thread
// (QPulseAudioSinkStream via handleIOError), so the cross-thread socket
// notifier loop cannot start. Its cost, read in the same sources: opening a
// stream (QPulseAudioSinkStream::startStream, a semaphore), corking it on stop
// and reconnecting the context (prepare(), every 3 s after a failure) wait on
// the sound server with no timeout, on the calling thread, which is the GUI
// thread for every QAudioSink Lightning opens. Those waits end when the server
// answers; the PipeWire orphan loop never ends.
//
// Scope, deliberately: only the Flatpak is moved (where the loop was measured,
// on the KDE 6.11 runtime). Formats built against a system Qt 6.10 or newer
// (the nix build, possibly an rpm) keep the PipeWire backend and stay exposed.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

namespace lightning::calls {

/// The QT_AUDIO_BACKEND value to set before anything in the process touches
/// Qt Multimedia, or empty to leave the environment alone. Only a Flatpak is
/// moved (`--socket=pulseaudio` already reaches pipewire-pulse there); any
/// value the user set wins, "pipewire" included.
inline QByteArray qtAudioBackendOverride(bool flatpak, bool userSetBackend)
{
    if (!flatpak || userSetBackend)
        return {};
    return QByteArrayLiteral("pulseaudio");
}

/// Whether a shared object whose file name starts with `prefix` is mapped
/// into this process (Linux: /proc/self/maps; false elsewhere). Evidence of a
/// LOAD only: --call-sounds-status uses it for libpipewire as a labelled
/// fallback where Qt cannot be asked which backend it chose.
inline bool sharedObjectLoaded(const char *prefix)
{
#ifdef Q_OS_LINUX
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QByteArray needle = QByteArray("/") + prefix;
    while (!maps.atEnd()) {
        if (maps.readLine().contains(needle))
            return true;
    }
#else
    Q_UNUSED(prefix);
#endif
    return false;
}

} // namespace lightning::calls
