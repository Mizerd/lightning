#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

// Remembers which network each room is bridged to, across restarts.
//
// The MSC2346 state (`m.bridge` / `uk.half-shot.bridge`) is not in sliding
// sync's required_state and matrix-sdk 0.18's get_state_events is store-only,
// so each answer costs a raw `/state` read (rust/src/bridges.rs,
// src/matrix/BridgeNetwork.h). Without persistence every launch started with
// no badges until each room was reopened. This only records answers;
// AppController still decides what to ask and when.
//
// Stored per room: the room id, sanitized network id, label and when it was
// learned. No user ids, content, names or aliases. Room ids are already in the
// SDK store, so this adds no new class of data. The file lives under the
// account root and is deleted with the account (bridgeLabelsFile).
//
// A negative answer is stored too; it is the common case and what keeps a
// sweep bounded. It gets a shorter lifetime, since a room that gains a bridge
// should not stay unlabelled for months.
//
// Rows expire by wall-clock age. `knows()` means a fresh row exists (positive
// or negative); `label()` answers only for a fresh positive row. Expired rows
// are ignored until overwritten or pruned.
class BridgeLabelStore
{
public:
    struct Entry {
        QString networkId;
        QString label;
        // Unix seconds.
        qint64 learnedAt = 0;

        bool isPositive() const { return !label.isEmpty(); }
    };

    // Lifetimes are judgements: bridges are long-lived, and being wrong costs
    // one stale chip until the next read.
    static constexpr qint64 kPositiveLifetimeSecs = 90LL * 24 * 60 * 60;
    static constexpr qint64 kNegativeLifetimeSecs = 14LL * 24 * 60 * 60;

    // Hard bound on file size; the oldest rows are pruned first.
    static constexpr int kMaxEntries = 4000;

    BridgeLabelStore() = default;

    // The caller supplies the path (bridgeLabelsFile). An empty path leaves the
    // store closed, which is a valid state.
    bool openFor(const QString &filePath);
    void close();
    bool isOpen() const { return !m_filePath.isEmpty(); }
    QString filePath() const { return m_filePath; }

    // The remembered label, when there is a fresh positive row.
    std::optional<Entry> label(const QString &roomId) const;

    // Whether a fresh row exists at all: "do I still need to ask".
    bool knows(const QString &roomId) const;

    // Records an answer; an empty `label` is the negative answer. Writes
    // through; on a failed write the in-memory answer stays for this session.
    void remember(const QString &roomId, const QString &networkId,
                  const QString &label);

    // Every fresh POSITIVE row, for seeding the room list at startup.
    QHash<QString, Entry> positiveLabels() const;

    // Total rows, fresh or not.
    int count() const { return m_entries.size(); }

    // Deletes the file. True means it is gone, including "was never there"; the
    // caller distinguishes those.
    static bool removeStore(const QString &filePath);

    // Pure, so the lifetime rule is testable.
    static bool entryIsFresh(const Entry &entry, qint64 nowSecs);

private:
    void load();
    bool save() const;
    void pruneToCap();

    QString m_filePath;
    QHash<QString, Entry> m_entries;
};
