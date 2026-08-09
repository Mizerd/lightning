#include "models/ReverseListProxyModel.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>

#include <algorithm>
#include <utility>

namespace {
// The release schedule is a TIME BUDGET, not a row count.
//
// A fixed count cannot work, because the cost of a row is not fixed: a one
// line message and a quoted reply with a media card and a reaction strip
// differ by an order of magnitude. Releasing three rows per 8 ms tick was
// still handing the view more work than the interval allowed, so ticks
// overran, the next fired immediately, and the event loop never got a turn —
// which is the timeline "locking" rather than simply refusing to scroll past
// the end of what has loaded.
//
// Instead: release rows one at a time until the budget is spent, then yield —
// and, critically, size the GAP from what the work actually cost.
//
// A fixed interval cannot bound the duty cycle. At least one row has to be
// built per tick or the queue never drains, so if a single row costs more than
// the interval the loop is saturated no matter how small the budget is. That
// is the state the user reported as input being queued and applied late: the
// timeline was not refusing to scroll, nothing was running.
//
// So after each tick the next one is scheduled at kIdleFactor times the time
// just spent. Whatever a row costs, construction gets at most 1/kIdleFactor of
// the wall clock and the rest belongs to input and painting. Cheap history
// drains almost immediately; expensive history takes longer but never locks
// the UI.
//
// The visible consequence is the intended one: the far edge of loaded history
// advances steadily instead of arriving in a lump, the reader can always move
// away from it, and scrolling INTO it stops at the edge — a boundary, not a
// freeze.
constexpr int kRevealBudgetMs = 3;
constexpr int kRevealMinIntervalMs = 16;
constexpr int kRevealMaxIntervalMs = 250;
constexpr int kIdleFactor = 4;      // ~25% of wall clock spent building
}

ReverseListProxyModel::ReverseListProxyModel(QObject *parent)
    : QAbstractProxyModel(parent)
{
    m_revealTimer.setInterval(kRevealMinIntervalMs);
    m_revealTimer.setSingleShot(false);
    connect(&m_revealTimer, &QTimer::timeout,
            this, &ReverseListProxyModel::revealNextChunk);
}

int ReverseListProxyModel::sourceRowTotal() const
{
    return sourceModel() ? sourceModel()->rowCount() : 0;
}

void ReverseListProxyModel::scheduleReveal()
{
    if (m_revealedRows >= sourceRowTotal()) {
        m_revealTimer.stop();
        return;
    }
    if (!m_revealTimer.isActive())
        m_revealTimer.start();
}

void ReverseListProxyModel::revealNextChunk()
{
    if (m_revealedRows >= sourceRowTotal()) {
        m_revealTimer.stop();
        return;
    }

    // The backlog is always the OLDEST source rows, which map to the proxy's
    // tail — the far/top edge of the rotated view. Releasing them therefore
    // appends, and cannot move a row the reader is already looking at.
    //
    // endInsertRows() builds the row synchronously, so elapsed() measures the
    // real construction cost and the budget adapts to whatever this particular
    // history happens to contain.
    QElapsedTimer spent;
    spent.start();
    int released = 0;
    do {
        beginInsertRows({}, m_revealedRows, m_revealedRows);
        ++m_revealedRows;
        endInsertRows();
        ++released;
    } while (m_revealedRows < sourceRowTotal()
             && spent.elapsed() < kRevealBudgetMs);

    // Counts and milliseconds only — no room, event or message content. This
    // is the number every scroll-performance guess so far has been missing:
    // what one timeline row actually costs to build.
    static const bool traceEnabled =
        qEnvironmentVariableIsSet("LIGHTNING_SCROLL_TRACE");
    if (traceEnabled) {
        qInfo("row-reveal released=%d elapsedMs=%lld perRowMs=%.1f backlog=%d",
              released, static_cast<long long>(spent.elapsed()),
              released > 0 ? double(spent.elapsed()) / released : 0.0,
              sourceRowTotal() - m_revealedRows);
    }

    if (m_revealedRows >= sourceRowTotal()) {
        m_revealTimer.stop();
        m_revealTimer.setInterval(kRevealMinIntervalMs);
        return;
    }

    // Yield for proportionally longer than the work just took, so the duty
    // cycle holds no matter how expensive this room's rows turn out to be.
    const int elapsed = static_cast<int>(spent.elapsed());
    m_revealTimer.setInterval(std::clamp(elapsed * kIdleFactor,
                                         kRevealMinIntervalMs,
                                         kRevealMaxIntervalMs));
}

void ReverseListProxyModel::disconnectSource()
{
    for (const auto &connection : std::as_const(m_sourceConnections))
        QObject::disconnect(connection);
    m_sourceConnections.clear();
}

