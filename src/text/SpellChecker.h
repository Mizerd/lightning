#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class SpellBackend;

// The composer's spell checker: ALL the policy, none of the dictionary.
//
// Split deliberately. `SpellBackend` answers one question about one word;
// everything that decides WHICH words are asked about lives here, so the
// Windows and Linux backends cannot drift into two different ideas of what a
// word is. That matters more in a chat composer than in a document editor: a
// message is full of things that are spelled correctly by not being English —
// room aliases, matrix ids, URLs, code spans, emoji shortcodes — and a
// checker that underlines them all is one the user turns off.
//
// WHAT IS NEVER ASKED ABOUT (see `forEachWord`): anything in a whitespace
// chunk carrying `://`, `@`, `` ` ``, `/`, `\` or a digit; anything opening
// with `#`, `!`, `:` or `~`; ALL-CAPS runs; single letters; and any word
// overlapping a caller-supplied skip range, which is how the composer's own
// re-anchored mention ranges are excluded. The word the caret is inside is
// skipped too, because underlining a word while it is still being typed is
// noise rather than information.
//
// PRIVACY. This class never sends anything anywhere: the backends are a local
// COM object and a local shared library. Nothing is logged — a misspelled
// word is message content, and message content does not go in a log. The only
// thing that leaves the process is what the user explicitly adds to their own
// platform dictionary.
class SpellChecker : public QObject
{
    Q_OBJECT
    // Whether a dictionary actually resolved on this machine. QML gates the
    // whole feature on it rather than drawing an underline it cannot justify.
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString language READ language NOTIFY availabilityChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY availabilityChanged)

public:
    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    // Resolves the platform backend. Called once at startup; separate from
    // the constructor so a test can install a fake instead.
    void initialize(const QString &preferredLanguage = QString{});

    bool available() const { return m_backend != nullptr; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QString language() const;
    QString backendName() const;

    // [{start, length}, ...] over `text`, in the same shape the composers
    // already use for mention ranges. `cursorPosition` names the caret (-1
    // for none) and the word containing it is never reported. `skipRanges`
    // are ranges to leave alone entirely — the composer passes its own
    // mention ranges, whose display names are people's names, not English.
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

    // Test seam. Takes ownership; a null backend restores "unavailable".
    void setBackendForTest(std::unique_ptr<SpellBackend> backend);

Q_SIGNALS:
    void availabilityChanged();
    void enabledChanged();
    // A word was added or ignored: every drawn underline is now stale.
    void dictionaryChanged();

private:
    bool wordIsCorrect(const QString &word) const;

    std::unique_ptr<SpellBackend> m_backend;
    bool m_enabled = true;
    // Bounded lookup cache. A composer re-checks the same words on every
    // keystroke, and a dictionary lookup is the expensive half.
    mutable QHash<QString, bool> m_cache;
    QSet<QString> m_ignored;
};
