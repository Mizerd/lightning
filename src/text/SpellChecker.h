#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

class SpellBackend;

// The composer's spell-check policy; SpellBackend supplies the dictionary.
// Keeping the policy here means every platform agrees on what a word is, which
// matters in chat: aliases, Matrix ids, URLs and code are not misspellings.
//
// Never checked (see `forEachWord`): anything in a whitespace
// chunk carrying `://`, `@`, `` ` ``, `/`, `\`, a digit or a dot between
// letters (a domain); anything opening with `#`, `!`, `:` or `~/`; ALL-CAPS
// runs; single letters; everything inside a fenced ``` block, an inline
// `code span` (spaces included) or a Markdown link destination `](…)`; any
// word overlapping a caller-supplied skip range (mentions, rich-mode code);
// and the word under the caret. A draft beyond kMaxCheckedChars is not
// checked at all.
//
// Changing `preferredLanguage` ("" = system) recreates the backend and clears
// the cache.
//
// Privacy: nothing is sent anywhere and nothing is logged, since words are
// message content. Only words the user adds reach their platform dictionary.
class SpellChecker : public QObject
{
    Q_OBJECT
    // Whether a dictionary resolved; QML gates the feature on it.
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // The resolved dictionary as a BCP-47 tag ("en-US"); "" when none.
    Q_PROPERTY(QString language READ language NOTIFY availabilityChanged)
    // A human label for `language` ("English (United States)").
    Q_PROPERTY(QString languageLabel READ languageLabel NOTIFY availabilityChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY availabilityChanged)
    // "" = Automatic; otherwise the BCP-47 tag the user chose.
    Q_PROPERTY(QString preferredLanguage READ preferredLanguage
                   WRITE setPreferredLanguage NOTIFY availabilityChanged)
    // BCP-47 tags the platform can check right now.
    Q_PROPERTY(QStringList availableLanguages READ availableLanguages
                   NOTIFY availabilityChanged)
    // [{tag, label}] for a picker: Automatic first, then every available
    // language sorted by label.
    Q_PROPERTY(QVariantList languageOptions READ languageOptions
                   NOTIFY availabilityChanged)
    // Why `available` is false: "" | "no-platform" | "no-library" |
    // "no-dictionary" (see SpellBackend.h).
    Q_PROPERTY(QString unavailableReason READ unavailableReason
                   NOTIFY availabilityChanged)

public:
    // The underline pass runs on the GUI thread; a huge paste must not freeze
    // it.
    static constexpr int kMaxCheckedChars = 20000;

    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    // Resolves the platform backend. Separate from the constructor so tests
    // can install a fake instead.
    void initialize(const QString &preferredLanguage = QString{});

    bool available() const { return m_backend != nullptr; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QString language() const;
    QString languageLabel() const;
    QString backendName() const;
    QString preferredLanguage() const { return m_preferredLanguage; }
    void setPreferredLanguage(const QString &tag);
    QStringList availableLanguages() const { return m_languages; }
    QVariantList languageOptions() const;
    QString unavailableReason() const { return m_unavailableReason; }

    // A picker label for a BCP-47 tag, in English ("Lithuanian",
    // "English (United Kingdom)"); the tag itself when Qt cannot name it.
    static QString labelForTag(const QString &tag);

    // [{start, length}, ...], the composers' mention-range shape. The word
    // containing `cursorPosition` (-1 for none) and anything overlapping
    // `skipRanges` is never reported.
    Q_INVOKABLE QVariantList misspelledRanges(
        const QString &text, int cursorPosition = -1,
        const QVariantList &skipRanges = QVariantList{}) const;
    // {word, start, length}; `word` is empty outside a checkable word.
    Q_INVOKABLE QVariantMap wordAt(const QString &text, int position) const;
    Q_INVOKABLE QStringList suggestions(const QString &word) const;
    // Writes the user's own platform dictionary.
    Q_INVOKABLE void addToDictionary(const QString &word);
    // Session-only; never persisted.
    Q_INVOKABLE void ignoreWord(const QString &word);

    // Test seams. `setBackendForTest` takes ownership; a null backend
    // restores "unavailable". `setBackendFactoryForTest` replaces the
    // platform factory so a language change can be observed.
    void setBackendForTest(std::unique_ptr<SpellBackend> backend);
    // Same shape as createPlatformSpellBackend().
    using BackendFactory = std::function<std::unique_ptr<SpellBackend>(
        const QString &, QString *, QStringList *)>;
    void setBackendFactoryForTest(BackendFactory factory);

Q_SIGNALS:
    void availabilityChanged();
    void enabledChanged();
    // A word was added or ignored, or the dictionary changed: every drawn
    // underline is now stale.
    void dictionaryChanged();

private:
    void resolve();
    bool wordIsCorrect(const QString &word) const;

    std::unique_ptr<SpellBackend> m_backend;
    BackendFactory m_factory;
    bool m_enabled = true;
    QString m_preferredLanguage;
    QString m_unavailableReason;
    QStringList m_languages;
    // Bounded; the same words are re-checked on every keystroke.
    mutable QHash<QString, bool> m_cache;
    QSet<QString> m_ignored;
};
