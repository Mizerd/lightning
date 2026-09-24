#include "models/ReverseListProxyModel.h"

#include "app/GuiStallTracer.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>

#include <algorithm>
#include <utility>

namespace {
// Row release is paced by a time budget, not a row count: row costs vary by an
// order of magnitude, and a fixed count per tick can overrun and starve the
// event loop. Release rows until the budget is spent, then schedule the next
// tick at kIdleFactor times the time just spent, so construction takes at most
// 1/kIdleFactor of wall clock whatever a row costs. Loaded history grows
// steadily and scrolling into it stops at the edge instead of freezing.
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

// How many rows pacing may expose. The cap keeps the reveal timer from undoing
// a window the pane just set.
bool ReverseListProxyModel::revealIdle() const
{
    return m_revealedRows >= revealTarget();
}

int ReverseListProxyModel::revealTarget() const
{
    const int available = std::max(0, sourceRowTotal() - m_windowSkip);
    return m_windowCap > 0 ? std::min(m_windowCap, available) : available;
}

// The single writer of m_windowSkip and the single emitter of
// windowChanged(). QML's `rowWindowSkip` binding depends only on this notify,
// so a write that does not emit leaves a stale skip (and atBottomEdge() false
// at the true live edge). Funnelling every write through here keeps that safe.
//
// Emits only on a real change, since this runs inside source-signal handlers.
// Callers update m_revealedRows first so rowCount() is coherent when the notify
// runs. The reset paths notify inside their begin/endResetModel bracket so the
// view never rebuilds against the outgoing room's window.
void ReverseListProxyModel::setWindowSkip(int skip)
{
    if (m_windowSkip == skip)
        return;
    m_windowSkip = skip;
    Q_EMIT windowChanged();
}

void ReverseListProxyModel::scheduleReveal()
{
    if (m_revealedRows >= revealTarget()) {
        m_revealTimer.stop();
        return;
    }
    if (!m_revealTimer.isActive())
        m_revealTimer.start();
}

void ReverseListProxyModel::releaseAll()
{
    // A jump must reach any row, so lift the cap as well as the backlog; the
    // pane re-establishes the window once the reader settles.
    m_windowCap = 0;
    const int total = sourceRowTotal() - m_windowSkip;
    if (m_revealedRows >= total)
        return;
    m_revealTimer.stop();
    m_revealTimer.setInterval(kRevealMinIntervalMs);
    beginInsertRows({}, m_revealedRows, total - 1);
    m_revealedRows = total;
    endInsertRows();
}

void ReverseListProxyModel::revealNextChunk()
{
    if (m_revealedRows >= revealTarget()) {
        m_revealTimer.stop();
        return;
    }

    // The backlog is always the oldest source rows, i.e. the proxy's tail (the
    // far edge of the rotated view), so releasing them appends and cannot move
    // a visible row. endInsertRows() builds rows synchronously, so elapsed()
    // measures real construction cost. The stall scope is a no-op unless
    // LIGHTNING_GUI_STALL_TRACE is set.
    stalltrace::Scope stallScope("row-reveal");
    QElapsedTimer spent;
    spent.start();
    int released = 0;
    do {
        beginInsertRows({}, m_revealedRows, m_revealedRows);
        ++m_revealedRows;
        endInsertRows();
        ++released;
        // Bound on revealTarget(), not sourceRowTotal(), or a single tick would
        // release straight through the window cap.
    } while (m_revealedRows < revealTarget()
             && spent.elapsed() < kRevealBudgetMs);

    // Counts and milliseconds only: what one timeline row costs to build.
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

    // Yield proportionally to the work just done so the duty cycle holds.
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
    setWindowSkip(0);
    m_windowCap = 0;
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
                // A row newer than the window's newest edge is outside it: grow
                // the skip so the window keeps covering the same rows (a live
                // message while the reader is deep in history).
                if (m_windowSkip > 0 && first > totalAfter - 1 - m_windowSkip) {
                    setWindowSkip(m_windowSkip + inserted);
                    return;
                }
                // Rows entirely inside the unreleased region are invisible; the
                // reveal timer paces them out (backward pagination).
                if (last < totalAfter - m_windowSkip - m_revealedRows)
                    return;
                const int proxyFirst = totalAfter - 1 - m_windowSkip - last;
                if (proxyFirst < 0)
                    return;
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
                int proxyFirst = totalBefore - 1 - m_windowSkip - last;
                int proxyLast = totalBefore - 1 - m_windowSkip - first;
                // Removals newer than the window shrink the skip, not the
                // exposed slice.
                if (proxyLast < 0) {
                    setWindowSkip(m_windowSkip
                                  - std::min(m_windowSkip, last - first + 1));
                    return;
                }
                // Entirely inside the unreleased backlog: the view never saw
                // these rows.
                if (proxyFirst >= rowCount())
                    return;
                proxyFirst = std::max(proxyFirst, 0);
                proxyLast = std::min(proxyLast, rowCount() - 1);
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
                // released count above what the source holds.
                setWindowSkip(std::min(m_windowSkip, sourceRowTotal()));
                m_revealedRows =
                    std::min(m_revealedRows, sourceRowTotal() - m_windowSkip);
                scheduleReveal();
            }));
        m_sourceConnections.append(connect(
            model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QList<int> &roles) {
                if (topLeft.parent().isValid() || bottomRight.parent().isValid())
                    return;
                // Subtract the window skip, as every other mapping does;
                // otherwise a change is announced on the wrong row and edits,
                // redactions and late decryptions never repaint.
                const int total = sourceRowTotal();
                int proxyFirst = total - 1 - m_windowSkip - bottomRight.row();
                int proxyLast = total - 1 - m_windowSkip - topLeft.row();
                // Changes to unreleased rows need no signal; those rows are
                // read fresh on release.
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
                // A reset (room switch or fresh snapshot) is released whole,
                // and the window goes with it: a stale skip would hide the new
                // room's live edge.
                setWindowSkip(0);
                m_windowCap = 0;
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

// Both directions key off the source total, not the released count: proxy
// row 0 is the newest row the window includes and the unreleased backlog is
// the tail. `m_windowSkip` shifts the newest edge; it is 0 whenever the reader
// can reach the bottom.
QModelIndex ReverseListProxyModel::mapToSource(
    const QModelIndex &proxyIndex) const
{
    if (!sourceModel() || !proxyIndex.isValid() || proxyIndex.parent().isValid())
        return {};
    const int sourceRow =
        sourceRowTotal() - 1 - m_windowSkip - proxyIndex.row();
    if (sourceRow < 0)
        return {};
    return sourceModel()->index(sourceRow, proxyIndex.column());
}

QModelIndex ReverseListProxyModel::mapFromSource(
    const QModelIndex &sourceIndex) const
{
    if (!sourceModel() || !sourceIndex.isValid()
        || sourceIndex.parent().isValid())
        return {};
    return index(sourceRowTotal() - 1 - m_windowSkip - sourceIndex.row(),
                 sourceIndex.column());
}

int ReverseListProxyModel::oldestExposedSourceRow() const
{
    const int rows = rowCount();
    if (rows <= 0)
        return -1;
    return sourceRowTotal() - m_windowSkip - rows;
}

// One structural op per end, oldest end first so proxy indices stay valid.
// Each op inserts or removes at one end; rows in the middle are never
// renumbered.
void ReverseListProxyModel::setWindow(int skipNewest, int rows)
{
    if (!sourceModel())
        return;
    const int total = sourceRowTotal();
    const int skip = std::clamp(skipNewest, 0, std::max(0, total));
    const int wanted = std::clamp(rows, 0, std::max(0, total - skip));

    // (1) The oldest end, at the current skip: the tail of the Column, so
    // nothing visible moves.
    const int keptAtCurrentSkip =
        std::clamp(wanted + (skip - m_windowSkip), 0,
                   std::max(0, total - m_windowSkip));
    const int exposed = rowCount();
    if (keptAtCurrentSkip < exposed) {
        beginRemoveRows({}, keptAtCurrentSkip, exposed - 1);
        m_revealedRows = keptAtCurrentSkip;
        endRemoveRows();
    } else if (keptAtCurrentSkip > exposed) {
        beginInsertRows({}, exposed, keptAtCurrentSkip - 1);
        m_revealedRows = keptAtCurrentSkip;
        endInsertRows();
    }

    // (2) The newest end. Raising the skip removes rows from the head (the pane
    //     compensates contentY by the height delta); lowering it inserts there.
    if (skip > m_windowSkip) {
        const int drop = std::min(skip - m_windowSkip, rowCount());
        if (drop > 0) {
            beginRemoveRows({}, 0, drop - 1);
            m_revealedRows -= drop;
            setWindowSkip(m_windowSkip + drop);
            endRemoveRows();
        } else {
            setWindowSkip(skip);
        }
    } else if (skip < m_windowSkip) {
        const int add = m_windowSkip - skip;
        beginInsertRows({}, 0, add - 1);
        m_revealedRows += add;
        setWindowSkip(skip);
        endInsertRows();
    }

    // Pacing must not undo the window: cap it at what is now exposed, unless
    // the window reaches the live edge with everything out (uncapped).
    m_windowCap = (m_windowSkip == 0 && m_revealedRows >= total)
                      ? 0 : rowCount();
    scheduleReveal();
    // No emit here: setWindowSkip() already notified if the skip moved, and the
    // exposed count is announced by rowsInserted/rowsRemoved.
}

bool ReverseListProxyModel::extendWindowAtOldEnd(int extraRows)
{
    if (extraRows <= 0 || !sourceModel())
        return false;
    // Only the window cap counts. m_windowCap == 0 means uncapped; anything
    // unexposed is pacing backlog that releases itself.
    if (m_windowCap <= 0)
        return false;
    const int available = std::max(0, sourceRowTotal() - m_windowSkip);
    if (m_windowCap >= available)
        return false;   // the cap is not the binding constraint
    if (m_revealedRows < m_windowCap)
        return false;   // pacing has not even reached the cap yet
    const int wanted = std::min(available, m_windowCap + extraRows);
    m_windowCap = wanted >= available ? 0 : wanted;
    scheduleReveal();
    return true;
}

bool ReverseListProxyModel::extendWindowAtNewEnd(int extraRows)
{
    if (extraRows <= 0 || !sourceModel())
        return false;
    // Nothing to give: the window already includes the live edge. The caller
    // needs that answer, since it tells the pane the view's bottom is the real
    // bottom.
    if (m_windowSkip <= 0)
        return false;

    // One insert at the head, never a reset or a mid-list renumbering: the pane
    // compensates contentY by these rows' summed height.
    const int add = std::min(extraRows, m_windowSkip);
    beginInsertRows({}, 0, add - 1);
    m_revealedRows += add;
    setWindowSkip(m_windowSkip - add);
    endInsertRows();

    // Grow the cap by exactly what was restored rather than assigning the
    // exposed count, which could strand rows pacing still owes the old end.
    if (m_windowCap > 0)
        m_windowCap += add;
    // Window reaches the live edge with everything exposed: no window.
    if (m_windowSkip == 0 && m_revealedRows >= sourceRowTotal())
        m_windowCap = 0;

    scheduleReveal();
    return true;   // setWindowSkip() above emitted windowChanged()
}

void ReverseListProxyModel::clearWindow()
{
    m_windowCap = 0;
    if (m_windowSkip != 0) {
        const int add = m_windowSkip;
        beginInsertRows({}, 0, add - 1);
        m_revealedRows += add;
        setWindowSkip(0);
        endInsertRows();
    }
    releaseAll();
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
    // The released slice, clamped against a source that shrank without a usable
    // signal and a skip that outran what is left.
    return std::clamp(m_revealedRows, 0,
                      std::max(0, sourceModel()->rowCount() - m_windowSkip));
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