void ReverseListProxyModel::setSourceModel(QAbstractItemModel *model)
{
    if (model == sourceModel())
        return;

    beginResetModel();
    m_revealTimer.stop();
    m_insertAnnounced = false;
    m_removeAnnounced = false;
    disconnectSource();
    QAbstractProxyModel::setSourceModel(model);
    m_revealedRows = model ? model->rowCount() : 0;

    if (model) {
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::rowsAboutToBeInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                m_insertAnnounced = false;
                if (parent.isValid())
                    return;
                const int inserted = last - first + 1;
                const int totalAfter = sourceRowTotal() + inserted;
                // Rows landing entirely inside the not-yet-released oldest
                // region change nothing the view can see. Stay silent and let
                // the reveal timer pace them out; this is the backward
                // pagination case, and it is why loading no longer blocks.
                if (last < totalAfter - m_revealedRows)
                    return;
                const int proxyFirst = totalAfter - 1 - last;
                beginInsertRows({}, proxyFirst, proxyFirst + inserted - 1);
                m_insertAnnounced = true;
                m_announcedInsertCount = inserted;
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                    return;
                if (m_insertAnnounced) {
                    m_revealedRows += m_announcedInsertCount;
                    m_insertAnnounced = false;
                    endInsertRows();
                }
                scheduleReveal();
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
                m_removeAnnounced = false;
                if (parent.isValid())
                    return;
                const int totalBefore = sourceRowTotal();
                int proxyFirst = totalBefore - 1 - last;
                int proxyLast = totalBefore - 1 - first;
                // Entirely inside the unreleased backlog: the view never saw
                // these rows, so there is nothing to remove from it.
                if (proxyLast < 0 || proxyFirst >= m_revealedRows)
                    return;
                proxyFirst = std::max(proxyFirst, 0);
                proxyLast = std::min(proxyLast, m_revealedRows - 1);
                beginRemoveRows({}, proxyFirst, proxyLast);
                m_removeAnnounced = true;
                m_announcedRemoveCount = proxyLast - proxyFirst + 1;
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                    return;
                if (m_removeAnnounced) {
                    m_revealedRows -= m_announcedRemoveCount;
                    m_removeAnnounced = false;
                    endRemoveRows();
                }
                // A removal can only shrink the backlog; never leave the
                // released count above what the source still holds.
                m_revealedRows = std::min(m_revealedRows, sourceRowTotal());
                scheduleReveal();
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QList<int> &roles) {
                if (topLeft.parent().isValid() || bottomRight.parent().isValid())
                    return;
                const int total = sourceRowTotal();
                int proxyFirst = total - 1 - bottomRight.row();
                int proxyLast = total - 1 - topLeft.row();
                // Clamp to what the view has actually been given; a change to
                // a still-unreleased row needs no signal, because the row will
                // be read fresh when it is released.
                if (proxyLast < 0 || proxyFirst >= m_revealedRows)
                    return;
                proxyFirst = std::max(proxyFirst, 0);
                proxyLast = std::min(proxyLast, m_revealedRows - 1);
                Q_EMIT dataChanged(index(proxyFirst, topLeft.column()),
                                   index(proxyLast, bottomRight.column()),
                                   roles);
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::modelAboutToBeReset,
            this, [this] {
                m_revealTimer.stop();
                beginResetModel();
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::modelReset,
            this, [this] {
                // A reset is a room switch or a fresh snapshot: release it
                // whole. The set is small (an initial timeline, not a paged
                // backlog) and the presentation gate covers it either way.
                m_revealedRows = sourceRowTotal();
                endResetModel();
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::headerDataChanged, this,
            [this](Qt::Orientation orientation, int first, int last) {
                if (orientation == Qt::Horizontal) {
                    Q_EMIT headerDataChanged(orientation, first, last);
                    return;
                }
                const int count = rowCount();
                Q_EMIT headerDataChanged(orientation,
                                         count - 1 - last,
                                         count - 1 - first);
            }));
    }
    endResetModel();
}

// Both directions key off the SOURCE total, not the released count: proxy row
// 0 is always the newest source row, and the unreleased backlog is the tail.
// Using rowCount() here would silently renumber every visible row whenever a
// page arrived.
QModelIndex ReverseListProxyModel::mapToSource(
    const QModelIndex &proxyIndex) const
{
    if (!sourceModel() || !proxyIndex.isValid() || proxyIndex.parent().isValid())
        return {};
    return sourceModel()->index(sourceRowTotal() - 1 - proxyIndex.row(),
                                proxyIndex.column());
}

QModelIndex ReverseListProxyModel::mapFromSource(
    const QModelIndex &sourceIndex) const
{
    if (!sourceModel() || !sourceIndex.isValid()
        || sourceIndex.parent().isValid())
        return {};
    return index(sourceRowTotal() - 1 - sourceIndex.row(),
                 sourceIndex.column());
}

QModelIndex ReverseListProxyModel::index(int row, int column,
                                        const QModelIndex &parent) const
{
    if (parent.isValid() || row < 0 || row >= rowCount()
        || column < 0 || column >= columnCount())
        return {};
    return createIndex(row, column);
}

QModelIndex ReverseListProxyModel::parent(const QModelIndex &) const
{
    return {};
}

int ReverseListProxyModel::rowCount(const QModelIndex &parent) const
{
    if (!sourceModel() || parent.isValid())
        return 0;
    // The released prefix, not the source total. std::min guards against a
    // source that shrank without a signal we could act on.
    return std::min(m_revealedRows, sourceModel()->rowCount());
}

int ReverseListProxyModel::columnCount(const QModelIndex &parent) const
{
    return !sourceModel() || parent.isValid() ? 0 : sourceModel()->columnCount();
}

QVariant ReverseListProxyModel::data(const QModelIndex &proxyIndex, int role) const
{
    return sourceModel() ? sourceModel()->data(mapToSource(proxyIndex), role)
                         : QVariant{};
}

Qt::ItemFlags ReverseListProxyModel::flags(const QModelIndex &proxyIndex) const
{
    return sourceModel() ? sourceModel()->flags(mapToSource(proxyIndex))
                         : Qt::NoItemFlags;
}

QHash<int, QByteArray> ReverseListProxyModel::roleNames() const
{
    return sourceModel() ? sourceModel()->roleNames()
                         : QHash<int, QByteArray>{};
}
