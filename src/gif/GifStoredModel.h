#pragma once

#include "gif/GifResponseParser.h"
#include "gif/GifResultModel.h"

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>

class QSettings;

class GifStoredModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    GifStoredModel(QSettings *settings, QString key, int maxRows,
                   QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_rows.size(); }

    Q_INVOKABLE bool contains(const QString &provider, const QString &id) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void clearAll();

    // Repoints persistence at `settings` (nullptr = none yet) and reloads,
    // replacing every row. Used by GifStarredStore to switch accounts without
    // recreating the model, whose identity GifSavedModel depends on. A null
    // `settings` clears all rows without persisting.
    void reopen(QSettings *settings);

    gif::GifResult resultAt(int row) const;

    static gif::GifResult fromVariantMap(const QVariantMap &map);

Q_SIGNALS:
    void countChanged();

protected:
    void insertFront(const gif::GifResult &r);
    bool removeEntry(const QString &provider, const QString &id);
    int indexOf(const QString &provider, const QString &id) const;

    void load();
    void save();

    QList<gif::GifResult> m_rows;

private:
    QSettings *m_settings;
    QString m_key;
    int m_maxRows;
};
