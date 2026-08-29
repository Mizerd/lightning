#include "text/SpellChecker.h"

#include "text/SpellBackend.h"

#include <QChar>

namespace {

// The cache is bounded rather than pruned: it holds words, one boolean each,
// and a composer that has genuinely produced four thousand distinct words has
// earned a fresh start more cheaply than an LRU costs to maintain.
constexpr int kMaxCachedWords = 4096;

// A chunk is a whitespace-delimited run. Anything in this list is not English
// and asking a dictionary about it produces a false underline.
bool chunkIsCheckable(QStringView chunk)
{
    if (chunk.isEmpty())
        return false;
    // Matrix ids and aliases (#room:server, !id:server, @user:server), emoji
    // shortcodes (:smile:) and shell/home paths (~/x) all announce
    // themselves in their first character.
    const QChar first = chunk.front();
    if (first == u'#' || first == u'!' || first == u':' || first == u'~'
        || first == u'@') {
        return false;
    }
    if (chunk.contains(u"://"))
        return false;
    for (const QChar c : chunk) {
        // A digit anywhere makes the chunk an identifier, a version, a time
        // or a measurement — never a word to correct. `@` catches mxids and
        // e-mail addresses mid-sentence; the slashes catch paths and dates;
        // a backtick is a code span.
        if (c.isDigit() || c == u'@' || c == u'/' || c == u'\\' || c == u'`'
            || c == u'_') {
            return false;
        }
    }
    return true;
}

// Inside a checkable chunk a word is a run of letters, with an apostrophe
// allowed BETWEEN letters so "doesn't" is one word and a quoted 'word' is
// still just the word. U+2019 is included because that is what most systems
// type and what Lightning's own text carries.
bool isWordCharacter(QChar c)
{
    return c.isLetter() || c.isMark();
}

bool isInnerApostrophe(QStringView text, int index)
{
    const QChar c = text.at(index);
    if (c != u'\'' && c != QChar(0x2019))
        return false;
    return index > 0 && index + 1 < text.size()
        && isWordCharacter(text.at(index - 1))
        && isWordCharacter(text.at(index + 1));
}

bool wordIsWorthChecking(QStringView word)
{
    // One letter is never a spelling mistake worth marking.
    if (word.size() < 2)
        return false;
    // ALL CAPS is an acronym far more often than it is a misspelling, and
    // chat is full of them. A word with an interior capital (CamelCase, an
    // identifier, a product name) is left alone for the same reason.
    bool sawLower = false;
    for (int i = 0; i < word.size(); ++i) {
        const QChar c = word.at(i);
        if (c.isUpper() && sawLower)
            return false; // interior capital
        if (c.isLower())
            sawLower = true;
    }
    if (!sawLower)
        return false; // no lowercase letter at all: an acronym
    return true;
}

// Walks `text` and calls `fn(start, length)` for every word worth checking.
template <typename F>
void forEachWord(const QString &text, F &&fn)
{
    const int n = text.size();
    int i = 0;
    while (i < n) {
        while (i < n && text.at(i).isSpace())
            ++i;
        int chunkStart = i;
        while (i < n && !text.at(i).isSpace())
            ++i;
        const int chunkEnd = i;
        if (chunkEnd <= chunkStart)
            continue;
        const QStringView chunk =
            QStringView{ text }.mid(chunkStart, chunkEnd - chunkStart);
        if (!chunkIsCheckable(chunk))
            continue;

        int j = chunkStart;
        while (j < chunkEnd) {
            while (j < chunkEnd && !isWordCharacter(text.at(j)))
                ++j;
            const int wordStart = j;
            while (j < chunkEnd
                   && (isWordCharacter(text.at(j))
                       || isInnerApostrophe(QStringView{ text }, j))) {
                ++j;
            }
            const int wordLength = j - wordStart;
            if (wordLength <= 0)
                continue;
            const QStringView word =
                QStringView{ text }.mid(wordStart, wordLength);
            if (wordIsWorthChecking(word))
                fn(wordStart, wordLength);
        }
    }
}

bool overlapsAnyRange(int start, int length, const QVariantList &ranges)
{
    const int end = start + length;
    for (const QVariant &value : ranges) {
        const QVariantMap range = value.toMap();
        const int rangeStart = range.value(QStringLiteral("start")).toInt();
        const int rangeLength = range.value(QStringLiteral("length")).toInt();
        if (rangeLength <= 0)
            continue;
        if (start < rangeStart + rangeLength && rangeStart < end)
            return true;
    }
    return false;
}

} // namespace

