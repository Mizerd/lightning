#pragma once

#include "gif/GifStoredModel.h"

// v0.6.1: locally-persisted GIF favorites (cross-provider; each entry keeps its
// provider identity). Toggle from the picker; stable, deduped, bounded.
class GifFavoritesModel : public GifStoredModel
{
    Q_OBJECT

public:
    explicit GifFavoritesModel(QSettings *settings, QObject *parent = nullptr);

    // Add if absent, remove if present. Returns the new favorite state.
    Q_INVOKABLE bool toggle(const QVariantMap &result);
    Q_INVOKABLE bool isFavorite(const QString &provider, const QString &id) const
    { return contains(provider, id); }
    Q_INVOKABLE void unfavorite(const QString &provider, const QString &id)
    { removeEntry(provider, id); }

Q_SIGNALS:
    void favoriteToggled(const QString &provider, const QString &id, bool on);

private:
    static constexpr int kMaxFavorites = 200;
};
