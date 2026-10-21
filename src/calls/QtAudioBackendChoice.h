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
// Scope (decided 2026-10-20): every Linux process whose RUNNING Qt is 6.10
// or newer, the only versions with the PipeWire backend: the Flatpak (where
// the loop was measured, on the KDE 6.11 runtime) and the builds against a
// system Qt (the nix build, possibly an rpm or deb). Measured on the nix
// build with the PipeWire backend (2026-10-20, a private PipeWire graph):
// after the daemon restarted, QMediaDevices never reported an output change
// again, so nothing could re-arm a call's given-up receive chain.
// HYPOTHESIS, not measured with this change: on the PulseAudio backend those
// notices survive a restart (the 2026-10-16 live run saw "audio outputs
// changed" after one on that backend), so the re-arm works again.
//
// THE COST, AND HOW IT IS PAID FOR. A new stream (QAudioSink::start(),
// QAudioSource::start(), and so every QMediaPlayer play() and QMediaRecorder
// record() with audio) while this backend has no server connection ABORTS
// the process in libpulse: pa_context_get_state(NULL) / pa_stream_new(NULL)
// (qpulseaudiosink.cpp, qpulseaudiosource.cpp; measured as SIGABRT for the
// call sounds, CallSoundMixer.h, commit 1efee79c). The window: from every
// server drop until Qt's retry about 3 s later, and for good if that retry
// finds no socket (Qt schedules no further one). The PipeWire backend never
// aborts there, so widening the PulseAudio backend widens this exposure, and
// every audio start is gated: the call sounds by PulseGate
// (PulseServerProbe.h), the voice player and recorder by QtAudioGate
// (media/QtAudioGate.h, a bounded wait, then a clear error), and the QML
// players through `qtAudio.ready` (main.cpp). A start nothing gates still
// aborts during the window.
//
// A user's own QT_AUDIO_BACKEND always wins. Outside the Flatpak the switch
// is made only when this Qt has its PulseAudio backend (libQt6Multimedia
// names libpulse among the libraries it needs: that feature is what links it)
// and a PulseAudio server (pipewire-pulse) answers on its socket; otherwise
// Qt keeps its default, and the log says why. Once Qt's audio is up, a value
// Lightning set is taken out of the environment again
// (releaseQtAudioBackendVariable()), so applications Lightning opens choose
// their own backend.
#pragma once

#include <QByteArray>
#include <QList>
#include <QFile>
#include <QString>

#include <cstring>

#ifdef Q_OS_LINUX
#include <elf.h>
#endif