SpellChecker::SpellChecker(QObject *parent)
    : QObject(parent)
{
}

SpellChecker::~SpellChecker() = default;

void SpellChecker::initialize(const QString &preferredLanguage)
{
    m_backend = createPlatformSpellBackend(preferredLanguage);
    m_cache.clear();
    Q_EMIT availabilityChanged();
}

void SpellChecker::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    Q_EMIT enabledChanged();
}

QString SpellChecker::language() const
{
    return m_backend ? m_backend->language() : QString{};
}

QString SpellChecker::backendName() const
{
    return m_backend ? m_backend->name() : QString{};
}

void SpellChecker::setBackendForTest(std::unique_ptr<SpellBackend> backend)
{
    m_backend = std::move(backend);
    m_cache.clear();
    Q_EMIT availabilityChanged();
}

bool SpellChecker::wordIsCorrect(const QString &word) const
{
    if (m_ignored.contains(word))
        return true;
    const auto cached = m_cache.constFind(word);
    if (cached != m_cache.constEnd())
        return cached.value();
    const bool correct = m_backend->isCorrect(word);
    if (m_cache.size() >= kMaxCachedWords)
        m_cache.clear();
    m_cache.insert(word, correct);
    return correct;
}

QVariantList SpellChecker::misspelledRanges(const QString &text,
                                            int cursorPosition,
                                            const QVariantList &skipRanges) const
{
    QVariantList out;
    if (!m_backend || !m_enabled || text.isEmpty())
        return out;

    forEachWord(text, [&](int start, int length) {
        // The caret sitting anywhere in the word, INCLUDING at either edge,
        // suppresses it: a word is "being typed" right up to the keystroke
        // that leaves it.
        if (cursorPosition >= start && cursorPosition <= start + length)
            return;
        if (!skipRanges.isEmpty() && overlapsAnyRange(start, length, skipRanges))
            return;
        if (wordIsCorrect(text.mid(start, length)))
            return;
        out.append(QVariantMap{
            { QStringLiteral("start"), start },
            { QStringLiteral("length"), length },
        });
    });
    return out;
}

QVariantMap SpellChecker::wordAt(const QString &text, int position) const
{
    QVariantMap out{
        { QStringLiteral("word"), QString{} },
        { QStringLiteral("start"), -1 },
        { QStringLiteral("length"), 0 },
    };
    if (text.isEmpty())
        return out;
    forEachWord(text, [&](int start, int length) {
        if (position < start || position > start + length)
            return;
        // Do not overwrite an earlier hit: at a boundary between two words
        // the first one wins, deterministically.
        if (out.value(QStringLiteral("start")).toInt() >= 0)
            return;
        out[QStringLiteral("word")] = text.mid(start, length);
        out[QStringLiteral("start")] = start;
        out[QStringLiteral("length")] = length;
    });
    return out;
}

QStringList SpellChecker::suggestions(const QString &word) const
{
    if (!m_backend || word.isEmpty())
        return {};
    return m_backend->suggest(word);
}

void SpellChecker::addToDictionary(const QString &word)
{
    if (!m_backend || word.isEmpty())
        return;
    m_backend->addToPersonalDictionary(word);
    // The platform now says this word is correct, so the cached "wrong" must
    // go. Clearing the whole cache rather than one key is deliberate: some
    // backends normalise case, so the entry that answers for this word may
    // not be spelled like it.
    m_cache.clear();
    Q_EMIT dictionaryChanged();
}

void SpellChecker::ignoreWord(const QString &word)
{
    if (word.isEmpty() || m_ignored.contains(word))
        return;
    m_ignored.insert(word);
    m_cache.remove(word);
    Q_EMIT dictionaryChanged();
}
