#include "presence/MusicSource.h"

#include "presence/SpotifyWindowTitles_win.h"

#if defined(HAVE_QT_DBUS) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
#include "presence/MprisMusicSource.h"
#endif

WindowTitleMusicSource::WindowTitleMusicSource(TitleProvider provider, int pollMs,
                                               QObject *parent)
    : MusicSource(parent)
    , m_provider(std::move(provider))
{
    m_timer.setInterval(pollMs);
    connect(&m_timer, &QTimer::timeout, this, &WindowTitleMusicSource::poll);
}

WindowTitleMusicSource::~WindowTitleMusicSource() = default;

void WindowTitleMusicSource::start()
{
    if (m_timer.isActive())
        return;
    m_timer.start();
    poll();
}

void WindowTitleMusicSource::stop()
{
    m_timer.stop();
    m_now = {};
}

bool WindowTitleMusicSource::running() const
{
    return m_timer.isActive();
}

void WindowTitleMusicSource::poll()
{
    if (!m_timer.isActive() || !m_provider)
        return;
    const musicdetect::NowPlaying next =
        musicdetect::fromSpotifyWindowTitles(m_provider());
    if (next == m_now)
        return;
    m_now = next;
    Q_EMIT nowPlayingChanged();
}

WindowTitleMusicSource::TitleProvider WindowTitleMusicSource::platformProvider()
{
    return [] {
        QStringList out;
        for (const std::wstring &caption : spotifywin::spotifyWindowTitles())
            out.append(QString::fromStdWString(caption));
        return out;
    };
}

std::unique_ptr<MusicSource> createPlatformMusicSource()
{
    if (!musicdetect::detectionSupported())
        return nullptr;
#if defined(Q_OS_WIN)
    return std::make_unique<WindowTitleMusicSource>(
        WindowTitleMusicSource::platformProvider());
#elif defined(HAVE_QT_DBUS) && !defined(Q_OS_MACOS)
    return std::make_unique<MprisMusicSource>();
#else
    return nullptr;
#endif
}
