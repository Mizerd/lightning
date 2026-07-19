#include "gif/GifBuildKeys.h"

// The generated header lives only in the build tree and may be absent (ordinary
// source build, or removed after packaging). __has_include keeps the source
// compiling and correct either way; a missing header means "no embedded key".
#if __has_include(<LightningGifBuildKeys.h>)
#  include <LightningGifBuildKeys.h>
#endif

#ifndef LIGHTNING_GIF_BUILD_GIPHY_KEY
#  define LIGHTNING_GIF_BUILD_GIPHY_KEY ""
#endif
#ifndef LIGHTNING_GIF_BUILD_KLIPY_KEY
#  define LIGHTNING_GIF_BUILD_KLIPY_KEY ""
#endif

namespace gif {

QString buildKeyFor(const QString &providerId)
{
    if (providerId == QStringLiteral("giphy"))
        return QString::fromUtf8(LIGHTNING_GIF_BUILD_GIPHY_KEY).trimmed();
    if (providerId == QStringLiteral("klipy"))
        return QString::fromUtf8(LIGHTNING_GIF_BUILD_KLIPY_KEY).trimmed();
    return QString();
}

QString resolveProviderKey(const QString &runtimeOverride,
                           const QString &buildKey)
{
    const QString override = runtimeOverride.trimmed();
    if (!override.isEmpty())
        return override;
    return buildKey.trimmed();
}

} // namespace gif
