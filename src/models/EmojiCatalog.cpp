#include "models/EmojiCatalog.h"

#include "app/SettingsManager.h"

#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>

#include <algorithm>

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
    // Don't open on an empty "Recently Used" grid: fall back to Smileys when
    // there are no resolvable recents. Decided once here rather than in
    // rebuild(), so the Recently Used tab keeps its own empty state. Tested
    // against resolved recents, since rebuild() drops entries not in the
    // catalogue.
    if (m_category == kCategories.constFirst() && !hasResolvableRecents()
        && kCategories.size() > 1) {
        m_category = kCategories.at(1);
    }
    rebuild();
}

bool EmojiCatalog::hasResolvableRecents() const
{
    if (!m_settings)
        return false;
    const QStringList recents = m_settings->recentEmoji();
    for (const QString &emoji : recents) {
        if (indexOf(emoji) >= 0)
            return true;
    }
    return false;
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
        // Comments are "# " lines. A bare '#' prefix is data: the keycap
        // sequence #️⃣ ('#' U+FE0F U+20E3) starts with it.
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
    // Per-category index buckets, built once, so a category switch is a list
    // swap rather than a catalogue rescan.
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        if (entry.emoji == entry.baseEmoji)
            m_categoryBuckets[entry.category].append(i);
    }
    // Lookup set for emojiOnlySequenceCount, built once: senders disagree about
    // U+FE0F, so clusters are matched against the VS16-stripped form.
    for (const Entry &entry : std::as_const(m_entries)) {
        QString stripped = entry.emoji;
        stripped.remove(QChar(0xFE0F));
        if (!stripped.isEmpty())
            m_sequencesNoVs16.insert(stripped);
    }
    // Colon-shortcode index, built once: the TSV's aliases column is already
    // gemoji-style short names ("+1 thumbsup" for 👍), space-separated, on
    // the tone-neutral base row only.
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        if (entry.emoji != entry.baseEmoji || entry.keywords.isEmpty())
            continue;
        const QStringList aliases =
            entry.keywords.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &alias : aliases) {
            const QString key = alias.toCaseFolded();
            if (!key.isEmpty() && !m_aliasToIndex.contains(key))
                m_aliasToIndex.insert(key, i);
        }
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
        // Swap in the precomputed bucket; no catalogue scan.
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
    // Cheap length gate: QTextBoundaryFinder is O(n) up front and this runs in
    // a per-delegate binding. A longer body cannot be a 1-3 emoji message.
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
    // The raw MRU list (for the quick-react strip), filtered by the same
    // validity check rebuild() uses so corrupted or legacy entries never reach
    // a consumer.
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

QString EmojiCatalog::toneAdjustedEmoji(const Entry &entry) const
{
    if (!entry.hasSkinTones)
        return entry.emoji;
    const QString tone = preferredTone();
    if (tone.isEmpty() || tone == QLatin1String("default"))
        return entry.emoji;
    for (int index : m_variants.value(entry.baseEmoji)) {
        if (m_entries.at(index).tone == tone)
            return m_entries.at(index).emoji;
    }
    return entry.emoji;
}

QVariantMap EmojiCatalog::completionRow(int index) const
{
    const Entry &entry = m_entries.at(index);
    QString shortcode;
    if (!entry.keywords.isEmpty()) {
        const QStringList aliases =
            entry.keywords.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!aliases.isEmpty())
            shortcode = aliases.first();
    }
    if (shortcode.isEmpty()) {
        // No curated alias (rare): a display-only slug from the name. Never
        // used for lookup, so it need not be unique.
        shortcode = entry.name.toCaseFolded();
        shortcode.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                          QStringLiteral("_"));
        while (shortcode.startsWith(QLatin1Char('_')))
            shortcode.remove(0, 1);
        while (shortcode.endsWith(QLatin1Char('_')))
            shortcode.chop(1);
    }
    return QVariantMap{
        { QStringLiteral("kind"), QStringLiteral("unicode") },
        { QStringLiteral("emoji"), toneAdjustedEmoji(entry) },
        { QStringLiteral("name"), entry.name },
        { QStringLiteral("shortcode"), shortcode },
    };
}

QVariantList EmojiCatalog::completionsForPrefix(const QString &prefix,
                                                int limit) const
{
    QVariantList out;
    const QString needle = prefix.trimmed().toCaseFolded();
    if (needle.isEmpty() || m_entries.isEmpty())
        return out;
    const int bound = limit > 0 ? limit : 12;
    const QStringList recents = m_settings ? m_settings->recentEmoji() : QStringList();

    // Tier 0: the name or a whole alias starts with the query. Tier 1: any
    // word inside the name starts with it. Tier 2: an alias contains it
    // anywhere. Ties keep catalogue order (roughly Unicode order, e.g. 👍
    // before 👎), except a recently used emoji is promoted ahead of
    // non-recent ties in the same tier.
    struct Candidate { int index; int tier; int recentRank; };
    QList<Candidate> candidates;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        if (entry.emoji != entry.baseEmoji)
            continue; // one row per family; tone applied on output
        const QString name = entry.name.toCaseFolded();
        const QStringList aliases = entry.keywords.isEmpty()
            ? QStringList()
            : entry.keywords.toCaseFolded()
                  .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int tier = -1;
        if (name.startsWith(needle)) {
            tier = 0;
        } else {
            for (const QString &alias : aliases) {
                if (alias.startsWith(needle)) { tier = 0; break; }
            }
        }
        if (tier < 0) {
            const QStringList words =
                name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString &word : words) {
                if (word.startsWith(needle)) { tier = 1; break; }
            }
        }
        if (tier < 0) {
            for (const QString &alias : aliases) {
                if (alias.contains(needle)) { tier = 2; break; }
            }
        }
        if (tier < 0)
            continue;
        candidates.append({ i, tier, static_cast<int>(recents.indexOf(entry.emoji)) });
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate &a, const Candidate &b) {
            if (a.tier != b.tier)
                return a.tier < b.tier;
            const bool aRecent = a.recentRank >= 0;
            const bool bRecent = b.recentRank >= 0;
            if (aRecent != bRecent)
                return aRecent;
            if (aRecent && a.recentRank != b.recentRank)
                return a.recentRank < b.recentRank;
            return a.index < b.index;
        });
    for (const Candidate &c : candidates) {
        if (out.size() >= bound)
            break;
        out.append(completionRow(c.index));
    }
    return out;
}

QString EmojiCatalog::emojiForShortcode(const QString &code) const
{
    const QString needle = code.trimmed().toCaseFolded();
    if (needle.isEmpty())
        return {};
    const auto it = m_aliasToIndex.constFind(needle);
    if (it == m_aliasToIndex.cend())
        return {};
    return toneAdjustedEmoji(m_entries.at(it.value()));
}
