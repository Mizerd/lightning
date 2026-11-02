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

enum class AudioPlatform { Linux, Windows, MacOS, Other };

struct QtAudioBackendInputs {
    AudioPlatform platform = AudioPlatform::Other;
    bool flatpak = false;
    bool userSetBackend = false;
    int qtMajor = 0;
    int qtMinor = 0;
    bool qtHasPulseBackend = false;
    bool pulseServerReachable = false;
};

struct QtAudioBackendDecision {
    QByteArray value;
    const char *reason = "";
};

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

inline bool releaseQtAudioBackendVariable(bool setByLightning)
{
    if (!setByLightning || !qEnvironmentVariableIsSet("QT_AUDIO_BACKEND"))
        return false;
    qunsetenv("QT_AUDIO_BACKEND");
    return true;
}

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

inline QString mappedObjectPath(const char *prefix)
{
#ifdef Q_OS_LINUX
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QByteArray name(prefix);
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

}
