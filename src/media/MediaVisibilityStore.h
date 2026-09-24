#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Which timeline images the user has hidden, Element-style.
//
// Purely local rendering state: hiding sends, edits and redacts nothing.
// There is no Matrix standard for it, so nothing is written to account data.
// It is persisted locally and strictly per account (no global fallback), and
// Settings -> Privacy & security shows the hidden count with a Show-all, so a
// forgotten hidden image can always be found again.
//
// Lives here rather than in a delegate, which is destroyed and recreated while
// scrolling. Keyed by `mediaKey` (the event id for remote events on the Rust
// backend).
class SettingsManager;

class MediaVisibilityStore : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenCountChanged)

public:
    explicit MediaVisibilityStore(QObject *parent = nullptr);

    Q_INVOKABLE bool isHidden(const QString &key) const;
    Q_INVOKABLE void hide(const QString &key);
    Q_INVOKABLE void show(const QString &key);
    Q_INVOKABLE void setHidden(const QString &key, bool hidden);
    /// Wiped on sign-out and account switch.
    Q_INVOKABLE void clear();

    /// Attaches persistence and loads this account's list. Without settings the
    /// store is session-only (test fixtures).
    void setSettings(SettingsManager *settings);
    /// Drops the in-memory set without writing (sign-out). clear() would
    /// persist the empty set and lose the account's saved list.
    void resetForSession();
    /// Re-reads for the active account on sign-in and account switch.
    void reloadForAccount();

    int hiddenCount() const { return int(m_hidden.size()); }

    /// Bounded: at the cap the oldest hidden image is revealed rather than
    /// refusing a new hide.
    static constexpr int kMaxHidden = 4096;

Q_SIGNALS:
    /// Rows re-query on this; `isHidden` is a call with no per-key NOTIFY.
    void hiddenChanged(const QString &key, bool hidden);
    void hiddenCountChanged();

private:
    void persist();

    SettingsManager *m_settings = nullptr;
    QSet<QString> m_hidden;
    /// Insertion order for cap eviction.
    QStringList m_order;
};
