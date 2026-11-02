#pragma once

#include <QAbstractProxyModel>
#include <QMetaObject>
#include <QTimer>
#include <QVector>

class ReverseListProxyModel final : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit ReverseListProxyModel(QObject *parent = nullptr);

    Q_INVOKABLE void releaseAll();

    Q_INVOKABLE void showLatestWindow(int rows);

    Q_PROPERTY(int windowSkip READ windowSkip NOTIFY windowChanged)
    int windowSkip() const { return m_windowSkip; }
    Q_INVOKABLE bool revealIdle() const;
    Q_INVOKABLE int oldestExposedSourceRow() const;
    Q_INVOKABLE void setWindow(int skipNewest, int rows);
    Q_INVOKABLE void clearWindow();

    Q_INVOKABLE bool extendWindowAtOldEnd(int extraRows);

    Q_INVOKABLE bool extendWindowAtNewEnd(int extraRows);

Q_SIGNALS:
    void windowChanged();

public:
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
    void setWindowSkip(int skip);
    void disconnectSource();
    int sourceRowTotal() const;
    void scheduleReveal();
    int revealTarget() const;
    void revealNextChunk();

    QVector<QMetaObject::Connection> m_sourceConnections;

    int m_revealedRows = 0;
    int m_windowSkip = 0;
    int m_windowCap = 0;
    // Set between an announced beginInsertRows/beginRemoveRows pair so the
    // matching source signal knows whether it opened one.
    bool m_insertAnnounced = false;
    int m_announcedInsertCount = 0;
    bool m_removeAnnounced = false;
    int m_announcedRemoveCount = 0;
    QTimer m_revealTimer;
};
