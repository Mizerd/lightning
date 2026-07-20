#include "models/EmojiCatalog.h"

#include "app/SettingsManager.h"

#include <QFile>
#include <QLoggingCategory>
#include <QSet>

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
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
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
}

void EmojiCatalog::clearRecent()
{
    if (!m_settings)
        return;
    m_settings->clearRecentEmoji();
    if (m_category == QLatin1String("Recently Used"))
        rebuild();
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
