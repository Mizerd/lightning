#pragma once

#include "stickers/StickerPack.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>

// The picker's grid: the images of ONE pack, already narrowed to the usage
// the surface asked for.
//
// The narrowing happens in the manager, not here, so this model is a plain
// presentation surface with no policy of its own — and a test can hand it any
// row set directly.
class StickerImageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        ShortcodeRole = Qt::UserRole + 1,
        UrlRole,
        BodyRole,
        MimetypeRole,
        WidthRole,
        HeightRole,
        SizeRole,
        IsEmoticonRole,
        IsStickerRole,
        // width/height as a ratio for stable grid sizing; 1.0 when the pack
        // declared no dimensions, which is common.
        AspectRole,
    };

    explicit StickerImageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_images.size(); }

    void reset(const QList<stickers::PackImage> &images);
    void clear();

    const QList<stickers::PackImage> &images() const { return m_images; }
    // The map the send/save paths take. Same shape as the roles above.
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    QList<stickers::PackImage> m_images;
};
