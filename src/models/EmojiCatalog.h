#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVariantList>

class SettingsManager;

class EmojiCatalog : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY categoryChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString dataVersion READ dataVersion CONSTANT)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)
    Q_PROPERTY(QString preferredTone READ preferredTone WRITE setPreferredTone NOTIFY preferredToneChanged)
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
    void adoptRecents(const QStringList &mru);
    Q_INVOKABLE bool contains(const QString &emoji) const;
    Q_INVOKABLE int catalogueCount() const { return m_entries.size(); }

    Q_INVOKABLE QVariantList completionsForPrefix(const QString &prefix,
                                                  int limit) const;
    Q_INVOKABLE QString emojiForShortcode(const QString &code) const;
    Q_INVOKABLE int emojiOnlySequenceCount(const QString &text) const;

Q_SIGNALS:
    void searchTextChanged();
    void categoryChanged();
    void countChanged();
    void preferredToneChanged();
    void recentEmojiChanged();
    void recentEmojiUsed(const QString &emoji);

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
    bool hasResolvableRecents() const;
    int indexOf(const QString &emoji) const;
    bool isKnownEmojiCluster(const QString &cluster) const;
    QVariantMap completionRow(int index) const;
    QString toneAdjustedEmoji(const Entry &entry) const;

    SettingsManager *m_settings = nullptr;
    QList<Entry> m_entries;
    QHash<QString, int> m_byEmoji;
    QHash<QString, int> m_aliasToIndex;
    // VS16-stripped forms of every catalogue sequence, built once at load.
    // Clients disagree about emitting U+FE0F presentation selectors; a
    // cluster missing (or carrying extra) VS16 still matches its sequence.
    QSet<QString> m_sequencesNoVs16;
    QHash<QString, QList<int>> m_categoryBuckets;
    QHash<QString, QList<int>> m_variants;
    QList<int> m_visible;
    QString m_searchText;
    QString m_category = QStringLiteral("Recently Used");
};
