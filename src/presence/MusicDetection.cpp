#include "presence/MusicDetection.h"

#include <QCoreApplication>
#include <QFile>
#include <QSet>
#include <QtGlobal>

namespace musicdetect {

namespace {

constexpr int kTitleMax = 100;
constexpr int kArtistMax = 80;
constexpr int kAlbumMax = 100;
constexpr int kLineMax = 200;
constexpr int kArtistsMax = 10;

bool isBidiControl(char32_t cp)
{
    return cp == 0x061C || cp == 0x200E || cp == 0x200F
        || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

bool isMeaningfulFormat(char32_t cp)
{
    return cp == 0x200C || cp == 0x200D || (cp >= 0xE0020 && cp <= 0xE007F);
}

bool isIdleSpotifyCaption(const QString &caption)
{
    return caption.compare(QLatin1String("Spotify"), Qt::CaseInsensitive) == 0
        || caption.compare(QLatin1String("Spotify Free"), Qt::CaseInsensitive) == 0
        || caption.compare(QLatin1String("Spotify Premium"), Qt::CaseInsensitive) == 0;
}

}

bool NowPlaying::showable() const
{
    return playing && !title.isEmpty() && !isAdvertisement(trackId);
}

QString cleanField(const QString &text, int maxChars)
{
    QString clean;
    clean.reserve(text.size());
    for (const char32_t cp : text.toUcs4()) {
        const auto cat = QChar::category(cp);
        if (cat == QChar::Other_Control) {
            clean.append(QLatin1Char(' '));
        } else if (isBidiControl(cp)
                   || (cat == QChar::Other_Format && !isMeaningfulFormat(cp))) {
            continue;
        } else {
            clean.append(QString::fromUcs4(&cp, 1));
        }
    }
    clean = clean.simplified();
    if (clean.size() > maxChars) {
        clean.truncate(maxChars > 1 ? maxChars - 1 : maxChars);
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
    return trackId.startsWith(QLatin1String("spotify:ad:"))
        || trackId.startsWith(QLatin1String("/com/spotify/ad/"));
}

NowPlaying fromMpris(const QString &playbackStatus, const QVariantMap &metadata)
{
    NowPlaying np;
    np.playing = playbackStatus == QLatin1String("Playing");
    np.title = cleanField(metadata.value(QStringLiteral("xesam:title")).toString(), kTitleMax);
    const QVariant artists = metadata.value(QStringLiteral("xesam:artist"));
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
        const QString artist = cleanField(caption.left(cut), kArtistMax);
        const QString title = cleanField(caption.mid(cut + kSeparator.size()), kTitleMax);
        if (artist.isEmpty() || title.isEmpty())
            continue;
        if (artist.compare(QLatin1String("Spotify"), Qt::CaseInsensitive) == 0)
            continue;
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
    const QString line = artist.isEmpty()
        ? QCoreApplication::translate("MusicActivity", "Listening to %1").arg(np.title)
        : QCoreApplication::translate("MusicActivity", "Listening to %1 by %2")
              .arg(np.title, artist);
    return cleanField(line, kLineMax);
}

bool flatpakGrantsSpotifyMpris(const QString &infoPath)
{
    QFile file(infoPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    bool inPolicy = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inPolicy = line == QLatin1String("[Session Bus Policy]");
            continue;
        }
        if (!inPolicy)
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString name = line.left(eq).trimmed();
        const QString level = line.mid(eq + 1).trimmed();
        if (name == QLatin1String("org.mpris.MediaPlayer2.spotify")
            && (level == QLatin1String("talk") || level == QLatin1String("own")))
            return true;
    }
    return false;
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
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID"))
        return flatpakGrantsSpotifyMpris(QStringLiteral("/.flatpak-info"));
    return qEnvironmentVariableIsEmpty("SNAP");
#else
    return false;
#endif
}

}
