#pragma once

#include <QConcatenateTablesProxyModel>
#include <QVariantMap>

class GifStoredModel;

class GifSavedModel : public QConcatenateTablesProxyModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    GifSavedModel(GifStoredModel *local, GifStoredModel *provider,
                  QObject *parent = nullptr);

    // The shared role table, stated explicitly. QConcatenateTablesProxyModel
    // does not forward its sources' role names on Qt 6.8.2 (the packaged
    // builds' Qt), though it does on 6.11. The picker's delegate resolves
    // required properties by name, and QQmlDelegateModel refuses to build a
    // delegate it cannot satisfy, so the tab rendered blank while count() was
    // correct. Answering GifResultModel's table is correct on both versions and
    // is the invariant the role forwarding already depends on;
    // GifCollectionsTest and GifSavedTabQmlTest pin it.
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();
};