namespace lightning::calls {

/// Where the process runs, as far as the backend choice cares.
enum class AudioPlatform { Linux, Windows, MacOS, Other };

/// The facts the choice depends on, all read before Qt Multimedia is used.
struct QtAudioBackendInputs {
    AudioPlatform platform = AudioPlatform::Other;
    bool flatpak = false;
    /// QT_AUDIO_BACKEND is set in the environment, to anything ("" included).
    bool userSetBackend = false;
    /// The RUNNING Qt (qVersion()), which is the one that picks the backend.
    int qtMajor = 0;
    int qtMinor = 0;
    /// This Qt was built with its PulseAudio backend (qtMultimediaHasPulseBackend()).
    bool qtHasPulseBackend = false;
    /// A PulseAudio server answers on the socket libpulse would use.
    bool pulseServerReachable = false;
};

struct QtAudioBackendDecision {
    /// The QT_AUDIO_BACKEND value to set, or empty to leave it alone.
    QByteArray value;
    /// Why, for the log: a short, stable phrase.
    const char *reason = "";
};

/// The QT_AUDIO_BACKEND value to set before anything in the process touches
/// Qt Multimedia, and why. See the top of this file.
inline QtAudioBackendDecision decideQtAudioBackend(const QtAudioBackendInputs &in)
{
    if (in.userSetBackend)
        return {{}, "the user set QT_AUDIO_BACKEND"};
    if (in.platform != AudioPlatform::Linux)
        return {{}, "not Linux (no PipeWire backend here)"};
    if (in.qtMajor < 6 || (in.qtMajor == 6 && in.qtMinor < 10))
        return {{}, "Qt before 6.10 has no PipeWire backend"};
    // The Flatpak grants --socket=pulseaudio and its runtime's Qt has the
    // backend; its validator requires PulseAudio, so it is not second-guessed.
    if (in.flatpak)
        return {QByteArrayLiteral("pulseaudio"),
                "Flatpak: Qt's PipeWire backend can spin the GUI thread"};
    if (!in.qtHasPulseBackend)
        return {{}, "this Qt has no PulseAudio backend; keeping Qt's default"};
    if (!in.pulseServerReachable)
        return {{}, "no PulseAudio server (pipewire-pulse) answered; keeping "
                    "Qt's default"};
    return {QByteArrayLiteral("pulseaudio"),
            "Qt 6.10+ PipeWire backend: orphaned streams spin the GUI thread "
            "and device changes stop after a PipeWire restart"};
}

/// Takes QT_AUDIO_BACKEND out of the environment again when Lightning set it
/// (`setByLightning`), once Qt has created its audio devices: Qt reads the
/// variable only then (QPlatformAudioDevices::create()) and caches its choice,
/// and a child process (an application opened through QDesktopServices,
/// which inherits this environment) must not inherit Lightning's choice. A
/// value the user set is never touched. Returns whether it unset anything.
inline bool releaseQtAudioBackendVariable(bool setByLightning)
{
    if (!setByLightning || !qEnvironmentVariableIsSet("QT_AUDIO_BACKEND"))
        return false;
    qunsetenv("QT_AUDIO_BACKEND");
    return true;
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
    // readAll(), not a readLine() loop: /proc files report size 0, so
    // atEnd() is true before the first line (the loop read nothing, and this
    // answered "no" for every library; seen as "libpipewire loaded=no" next to
    // Qt naming its PipeWire backend, 2026-10-16).
    return maps.readAll().contains(needle);
#else
    Q_UNUSED(prefix);
#endif
    return false;
}

/// The DT_NEEDED entries of the ELF64 little-endian file `path` (a shared
/// library), read from the file; empty for anything else or on any doubt.
inline QList<QByteArray> elfNeededLibraries(const QString &path)
{
    QList<QByteArray> needed;
#ifdef Q_OS_LINUX
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return needed;
    const qint64 size = file.size();
    const auto readAt = [&file, size](qint64 offset, qint64 length) {
        if (offset < 0 || length < 0 || offset > size || length > size - offset
            || !file.seek(offset))
            return QByteArray();
        return file.read(length);
    };
    Elf64_Ehdr header;
    const QByteArray head = readAt(0, sizeof header);
    if (head.size() != sizeof header)
        return needed;
    std::memcpy(&header, head.constData(), sizeof header);
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0
        || header.e_ident[EI_CLASS] != ELFCLASS64
        || header.e_ident[EI_DATA] != ELFDATA2LSB
        || header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 512)
        return needed;
    QList<Elf64_Phdr> loads;
    Elf64_Phdr dynamic{};
    bool haveDynamic = false;
    for (int i = 0; i < header.e_phnum; ++i) {
        const QByteArray raw = readAt(
            qint64(header.e_phoff) + qint64(i) * qint64(sizeof(Elf64_Phdr)),
            sizeof(Elf64_Phdr));
        if (raw.size() != sizeof(Elf64_Phdr))
            return needed;
        Elf64_Phdr ph;
        std::memcpy(&ph, raw.constData(), sizeof ph);
        if (ph.p_type == PT_LOAD)
            loads.append(ph);
        else if (ph.p_type == PT_DYNAMIC) {
            dynamic = ph;
            haveDynamic = true;
        }
    }
    if (!haveDynamic || dynamic.p_filesz > (1u << 20))
        return needed;
    const QByteArray dyn = readAt(qint64(dynamic.p_offset), qint64(dynamic.p_filesz));
    quint64 strtabAddr = 0;
    quint64 strtabSize = 0;
    QList<quint64> offsets;
    for (qsizetype at = 0; at + qsizetype(sizeof(Elf64_Dyn)) <= dyn.size();
         at += sizeof(Elf64_Dyn)) {
        Elf64_Dyn entry;
        std::memcpy(&entry, dyn.constData() + at, sizeof entry);
        if (entry.d_tag == DT_NULL)
            break;
        if (entry.d_tag == DT_NEEDED)
            offsets.append(entry.d_un.d_val);
        else if (entry.d_tag == DT_STRTAB)
            strtabAddr = entry.d_un.d_ptr;
        else if (entry.d_tag == DT_STRSZ)
            strtabSize = entry.d_un.d_val;
    }
    // The string table's address is virtual; the PT_LOAD holding it maps it
    // to the file.
    qint64 strtabOffset = -1;
    for (const Elf64_Phdr &load : loads) {
        if (strtabAddr >= load.p_vaddr
            && strtabAddr < load.p_vaddr + load.p_filesz) {
            strtabOffset = qint64(load.p_offset + (strtabAddr - load.p_vaddr));
            break;
        }
    }
    if (strtabOffset < 0 || strtabSize == 0 || strtabSize > (1u << 20))
        return needed;
    const QByteArray strings = readAt(strtabOffset, qint64(strtabSize));
    for (const quint64 offset : offsets) {
        if (offset >= quint64(strings.size()))
            continue;
        const char *name = strings.constData() + offset;
        needed.append(QByteArray(name, qstrnlen(name, strings.size() - offset)));
    }
#else
    Q_UNUSED(path);
#endif
    return needed;
}

/// The path of the first object mapped into this process whose file name
/// starts with `prefix` (Linux: /proc/self/maps); empty if none.
inline QString mappedObjectPath(const char *prefix)
{
#ifdef Q_OS_LINUX
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QByteArray name(prefix);
    // readAll(): see sharedObjectLoaded(). A line is "address perms offset
    // dev inode path", and the path, which may contain spaces, is everything
    // from its first '/' (the fields before it never contain one).
    for (const QByteArray &line : maps.readAll().split('\n')) {
        const qsizetype slash = line.indexOf('/');
        if (slash < 0)
            continue;
        QByteArray path = line.mid(slash);
        if (path.endsWith(" (deleted)"))
            path.chop(10);
        const QByteArray file = path.mid(path.lastIndexOf('/') + 1);
        if (file.startsWith(name))
            return QString::fromLocal8Bit(path);
    }
#else
    Q_UNUSED(prefix);
#endif
    return {};
}

/// Whether the Qt Multimedia in this process was built with its PulseAudio
/// backend: libQt6Multimedia itself names libpulse among the libraries it
/// needs (that feature is what links it). False when it cannot be read (a
/// static Qt, another platform): Qt's default is then left alone.
inline bool qtMultimediaHasPulseBackend()
{
    const QString path = mappedObjectPath("libQt6Multimedia.so");
    if (path.isEmpty())
        return false;
    for (const QByteArray &name : elfNeededLibraries(path)) {
        if (name.startsWith("libpulse.so") || name.startsWith("libpulse-simple"))
            return true;
    }
    return false;
}

} // namespace lightning::calls
