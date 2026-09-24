#include "storage/BridgeLabelStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>

#include <algorithm>
#include <limits>
#include <vector>

namespace {

// Short keys: the file is read on the GUI thread before the room list paints.
constexpr auto kKeyNetwork = "n";
constexpr auto kKeyLabel = "l";
constexpr auto kKeyLearnedAt = "t";

qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

} // namespace

bool BridgeLabelStore::entryIsFresh(const Entry &entry, qint64 nowSecs)
{
    if (entry.learnedAt <= 0)
        return false;
    // A future timestamp means the clock moved backwards; treat the row as
    // expired.
    if (entry.learnedAt > nowSecs)
        return false;
    const qint64 lifetime = entry.isPositive() ? kPositiveLifetimeSecs
                                               : kNegativeLifetimeSecs;
    return nowSecs - entry.learnedAt < lifetime;
}

bool BridgeLabelStore::openFor(const QString &filePath)
{
    close();
    if (filePath.isEmpty())
        return false;
    m_filePath = filePath;
    load();
    return true;
}

void BridgeLabelStore::close()
{
    m_filePath.clear();
    m_entries.clear();
}

void BridgeLabelStore::load()
{
    m_entries.clear();
    QFile file(m_filePath);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly))
        return;
    // Capped: read on the GUI thread at login, and anything larger than
    // kMaxEntries rows could produce is corruption or hostility.
    constexpr qint64 kMaxFileBytes = 4 * 1024 * 1024;
    if (file.size() > kMaxFileBytes)
        return;
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    // A corrupt file is discarded, not repaired; everything is re-derivable.
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return;

    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject row = it.value().toObject();
        Entry entry;
        entry.networkId = row.value(QLatin1String(kKeyNetwork)).toString();
        entry.label = row.value(QLatin1String(kKeyLabel)).toString();
        entry.learnedAt =
            static_cast<qint64>(row.value(QLatin1String(kKeyLearnedAt)).toDouble());
        if (it.key().isEmpty() || entry.learnedAt <= 0)
            continue;
        m_entries.insert(it.key(), entry);
    }
    pruneToCap();
}

bool BridgeLabelStore::save() const
{
    if (m_filePath.isEmpty())
        return false;
    const QFileInfo info(m_filePath);
    QDir().mkpath(info.absolutePath());

    QJsonObject root;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        QJsonObject row;
        if (!it.value().networkId.isEmpty())
            row.insert(QLatin1String(kKeyNetwork), it.value().networkId);
        if (!it.value().label.isEmpty())
            row.insert(QLatin1String(kKeyLabel), it.value().label);
        row.insert(QLatin1String(kKeyLearnedAt),
                   static_cast<double>(it.value().learnedAt));
        root.insert(it.key(), row);
    }

    // Atomic, so a crash cannot leave a truncated file that load() discards
    // whole.
    QSaveFile out(m_filePath);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!out.commit())
        return false;
    QFile::setPermissions(m_filePath,
                          QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

void BridgeLabelStore::pruneToCap()
{
    if (m_entries.size() <= kMaxEntries)
        return;
    std::vector<std::pair<qint64, QString>> byAge;
    byAge.reserve(static_cast<size_t>(m_entries.size()));
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        byAge.emplace_back(it.value().learnedAt, it.key());
    // Rows stamped in the future are evicted first. entryIsFresh() never
    // believes them, so sort them to the front; clamping the key to `now` would
    // still keep them ahead of legitimate rows.
    const qint64 now = nowSecs();
    const auto sortKey = [now](qint64 stamp) {
        return stamp > now ? std::numeric_limits<qint64>::min() : stamp;
    };
    std::sort(byAge.begin(), byAge.end(),
              [&sortKey](const auto &a, const auto &b) {
                  return sortKey(a.first) < sortKey(b.first);
              });
    const int excess = m_entries.size() - kMaxEntries;
    for (int i = 0; i < excess; ++i)
        m_entries.remove(byAge[static_cast<size_t>(i)].second);
}

std::optional<BridgeLabelStore::Entry>
BridgeLabelStore::label(const QString &roomId) const
{
    const auto it = m_entries.constFind(roomId);
    if (it == m_entries.constEnd())
        return std::nullopt;
    if (!it.value().isPositive())
        return std::nullopt;
    if (!entryIsFresh(it.value(), nowSecs()))
        return std::nullopt;
    return it.value();
}

bool BridgeLabelStore::knows(const QString &roomId) const
{
    const auto it = m_entries.constFind(roomId);
    if (it == m_entries.constEnd())
        return false;
    return entryIsFresh(it.value(), nowSecs());
}

void BridgeLabelStore::remember(const QString &roomId, const QString &networkId,
                                const QString &label)
{
    if (!isOpen() || roomId.isEmpty())
        return;
    Entry entry;
    entry.networkId = networkId;
    entry.label = label;
    entry.learnedAt = nowSecs();
    m_entries.insert(roomId, entry);
    pruneToCap();
    save();
}

QHash<QString, BridgeLabelStore::Entry> BridgeLabelStore::positiveLabels() const
{
    const qint64 now = nowSecs();
    QHash<QString, Entry> out;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!it.value().isPositive())
            continue;
        if (!entryIsFresh(it.value(), now))
            continue;
        out.insert(it.key(), it.value());
    }
    return out;
}

bool BridgeLabelStore::removeStore(const QString &filePath)
{
    if (filePath.isEmpty())
        return false;
    if (!QFile::exists(filePath))
        return true;
    return QFile::remove(filePath);
}
