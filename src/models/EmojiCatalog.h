#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVariantList>

class SettingsManager;

// Process-lifetime, local-only Unicode emoji catalogue and filtered picker
// model. The committed TSV is parsed once; filtering retains integer indexes
// and never copies or reparses the full catalogue.
class EmojiCatalog : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY categoryChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString dataVersion READ dataVersion CONSTANT)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)
    Q_PROPERTY(QString preferredTone READ preferredTone WRITE setPreferredTone NOTIFY preferredToneChanged)
    // The persisted MRU recent-emoji list (settings key emoji/recent),
    // read-only, for consumers wanting the raw ordered list (the quick-react
    // strip). Filtered through rebuild()'s validity check.
    Q_PROPERTY(QStringList recentEmoji READ recentEmoji NOTIFY recentEmojiChanged)

public:
    enum Role {
        EmojiRole = Qt::UserRole + 1,
        NameRole,
        KeywordsRole,
        CategoryRole,
        BaseEmojiRole,
        HasSkinTonesRole,
        ToneVariantRole,
        AccessibleLabelRole,
    };

    explicit EmojiCatalog(SettingsManager *settings, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);
    QString category() const { return m_category; }
    void setCategory(const QString &category);
    QString dataVersion() const { return QStringLiteral("Unicode Emoji 17.0"); }
    QStringList categories() const;
    QString preferredTone() const;
    void setPreferredTone(const QString &tone);
    QStringList recentEmoji() const;

    Q_INVOKABLE QVariantList variantsFor(const QString &baseEmoji) const;
    Q_INVOKABLE void recordUse(const QString &emoji);
    Q_INVOKABLE void clearRecent();
    Q_INVOKABLE bool contains(const QString &emoji) const;
    Q_INVOKABLE int catalogueCount() const { return m_entries.size(); }

    // ── Colon-shortcode completion (":thumbs" -> 👍) ─────────────────────
    //
    // Candidates for the composer's `:query` popup, merged there with custom
    // pack emoticons. Ranked prefix-on-name-or-alias first, then word-prefix
    // within the name, then a keyword (alias) substring; recently used
    // entries are boosted within their tier. One row per emoji family (skin
    // tones collapse to the caller's preferredTone), each
    // {kind: "unicode", emoji, name, shortcode}. `shortcode` is the
    // catalogue's own first alias (gemoji-style, e.g. "thumbsup") or, absent
    // one, a slug derived from the name — display only, never required to be
    // unique or reversible.
    Q_INVOKABLE QVariantList completionsForPrefix(const QString &prefix,
                                                  int limit) const;
    // The emoji for an exact shortcode/alias match (e.g. "thumbsup" or "+1"),
    // preferredTone applied, or "" when `code` names no Unicode alias. Used
    // to auto-convert a completed `:code:` the caller has already confirmed
    // is not a custom pack shortcode.
    Q_INVOKABLE QString emojiForShortcode(const QString &code) const;
    // Big-emoji support: the number of user-perceived emoji sequences when
    // the text consists ONLY of catalogue emoji and whitespace, else 0.
    // One grapheme cluster (ZWJ family, flag, keycap, tone variant,
    // VS16-qualified form) counts as one emoji. The count saturates at 4 —
    // callers only distinguish 1..3 ("render large") from everything else,
    // so 4 means "four or more". O(text length) with O(1) hash lookups
    // against the catalogue loaded at startup; never a catalogue scan.
    Q_INVOKABLE int emojiOnlySequenceCount(const QString &text) const;

Q_SIGNALS:
    void searchTextChanged();
    void categoryChanged();
    void countChanged();
    void preferredToneChanged();
    void recentEmojiChanged();

private:
    struct Entry {
        QString emoji;
        QString name;
        QString keywords;
        QString category;
        QString baseEmoji;
        QString tone;
        QString searchKey;
        bool hasSkinTones = false;
    };

    void load();
    void rebuild();
    // Whether the MRU holds at least one emoji this catalogue can resolve,
    // using rebuild()'s test so it agrees with the Recently Used grid.
    bool hasResolvableRecents() const;
    int indexOf(const QString &emoji) const;
    bool isKnownEmojiCluster(const QString &cluster) const;
    // A base-entry index resolved to its completion row, preferredTone
    // applied to `emoji`.
    QVariantMap completionRow(int index) const;
    // `entry`'s emoji, substituting the preferredTone variant when one is set
    // and the family has tones. Shared by completionRow and emojiForShortcode
    // so a shortcode and a search result never disagree on the tone.
    QString toneAdjustedEmoji(const Entry &entry) const;

    SettingsManager *m_settings = nullptr;
    QList<Entry> m_entries;
    QHash<QString, int> m_byEmoji;
    // Case-folded alias/shortcode -> base-entry index, e.g. "thumbsup" and
    // "+1" both -> the 👍 row. Built once at load() from the TSV's own
    // aliases column; first entry wins a collision. Empty aliases (skin-tone
    // variant rows) contribute nothing.
    QHash<QString, int> m_aliasToIndex;
    // VS16-stripped forms of every catalogue sequence, built once at load.
    // Clients disagree about emitting U+FE0F presentation selectors; a
    // cluster missing (or carrying extra) VS16 still matches its sequence.
    QSet<QString> m_sequencesNoVs16;
    // Base-emoji indices per category, built once at load.
    QHash<QString, QList<int>> m_categoryBuckets;
    QHash<QString, QList<int>> m_variants;
    QList<int> m_visible;
    QString m_searchText;
    QString m_category = QStringLiteral("Recently Used");
};
