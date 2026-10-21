#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

// "Listening to …" music activity (Labs): the pure half. What Spotify is
// playing, read from its MPRIS properties (Linux) or its main-window title
// (Windows), reduced to the one status line Matrix can carry. Qt Core only,
// no D-Bus and no Win32, so every rule here is testable on any platform.
//
// Spotify only, as on Discord: the desktop client's own MPRIS name, never a
// browser tab or another player. Only the title and the artist are ever
// published; the album stays on this machine (the local preview).
namespace musicdetect {

// The Spotify desktop client owns this name in every Linux packaging (deb,
// snap and Flatpak Spotify alike). Third-party Spotify clients own other
// names and are not accepted.
inline constexpr char kSpotifyMprisService[] = "org.mpris.MediaPlayer2.spotify";
inline constexpr char kMprisObjectPath[] = "/org/mpris/MediaPlayer2";
inline constexpr char kMprisPlayerInterface[] = "org.mpris.MediaPlayer2.Player";

struct NowPlaying {
    QString title;
    QStringList artists;
    QString album;
    QString trackId;
    bool playing = false;

    // Playing, titled, and not an advertisement.
    bool showable() const;
    bool operator==(const NowPlaying &) const = default;
};

// From the MPRIS Player properties. `metadata` is plain types: the D-Bus
// layer has already turned the trackid object path into a string and
// `xesam:artist` into a string list.
NowPlaying fromMpris(const QString &playbackStatus, const QVariantMap &metadata);

// Spotify plays advertisements on the free tier; they carry an ad track id.
bool isAdvertisement(const QString &trackId);

// Spotify's Windows client names its main window "Artist - Title" while a
// track plays and "Spotify", "Spotify Free" or "Spotify Premium" otherwise.
// `titles` are the captions of the Spotify process's top-level windows.
// NOT TESTED on a live client: how a podcast episode and an advertisement are
// captioned. An ad captioned "Brand - Something" would read as a song; the
// captions filtered here are the documented idle names and "Advertisement".
NowPlaying fromSpotifyWindowTitles(const QStringList &titles);

// Same song: what a skip changes. Playback state is not part of it.
bool sameTrack(const NowPlaying &a, const NowPlaying &b);

// Bidi controls and invisible format characters dropped (ZWJ, ZWNJ and flag
// tag characters kept: they are part of how emoji and some scripts are
// written), control characters turned into spaces, whitespace collapsed,
// bounded.
QString cleanField(const QString &text, int maxChars);

// "Listening to <title> by <artist>" (translated), "Listening to <title>"
// without an artist, and "" for anything not showable. At most 200
// characters, the bound PresenceManager applies to a status line.
QString statusTextFor(const NowPlaying &np);

// The artists as one string ("A, B").
QString artistLine(const NowPlaying &np);

// False where this build has no way to see the player: macOS, a Linux build
// without QtDBus, and the Snap (strict confinement grants no MPRIS access).
bool detectionSupported();

}   // namespace musicdetect
