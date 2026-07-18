#include "gif/GifFavoritesModel.h"

GifFavoritesModel::GifFavoritesModel(QSettings *settings, QObject *parent)
    : GifStoredModel(settings, QStringLiteral("gif/favorites"), kMaxFavorites,
                     parent)
{
}

bool GifFavoritesModel::toggle(const QVariantMap &result)
{
    const gif::GifResult r = fromVariantMap(result);
    if (r.provider.isEmpty() || r.id.isEmpty())
        return false;
    if (contains(r.provider, r.id)) {
        removeEntry(r.provider, r.id);
        Q_EMIT favoriteToggled(r.provider, r.id, false);
        return false;
    }
    insertFront(r);
    Q_EMIT favoriteToggled(r.provider, r.id, true);
    return true;
}
