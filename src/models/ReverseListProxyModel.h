#pragma once

#include <QAbstractProxyModel>
#include <QMetaObject>
#include <QTimer>
#include <QVector>

// Flat-list proxy that exposes source rows in reverse order without sorting
// on model data. Source prepends therefore become proxy appends, while source
// appends become proxy prepends. TimelinePane combines this with a rotated
// viewport so loading older history extends the far/top edge instead of
// inserting before every visible delegate.
//
// It also PACES how fast newly paginated history becomes visible to the view.
// TimelinePane instantiates every row it is given (no height virtualization,
// so that row heights are measured rather than estimated), and a backward
// pagination page is around twenty rows at once. Handing all of them over in
// one event-loop turn means constructing twenty full message delegates before
// the next frame can be painted, which the user feels as the timeline locking
// up while history loads.
//
// So the oldest source rows are held back and released a few per timer tick.
// This is purely a delivery schedule, never a filter: nothing is dropped, the
// source model stays authoritative for every non-visual consumer, and the
// backlog drains within a few frames. Rows that arrive at the NEWEST end (a
// live message) are exempt and appear immediately — pacing there would be
// visible.
class ReverseListProxyModel final : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit ReverseListProxyModel(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    QModelIndex mapToSource(const QModelIndex &proxyIndex) const override;
    QModelIndex mapFromSource(const QModelIndex &sourceIndex) const override;
    QModelIndex index(int row, int column,
                      const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &proxyIndex,
                  int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &proxyIndex) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    void disconnectSource();
    // Rows in the source model, regardless of how many are released yet.
    int sourceRowTotal() const;
    void scheduleReveal();
    void revealNextChunk();

    QVector<QMetaObject::Connection> m_sourceConnections;

    // How many of the newest source rows are currently exposed. Always
    // <= sourceRowTotal(); the difference is the paced backlog of oldest rows.
    int m_revealedRows = 0;
    // Set between an announced beginInsertRows/beginRemoveRows pair so the
    // matching source signal knows whether it opened one.
    bool m_insertAnnounced = false;
    int m_announcedInsertCount = 0;
    bool m_removeAnnounced = false;
    int m_announcedRemoveCount = 0;
    QTimer m_revealTimer;
};
