#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace musicdetect {

inline constexpr char kSpotifyMprisService[] = "org.mpris.MediaPlayer2.spotify";
inline constexpr char kMprisObjectPath[] = "/org/mpris/MediaPlayer2";
inline constexpr char kMprisPlayerInterface[] = "org.mpris.MediaPlayer2.Player";

struct NowPlaying {
    QString title;
    QStringList artists;
    QString album;
    QString trackId;
    bool playing = false;

    bool showable() const;
    bool operator==(const NowPlaying &) const = default;
};

NowPlaying fromMpris(const QString &playbackStatus, const QVariantMap &metadata);

bool isAdvertisement(const QString &trackId);

NowPlaying fromSpotifyWindowTitles(const QStringList &titles);

bool sameTrack(const NowPlaying &a, const NowPlaying &b);

QString cleanField(const QString &text, int maxChars);

QString statusTextFor(const NowPlaying &np);

QString artistLine(const NowPlaying &np);

bool detectionSupported();

// True when the Flatpak instance info at `infoPath` (normally /.flatpak-info)
// lists a Session Bus Policy of talk or own for Spotify's MPRIS name, the one
// permission "Listening to" needs. The default Flatpak grants none, so the
// feature is simply unavailable there until the user allows it
// (flatpak override --talk-name=org.mpris.MediaPlayer2.spotify).
bool flatpakGrantsSpotifyMpris(const QString &infoPath);

}
