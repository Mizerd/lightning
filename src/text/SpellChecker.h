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
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString language READ language NOTIFY availabilityChanged)
    Q_PROPERTY(QStringList languages READ languages NOTIFY availabilityChanged)
    Q_PROPERTY(QString languageLabel READ languageLabel NOTIFY availabilityChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY availabilityChanged)
    Q_PROPERTY(QString preferredLanguage READ preferredLanguage
                   WRITE setPreferredLanguage NOTIFY availabilityChanged)
    Q_PROPERTY(QStringList extraLanguages READ extraLanguages
                   WRITE setExtraLanguages NOTIFY availabilityChanged)
    Q_PROPERTY(QStringList missingLanguages READ missingLanguages
                   NOTIFY availabilityChanged)
    Q_PROPERTY(QStringList availableLanguages READ availableLanguages
                   NOTIFY availabilityChanged)
    Q_PROPERTY(QVariantList languageOptions READ languageOptions
                   NOTIFY availabilityChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason
                   NOTIFY availabilityChanged)
    Q_PROPERTY(bool autocorrectEnabled READ autocorrectEnabled
                   WRITE setAutocorrectEnabled NOTIFY autocorrectEnabledChanged)
    Q_PROPERTY(bool autocorrectDefault READ autocorrectDefault CONSTANT)
    Q_PROPERTY(QStringList personalWords READ personalWords
                   NOTIFY personalDictionaryChanged)

public:
    // The un-windowed underline pass runs on the GUI thread; a huge paste
    // must not freeze it.
    static constexpr int kMaxCheckedChars = 20000;
    static constexpr int kMaxScannedChars = 400000;
    static constexpr int kMaxWindowChars = 20000;
    static constexpr int kMaxReportedRanges = 300;
    static constexpr int kMaxPersonalWords = 5000;
    static constexpr int kMaxTranspositionCache = 512;

    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    void initialize(const QString &preferredLanguage = QString{});
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
    void setInterfaceLanguage(const QString &tag);
    QStringList missingLanguages() const { return m_missingLanguages; }
    QStringList availableLanguages() const { return m_languages; }
    QVariantList languageOptions() const;
    QString unavailableReason() const { return m_unavailableReason; }
    bool autocorrectEnabled() const { return m_autocorrect; }
    void setAutocorrectEnabled(bool enabled);
    static bool autocorrectDefault();
    QStringList personalWords() const;

    void setPersonalDictionaryPath(const QString &path);
    QString personalDictionaryPath() const { return m_personalPath; }

    static QString labelForTag(const QString &tag);
    // Characters after which autocorrect may act: whitespace and , ; ! ?
    // Not '.' (domains, file names) and not ':' (emoji shortcodes).
    static bool isAutocorrectBoundary(QChar c);
    Q_INVOKABLE bool isAutocorrectBoundaryText(const QString &typed) const;

    Q_INVOKABLE QVariantList misspelledRanges(
        const QString &text, int cursorPosition = -1,
        const QVariantList &skipRanges = QVariantList{}, int windowStart = -1,
        int windowEnd = -1) const;
    Q_INVOKABLE QVariantMap wordAt(const QString &text, int position) const;
    Q_INVOKABLE QStringList suggestions(const QString &word) const;
    Q_INVOKABLE void addToDictionary(const QString &word);
    Q_INVOKABLE void removeFromDictionary(const QString &word);
    Q_INVOKABLE void ignoreWord(const QString &word);

    Q_INVOKABLE QVariantMap autocorrection(
        const QString &text, int boundary,
        const QVariantList &skipRanges = QVariantList{}) const;
    Q_INVOKABLE void rejectAutocorrection(const QString &original);

    void setBackendForTest(std::unique_ptr<SpellBackend> backend);
    void setBackendsForTest(std::vector<std::unique_ptr<SpellBackend>> backends);
    using BackendFactory = std::function<std::unique_ptr<SpellBackend>(
        const QString &, QString *, QStringList *)>;
    void setBackendFactoryForTest(BackendFactory factory);
    int backendLookupsForTest() const { return m_lookups; }

Q_SIGNALS:
    void availabilityChanged();
    void enabledChanged();
    void autocorrectEnabledChanged();
    void personalDictionaryChanged();
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
    mutable QHash<QString, bool> m_cache;
    mutable QHash<QString, QString> m_transpositionCache;
    mutable int m_lookups = 0;
    QSet<QString> m_ignored;
    QSet<QString> m_rejectedCorrections;
    QSet<QString> m_personal;
    QString m_personalPath;
};
