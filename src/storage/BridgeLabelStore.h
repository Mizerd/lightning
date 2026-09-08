#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

// Remembering which network a room is bridged to, across restarts.
//
// # Why this exists
//
// The MSC2346 answer is expensive to obtain and cheap to keep. `m.bridge` /
// `uk.half-shot.bridge` is not in sliding sync's `required_state` and
// `Room::get_state_events` is store-only in matrix-sdk 0.18, so the SDK store
// answers empty for every room and the real answer costs a raw `/state` read
// (rust/src/bridges.rs, and the reasoning in src/matrix/BridgeNetwork.h).
// AppController therefore asked once per room per SESSION, and only for a room
// the user actually opened.
//
// Reported by a tester on 2026-09-08: "when I open lightning, the tags are
// shown how they used to show, missing on some chats, only when I click on
// chats does the correct tag show up, and it doesn't persist between
// restarting the application." Both halves of that are this: nothing was ever
// written down, so every launch started from nothing and every room had to be
// visited again.
//
// This class is the writing down. It is deliberately NOT a cache in front of
// the request — AppController still decides what to ask and when; this only
// remembers what came back, so the next launch can paint the badge before any
// request exists.
//
// # What is stored, and why that is safe
//
// One row per room: the room id, the sanitised network id, the label, and
// when it was learned. Nothing else. No user ids (rust/src/bridges.rs
// deliberately never forwards `bridgebot` or `creator`), no message content,
// no room name, no alias. Room ids already live on disk in the SDK store for
// every joined room, so this introduces no new class of data; it is written
// under the account's own root and deleted with the account, exactly like
// `starredGifsDir` (see matrix::app_data::bridgeLabelsFile).
//
// # A NEGATIVE ANSWER IS A RESULT AND IS STORED
//
// "This room advertises no bridge" is the common case and it is the whole
// reason a sweep can be bounded: without recording it, every launch re-asks
// every room it has already answered for, which is the expensive half of the
// defect rather than a missing feature. It is stored with a SHORTER lifetime
// than a positive one, because a room that gains a bridge must not stay
// unlabelled for a quarter of a year, whereas a room that has one rarely
// stops.
//
// # Freshness
//
// Every row carries the wall-clock second it was learned and is ignored once
// past its lifetime, so a stale answer decays instead of being believed
// forever. `knows()` answers "there is a FRESH row, positive or negative" —
// that is the question a sweep asks — and `label()` answers with the row only
// when it is fresh AND positive. An expired row is not deleted on read: it is
// simply not answered with, and is overwritten by the next real answer or
// dropped by the next prune.
class BridgeLabelStore
{
public:
    struct Entry {
        QString networkId;
        QString label;
        // Seconds since the Unix epoch, from the system clock.
        qint64 learnedAt = 0;

        bool isPositive() const { return !label.isEmpty(); }
    };

    // A positive answer is believed for this long, a negative one for this
    // long. Both are judgements rather than measurements: a bridge is a
    // long-lived property of a room, and the cost of being wrong is one
    // stale chip until the next read.
    static constexpr qint64 kPositiveLifetimeSecs = 90LL * 24 * 60 * 60;
    static constexpr qint64 kNegativeLifetimeSecs = 14LL * 24 * 60 * 60;

    // A hard bound, so a very large account cannot grow this file without
    // limit. Prunes the oldest rows first — the ones whose freshness is
    // closest to expiring anyway.
    static constexpr int kMaxEntries = 4000;

    BridgeLabelStore() = default;

    // `filePath` is supplied by the caller (AppController, via
    // matrix::app_data::bridgeLabelsFile(userId)). This class never derives,
    // guesses or falls back to a path of its own: an empty path leaves the
    // store closed and inert, which is a working state, not an error.
    bool openFor(const QString &filePath);
    void close();
    bool isOpen() const { return !m_filePath.isEmpty(); }
    QString filePath() const { return m_filePath; }

    // The remembered label for a room, when there is a fresh POSITIVE row.
    std::optional<Entry> label(const QString &roomId) const;

    // Whether a fresh row exists at all, positive or negative. This is the
    // question "do I still need to ask about this room".
    bool knows(const QString &roomId) const;

    // Record an answer. An empty `label` records the negative answer, which
    // is a real result and not an absence. Writes through immediately; a
    // failed write leaves the in-memory answer in place, because a badge that
    // is right for this session is better than no badge at all.
    void remember(const QString &roomId, const QString &networkId,
                  const QString &label);

    // Every fresh POSITIVE row, for seeding the room list at startup.
    QHash<QString, Entry> positiveLabels() const;

    // Total rows held, fresh or not. For tests and diagnostics.
    int count() const { return m_entries.size(); }

    // Delete the file. Reported as a tri-state by the caller's own cleanup
    // path; here, true means "the file is gone", which includes "was never
    // there" — the caller distinguishes those, this does not.
    static bool removeStore(const QString &filePath);

    // Pure, so the lifetime rule is testable without a clock or a file.
    static bool entryIsFresh(const Entry &entry, qint64 nowSecs);

private:
    void load();
    bool save() const;
    void pruneToCap();

    QString m_filePath;
    QHash<QString, Entry> m_entries;
};
