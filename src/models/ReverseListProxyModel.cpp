#include "models/ReverseListProxyModel.h"

#include "app/GuiStallTracer.h"

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

// How many rows pacing is allowed to expose. Without the cap the reveal timer
// would immediately undo a window the pane just set — pacing is a delivery
// schedule for rows the reader has not reached, and the window is a statement
// about which rows those are.
int ReverseListProxyModel::revealTarget() const
{
    const int available = std::max(0, sourceRowTotal() - m_windowSkip);
    return m_windowCap > 0 ? std::min(m_windowCap, available) : available;
}

// THE single writer of m_windowSkip, and the single emitter of
// windowChanged().
//
// Why a funnel rather than an emit beside each assignment: the skip was being
// written in eight places and five of them never notified at all — the
// modelReset lambda, setSourceModel, a live insert landing newer than the
// window, a removal newer than the window, and the rowsRemoved clamp. QML's
// `rowWindowSkip` is a NOTIFY-gated binding whose only other dependency is a
// constant, so after a room switch or a jump-to-live trim it kept the previous
// room's skip indefinitely, and atBottomEdge() — which refuses while the skip
// is non-zero, correctly, because a windowed view's physical bottom is not the
// newest message — then reported false at the TRUE live edge. The jump pill
// stayed up and follow-latest never re-engaged. clearWindow() could not repair
// it either: it guards its work on `m_windowSkip != 0`, which is already false
// in that state. A ninth write site will exist one day; funnelling is what
// makes that safe.
//
// Emitting only on a real change matters as much as emitting at all: this runs
// from inside source-signal handlers, and a notify on every removal that left
// the skip alone would re-run the pane's binding for nothing.
//
// Call sites finish updating m_revealedRows BEFORE calling this where both
// move together, so rowCount() is already coherent when the notify runs. The
// two reset paths deliberately notify from inside their
// beginResetModel/endResetModel bracket: the skip has to be correct before
// endResetModel() or the view rebuilds against the outgoing room's window.
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
    // A jump needs to address ANY row, so this lifts the cap as well as the
    // backlog — the window is re-established by the pane once the reader
    // settles again.
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

    // The backlog is always the OLDEST source rows, which map to the proxy's
    // tail — the far/top edge of the rotated view. Releasing them therefore
    // appends, and cannot move a row the reader is already looking at.
    //
    // endInsertRows() builds the row synchronously, so elapsed() measures the
    // real construction cost and the budget adapts to whatever this particular
    // history happens to contain.
    // Attributed for stall tracing (2026-08-19): a live capture showed GUI
    // stalls of 333/369/1062 ms categorised "unattributed" while pagination
    // ran, and endInsertRows() below builds a full message delegate
    // synchronously — the single largest candidate. No-op unless
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
        // Bound on revealTarget(), NOT sourceRowTotal(). The guard at
        // the top of this function stops the timer from STARTING past the
        // cap, but with sourceRowTotal() here a single tick kept releasing
        // straight through it — which is exactly the "pacing undoes the
        // window" failure m_windowCap exists to prevent, reachable in every
        // trimmed window (cap < available).
    } while (m_revealedRows < revealTarget()
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
                // A row landing NEWER than the window's newest edge is
                // outside it: the window must keep covering the same source
                // rows, so absorb it by growing the skip instead of showing
                // it. This is the live-message case while the reader is deep
                // in history — without it the whole window would slide one
                // row older on every incoming message.
                if (m_windowSkip > 0 && first > totalAfter - 1 - m_windowSkip) {
                    setWindowSkip(m_windowSkip + inserted);
                    return;
                }
                // Rows landing entirely inside the not-yet-released oldest
                // region change nothing the view can see. Stay silent and let
                // the reveal timer pace them out; this is the backward
                // pagination case, and it is why loading no longer blocks.
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
                // Rows removed NEWER than the window shrink the skip, not the
                // exposed slice: the window keeps covering the same rows.
                if (proxyLast < 0) {
                    setWindowSkip(m_windowSkip
                                  - std::min(m_windowSkip, last - first + 1));
                    return;
                }
                // Entirely inside the unreleased backlog: the view never saw
                // these rows, so there is nothing to remove from it.
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
                // released count above what the source still holds.
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
                // The window goes with it — a fresh snapshot has no reader
                // position to be windowed around, and leaving a stale skip
                // would hide the live edge of the new room.
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

// Both directions key off the SOURCE total, not the released count: proxy row
// 0 is the newest source row the WINDOW includes, and the unreleased backlog
// is the tail. Using rowCount() here would silently renumber every visible
// row whenever a page arrived. `m_windowSkip` shifts the newest edge: it is 0
// in every state where the reader can reach the bottom of the view.
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

// One structural op per end, oldest end first so proxy indices stay valid
// through the transition. Each op is a plain insert or remove at ONE end —
// there is no path here that renumbers rows in the middle.
void ReverseListProxyModel::setWindow(int skipNewest, int rows)
{
    if (!sourceModel())
        return;
    const int total = sourceRowTotal();
    const int skip = std::clamp(skipNewest, 0, std::max(0, total));
    const int wanted = std::clamp(rows, 0, std::max(0, total - skip));

    // (1) The OLDEST end, at the current skip. Free of any reader-visible
    //     movement: this is the tail of the Column.
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

    // (2) The NEWEST end. Raising the skip removes rows from the HEAD, which
    //     shifts every kept row — the pane compensates contentY by the exact
    //     height delta. Lowering it inserts at the head.
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

    // Pacing must not undo the window: cap it at what we now expose, unless
    // the window reaches the live edge and everything is out (uncapped).
    m_windowCap = (m_windowSkip == 0 && m_revealedRows >= total)
                      ? 0 : rowCount();
    scheduleReveal();
    // No emit here: setWindowSkip() above has already notified if the skip
    // moved, and a second unconditional emit would fire on every settle-time
    // window that only trimmed the OLD end — where the property this signal
    // notifies is unchanged and the exposed count is already announced by the
    // model's own rowsInserted/rowsRemoved.
}

bool ReverseListProxyModel::extendWindowAtOldEnd(int extraRows)
{
    if (extraRows <= 0 || !sourceModel())
        return false;
    // ONLY the window's cap counts here. m_windowCap == 0 means uncapped, so
    // whatever is unexposed is the pacing backlog, which releases itself.
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
    // Nothing to give: the window already includes the live edge, and the
    // caller needs that answer rather than a silent no-op insert — it is what
    // tells the pane the bottom of the view is now honestly the bottom.
    if (m_windowSkip <= 0)
        return false;

    // ONE insert at the head, never a reset and never a mid-list renumbering:
    // the pane compensates contentY by the exact summed height of these rows,
    // and a reset would destroy both the measurement and every delegate the
    // window exists to avoid rebuilding.
    const int add = std::min(extraRows, m_windowSkip);
    beginInsertRows({}, 0, add - 1);
    m_revealedRows += add;
    setWindowSkip(m_windowSkip - add);
    endInsertRows();

    // Pacing must still not undo the window — setWindow()'s rule, expressed
    // as a delta instead of as `rowCount()`. Assigning the exposed count here
    // would silently retire whatever the OLD end still owes: pacing may not
    // have reached the cap setWindow established, and lowering the cap to
    // whatever happens to be out at this instant would strand those rows for
    // good. Growing it by exactly what was just restored keeps both ends'
    // promises intact.
    if (m_windowCap > 0)
        m_windowCap += add;
    // The one uncapped state is unchanged: the window reaches the live edge
    // with everything exposed, which is simply "no window".
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
    // The released slice, not the source total. The clamp guards against a
    // source that shrank without a signal we could act on, and against a
    // skip that outran what is left.
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
