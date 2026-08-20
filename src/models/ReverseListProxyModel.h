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

    // Release the whole paced backlog now, in one batch.
    //
    // Pacing is a delivery schedule for rows the reader has not reached yet,
    // and it is only ever correct while nothing needs to address them. Jumping
    // to a specific event — a reply target, a search hit, a permalink — does
    // need to: the row exists in the source model, and a view that has not
    // been handed it yet would resolve the jump to "no such row" and silently
    // do nothing. Callers on those paths release first.
    //
    // Deliberately synchronous and unpaced: the reader has asked to go
    // somewhere specific and a brief hitch is the honest cost of arriving,
    // where a silent no-op is not.
    Q_INVOKABLE void releaseAll();

    // ── 2026-08-19: the sliding window ──────────────────────────────────
    //
    // Why this exists: the view instantiates every row it is given, and
    // per-frame cost was MEASURED to scale with the total instantiated item
    // count — scene-graph node sync and event delivery both walk the whole
    // tree, while layout is only ~1% of a frame. At 1000 loaded rows that is
    // ~10.4 ms per wheel notch against ~4.4 ms at 600, so reading deep in a
    // long room degrades badly. Pacing alone cannot help: it only delays
    // rows, it never takes any back.
    //
    // The window is TWO integers, and every transition between two windows
    // is a single insert-or-remove at one end:
    //   * `windowSkip` — how many of the NEWEST source rows are excluded.
    //     Proxy row 0 is the (windowSkip)th newest, not necessarily the
    //     newest. Zero means "the window includes the live edge", which is
    //     the only state in which the physical bottom of the view is the
    //     newest message — so the pane must return to zero before the
    //     reader can reach the bottom.
    //   * the exposed count (`m_revealedRows`), unchanged in meaning.
    //
    // Releasing at the OLDEST end is free: those rows are at the far end of
    // the rotated view, and removing children from the tail of a Column
    // moves nothing the reader can see. Releasing at the NEWEST end shifts
    // every kept row, so the pane corrects contentY by the exact height
    // delta — never an estimate, and only when the reader is settled.
    Q_PROPERTY(int windowSkip READ windowSkip NOTIFY windowChanged)
    int windowSkip() const { return m_windowSkip; }
    // The oldest source row currently exposed (-1 when nothing is).
    Q_INVOKABLE int oldestExposedSourceRow() const;
    // Move to the window (skipNewest, rows). Clamped to what the source
    // holds; performs at most one structural op per end, oldest end first.
    Q_INVOKABLE void setWindow(int skipNewest, int rows);
    // Back to "everything, live edge included" — what every jump/search
    // path needs before it can address an arbitrary row.
    Q_INVOKABLE void clearWindow();

    // Re-expose up to `extraRows` more of the OLDEST source rows the window
    // is holding back, through the PACED reveal rather than a synchronous
    // insert. Rows older than the window's oldest exposed row are already in
    // the source model, so this is a local operation: the pane calls it when
    // the reader reaches the window's old edge, instead of asking the
    // homeserver for history it already has. Releasing at the oldest end
    // appends to the tail of the rotated view, beyond the reader, so nothing
    // the reader is looking at moves.
    //
    // Returns true only when the WINDOW's cap was what withheld rows and has
    // now been raised. False means the caller should fall back to its normal
    // behaviour (for the pane: ask the server). Distinguishing this from the
    // PACING backlog matters: pacing also leaves rows unexposed, but it is
    // already releasing them on its own timer, and treating that as "the
    // window is withholding" swallowed the near-top request on every
    // timeline whose initial reveal was still in flight.
    Q_INVOKABLE bool extendWindowAtOldEnd(int extraRows);

    // The symmetric half: give back up to `extraRows` of the NEWEST source
    // rows the window is holding back, synchronously, as ONE insert at the
    // head.
    //
    // Why this is needed at all: under a window `wheelMinY()` is the window's
    // SYNTHETIC newest edge, so a sustained downward gesture longer than the
    // pane's runway reaches it and the motion settles — the reader stops
    // before the newest message while those rows sit loaded in the source
    // model, hidden by nothing but the skip.
    //
    // Why NOT paced like the old end: these are rows the reader is actively
    // moving toward, and handing them over a few per tick would stall the
    // gesture exactly the same way. They also have to exist synchronously,
    // because a head insert shifts every kept row and the pane corrects
    // contentY by their exact summed MEASURED height — the same arithmetic
    // setWindow()'s newest-end release already performs, in reverse.
    //
    // Returns false when the window is already at the live edge (skip == 0):
    // the caller must be able to tell "extended" from "already live", because
    // "already live" is the one state in which the physical bottom of the
    // view really is the newest message.
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
    // THE only writer of m_windowSkip, and the only emitter of
    // windowChanged(). Every mutation goes through here — see the comment on
    // the definition for what happened when five of them did not.
    void setWindowSkip(int skip);
    void disconnectSource();
    // Rows in the source model, regardless of how many are released yet.
    int sourceRowTotal() const;
    void scheduleReveal();
    // Rows pacing may expose: the window cap, or everything when uncapped.
    int revealTarget() const;
    void revealNextChunk();

    QVector<QMetaObject::Connection> m_sourceConnections;

    // How many of the newest source rows are currently exposed. Always
    // <= sourceRowTotal(); the difference is the paced backlog of oldest rows.
    int m_revealedRows = 0;
    // How many of the NEWEST source rows the window excludes. 0 = the
    // window reaches the live edge (the only state where proxy row 0 is the
    // newest message). Never assign this directly — setWindowSkip().
    int m_windowSkip = 0;
    // Guards the reveal timer from undoing a deliberate window: pacing may
    // only ever grow the window toward the OLDEST end, never past a cap the
    // pane set, and never back over rows the pane released at the newest end.
    int m_windowCap = 0;   // 0 = uncapped
    // Set between an announced beginInsertRows/beginRemoveRows pair so the
    // matching source signal knows whether it opened one.
    bool m_insertAnnounced = false;
    int m_announcedInsertCount = 0;
    bool m_removeAnnounced = false;
    int m_announcedRemoveCount = 0;
    QTimer m_revealTimer;
};
