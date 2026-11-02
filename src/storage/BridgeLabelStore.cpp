#include "storage/BridgeLabelStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThreadPool>

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

QMutex &fileMutex()
{
    static QMutex m;
    return m;
}
// Per file, so one account's removal never cancels or reorders another's
// write. `epoch` is bumped by removeStore(): a write queued before a delete
// must not bring the file back. `written` is the newest serialization written,
// so a slow older one never overwrites it. Guarded by fileMutex().
struct PathState
{
    quint64 epoch = 0;
    quint64 written = 0;
};
QHash<QString, PathState> &pathStates()
{
    static QHash<QString, PathState> states;
    return states;
}
quint64 g_seq = 0;

bool writeFile(const QString &path, const QByteArray &bytes, quint64 seq,
               const quint64 *epoch)
{
    QMutexLocker lock(&fileMutex());
    PathState &state = pathStates()[path];
    if (epoch && *epoch != state.epoch)
        return false;
    if (seq < state.written)
        return true;
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Atomic, so a crash cannot leave a truncated file that load() discards
    // whole.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(bytes);
    if (!out.commit())
        return false;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    state.written = seq;
    return true;
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
    m_dirty = false;
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

QByteArray BridgeLabelStore::serialize() const
{
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
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool BridgeLabelStore::save() const
{
    if (m_filePath.isEmpty())
        return false;
    quint64 seq;
    {
        QMutexLocker lock(&fileMutex());
        seq = ++g_seq;
    }
    return writeFile(m_filePath, serialize(), seq, nullptr);
}

void BridgeLabelStore::flushAsync()
{
    if (!m_dirty || !isOpen())
        return;
    const QString path = m_filePath;
    const QByteArray bytes = serialize();
    quint64 seq, epoch;
    {
        QMutexLocker lock(&fileMutex());
        seq = ++g_seq;
        epoch = pathStates()[path].epoch;
    }
    m_dirty = false;
    QThreadPool::globalInstance()->start(
        [path, bytes, seq, epoch] { writeFile(path, bytes, seq, &epoch); });
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
    m_dirty = !save();
}

void BridgeLabelStore::rememberDeferred(const QString &roomId,
                                        const QString &networkId,
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
    m_dirty = true;
}

bool BridgeLabelStore::flush()
{
    if (!m_dirty || !isOpen())
        return true;
    const bool ok = save();
    // A failed write stays dirty so the next flush retries it.
    m_dirty = !ok;
    return ok;
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
    // Waits for a write in flight and cancels the ones still queued.
    QMutexLocker lock(&fileMutex());
    ++pathStates()[filePath].epoch;
    if (!QFile::exists(filePath))
        return true;
    return QFile::remove(filePath);
}
