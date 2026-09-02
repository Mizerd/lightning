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

// The composer's spell checker: ALL the policy, none of the dictionary.
//
// Split deliberately. `SpellBackend` answers one question about one word;
// everything that decides WHICH words are asked about lives here, so the
// Windows, macOS and Linux backends cannot drift into three different ideas
// of what a word is. That matters more in a chat composer than in a document
// editor: a message is full of things that are spelled correctly by not
// being English — room aliases, matrix ids, URLs, code spans, emoji
// shortcodes — and a checker that underlines them all is one the user turns
// off.
//
// WHAT IS NEVER ASKED ABOUT (see `forEachWord`): anything in a whitespace
// chunk carrying `://`, `@`, `` ` ``, `/`, `\`, a digit or a dot between
// letters (a domain); anything opening with `#`, `!`, `:` or `~/`; ALL-CAPS
// runs; single letters; everything inside a fenced ``` block, an inline
// `code span` (spaces included) or a Markdown link destination `](…)`; and
// any word overlapping a caller-supplied skip range, which is how the
// composers exclude their own re-anchored mention ranges and, in rich mode,
// code fragments. The word the caret is inside is skipped too, because
// underlining a word while it is still being typed is noise rather than
// information. A draft beyond kMaxCheckedChars is not checked at all rather
// than checked slowly.
//
// LANGUAGE. `preferredLanguage` is a BCP-47 tag or "" for "the system's own
// preference" (Automatic); the resolved dictionary is `language()`, the
// picker's rows are `languageOptions()`. Changing the preference recreates
// the backend and invalidates every cached answer.
//
// PRIVACY. This class never sends anything anywhere: the backends are a local
// COM object, an AppKit singleton and a local shared library. Nothing is
// logged — a misspelled word is message content, and message content does
// not go in a log. The only thing that leaves the process is what the user
// explicitly adds to their own platform dictionary.
class SpellChecker : public QObject
{
    Q_OBJECT
    // Whether a dictionary actually resolved on this machine. QML gates the
    // whole feature on it rather than drawing an underline it cannot justify.
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
    // A draft longer than this is not checked: the underline pass runs on the
    // GUI thread and a pathological paste must not freeze it.
    static constexpr int kMaxCheckedChars = 20000;

    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    // Resolves the platform backend for `preferredLanguage` ("" = system).
    // Called once at startup; separate from the constructor so a test can
    // install a fake instead.
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

    // [{start, length}, ...] over `text`, in the same shape the composers
    // already use for mention ranges. `cursorPosition` names the caret (-1
    // for none) and the word containing it is never reported. `skipRanges`
    // are ranges to leave alone entirely — the composer passes its own
    // mention ranges, whose display names are people's names, not English,
    // and the rich composer adds its code fragments.
    Q_INVOKABLE QVariantList misspelledRanges(
        const QString &text, int cursorPosition = -1,
        const QVariantList &skipRanges = QVariantList{}) const;
    // {word, start, length} for the word at `position`; `word` is empty when
    // the position is not inside a checkable word. Drives the context menu.
    Q_INVOKABLE QVariantMap wordAt(const QString &text, int position) const;
    Q_INVOKABLE QStringList suggestions(const QString &word) const;
    // Writes the user's own platform dictionary.
    Q_INVOKABLE void addToDictionary(const QString &word);
    // Session-only: forgotten on quit, and never written anywhere.
    Q_INVOKABLE void ignoreWord(const QString &word);

    // Test seams. `setBackendForTest` takes ownership; a null backend
    // restores "unavailable". `setBackendFactoryForTest` replaces the
    // platform factory so a language change can be observed.
    void setBackendForTest(std::unique_ptr<SpellBackend> backend);
    // (preferred tag, failure out, available-languages out) — the same shape
    // as createPlatformSpellBackend, so a fake can answer "no dictionary for
    // that one, but here is what I have".
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
    // Bounded lookup cache. A composer re-checks the same words on every
    // keystroke, and a dictionary lookup is the expensive half.
    mutable QHash<QString, bool> m_cache;
    QSet<QString> m_ignored;
};
