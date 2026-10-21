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
#include <vector>

class SpellBackend;

// The composer's spell-check policy; SpellBackend supplies the dictionaries.
// Keeping the policy here means every platform agrees on what a word is, which
// matters in chat: aliases, Matrix ids, URLs and code are not misspellings.
//
// Never checked (see `forEachWord`): anything in a whitespace
// chunk carrying `://`, `@`, `` ` ``, `/`, `\`, a digit or a dot between
// letters (a domain); anything opening with `#`, `!`, `:` or `~/`; ALL-CAPS
// runs; single letters; everything inside a fenced ``` block, an inline
// `code span` (spaces included) or a Markdown link destination `](…)`; any
// word overlapping a caller-supplied skip range (mentions, rich-mode code);
// and the word under the caret. Without a window a draft beyond
// kMaxCheckedChars is not checked at all; with one (the visible region) only
// the words inside it reach a dictionary, up to kMaxScannedChars.
//
// Languages: several at once. A word is right when ANY active dictionary
// accepts it. "Automatic" is the system language plus the app's UI language
// when a dictionary exists for it; an explicit primary language and any
// "also check" languages are honoured exactly and reported when missing.
//
// The personal dictionary is Lightning's own file (one word per line, per
// operating-system user), so it works identically on every platform and
// every engine, and a word can be removed again.
//
// Autocorrect (off unless the user or the platform default turns it on) is
// deliberately narrow; see autocorrection().
//
// Privacy: nothing is sent anywhere and nothing is logged, since words are
// message content.
class SpellChecker : public QObject
{
    Q_OBJECT
    // Whether at least one dictionary resolved; QML gates the feature on it.
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // The first resolved dictionary as a BCP-47 tag ("en-US"); "" when none.
    Q_PROPERTY(QString language READ language NOTIFY availabilityChanged)
    // Every resolved dictionary, in priority order.
    Q_PROPERTY(QStringList languages READ languages NOTIFY availabilityChanged)
    // Human labels for `languages`, joined ("English (United States),
    // Lithuanian").
    Q_PROPERTY(QString languageLabel READ languageLabel NOTIFY availabilityChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY availabilityChanged)
    // "" = Automatic; otherwise the BCP-47 tag the user chose.
    Q_PROPERTY(QString preferredLanguage READ preferredLanguage
                   WRITE setPreferredLanguage NOTIFY availabilityChanged)
    // Languages checked IN ADDITION to the primary one.
    Q_PROPERTY(QStringList extraLanguages READ extraLanguages
                   WRITE setExtraLanguages NOTIFY availabilityChanged)
    // Explicitly chosen languages (primary or extra) with no dictionary here.
    Q_PROPERTY(QStringList missingLanguages READ missingLanguages
                   NOTIFY availabilityChanged)
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
    Q_PROPERTY(bool autocorrectEnabled READ autocorrectEnabled
                   WRITE setAutocorrectEnabled NOTIFY autocorrectEnabledChanged)
    // The platform's default (true only on macOS with its own autocorrect on).
    Q_PROPERTY(bool autocorrectDefault READ autocorrectDefault CONSTANT)
    // The personal dictionary, sorted.
    Q_PROPERTY(QStringList personalWords READ personalWords
                   NOTIFY personalDictionaryChanged)

public:
    // The un-windowed underline pass runs on the GUI thread; a huge paste
    // must not freeze it.
    static constexpr int kMaxCheckedChars = 20000;
    // With a window, only that window's words are looked up, so a far longer
    // draft can be checked where the user is looking.
    static constexpr int kMaxScannedChars = 400000;
    static constexpr int kMaxWindowChars = 20000;
    // Bounds the rectangles QML draws.
    static constexpr int kMaxReportedRanges = 300;
    // Bounds the personal dictionary file.
    static constexpr int kMaxPersonalWords = 5000;
    // Bounds the autocorrect transposition answers.
    static constexpr int kMaxTranspositionCache = 512;

    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    // Resolves the platform backends. Separate from the constructor so tests
    // can install a fake instead.
    void initialize(const QString &preferredLanguage = QString{});
    // All language inputs at once, resolved once.
    void configure(const QString &preferredLanguage,
                   const QStringList &extraLanguages,
                   const QString &interfaceLanguage);

    bool available() const { return !m_backends.empty(); }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QString language() const;
    QStringList languages() const;
    QString languageLabel() const;
    QString backendName() const;
    QString preferredLanguage() const { return m_preferredLanguage; }
    void setPreferredLanguage(const QString &tag);
    QStringList extraLanguages() const { return m_extraLanguages; }
    void setExtraLanguages(const QStringList &tags);
    // The app's UI language; joins Automatic when a dictionary exists.
    void setInterfaceLanguage(const QString &tag);
    QStringList missingLanguages() const { return m_missingLanguages; }
    QStringList availableLanguages() const { return m_languages; }
    QVariantList languageOptions() const;
    QString unavailableReason() const { return m_unavailableReason; }
    bool autocorrectEnabled() const { return m_autocorrect; }
    void setAutocorrectEnabled(bool enabled);
    static bool autocorrectDefault();
    QStringList personalWords() const;

