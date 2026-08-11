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
    // v0.6.5: the persisted MRU recent-emoji list (settings key emoji/recent),
    // exposed read-only for consumers that want the raw ordered list directly
    // (the quick-react strip) rather than the filtered/paged GridView model.
    // Filtered through the same validity check rebuild() uses, so a corrupted
    // or legacy settings entry can never reach a consumer.
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
    int indexOf(const QString &emoji) const;
    bool isKnownEmojiCluster(const QString &cluster) const;

    SettingsManager *m_settings = nullptr;
    QList<Entry> m_entries;
    QHash<QString, int> m_byEmoji;
    // VS16-stripped forms of every catalogue sequence, built once at load.
    // Clients disagree about emitting U+FE0F presentation selectors; a
    // cluster missing (or carrying extra) VS16 still matches its sequence.
    QSet<QString> m_sequencesNoVs16;
    // v0.7: base-emoji indices per category, built once at load so a
    // category switch is a bucket swap, never a full-catalogue rescan.
    QHash<QString, QList<int>> m_categoryBuckets;
    QHash<QString, QList<int>> m_variants;
    QList<int> m_visible;
    QString m_searchText;
    QString m_category = QStringLiteral("Recently Used");
};
