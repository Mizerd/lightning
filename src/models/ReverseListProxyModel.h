#pragma once

#include <QAbstractProxyModel>
#include <QMetaObject>
#include <QTimer>
#include <QVector>

// Flat-list proxy that exposes source rows in reverse order. Source prepends
// become proxy appends, so with TimelinePane's rotated viewport loading older
// history extends the far edge instead of inserting before visible delegates.
//
// It also paces how fast paginated history reaches the view: TimelinePane
// instantiates every row (heights are measured, not estimated), and handing a
// whole page over in one event-loop turn stalls painting. The oldest rows are
// held back and released a few per tick. This is a delivery schedule, never a
// filter; rows arriving at the newest end (live messages) appear immediately.
class ReverseListProxyModel final : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit ReverseListProxyModel(QObject *parent = nullptr);

    // Release the whole paced backlog now. Jumping to a specific event (reply
    // target, search hit, permalink) must be able to address rows the view has
    // not been handed yet, so those paths release first. Synchronous by design:
    // a brief hitch beats a silent no-op.
    Q_INVOKABLE void releaseAll();

    // ── Sliding window ──────────────────────────────────────────────────
    //
    // Per-frame cost scales with the number of instantiated rows, so a long
    // room read deep gets slow; pacing only delays rows and never takes any
    // back.
    //
    // The window is two integers, and every transition is a single insert or
    // remove at one end:
    //   * `windowSkip`: how many of the newest source rows are excluded. Zero
    //     means the window includes the live edge, the only state in which the
    //     view's physical bottom is the newest message.
    //   * the exposed count (`m_revealedRows`).
    //
    // Releasing at the oldest end moves nothing visible (tail of the rotated
    // Column). Releasing at the newest end shifts every kept row, so the pane
    // corrects contentY by the exact height delta, only while the reader is
    // settled.
    Q_PROPERTY(int windowSkip READ windowSkip NOTIFY windowChanged)
    int windowSkip() const { return m_windowSkip; }
    // The oldest source row currently exposed (-1 when nothing is).
    /// True when the paced reveal has nothing left to hand over: the same
    /// condition that stops the reveal timer, so callers (tests) can wait on
    /// the producer rather than on an adaptive-interval poll. Not bindable (no
    /// change signal); call it, do not bind to it. Defined in the .cpp on
    /// purpose: an inline body would call a private out-of-line member and add
    /// a link dependency to every includer.
    Q_INVOKABLE bool revealIdle() const;
    Q_INVOKABLE int oldestExposedSourceRow() const;
    // Move to the window (skipNewest, rows), clamped to the source. At most one
    // structural op per end, oldest end first.
    Q_INVOKABLE void setWindow(int skipNewest, int rows);
    // Back to everything including the live edge; needed before any jump/search
    // can address an arbitrary row.
    Q_INVOKABLE void clearWindow();

    // Re-expose up to `extraRows` more of the oldest rows the window holds
    // back, through the paced reveal. They are already in the source model, so
    // the pane calls this at the window's old edge instead of asking the
    // server; nothing visible moves.
    //
    // Returns true only when the window cap was withholding rows and has been
    // raised. False means fall back (the pane asks the server). Rows still in
    // the pacing backlog do not count: they release on their own timer.
    Q_INVOKABLE bool extendWindowAtOldEnd(int extraRows);

    // Give back up to `extraRows` of the newest rows the window holds back,
    // synchronously, as one head insert. Under a window wheelMinY() is a
    // synthetic newest edge, so a long downward gesture would otherwise stop
    // short of loaded messages. Not paced: the reader is moving toward these
    // rows, and the pane must measure them at once to correct contentY.
    //
    // Returns false when the window is already at the live edge (skip == 0), so
    // the caller can tell "extended" from "already live".
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
    // The only writer of m_windowSkip and emitter of windowChanged(); see the
    // definition.
    void setWindowSkip(int skip);
    void disconnectSource();
    // Rows in the source model, regardless of how many are released yet.
    int sourceRowTotal() const;
    void scheduleReveal();
    // Rows pacing may expose: the window cap, or everything when uncapped.
    int revealTarget() const;
    void revealNextChunk();

    QVector<QMetaObject::Connection> m_sourceConnections;

    // How many of the newest source rows are exposed; <= sourceRowTotal(). The
    // difference is the paced backlog of oldest rows.
    int m_revealedRows = 0;
    // How many of the newest source rows the window excludes; 0 means it
    // reaches the live edge. Only written through setWindowSkip().
    int m_windowSkip = 0;
    // Keeps the reveal timer from undoing a window: pacing grows the window
    // only toward the oldest end, never past the pane's cap and never back over
    // rows the pane released at the newest end.
    int m_windowCap = 0;   // 0 = uncapped
    // Set between an announced beginInsertRows/beginRemoveRows pair so the
    // matching source signal knows whether it opened one.
    bool m_insertAnnounced = false;
    int m_announcedInsertCount = 0;
    bool m_removeAnnounced = false;
    int m_announcedRemoveCount = 0;
    QTimer m_revealTimer;
};
