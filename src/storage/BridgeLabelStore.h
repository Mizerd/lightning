#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

class BridgeLabelStore
{
public:
    struct Entry {
        QString networkId;
        QString label;
        qint64 learnedAt = 0;

        bool isPositive() const { return !label.isEmpty(); }
    };

    static constexpr qint64 kPositiveLifetimeSecs = 90LL * 24 * 60 * 60;
    static constexpr qint64 kNegativeLifetimeSecs = 14LL * 24 * 60 * 60;

    static constexpr int kMaxEntries = 4000;

    BridgeLabelStore() = default;

    bool openFor(const QString &filePath);
    void close();
    bool isOpen() const { return !m_filePath.isEmpty(); }
    QString filePath() const { return m_filePath; }

    std::optional<Entry> label(const QString &roomId) const;

    bool knows(const QString &roomId) const;

    void remember(const QString &roomId, const QString &networkId,
                  const QString &label);

    void rememberDeferred(const QString &roomId, const QString &networkId,
                          const QString &label);
    bool isDirty() const { return m_dirty; }
    // Writes the deferred answers, if any. close() drops them instead: a
    // closing store may be the one a sign-out is about to delete.
    bool flush();
    void flushAsync();

    QHash<QString, Entry> positiveLabels() const;

    int count() const { return m_entries.size(); }

    static bool removeStore(const QString &filePath);

    static bool entryIsFresh(const Entry &entry, qint64 nowSecs);

private:
    void load();
    bool save() const;
    QByteArray serialize() const;
    void pruneToCap();

    QString m_filePath;
    QHash<QString, Entry> m_entries;
    bool m_dirty = false;
};
