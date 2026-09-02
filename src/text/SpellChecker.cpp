#include "text/SpellChecker.h"

#include "text/SpellBackend.h"

#include <QChar>
#include <QLocale>

#include <algorithm>

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
    // shortcodes (:smile:) and home paths (~/x) all announce themselves in
    // their first characters. A lone `~` is left alone so `~~struck~~`
    // words are still checked.
    const QChar first = chunk.front();
    if (first == u'#' || first == u'!' || first == u':' || first == u'@')
        return false;
    if (first == u'~' && chunk.size() > 1 && chunk.at(1) != u'~')
        return false;
    if (chunk.contains(u"://"))
        return false;
    for (int i = 0; i < chunk.size(); ++i) {
        const QChar c = chunk.at(i);
        // A digit anywhere makes the chunk an identifier, a version, a time
        // or a measurement — never a word to correct. `@` catches mxids and
        // e-mail addresses mid-sentence; the slashes catch paths and dates;
        // a backtick is a code span.
        if (c.isDigit() || c == u'@' || c == u'/' || c == u'\\' || c == u'`'
            || c == u'_') {
            return false;
        }
        // A dot with a letter on both sides is a domain name or a file name
        // ("matrix.org", "notes.txt"), not a sentence.
        if (c == u'.' && i > 0 && i + 1 < chunk.size() && chunk.at(i - 1).isLetter()
            && chunk.at(i + 1).isLetter()) {
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

struct Span {
    int start;
    int end; // exclusive
};

// Regions of the text that are never natural language: fenced ``` blocks,
// inline `code spans` (which may hold spaces, so the chunk rule above cannot
// see their middle words) and Markdown link destinations `](…)`. Computed
// once per pass, in document order.
QList<Span> excludedSpans(const QString &text)
{
    QList<Span> spans;
    const int n = text.size();
    int lineStart = 0;
    bool inFence = false;
    int fenceStart = 0;
    // A line ends at '\n' (the Markdown editor) OR at U+2029 (what a rich
    // TextEdit's getText() puts between paragraphs), so no rule ever runs
    // "to the end of the line" across the whole draft.
    auto nextLineEnd = [&](int from) {
        for (int k = from; k < n; ++k) {
            const QChar c = text.at(k);
            if (c == QLatin1Char('\n') || c == QChar::ParagraphSeparator
                || c == QChar::LineSeparator) {
                return k;
            }
        }
        return n;
    };
    // A fence is a line that is ONLY a run of three or more backticks (or
    // tildes) plus an info string; a backtick anywhere after the run makes
    // it an inline span on its own line (CommonMark), not a fence.
    auto isFenceLine = [](QStringView trimmed) {
        if (!(trimmed.startsWith(u"```") || trimmed.startsWith(u"~~~")))
            return false;
        const QChar marker = trimmed.front();
        int run = 0;
        while (run < trimmed.size() && trimmed.at(run) == marker)
            ++run;
        return !trimmed.mid(run).contains(QLatin1Char('`'));
    };
    while (lineStart <= n) {
        const int lineEnd = nextLineEnd(lineStart);
        const QStringView line = QStringView{ text }.mid(lineStart, lineEnd - lineStart);
        const QStringView trimmed = line.trimmed();
        if (isFenceLine(trimmed)) {
            if (!inFence) {
                inFence = true;
                fenceStart = lineStart;
            } else {
                inFence = false;
                spans.append({ fenceStart, lineEnd });
            }
        } else if (!inFence) {
            // Inline code: backtick RUNS pair up on one line (CommonMark:
            // ``a ` b`` is one span delimited by double backticks). An
            // unmatched opening run excludes the rest of the line — while
            // it is being typed that is what the user means.
            auto runEnd = [&](int at) {
                int e = at;
                while (e < lineEnd && text.at(e) == QLatin1Char('`'))
                    ++e;
                return e;
            };
            int i = lineStart;
            while (i < lineEnd) {
                const int open = text.indexOf(QLatin1Char('`'), i);
                if (open < 0 || open >= lineEnd)
                    break;
                const int openEnd = runEnd(open);
                const int close = text.indexOf(QLatin1Char('`'), openEnd);
                if (close < 0 || close >= lineEnd) {
                    spans.append({ open, lineEnd });
                    break;
                }
                const int closeEnd = runEnd(close);
                spans.append({ open, closeEnd });
                i = closeEnd;
            }
            // Markdown link destinations: `](` up to the closing `)`.
            i = lineStart;
            while (i < lineEnd) {
                const int open = text.indexOf(QLatin1String("]("), i);
                if (open < 0 || open >= lineEnd)
                    break;
                int close = text.indexOf(QLatin1Char(')'), open + 2);
                if (close < 0 || close >= lineEnd)
                    close = lineEnd - 1;
                spans.append({ open + 2, close + 1 });
                i = close + 1;
            }
        }
        lineStart = lineEnd + 1;
    }
    if (inFence)
        spans.append({ fenceStart, n }); // an unterminated fence runs to the end
    std::sort(spans.begin(), spans.end(),
              [](const Span &a, const Span &b) { return a.start < b.start; });
    return spans;
}

bool insideExcluded(const QList<Span> &spans, int start, int length)
{
    const int end = start + length;
    for (const Span &s : spans) {
        if (s.start >= end)
            break;
        if (start < s.end && s.start < end)
            return true;
    }
    return false;
}

// Walks `text` and calls `fn(start, length)` for every word worth checking.
template <typename F>
void forEachWord(const QString &text, F &&fn)
{
    const QList<Span> excluded = excludedSpans(text);
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
            if (insideExcluded(excluded, wordStart, wordLength))
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
    m_preferredLanguage = spellTagToBcp47(preferredLanguage);
    resolve();
}

void SpellChecker::resolve()
{
    QString failure;
    QStringList offered;
    std::unique_ptr<SpellBackend> next = m_factory
        ? m_factory(m_preferredLanguage, &failure, &offered)
        : createPlatformSpellBackend(m_preferredLanguage, &failure, &offered);
    m_backend = std::move(next);
    m_unavailableReason = m_backend ? QString() : failure;
    // What the platform can check survives a failed open: the picker must
    // keep offering it, or a stored preference the machine no longer has
    // would leave the user with no way to choose another.
    m_languages = m_backend ? m_backend->availableLanguages() : offered;
    // A different dictionary can answer differently for every word.
    m_cache.clear();
    Q_EMIT availabilityChanged();
    Q_EMIT dictionaryChanged();
}

void SpellChecker::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    Q_EMIT enabledChanged();
}

void SpellChecker::setPreferredLanguage(const QString &tag)
{
    const QString normalized = spellTagToBcp47(tag);
    if (normalized == m_preferredLanguage)
        return;
    m_preferredLanguage = normalized;
    resolve();
}

QString SpellChecker::language() const
{
    return m_backend ? m_backend->language() : QString();
}

QString SpellChecker::languageLabel() const
{
    return labelForTag(language());
}

QString SpellChecker::backendName() const
{
    return m_backend ? m_backend->name() : QString();
}

QString SpellChecker::labelForTag(const QString &tag)
{
    const QString bcp47 = spellTagToBcp47(tag);
    if (bcp47.isEmpty())
        return {};
    const QLocale locale(bcp47);
    if (locale.language() == QLocale::C || locale.language() == QLocale::AnyLanguage)
        return bcp47;
    QString label = QLocale::languageToString(locale.language());
    // A territory is shown only when the tag carried one: "English (United
    // Kingdom)" against "English (United States)", but plain "Lithuanian"
    // for "lt".
    if (bcp47.contains(QLatin1Char('-')) && locale.territory() != QLocale::AnyTerritory)
        label += QStringLiteral(" (") + QLocale::territoryToString(locale.territory())
            + QLatin1Char(')');
    return label;
}

QVariantList SpellChecker::languageOptions() const
{
    QVariantList out;
    out.append(QVariantMap{ { QStringLiteral("tag"), QString() },
                            { QStringLiteral("label"), tr("Automatic (system language)") } });
    QList<QPair<QString, QString>> rows;
    for (const QString &tag : m_languages) {
        const QString bcp47 = spellTagToBcp47(tag);
        bool seen = false;
        for (const auto &row : rows)
            if (row.first == bcp47)
                seen = true;
        if (!seen)
            rows.append({ bcp47, labelForTag(bcp47) });
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto &a, const auto &b) {
                         return a.second.localeAwareCompare(b.second) < 0;
                     });
    for (const auto &row : rows)
        out.append(QVariantMap{ { QStringLiteral("tag"), row.first },
                                { QStringLiteral("label"), row.second } });
    return out;
}

void SpellChecker::setBackendForTest(std::unique_ptr<SpellBackend> backend)
{
    m_backend = std::move(backend);
    m_unavailableReason = m_backend ? QString() : QStringLiteral("no-dictionary");
    m_languages = m_backend ? m_backend->availableLanguages() : QStringList();
    m_cache.clear();
    Q_EMIT availabilityChanged();
}

void SpellChecker::setBackendFactoryForTest(BackendFactory factory)
{
    m_factory = std::move(factory);
}

bool SpellChecker::wordIsCorrect(const QString &word) const
{
    if (m_ignored.contains(word))
        return true;
    const auto cached = m_cache.constFind(word);
    if (cached != m_cache.constEnd())
        return cached.value();
    const bool correct = !m_backend || m_backend->isCorrect(word);
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
    if (!m_backend || !m_enabled || text.isEmpty() || text.size() > kMaxCheckedChars)
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
    if (text.isEmpty() || text.size() > kMaxCheckedChars)
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
