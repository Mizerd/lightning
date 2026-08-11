#include "models/EmojiCatalog.h"

#include "app/SettingsManager.h"

#include <QFile>
#include <QLoggingCategory>
#include <QSet>
#include <QTextBoundaryFinder>

Q_LOGGING_CATEGORY(lcEmoji, "matrix.emoji")

namespace {
constexpr int kMaximumSearchResults = 512;
const QStringList kCategories = {
    QStringLiteral("Recently Used"),
    QStringLiteral("Smileys & Emotion"),
    QStringLiteral("People & Body"),
    QStringLiteral("Animals & Nature"),
    QStringLiteral("Food & Drink"),
    QStringLiteral("Travel & Places"),
    QStringLiteral("Activities"),
    QStringLiteral("Objects"),
    QStringLiteral("Symbols"),
    QStringLiteral("Flags"),
};
}

EmojiCatalog::EmojiCatalog(SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent), m_settings(settings)
{
    load();
    rebuild();
}

void EmojiCatalog::load()
{
    QFile file(QStringLiteral(":/qt/qml/MatrixClient/data/emoji-catalog.tsv"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCCritical(lcEmoji) << "local emoji catalogue unavailable" << file.errorString();
        return;
    }
    QSet<QString> duplicates;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine());
        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        // Comments are "# " lines. A bare '#' prefix is NOT a comment: the
        // keycap sequence #️⃣ ('#' U+FE0F U+20E3) is a data row, and the
        // old prefix test silently dropped it from the catalogue.
        if (line.isEmpty() || line.startsWith(QLatin1String("# ")))
            continue;
        const QStringList fields = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        if (fields.size() != 6 || fields[0].isEmpty() || fields[1].isEmpty()
            || !kCategories.contains(fields[3]) || m_byEmoji.contains(fields[0])) {
            if (!fields.isEmpty() && m_byEmoji.contains(fields[0]))
                duplicates.insert(fields[0]);
            continue;
        }
        Entry entry{fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], {}, false};
        entry.searchKey = (entry.name + QLatin1Char(' ') + entry.keywords
                           + QLatin1Char(' ') + entry.category).toCaseFolded();
        entry.searchKey.replace(QLatin1Char('_'), QLatin1Char(' '));
        entry.searchKey.replace(QLatin1Char(':'), QLatin1Char(' '));
        if (entry.searchKey.contains(QLatin1String("technologist")))
            entry.searchKey += QStringLiteral(" developer coder programmer");
        const int index = m_entries.size();
        m_byEmoji.insert(entry.emoji, index);
        m_variants[entry.baseEmoji].append(index);
        m_entries.append(std::move(entry));
    }
    for (auto it = m_variants.cbegin(); it != m_variants.cend(); ++it) {
        if (it.value().size() > 1) {
            for (int index : it.value())
                m_entries[index].hasSkinTones = true;
        }
    }
    // v0.7: per-category index buckets, computed exactly once. Category
    // switches swap the visible list from the bucket instead of rescanning
    // the whole catalogue, so tab changes are a constant-time list swap.
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        if (entry.emoji == entry.baseEmoji)
            m_categoryBuckets[entry.category].append(i);
    }
    // Presentation-selector-tolerant lookup set for emojiOnlySequenceCount,
    // built exactly once: senders disagree about U+FE0F, so a received
    // cluster is matched against the VS16-stripped catalogue form.
    for (const Entry &entry : std::as_const(m_entries)) {
        QString stripped = entry.emoji;
        stripped.remove(QChar(0xFE0F));
        if (!stripped.isEmpty())
            m_sequencesNoVs16.insert(stripped);
    }
    qCInfo(lcEmoji) << "loaded local" << dataVersion() << "catalogue:"
                    << m_entries.size() << "sequences; duplicates ignored:"
                    << duplicates.size();
}

int EmojiCatalog::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visible.size();
}

QVariant EmojiCatalog::data(const QModelIndex &modelIndex, int role) const
{
    if (!modelIndex.isValid() || modelIndex.row() < 0 || modelIndex.row() >= m_visible.size())
        return {};
    const Entry &entry = m_entries[m_visible[modelIndex.row()]];
    switch (role) {
    case EmojiRole: return entry.emoji;
    case NameRole: return entry.name;
    case KeywordsRole: return entry.keywords;
    case CategoryRole: return entry.category;
    case BaseEmojiRole: return entry.baseEmoji;
    case HasSkinTonesRole: return entry.hasSkinTones;
    case ToneVariantRole: return entry.tone;
    case AccessibleLabelRole: return entry.name;
    default: return {};
    }
}

QHash<int, QByteArray> EmojiCatalog::roleNames() const
{
    return {{EmojiRole, "emoji"}, {NameRole, "name"}, {KeywordsRole, "keywords"},
            {CategoryRole, "category"}, {BaseEmojiRole, "baseEmoji"},
            {HasSkinTonesRole, "hasSkinTones"}, {ToneVariantRole, "toneVariant"},
            {AccessibleLabelRole, "accessibleLabel"}};
}

QStringList EmojiCatalog::categories() const { return kCategories; }

void EmojiCatalog::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    Q_EMIT searchTextChanged();
    rebuild();
}

