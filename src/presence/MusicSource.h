#pragma once

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

#include "presence/MusicDetection.h"

// Where the music activity learns what Spotify plays. One implementation per
// platform mechanism (MPRIS on Linux, the window title on Windows), all behind
// this interface so MusicActivity's rules are tested against a fake and each
// platform layer is tested on its own.
//
// start() begins watching and may report later (asynchronously); stop() drops
// every subscription and timer, after which no signal is emitted. A source is
// created only while the feature is active and destroyed when it is not.
class MusicSource : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~MusicSource() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual musicdetect::NowPlaying nowPlaying() const = 0;

Q_SIGNALS:
    void nowPlayingChanged();
};

// Polls a list of window captions and parses them as Spotify's Windows client
// names its main window. The caption provider is injected: the real one
// (EnumWindows over Spotify.exe, src/presence/SpotifyWindowTitles_win.cpp)
// exists only on Windows, and a test drives this class with a fake on any
// platform.
class WindowTitleMusicSource : public MusicSource
{
    Q_OBJECT
public:
    using TitleProvider = std::function<QStringList()>;
    explicit WindowTitleMusicSource(TitleProvider provider, int pollMs = 5000,
                                    QObject *parent = nullptr);
    ~WindowTitleMusicSource() override;

    void start() override;
    void stop() override;
    musicdetect::NowPlaying nowPlaying() const override { return m_now; }
    bool running() const;

    // The platform provider: Spotify.exe's top-level window captions on
    // Windows, nothing elsewhere.
    static TitleProvider platformProvider();

private:
    void poll();

    TitleProvider m_provider;
    QTimer m_timer;
    musicdetect::NowPlaying m_now;
};

// The platform's source, or nullptr where detection is unsupported.
std::unique_ptr<MusicSource> createPlatformMusicSource();
