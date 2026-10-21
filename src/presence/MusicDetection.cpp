#include "presence/MusicDetection.h"

#include <QCoreApplication>
#include <QSet>
#include <QtGlobal>

namespace musicdetect {

namespace {

constexpr int kTitleMax = 100;
constexpr int kArtistMax = 80;
constexpr int kAlbumMax = 100;
constexpr int kLineMax = 200;
// A track has a handful of artists; a list longer than this is not a credit
// line anyone reads, and it bounds the work on hostile metadata.
constexpr int kArtistsMax = 10;

// Bidirectional controls: they can reorder another person's status line.
bool isBidiControl(char32_t cp)
{
    return cp == 0x061C || cp == 0x200E || cp == 0x200F
        || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

// Format characters that are part of how text is WRITTEN and must survive:
// ZWNJ/ZWJ (Persian and other scripts; emoji sequences like 👩\u200D💻) and the
// tag characters of subdivision flags.
bool isMeaningfulFormat(char32_t cp)
{
    return cp == 0x200C || cp == 0x200D || (cp >= 0xE0020 && cp <= 0xE007F);
}

// What Spotify's Windows main window is called when nothing is playing.
bool isIdleSpotifyCaption(const QString &caption)
{
    return caption.compare(QLatin1String("Spotify"), Qt::CaseInsensitive) == 0
        || caption.compare(QLatin1String("Spotify Free"), Qt::CaseInsensitive) == 0
        || caption.compare(QLatin1String("Spotify Premium"), Qt::CaseInsensitive) == 0;
}

}   // namespace

bool NowPlaying::showable() const
{
    return playing && !title.isEmpty() && !isAdvertisement(trackId);
}

QString cleanField(const QString &text, int maxChars)
{
    QString clean;
    clean.reserve(text.size());
    // By code point: a QChar loop sees half of every emoji.
    for (const char32_t cp : text.toUcs4()) {
        const auto cat = QChar::category(cp);
        if (cat == QChar::Other_Control) {
            clean.append(QLatin1Char(' '));   // a tab or newline separates words
        } else if (isBidiControl(cp)
                   || (cat == QChar::Other_Format && !isMeaningfulFormat(cp))) {
            continue;   // dropped, not spaced: they sit inside words
        } else {
            clean.append(QString::fromUcs4(&cp, 1));
        }
    }
    clean = clean.simplified();
    if (clean.size() > maxChars) {
        // The ellipsis is inside the budget, so the bound still holds and a
        // cut title reads as cut rather than as a dangling "On A by Artist".
        clean.truncate(maxChars > 1 ? maxChars - 1 : maxChars);
        // Never leave half of a surrogate pair at the cut.
        if (!clean.isEmpty() && clean.back().isHighSurrogate())
            clean.chop(1);
        clean = clean.trimmed();
        if (maxChars > 1)
            clean.append(QChar(0x2026));
    }
    return clean;
}

bool isAdvertisement(const QString &trackId)
{
    // "spotify:ad:<id>" in older clients, "/com/spotify/ad/<id>" since the
    // track id became an object path.
    return trackId.startsWith(QLatin1String("spotify:ad:"))
        || trackId.startsWith(QLatin1String("/com/spotify/ad/"));
}

NowPlaying fromMpris(const QString &playbackStatus, const QVariantMap &metadata)
{
    NowPlaying np;
    np.playing = playbackStatus == QLatin1String("Playing");
    np.title = cleanField(metadata.value(QStringLiteral("xesam:title")).toString(), kTitleMax);
    const QVariant artists = metadata.value(QStringLiteral("xesam:artist"));
    // The spec says a list; a lone string is tolerated.
    const QStringList raw = artists.metaType().id() == QMetaType::QStringList
                                ? artists.toStringList()
                                : QStringList { artists.toString() };
    QSet<QString> seen;
    for (const QString &a : raw) {
        if (np.artists.size() >= kArtistsMax)
            break;
        const QString clean = cleanField(a, kArtistMax);
        if (!clean.isEmpty() && !seen.contains(clean)) {
            seen.insert(clean);
            np.artists.append(clean);
        }
    }
    np.album = cleanField(metadata.value(QStringLiteral("xesam:album")).toString(), kAlbumMax);
    np.trackId = metadata.value(QStringLiteral("mpris:trackid")).toString();
    return np;
}

NowPlaying fromSpotifyWindowTitles(const QStringList &titles)
{
    static const QString kSeparator = QStringLiteral(" - ");
    for (const QString &raw : titles) {
        const QString caption = raw.trimmed();
        if (caption.isEmpty() || isIdleSpotifyCaption(caption))
            continue;
        if (caption.contains(QLatin1String("Advertisement"), Qt::CaseInsensitive))
            continue;
        const qsizetype cut = caption.indexOf(kSeparator);
        if (cut <= 0)
            continue;
        // "Artist - Title - Remastered 2011": artists rarely contain the
        // separator and titles often do, so the FIRST one splits.
        const QString artist = cleanField(caption.left(cut), kArtistMax);
        const QString title = cleanField(caption.mid(cut + kSeparator.size()), kTitleMax);
        if (artist.isEmpty() || title.isEmpty())
            continue;
        if (artist.compare(QLatin1String("Spotify"), Qt::CaseInsensitive) == 0)
            continue;   // "Spotify - Advertisement"-style captions
        NowPlaying np;
        np.playing = true;
        np.title = title;
        np.artists = { artist };
        return np;
    }
    return {};
}

bool sameTrack(const NowPlaying &a, const NowPlaying &b)
{
    return a.title == b.title && a.artists == b.artists && a.album == b.album
        && a.trackId == b.trackId;
}

QString artistLine(const NowPlaying &np)
{
    return np.artists.join(QStringLiteral(", "));
}

QString statusTextFor(const NowPlaying &np)
{
    if (!np.showable())
        return {};
    const QString artist = cleanField(artistLine(np), kArtistMax);
    // Discord's two phrases: "Listening to …" on the status line, "by
    // <artist>" on its profile card. Translated: a status line is text the
    // user publishes, like a status they type.
    const QString line = artist.isEmpty()
        ? QCoreApplication::translate("MusicActivity", "Listening to %1").arg(np.title)
        : QCoreApplication::translate("MusicActivity", "Listening to %1 by %2")
              .arg(np.title, artist);
    return cleanField(line, kLineMax);
}

bool detectionSupported()
{
#if defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_MACOS)
    return false;
#elif defined(HAVE_QT_DBUS)
    // Strict confinement mediates the session bus and no plug grants talking
    // to an unconfined MPRIS player.
    return qEnvironmentVariableIsEmpty("SNAP");
#else
    return false;
#endif
}

}   // namespace musicdetect
