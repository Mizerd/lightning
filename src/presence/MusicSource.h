#pragma once

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

#include "presence/MusicDetection.h"

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

    static TitleProvider platformProvider();

private:
    void poll();

    TitleProvider m_provider;
    QTimer m_timer;
    musicdetect::NowPlaying m_now;
};

std::unique_ptr<MusicSource> createPlatformMusicSource();