void EmojiCatalog::setCategory(const QString &category)
{
    if (!kCategories.contains(category) || m_category == category)
        return;
    m_category = category;
    Q_EMIT categoryChanged();
    rebuild();
}

void EmojiCatalog::rebuild()
{
    beginResetModel();
    m_visible.clear();
    const QString query = m_searchText.trimmed().toCaseFolded();
    if (query.isEmpty() && m_category == QLatin1String("Recently Used")) {
        if (m_settings) {
            for (const QString &emoji : m_settings->recentEmoji()) {
                const int index = indexOf(emoji);
                if (index >= 0)
                    m_visible.append(index);
            }
        }
    } else if (query.isEmpty()) {
        // v0.7: category switches swap the precomputed bucket built once at
        // load — no per-switch scan over the whole catalogue.
        m_visible = m_categoryBuckets.value(m_category);
    } else {
        for (int i = 0; i < m_entries.size(); ++i) {
            const Entry &entry = m_entries[i];
            // The grid shows one default/base sequence per family; validated
            // variants are exposed by variantsFor().
            if (entry.emoji != entry.baseEmoji)
                continue;
            bool matches = true;
            const QStringList words = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString &word : words) {
                if (!entry.searchKey.contains(word)) { matches = false; break; }
            }
            if (!matches) continue;
            m_visible.append(i);
            if (m_visible.size() >= kMaximumSearchResults)
                break;
        }
    }
    endResetModel();
    Q_EMIT countChanged();
}

int EmojiCatalog::indexOf(const QString &emoji) const
{
    const auto it = m_byEmoji.constFind(emoji);
    return it == m_byEmoji.cend() ? -1 : it.value();
}

bool EmojiCatalog::isKnownEmojiCluster(const QString &cluster) const
{
    if (m_byEmoji.contains(cluster))
        return true;
    QString stripped = cluster;
    stripped.remove(QChar(0xFE0F));
    return !stripped.isEmpty() && m_sequencesNoVs16.contains(stripped);
}

int EmojiCatalog::emojiOnlySequenceCount(const QString &text) const
{
    if (text.isEmpty() || m_entries.isEmpty())
        return 0;
    // Cheap length gate: QTextBoundaryFinder pays O(n) up front for the
    // whole string, and this runs in a per-delegate binding. Three emoji
    // sequences fit well inside this bound even with generous whitespace;
    // a longer body cannot be a 1-3-emoji message worth enlarging.
    if (text.size() > 256)
        return 0;
    int count = 0;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    const int size = text.size();
    int start = 0;
    while (start < size) {
        finder.setPosition(start);
        int end = int(finder.toNextBoundary());
        if (end <= start)
            end = size;
        const QString cluster = text.mid(start, end - start);
        start = end;
        bool whitespace = true;
        for (const QChar &c : cluster) {
            if (!c.isSpace()) {
                whitespace = false;
                break;
            }
        }
        if (whitespace)
            continue;
        // Any non-whitespace cluster that is not a catalogue emoji makes
        // the whole message ordinary text — even after three emoji.
        if (!isKnownEmojiCluster(cluster))
            return 0;
        if (count < 4)
            ++count;
    }
    return count;
}

bool EmojiCatalog::contains(const QString &emoji) const { return indexOf(emoji) >= 0; }

QVariantList EmojiCatalog::variantsFor(const QString &baseEmoji) const
{
    QVariantList result;
    const auto indexes = m_variants.value(baseEmoji);
    for (int index : indexes) {
        const Entry &entry = m_entries[index];
        result.append(QVariantMap{{QStringLiteral("emoji"), entry.emoji},
                                  {QStringLiteral("name"), entry.name},
                                  {QStringLiteral("tone"), entry.tone}});
    }
    return result;
}

void EmojiCatalog::recordUse(const QString &emoji)
{
    if (!m_settings || !contains(emoji))
        return;
    m_settings->recordRecentEmoji(emoji);
    if (m_category == QLatin1String("Recently Used") && m_searchText.trimmed().isEmpty())
        rebuild();
    Q_EMIT recentEmojiChanged();
}

void EmojiCatalog::clearRecent()
{
    if (!m_settings)
        return;
    m_settings->clearRecentEmoji();
    if (m_category == QLatin1String("Recently Used"))
        rebuild();
    Q_EMIT recentEmojiChanged();
}

QStringList EmojiCatalog::recentEmoji() const
{
    // v0.6.5: the raw MRU list for consumers that want it directly (the
    // quick-react strip), filtered through the same validity check rebuild()
    // uses so a corrupted or legacy settings entry can never reach one.
    QStringList out;
    if (!m_settings)
        return out;
    for (const QString &emoji : m_settings->recentEmoji()) {
        if (contains(emoji))
            out.append(emoji);
    }
    return out;
}

QString EmojiCatalog::preferredTone() const
{
    return m_settings ? m_settings->preferredEmojiTone() : QString();
}

void EmojiCatalog::setPreferredTone(const QString &tone)
{
    if (!m_settings || preferredTone() == tone)
        return;
    m_settings->setPreferredEmojiTone(tone);
    Q_EMIT preferredToneChanged();
}