    // Where the personal dictionary lives; loads it. "" keeps it in memory.
    void setPersonalDictionaryPath(const QString &path);
    QString personalDictionaryPath() const { return m_personalPath; }

    // A picker label for a BCP-47 tag, in English ("Lithuanian",
    // "English (United Kingdom)"); the tag itself when Qt cannot name it.
    static QString labelForTag(const QString &tag);
    // Characters after which autocorrect may act: whitespace and , ; ! ?
    // Not '.' (domains, file names) and not ':' (emoji shortcodes).
    static bool isAutocorrectBoundary(QChar c);

    // [{start, length}, ...], the composers' mention-range shape. The word
    // containing `cursorPosition` (-1 for none) and anything overlapping
    // `skipRanges` is never reported. With `windowStart`/`windowEnd` (>= 0)
    // only words overlapping [windowStart, windowEnd) are looked up.
    Q_INVOKABLE QVariantList misspelledRanges(
        const QString &text, int cursorPosition = -1,
        const QVariantList &skipRanges = QVariantList{}, int windowStart = -1,
        int windowEnd = -1) const;
    // {word, start, length}; `word` is empty outside a checkable word.
    Q_INVOKABLE QVariantMap wordAt(const QString &text, int position) const;
    // From every active dictionary, interleaved best-first, at most 16.
    Q_INVOKABLE QStringList suggestions(const QString &word) const;
    // Writes Lightning's personal dictionary.
    Q_INVOKABLE void addToDictionary(const QString &word);
    Q_INVOKABLE void removeFromDictionary(const QString &word);
    // Session-only; never persisted.
    Q_INVOKABLE void ignoreWord(const QString &word);

    // The correction for the word that ends exactly at `boundary`, the
    // position of the boundary character the user has just typed:
    // {start, length, original, replacement}, or an empty map. Empty unless
    // checking and autocorrect are on, the character at `boundary` is an
    // autocorrect boundary, the draft is not a /command, the word is outside
    // every skip rule above, EVERY active dictionary rejects it, the user has
    // not reverted it this session, and one of these is certain enough:
    //  1. the English typo table (only with an English dictionary active;
    //     the replacement must itself be accepted);
    //  2. the platform's own correction (Windows, macOS);
    //  3. otherwise, for a 4..24-letter all-lowercase word, the ONE swap of
    //     two neighbouring letters that the dictionaries accept ("wrold" ->
    //     "world"); never a Capitalised word, never another kind of edit.
    // A lone "i" (and "i'm", "i've", "i'll", "i'd") becomes "I..." only when
    // every active language is English.
    Q_INVOKABLE QVariantMap autocorrection(
        const QString &text, int boundary,
        const QVariantList &skipRanges = QVariantList{}) const;
    // The user undid a correction of `original`: never correct it again
    // this session.
    Q_INVOKABLE void rejectAutocorrection(const QString &original);

    // Test seams. `setBackendForTest` takes ownership; a null backend
    // restores "unavailable". `setBackendFactoryForTest` replaces the
    // platform factory so a language change can be observed.
    void setBackendForTest(std::unique_ptr<SpellBackend> backend);
    void setBackendsForTest(std::vector<std::unique_ptr<SpellBackend>> backends);
    // Same shape as createPlatformSpellBackend().
    using BackendFactory = std::function<std::unique_ptr<SpellBackend>(
        const QString &, QString *, QStringList *)>;
    void setBackendFactoryForTest(BackendFactory factory);
    // How many single-word lookups reached a backend; tests assert bounds.
    int backendLookupsForTest() const { return m_lookups; }

Q_SIGNALS:
    void availabilityChanged();
    void enabledChanged();
    void autocorrectEnabledChanged();
    void personalDictionaryChanged();
    // A word was added, removed or ignored, or the dictionary changed: every
    // drawn underline is now stale.
    void dictionaryChanged();

private:
    void resolve();
    bool wordIsCorrect(const QString &word) const;
    bool inPersonalDictionary(const QString &word) const;
    bool onlyEnglishActive() const;
    bool anyEnglishActive() const;
    QString acceptedOrEmpty(const QString &replacement) const;
    QString transpositionFor(const QString &lower) const;
    void savePersonalDictionary() const;
    void installBackends(std::vector<std::unique_ptr<SpellBackend>> backends);

    std::vector<std::unique_ptr<SpellBackend>> m_backends;
    BackendFactory m_factory;
    bool m_enabled = true;
    bool m_autocorrect = false;
    QString m_preferredLanguage;
    QStringList m_extraLanguages;
    QString m_interfaceLanguage;
    QStringList m_missingLanguages;
    QString m_unavailableReason;
    QStringList m_languages;
    // Bounded; the same words are re-checked on every keystroke.
    mutable QHash<QString, bool> m_cache;
    // Autocorrect step 3 per lowercase word; "" = no correction.
    mutable QHash<QString, QString> m_transpositionCache;
    mutable int m_lookups = 0;
    QSet<QString> m_ignored;
    QSet<QString> m_rejectedCorrections;
    QSet<QString> m_personal;
    QString m_personalPath;
};
