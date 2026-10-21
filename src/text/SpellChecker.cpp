#include "text/SpellChecker.h"

#include "text/SpellBackend.h"

#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {

// Cleared when full; cheaper than maintaining an LRU.
constexpr int kMaxCachedWords = 4096;

// A chunk is a whitespace-delimited run. Rejects chunks that are not natural
// language.
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
        // Digits: identifiers, versions, times. `@`: mxids and e-mail.
        // Slashes: paths and dates. Backtick: code.
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

// A word is a run of letters, with an apostrophe (' or U+2019) allowed
// between letters so "doesn't" is one word.
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
    if (word.size() < 2)
        return false;
    // Skip acronyms and words with an interior capital (CamelCase, names).
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

// Fenced ``` blocks, inline `code spans` (which may contain spaces, so the
// chunk rule cannot catch them) and Markdown link destinations `](…)`, in
// document order.
QList<Span> excludedSpans(const QString &text)
{
    QList<Span> spans;
    const int n = text.size();
    int lineStart = 0;
    bool inFence = false;
    int fenceStart = 0;
    // A line ends at '\n' or at U+2029/U+2028, which a rich TextEdit's
    // getText() uses between paragraphs.
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
    // Three or more backticks or tildes plus an info string. A backtick
    // after the run makes it an inline span instead (CommonMark).
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
            // Inline code: backtick runs pair up within a line. An unmatched
            // run excludes the rest of the line, as it is still being typed.
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

// The excluded spans, sorted by start, with a running maximum of their ends
// so "does [start, end) overlap any span" is one binary search however many
// spans a pasted draft has.
struct Exclusions {
    QList<Span> spans;
    QList<int> maxEnd; // maxEnd[i] = max(spans[0..i].end)

    explicit Exclusions(QList<Span> sorted)
        : spans(std::move(sorted))
    {
        maxEnd.reserve(spans.size());
        int running = -1;
        for (const Span &s : std::as_const(spans)) {
            running = std::max(running, s.end);
            maxEnd.append(running);
        }
    }

    bool overlaps(int start, int length) const
    {
        const int end = start + length;
        // Spans starting before `end`; one of them overlaps exactly when the
        // furthest end among them is past `start`.
        const auto it = std::lower_bound(
            spans.cbegin(), spans.cend(), end,
            [](const Span &s, int value) { return s.start < value; });
        const qsizetype count = it - spans.cbegin();
        return count > 0 && maxEnd.at(count - 1) > start;
    }
};

// Walks `text` and calls `fn(start, length)` for every word worth checking
// that overlaps [from, to) (the whole text when `to` < 0).
template <typename F>
void forEachWord(const QString &text, F &&fn, int from = 0, int to = -1)
{
    const Exclusions excluded(excludedSpans(text));
    const int n = text.size();
    const int limit = to < 0 ? n : std::min(n, to);
    int i = 0;
    // Start at the beginning of the whitespace chunk holding `from`, so a
    // chunk cut by the window is still judged whole.
    if (from > 0 && from < n) {
        i = from;
        while (i > 0 && !text.at(i - 1).isSpace())
            --i;
    }
    while (i < n) {
        while (i < n && text.at(i).isSpace())
            ++i;
        if (i >= limit)
            break;
        int chunkStart = i;
        while (i < n && !text.at(i).isSpace())
            ++i;
        const int chunkEnd = i;
        if (chunkEnd <= chunkStart)
            continue;
        if (chunkEnd <= from)
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
            if (wordStart + wordLength <= from || wordStart >= limit)
                continue;
            if (excluded.overlaps(wordStart, wordLength))
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

bool isEnglish(const QString &tag)
{
    return tag == QLatin1String("en") || tag.startsWith(QLatin1String("en-"))
        || tag.startsWith(QLatin1String("en_"));
}

// Common English typos with exactly one intended word. Every entry is applied
// only when the active dictionaries reject the typo AND accept the
// replacement, so a dictionary that knows "alot" as a word disables it.
// Deliberately absent: words that are also real words (cant, wont, wether,
// juts) and typos with two plausible targets (im, its/it's).
const QHash<QString, QString> &englishTypos()
{
    static const QHash<QString, QString> table = {
        { QStringLiteral("teh"), QStringLiteral("the") },
        { QStringLiteral("hte"), QStringLiteral("the") },
        { QStringLiteral("adn"), QStringLiteral("and") },
        { QStringLiteral("nad"), QStringLiteral("and") },
        { QStringLiteral("taht"), QStringLiteral("that") },
        { QStringLiteral("thta"), QStringLiteral("that") },
        { QStringLiteral("thier"), QStringLiteral("their") },
        { QStringLiteral("tihs"), QStringLiteral("this") },
        { QStringLiteral("thsi"), QStringLiteral("this") },
        { QStringLiteral("wiht"), QStringLiteral("with") },
        { QStringLiteral("wtih"), QStringLiteral("with") },
        { QStringLiteral("whcih"), QStringLiteral("which") },
        { QStringLiteral("wich"), QStringLiteral("which") },
        { QStringLiteral("waht"), QStringLiteral("what") },
        { QStringLiteral("whta"), QStringLiteral("what") },
        { QStringLiteral("yuo"), QStringLiteral("you") },
        { QStringLiteral("jsut"), QStringLiteral("just") },
        { QStringLiteral("knwo"), QStringLiteral("know") },
        { QStringLiteral("konw"), QStringLiteral("know") },
        { QStringLiteral("liek"), QStringLiteral("like") },
        { QStringLiteral("mroe"), QStringLiteral("more") },
        { QStringLiteral("ohter"), QStringLiteral("other") },
        { QStringLiteral("peopel"), QStringLiteral("people") },
        { QStringLiteral("poeple"), QStringLiteral("people") },
        { QStringLiteral("shoudl"), QStringLiteral("should") },
        { QStringLiteral("woudl"), QStringLiteral("would") },
        { QStringLiteral("coudl"), QStringLiteral("could") },
        { QStringLiteral("soem"), QStringLiteral("some") },
        { QStringLiteral("somthing"), QStringLiteral("something") },
        { QStringLiteral("thnak"), QStringLiteral("thank") },
        { QStringLiteral("thnaks"), QStringLiteral("thanks") },
        { QStringLiteral("wnat"), QStringLiteral("want") },
        { QStringLiteral("wrok"), QStringLiteral("work") },
        { QStringLiteral("abotu"), QStringLiteral("about") },
        { QStringLiteral("agian"), QStringLiteral("again") },
        { QStringLiteral("becuase"), QStringLiteral("because") },
        { QStringLiteral("beacuse"), QStringLiteral("because") },
        { QStringLiteral("becasue"), QStringLiteral("because") },
        { QStringLiteral("recieve"), QStringLiteral("receive") },
        { QStringLiteral("recieved"), QStringLiteral("received") },
        { QStringLiteral("recieving"), QStringLiteral("receiving") },
        { QStringLiteral("beleive"), QStringLiteral("believe") },
        { QStringLiteral("belive"), QStringLiteral("believe") },
        { QStringLiteral("wierd"), QStringLiteral("weird") },
        { QStringLiteral("freind"), QStringLiteral("friend") },
        { QStringLiteral("seperate"), QStringLiteral("separate") },
        { QStringLiteral("definately"), QStringLiteral("definitely") },
        { QStringLiteral("occured"), QStringLiteral("occurred") },
        { QStringLiteral("occurence"), QStringLiteral("occurrence") },
        { QStringLiteral("untill"), QStringLiteral("until") },
        { QStringLiteral("accomodate"), QStringLiteral("accommodate") },
        { QStringLiteral("acheive"), QStringLiteral("achieve") },
        { QStringLiteral("adress"), QStringLiteral("address") },
        { QStringLiteral("goverment"), QStringLiteral("government") },
        { QStringLiteral("happend"), QStringLiteral("happened") },
        { QStringLiteral("immediatly"), QStringLiteral("immediately") },
        { QStringLiteral("independant"), QStringLiteral("independent") },
        { QStringLiteral("knowlege"), QStringLiteral("knowledge") },
        { QStringLiteral("neccessary"), QStringLiteral("necessary") },
        { QStringLiteral("noticable"), QStringLiteral("noticeable") },
        { QStringLiteral("occassion"), QStringLiteral("occasion") },
        { QStringLiteral("posession"), QStringLiteral("possession") },
        { QStringLiteral("publically"), QStringLiteral("publicly") },
        { QStringLiteral("recomend"), QStringLiteral("recommend") },
        { QStringLiteral("remeber"), QStringLiteral("remember") },
        { QStringLiteral("succesful"), QStringLiteral("successful") },
        { QStringLiteral("tommorow"), QStringLiteral("tomorrow") },
        { QStringLiteral("tommorrow"), QStringLiteral("tomorrow") },
        { QStringLiteral("truely"), QStringLiteral("truly") },
        { QStringLiteral("finaly"), QStringLiteral("finally") },
        { QStringLiteral("enviroment"), QStringLiteral("environment") },
        { QStringLiteral("existance"), QStringLiteral("existence") },
        { QStringLiteral("grammer"), QStringLiteral("grammar") },
        { QStringLiteral("lenght"), QStringLiteral("length") },
        { QStringLiteral("strenght"), QStringLiteral("strength") },
        { QStringLiteral("libary"), QStringLiteral("library") },
        { QStringLiteral("probaly"), QStringLiteral("probably") },
        { QStringLiteral("similiar"), QStringLiteral("similar") },
        { QStringLiteral("suprise"), QStringLiteral("surprise") },
        { QStringLiteral("unfortunatly"), QStringLiteral("unfortunately") },
        { QStringLiteral("usualy"), QStringLiteral("usually") },
        { QStringLiteral("writting"), QStringLiteral("writing") },
        { QStringLiteral("dont"), QStringLiteral("don't") },
        { QStringLiteral("doesnt"), QStringLiteral("doesn't") },
        { QStringLiteral("didnt"), QStringLiteral("didn't") },
        { QStringLiteral("isnt"), QStringLiteral("isn't") },
        { QStringLiteral("wasnt"), QStringLiteral("wasn't") },
        { QStringLiteral("arent"), QStringLiteral("aren't") },
        { QStringLiteral("werent"), QStringLiteral("weren't") },
        { QStringLiteral("havent"), QStringLiteral("haven't") },
        { QStringLiteral("hasnt"), QStringLiteral("hasn't") },
        { QStringLiteral("couldnt"), QStringLiteral("couldn't") },
        { QStringLiteral("shouldnt"), QStringLiteral("shouldn't") },
        { QStringLiteral("wouldnt"), QStringLiteral("wouldn't") },
        { QStringLiteral("theyre"), QStringLiteral("they're") },
        { QStringLiteral("youre"), QStringLiteral("you're") },
        { QStringLiteral("i'm"), QStringLiteral("I'm") },
        { QStringLiteral("i've"), QStringLiteral("I've") },
        { QStringLiteral("i'll"), QStringLiteral("I'll") },
        { QStringLiteral("i'd"), QStringLiteral("I'd") },
    };
    return table;
}

enum class CasePattern { Lower, Capitalised, Other };

CasePattern casePatternOf(const QString &word)
{
    bool allLower = true;
    bool restLower = true;
    for (int i = 0; i < word.size(); ++i) {
        const QChar c = word.at(i);
        if (c.isUpper()) {
            allLower = false;
            if (i > 0)
                restLower = false;
        }
    }
    if (allLower)
        return CasePattern::Lower;
    if (restLower && !word.isEmpty() && word.at(0).isUpper())
        return CasePattern::Capitalised;
    return CasePattern::Other;
}

QString withCasePattern(const QString &replacement, CasePattern pattern)
{
    if (pattern == CasePattern::Capitalised && !replacement.isEmpty())
        return replacement.at(0).toUpper() + replacement.mid(1);
    return replacement;
}

QString normalizedWord(const QString &word)
{
    return word.trimmed();
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

void SpellChecker::configure(const QString &preferredLanguage,
                             const QStringList &extraLanguages,
                             const QString &interfaceLanguage)
{
    m_preferredLanguage = spellTagToBcp47(preferredLanguage);
    m_extraLanguages.clear();
    for (const QString &tag : extraLanguages) {
        const QString bcp47 = spellTagToBcp47(tag);
        if (!bcp47.isEmpty() && !m_extraLanguages.contains(bcp47))
            m_extraLanguages << bcp47;
    }
    m_interfaceLanguage = spellTagToBcp47(interfaceLanguage);
    resolve();
}

void SpellChecker::resolve()
{
    std::vector<std::unique_ptr<SpellBackend>> next;
    QStringList offered;
    QString primaryFailure;
    m_missingLanguages.clear();
    auto create = [this](const QString &tag, QString *failure,
                         QStringList *languages) {
        return m_factory ? m_factory(tag, failure, languages)
                         : createPlatformSpellBackend(tag, failure, languages);
    };
    auto addOffered = [&offered](const QStringList &tags) {
        for (const QString &tag : tags) {
            const QString bcp47 = spellTagToBcp47(tag);
            if (!offered.contains(bcp47))
                offered << bcp47;
        }
    };
    // A second request that resolves to a dictionary already active (the UI
    // language "en" after the system's "en-US") adds nothing.
    auto alreadyActive = [&next](const SpellBackend &candidate) {
        for (const auto &b : next)
            if (b->language() == candidate.language()
                && b->name() == candidate.name())
                return true;
        return false;
    };

    // A provider with a dictionary file but no word data (the host's hspell
    // for Hebrew) accepts EVERY word, and a word is correct when ANY active
    // language accepts it - so one such dictionary silently turns all
    // checking off. A dictionary that takes a nonsense string is not a
    // dictionary: every creation path below refuses it here.
    // Nonsense in several scripts, and ALL must be accepted: a working
    // dictionary may pass words outside its own script unjudged (hunspell's
    // "not representable in this dictionary's encoding", a Russian or Hindi
    // provider handed Latin text), so one Latin probe would discard real
    // non-Latin dictionaries (MASTER-1 M4).
    auto acceptsEverything = [](const SpellBackend &b) {
        static const QStringList nonsense = {
            QStringLiteral("qzxwvjkq"),
            QStringLiteral("\u0449\u0437\u0445\u044a\u0436\u043a\u0449"),   // Cyrillic
            QStringLiteral("\u03be\u03c8\u03b6\u03c6\u03c9\u03be\u03c8"),   // Greek
            QStringLiteral("\u05e5\u05e7\u05d6\u05e9\u05da\u05e5\u05e7"),   // Hebrew
            QStringLiteral("\u0636\u0638\u0630\u0634\u0636\u0638\u0630"),   // Arabic
            QStringLiteral("\u0919\u091e\u0922\u0923\u0919\u091e"),         // Devanagari
        };
        for (const QString &word : nonsense) {
            if (!b.isCorrect(word))
                return false;
        }
        return true;
    };

    QStringList languages;
    auto primary = create(m_preferredLanguage, &primaryFailure, &languages);
    addOffered(languages);
    if (primary)
        addOffered(primary->availableLanguages());
    if (primary && acceptsEverything(*primary)) {
        primary.reset();
        primaryFailure = QStringLiteral("no-dictionary");
    }
    if (primary) {
        next.push_back(std::move(primary));
    } else if (!m_preferredLanguage.isEmpty()) {
        m_missingLanguages << m_preferredLanguage;
    }
    // Automatic also checks the app's own language: a Lithuanian UI on an
    // English system types both. Silent when there is no dictionary for it.
    // Not built at all when an active dictionary already covers it ("en"
    // after the system's "en-US"): opening a dictionary is the costly part.
    auto covered = [&next](const QString &tag) {
        const bool regional = tag.contains(QLatin1Char('-'));
        for (const auto &b : next) {
            const QString have = b->language();
            if (regional ? have == tag
                         : have.section(QLatin1Char('-'), 0, 0) == tag)
                return true;
        }
        return false;
    };
    if (m_preferredLanguage.isEmpty() && !m_interfaceLanguage.isEmpty()
        && !covered(m_interfaceLanguage)) {
        QString ignored;
        languages.clear();
        auto ui = create(m_interfaceLanguage, &ignored, &languages);
        addOffered(languages);
        if (ui && !acceptsEverything(*ui) && !alreadyActive(*ui))
            next.push_back(std::move(ui));
    }
    for (const QString &tag : std::as_const(m_extraLanguages)) {
        QString failure;
        languages.clear();
        auto extra = create(tag, &failure, &languages);
        addOffered(languages);
        if (!extra) {
            if (!m_missingLanguages.contains(tag))
                m_missingLanguages << tag;
            continue;
        }
        if (acceptsEverything(*extra)) {
            if (!m_missingLanguages.contains(tag))
                m_missingLanguages << tag;
            continue;
        }
        if (!alreadyActive(*extra))
            next.push_back(std::move(extra));
    }

    m_unavailableReason = next.empty()
        ? (primaryFailure.isEmpty() ? QStringLiteral("no-dictionary") : primaryFailure)
        : QString();
    m_languages = offered;
    m_backends = std::move(next);
    // A different dictionary can answer differently for every word.
    m_cache.clear();
    m_transpositionCache.clear();
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

void SpellChecker::setExtraLanguages(const QStringList &tags)
{
    QStringList normalized;
    for (const QString &tag : tags) {
        const QString bcp47 = spellTagToBcp47(tag);
        if (!bcp47.isEmpty() && !normalized.contains(bcp47))
            normalized << bcp47;
    }
    if (normalized == m_extraLanguages)
        return;
    m_extraLanguages = normalized;
    resolve();
}

void SpellChecker::setInterfaceLanguage(const QString &tag)
{
    const QString normalized = spellTagToBcp47(tag);
    if (normalized == m_interfaceLanguage)
        return;
    m_interfaceLanguage = normalized;
    // Only Automatic consults it.
    if (m_preferredLanguage.isEmpty())
        resolve();
}

void SpellChecker::setAutocorrectEnabled(bool enabled)
{
    if (m_autocorrect == enabled)
        return;
    m_autocorrect = enabled;
    Q_EMIT autocorrectEnabledChanged();
}

bool SpellChecker::autocorrectDefault()
{
    return platformAutocorrectDefault();
}

QString SpellChecker::language() const
{
    return m_backends.empty() ? QString() : m_backends.front()->language();
}

QStringList SpellChecker::languages() const
{
    QStringList out;
    for (const auto &b : m_backends)
        if (!out.contains(b->language()))
            out << b->language();
    return out;
}

QString SpellChecker::languageLabel() const
{
    QStringList labels;
    for (const QString &tag : languages())
        labels << labelForTag(tag);
    return labels.join(QStringLiteral(", "));
}

QString SpellChecker::backendName() const
{
    QStringList names;
    for (const auto &b : m_backends)
        if (!names.contains(b->name()))
            names << b->name();
    return names.join(QLatin1Char('+'));
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
    // Territory only when the tag has one: "English (United Kingdom)", but
    // plain "Lithuanian" for "lt".
    if (bcp47.contains(QLatin1Char('-')) && locale.territory() != QLocale::AnyTerritory)
        label += QStringLiteral(" (") + QLocale::territoryToString(locale.territory())
            + QLatin1Char(')');
    return label;
}

bool SpellChecker::isAutocorrectBoundary(QChar c)
{
    return c.isSpace() || c == u',' || c == u';' || c == u'!' || c == u'?'
        || c == u')' || c == u'"' || c == QChar(0x201D);
}

bool SpellChecker::isAutocorrectBoundaryText(const QString &typed) const
{
    return typed.size() == 1 && isAutocorrectBoundary(typed.at(0));
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

void SpellChecker::installBackends(std::vector<std::unique_ptr<SpellBackend>> backends)
{
    m_backends.clear();
    for (auto &b : backends)
        if (b)
            m_backends.push_back(std::move(b));
    m_unavailableReason = m_backends.empty() ? QStringLiteral("no-dictionary") : QString();
    m_languages.clear();
    for (const auto &b : m_backends)
        for (const QString &tag : b->availableLanguages())
            if (!m_languages.contains(tag))
                m_languages << tag;
    m_cache.clear();
    m_transpositionCache.clear();
    Q_EMIT availabilityChanged();
}

void SpellChecker::setBackendForTest(std::unique_ptr<SpellBackend> backend)
{
    std::vector<std::unique_ptr<SpellBackend>> list;
    list.push_back(std::move(backend));
    installBackends(std::move(list));
}

void SpellChecker::setBackendsForTest(std::vector<std::unique_ptr<SpellBackend>> backends)
{
    installBackends(std::move(backends));
}

void SpellChecker::setBackendFactoryForTest(BackendFactory factory)
{
    m_factory = std::move(factory);
}

bool SpellChecker::inPersonalDictionary(const QString &word) const
{
    if (m_personal.isEmpty())
        return false;
    if (m_personal.contains(word))
        return true;
    // A personal "matrix" also covers "Matrix" at the start of a sentence,
    // the way a dictionary treats its own lowercase words.
    if (!word.isEmpty() && word.at(0).isUpper()) {
        const QString lowered = word.at(0).toLower() + word.mid(1);
        return m_personal.contains(lowered);
    }
    return false;
}

bool SpellChecker::wordIsCorrect(const QString &word) const
{
    if (m_ignored.contains(word) || inPersonalDictionary(word))
        return true;
    const auto cached = m_cache.constFind(word);
    if (cached != m_cache.constEnd())
        return cached.value();
    bool correct = m_backends.empty();
    for (const auto &b : m_backends) {
        ++m_lookups;
        if (b->isCorrect(word)) {
            correct = true;
            break;
        }
    }
    if (m_cache.size() >= kMaxCachedWords)
        m_cache.clear();
    m_cache.insert(word, correct);
    return correct;
}

QVariantList SpellChecker::misspelledRanges(const QString &text,
                                            int cursorPosition,
                                            const QVariantList &skipRanges,
                                            int windowStart,
                                            int windowEnd) const
{
    QVariantList out;
    if (m_backends.empty() || !m_enabled || text.isEmpty())
        return out;
    const bool windowed = windowStart >= 0 && windowEnd > windowStart;
    if (!windowed && text.size() > kMaxCheckedChars)
        return out;
    if (windowed && text.size() > kMaxScannedChars)
        return out;
    const int from = windowed ? std::min<int>(windowStart, text.size()) : 0;
    const int to = windowed
        ? std::min<int>({ windowEnd, int(text.size()), from + kMaxWindowChars })
        : -1;

    forEachWord(
        text,
        [&](int start, int length) {
            if (out.size() >= kMaxReportedRanges)
                return;
            // The caret anywhere in the word, including either edge, means it
            // is still being typed.
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
        },
        from, to);
    return out;
}

QVariantMap SpellChecker::wordAt(const QString &text, int position) const
{
    QVariantMap out{
        { QStringLiteral("word"), QString{} },
        { QStringLiteral("start"), -1 },
        { QStringLiteral("length"), 0 },
    };
    if (text.isEmpty() || text.size() > kMaxScannedChars || position < 0)
        return out;
    // Only the chunk around `position` matters; the window keeps a long draft
    // cheap.
    forEachWord(
        text,
        [&](int start, int length) {
            if (position < start || position > start + length)
                return;
            // At a boundary between two words, the first wins.
            if (out.value(QStringLiteral("start")).toInt() >= 0)
                return;
            out[QStringLiteral("word")] = text.mid(start, length);
            out[QStringLiteral("start")] = start;
            out[QStringLiteral("length")] = length;
        },
        std::max(0, position - 1), position + 1);
    return out;
}

QStringList SpellChecker::suggestions(const QString &word) const
{
    if (m_backends.empty() || word.isEmpty())
        return {};
    std::vector<QStringList> lists;
    for (const auto &b : m_backends)
        lists.push_back(b->suggest(word));
    // Interleaved: each language's best guess before anyone's second.
    QStringList out;
    for (int rank = 0; out.size() < 16; ++rank) {
        bool any = false;
        for (const QStringList &list : lists) {
            if (rank >= list.size())
                continue;
            any = true;
            if (!out.contains(list.at(rank)))
                out << list.at(rank);
            if (out.size() >= 16)
                break;
        }
        if (!any)
            break;
    }
    return out;
}

QStringList SpellChecker::personalWords() const
{
    QStringList out(m_personal.cbegin(), m_personal.cend());
    out.sort(Qt::CaseInsensitive);
    return out;
}

void SpellChecker::setPersonalDictionaryPath(const QString &path)
{
    m_personalPath = path;
    QSet<QString> loaded;
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // Bounded: a hand-edited or corrupt file cannot balloon memory.
            while (!file.atEnd() && loaded.size() < kMaxPersonalWords) {
                const QByteArray line = file.readLine();
                if (line.size() > 256)
                    continue; // not a word
                const QString word = normalizedWord(QString::fromUtf8(line));
                if (!word.isEmpty() && !word.startsWith(QLatin1Char('#')))
                    loaded.insert(word);
            }
        }
    }
    m_personal = loaded;
    Q_EMIT personalDictionaryChanged();
    Q_EMIT dictionaryChanged();
}

void SpellChecker::savePersonalDictionary() const
{
    if (m_personalPath.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_personalPath).absolutePath());
    QSaveFile file(m_personalPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QByteArray body;
    for (const QString &word : personalWords())
        body += word.toUtf8() + '\n';
    file.write(body);
    file.commit();
}

void SpellChecker::addToDictionary(const QString &word)
{
    const QString normalized = normalizedWord(word);
    if (normalized.isEmpty() || normalized.contains(QLatin1Char('\n'))
        || m_personal.contains(normalized)
        || m_personal.size() >= kMaxPersonalWords) {
        return;
    }
    m_personal.insert(normalized);
    savePersonalDictionary();
    m_cache.remove(normalized);
    Q_EMIT personalDictionaryChanged();
    Q_EMIT dictionaryChanged();
}

void SpellChecker::removeFromDictionary(const QString &word)
{
    if (!m_personal.remove(normalizedWord(word)))
        return;
    savePersonalDictionary();
    // Every cached answer about it, any case, is stale.
    m_cache.clear();
    Q_EMIT personalDictionaryChanged();
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

void SpellChecker::rejectAutocorrection(const QString &original)
{
    if (!original.isEmpty())
        m_rejectedCorrections.insert(original);
}

bool SpellChecker::onlyEnglishActive() const
{
    if (m_backends.empty())
        return false;
    for (const auto &b : m_backends)
        if (!isEnglish(b->language()))
            return false;
    return true;
}

bool SpellChecker::anyEnglishActive() const
{
    for (const auto &b : m_backends)
        if (isEnglish(b->language()))
            return true;
    return false;
}

// `replacement` when every word in it is accepted, else "".
QString SpellChecker::acceptedOrEmpty(const QString &replacement) const
{
    if (replacement.isEmpty())
        return {};
    const QStringList parts = replacement.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool accepted = false;
        for (const auto &b : m_backends) {
            ++m_lookups;
            if (b->isCorrect(part)) {
                accepted = true;
                break;
            }
        }
        if (!accepted)
            return {};
    }
    return replacement;
}

QVariantMap SpellChecker::autocorrection(const QString &text, int boundary,
                                         const QVariantList &skipRanges) const
{
    if (m_backends.empty() || !m_enabled || !m_autocorrect)
        return {};
    if (boundary <= 0 || boundary >= text.size() || text.size() > kMaxCheckedChars)
        return {};
    if (!isAutocorrectBoundary(text.at(boundary)))
        return {};
    // A /command's arguments are not prose.
    if (QStringView(text).trimmed().startsWith(QLatin1Char('/')))
        return {};

    auto result = [](int start, const QString &original, const QString &replacement) {
        return QVariantMap{
            { QStringLiteral("start"), start },
            { QStringLiteral("length"), original.size() },
            { QStringLiteral("original"), original },
            { QStringLiteral("replacement"), replacement },
        };
    };

    // A lone "i" is a single letter, which forEachWord never reports.
    const int iAt = boundary - 1;
    if (text.at(iAt) == u'i' && (iAt == 0 || text.at(iAt - 1).isSpace())) {
        if (!onlyEnglishActive() || m_rejectedCorrections.contains(QStringLiteral("i"))
            || (!skipRanges.isEmpty() && overlapsAnyRange(iAt, 1, skipRanges))) {
            return {};
        }
        // Inside code or a link destination it stays as typed.
        const Exclusions excluded(excludedSpans(text));
        if (excluded.overlaps(iAt, 1))
            return {};
        return result(iAt, QStringLiteral("i"), QStringLiteral("I"));
    }

    // The word ending exactly at the boundary, through the same rules as the
    // underline (so code, URLs, mentions, shortcodes are never touched).
    int start = -1;
    int length = 0;
    int chunkStart = boundary;
    while (chunkStart > 0 && !text.at(chunkStart - 1).isSpace())
        --chunkStart;
    forEachWord(
        text,
        [&](int s, int l) {
            if (s + l == boundary) {
                start = s;
                length = l;
            }
        },
        chunkStart, boundary);
    if (start < 0)
        return {};
    if (!skipRanges.isEmpty() && overlapsAnyRange(start, length, skipRanges))
        return {};
    const QString original = text.mid(start, length);
    if (m_rejectedCorrections.contains(original) || wordIsCorrect(original))
        return {};
    const CasePattern pattern = casePatternOf(original);
    if (pattern == CasePattern::Other)
        return {};
    const QString lower = original.toLower();

    // 1. The English typo table. A typographic apostrophe (macOS and phone
    // keyboards type U+2019) matches the table's ASCII one and is kept.
    if (anyEnglishActive()) {
        const bool curly = lower.contains(QChar(0x2019));
        QString key = lower;
        key.replace(QChar(0x2019), QLatin1Char('\''));
        const auto hit = englishTypos().constFind(key);
        if (hit != englishTypos().constEnd()) {
            QString fix = withCasePattern(hit.value(), pattern);
            if (acceptedOrEmpty(fix).isEmpty())
                fix.clear();
            if (curly)
                fix.replace(QLatin1Char('\''), QChar(0x2019));
            if (!fix.isEmpty())
                return result(start, original, fix);
        }
    }

    // 2. The platform's own autocorrect, when it has one.
    bool native = false;
    for (const auto &b : m_backends) {
        if (!b->hasNativeAutoCorrection())
            continue;
        native = true;
        const QString fix = b->autoCorrection(original);
        if (!fix.isEmpty() && fix != original)
            return result(start, original, fix);
    }
    if (native)
        return {}; // the platform said no; do not second-guess it

    // 3. Only an adjacent transposition ("wrold" -> "world"), only for an
    // all-lowercase word, and only when exactly ONE swap of two neighbouring
    // letters gives a word the dictionaries accept. Deliberately narrow (lead
    // decision, review round 1): measured with a real en_US dictionary, the
    // engine's best one-edit suggestion turns haha into aha, async into sync,
    // hmmm into hmm and lolz into loll, and a Capitalised word is usually a
    // name (Lukas, Matrixy). Checked with isCorrect() -- never suggest(),
    // which costs 18-200 ms per dictionary on the GUI thread -- and the answer,
    // "none" included, is cached per lowercase word until the dictionaries
    // change.
    if (pattern != CasePattern::Lower || length < 4 || length > 24)
        return {};
    const QString replacement = transpositionFor(lower);
    if (replacement.isEmpty())
        return {};
    return result(start, original, replacement);
}

QString SpellChecker::transpositionFor(const QString &lower) const
{
    const auto cached = m_transpositionCache.constFind(lower);
    if (cached != m_transpositionCache.constEnd())
        return cached.value();
    QString candidate;
    bool ambiguous = false;
    for (int i = 0; i + 1 < lower.size() && !ambiguous; ++i) {
        if (lower.at(i) == lower.at(i + 1))
            continue;
        QString swapped = lower;
        swapped[i] = lower.at(i + 1);
        swapped[i + 1] = lower.at(i);
        if (swapped == candidate)
            continue;
        bool accepted = false;
        for (const auto &b : m_backends) {
            ++m_lookups;
            if (b->isCorrect(swapped)) {
                accepted = true;
                break;
            }
        }
        if (!accepted)
            continue;
        if (candidate.isEmpty())
            candidate = swapped;
        else
            ambiguous = true;
    }
    const QString answer = ambiguous ? QString() : candidate;
    if (m_transpositionCache.size() >= kMaxTranspositionCache)
        m_transpositionCache.clear();
    m_transpositionCache.insert(lower, answer);
    return answer;
}
