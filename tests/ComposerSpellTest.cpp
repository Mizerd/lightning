#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "app/TrayIcon.h"
#include "text/SpellBackend.h"
#include "text/SpellChecker.h"

namespace {

// A dictionary that knows exactly what a case needs it to know, and COUNTS
// what it was asked. The count is half the point: the policy this suite
// guards is mostly about words the checker must never ask about at all, and
// "no range came back" is also what a broken tokenizer produces.
class FakeBackend : public SpellBackend
{
public:
    explicit FakeBackend(QStringList known)
        : m_known(std::move(known))
    {
    }

    bool isCorrect(const QString &word) const override
    {
        m_asked << word;
        return m_known.contains(word, Qt::CaseInsensitive);
    }
    QStringList suggest(const QString &word) const override
    {
        if (word == QStringLiteral("teh"))
            return { QStringLiteral("the"), QStringLiteral("ten") };
        return {};
    }
    void addToPersonalDictionary(const QString &word) override
    {
        m_added << word;
        m_known << word;
    }
    QString language() const override { return m_language; }
    QStringList availableLanguages() const override
    { return { QStringLiteral("en-US"), QStringLiteral("lt-LT"), QStringLiteral("en-GB") }; }
    QString name() const override { return QStringLiteral("fake"); }

    mutable QStringList m_asked;
    QStringList m_added;
    QStringList m_known;
    QString m_language = QStringLiteral("en-US");
};

// Convenience: the checker plus a borrowed pointer to the backend inside it.
struct Fixture
{
    SpellChecker checker;
    FakeBackend *backend = nullptr;

    explicit Fixture(QStringList known = { QStringLiteral("hello"),
                                           QStringLiteral("world"),
                                           QStringLiteral("message"),
                                           QStringLiteral("the") })
    {
        auto owned = std::make_unique<FakeBackend>(std::move(known));
        backend = owned.get();
        checker.setBackendForTest(std::move(owned));
    }
};

QString qmlSource(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR) + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// CODE LINES ONLY, and this is not fussiness: the first version of the ban
// below failed on a correct tree because the composer's own comment NAMES
// the flag it must never set. That is the fourth time a scan in this
// repository has matched a token in a comment.
//
// Whole-line only, deliberately. A "strip trailing comments" regex is a
// parser — `//` appears inside string literals (every URL has one) and a
// negated character class happily eats newlines — so this drops a line only
// when it BEGINS with a comment marker, which is what every explanatory
// block in these two files looks like. It cannot silently swallow code.
QString qmlCodeLines(const QString &source)
{
    QStringList kept;
    const QStringList lines = source.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("//"))
            || trimmed.startsWith(QLatin1String("/*"))
            || trimmed.startsWith(QLatin1Char('*'))) {
            continue;
        }
        kept << line;
    }
    return kept.join(QLatin1Char('\n'));
}

} // namespace

// Two things this suite defends, and they are different in kind.
//
// 1. THE COMPOSER MUST NEVER TELL THE PLATFORM TO STOP PREDICTING.
//    `Qt.ImhNoPredictiveText` and `Qt.ImhSensitiveData` each switch off the
//    platform's own prediction, autocorrect and IME learning. Neither has
//    ever been set on either composer, and this suite is what keeps that
//    from being an accident. It is a source scan, so it holds for the
//    composers as written rather than for one instantiated in a harness.
//
// 2. WHAT THE SPELL CHECKER IS ALLOWED TO ASK ABOUT. A chat message is full
//    of correctly-spelled things that are not words — mxids, aliases, URLs,
//    code, versions — and a checker that underlines them is one the user
//    turns off. Every skip rule here is a rule a dictionary would otherwise
//    reject.
class ComposerSpellTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // ---- 1: input method hints ----------------------------------------

    // ── v0.9 spell-checking round: exclusions, Unicode ranges, language ──

    void codeIsNeverChecked()
    {
        Fixture f({ QStringLiteral("hello"), QStringLiteral("and"), QStringLiteral("then"),
                    QStringLiteral("twice") });
        const QString text = QStringLiteral(
            "`teh` and ``teh `` twice``\n```\ninside teh\n```\nand `teh in span` then teh");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value(QStringLiteral("start")).toInt(),
                 text.lastIndexOf(QStringLiteral("teh")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("inside")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("span")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("twice")));
    }

    void richParagraphSeparatorsEndALineTooAndAnInlineSpanIsNotAFence()
    {
        Fixture f({ QStringLiteral("hello") });
        // A rich TextEdit's getText() separates paragraphs with U+2029, not
        // '\n'. A lone backtick in one paragraph must not swallow the next.
        const QString rich = QStringLiteral("a ` stray") + QChar::ParagraphSeparator
            + QStringLiteral("teh");
        const QVariantList ranges = f.checker.misspelledRanges(rich);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value(QStringLiteral("start")).toInt(),
                 rich.indexOf(QStringLiteral("teh")));
        // ```teh``` on its own line is an inline span (CommonMark: an info
        // string may not contain backticks), not a fence over the rest.
        const QString span = QStringLiteral("```teh```\nteh");
        const QVariantList afterSpan = f.checker.misspelledRanges(span);
        QCOMPARE(afterSpan.size(), 1);
        QCOMPARE(afterSpan.first().toMap().value(QStringLiteral("start")).toInt(),
                 span.lastIndexOf(QStringLiteral("teh")));
    }

    void anUnterminatedFenceExcludesTheRestOfTheDraft()
    {
        Fixture f;
        QCOMPARE(f.checker.misspelledRanges(QStringLiteral("hello\n```\nteh\nteh")).size(), 0);
    }

    void markdownLinkDestinationsAreSkippedButTheirTextIsChecked()
    {
        Fixture f({ QStringLiteral("hello"), QStringLiteral("docs"), QStringLiteral("see"),
                    QStringLiteral("and") });
        const QString text =
            QStringLiteral("see [teh docs](https://example.org/teh) and [teh](docs) teh");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QList<int> starts;
        for (const QVariant &v : ranges)
            starts << v.toMap().value(QStringLiteral("start")).toInt();
        QCOMPARE(starts, (QList<int>{ 5, 45, 56 }));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("docs")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("example")));
    }

    void domainsPathsCommandsAndShortcodesAreNotWords()
    {
        Fixture f;
        const QString text = QStringLiteral(
            "/topic teh matrix.org notes.txt /usr/teh ~/teh :teh: @teh:x.org #teh:x.org teh");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QCOMPARE(ranges.size(), 2);
        QCOMPARE(ranges.first().toMap().value(QStringLiteral("start")).toInt(), 7);
        QCOMPARE(ranges.last().toMap().value(QStringLiteral("start")).toInt(),
                 text.lastIndexOf(QStringLiteral("teh")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("org")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("txt")));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("topic")));
    }

    void strikeThroughAndEmphasisMarkersDoNotHideTheWord()
    {
        Fixture f;
        const QString text = QStringLiteral("~~teh~~ **teh** *teh* _teh_ hello");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QList<int> starts;
        for (const QVariant &v : ranges)
            starts << v.toMap().value(QStringLiteral("start")).toInt();
        // `_teh_` is deliberately NOT checked: an underscore marks an
        // identifier far more often than emphasis in a chat.
        QCOMPARE(starts, (QList<int>{ 2, 10, 17 }));
    }

    void emojiAndNonBmpTextDoNotShiftTheRanges()
    {
        Fixture f;
        // U+1F600 and U+1D518 are surrogate pairs: two UTF-16 units each.
        const QString text = QString::fromUtf8("😀 teh 😀teh😀 𝔘 teh");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QCOMPARE(ranges.size(), 3);
        int from = 0;
        for (const QVariant &v : ranges) {
            const QVariantMap r = v.toMap();
            const int expected = text.indexOf(QStringLiteral("teh"), from);
            QCOMPARE(r.value(QStringLiteral("start")).toInt(), expected);
            QCOMPARE(r.value(QStringLiteral("length")).toInt(), 3);
            QCOMPARE(text.mid(expected, 3), QStringLiteral("teh"));
            from = expected + 3;
        }
    }

    void lithuanianCombiningMarksAndParagraphsAreWholeWords()
    {
        Fixture f({ QString::fromUtf8("ąžuolas"), QStringLiteral("hello") });
        const QString decomposed = QStringLiteral("cafe") + QChar(0x0301);
        const QString text = QString::fromUtf8("ąžuolas teh\n\n") + decomposed
            + QStringLiteral(" teh\nhello");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QCOMPARE(ranges.size(), 3);
        QVERIFY(f.backend->m_asked.contains(decomposed));
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("cafe")));
        QCOMPARE(f.backend->m_asked.count(QString::fromUtf8("ąžuolas")), 1);
    }

    void changingTheLanguageRecreatesTheBackendAndForgetsEveryAnswer()
    {
        SpellChecker checker;
        QStringList requested;
        checker.setBackendFactoryForTest([&requested](const QString &tag, QString *failure,
                                                      QStringList *offered)
                                             -> std::unique_ptr<SpellBackend> {
            requested << tag;
            *offered = { QStringLiteral("en-US"), QStringLiteral("lt-LT") };
            if (tag == QStringLiteral("xx-XX")) {
                *failure = QStringLiteral("no-dictionary");
                return {};
            }
            auto b = std::make_unique<FakeBackend>(
                QStringList{ tag == QStringLiteral("lt-LT") ? QStringLiteral("labas")
                                                            : QStringLiteral("hello") });
            b->m_language = tag.isEmpty() ? QStringLiteral("en-US") : tag;
            return b;
        });
        checker.initialize();
        QCOMPARE(requested, QStringList{ QString() });
        QVERIFY(checker.available());
        QCOMPARE(checker.preferredLanguage(), QString());
        QCOMPARE(checker.language(), QStringLiteral("en-US"));
        QCOMPARE(checker.misspelledRanges(QStringLiteral("labas hello")).size(), 1);

        QSignalSpy dictionary(&checker, &SpellChecker::dictionaryChanged);
        checker.setPreferredLanguage(QStringLiteral("lt_LT")); // POSIX in, BCP-47 kept
        QCOMPARE(checker.preferredLanguage(), QStringLiteral("lt-LT"));
        QCOMPARE(requested.last(), QStringLiteral("lt-LT"));
        QCOMPARE(dictionary.size(), 1);
        const QVariantList ranges = checker.misspelledRanges(QStringLiteral("labas hello"));
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value(QStringLiteral("start")).toInt(), 6);
        QCOMPARE(checker.languageLabel(), QStringLiteral("Lithuanian (Lithuania)"));

        const QVariantList options = checker.languageOptions();
        QVERIFY(options.size() >= 4);
        QCOMPARE(options.first().toMap().value(QStringLiteral("tag")).toString(), QString());
        QStringList labels;
        for (const QVariant &v : options)
            labels << v.toMap().value(QStringLiteral("label")).toString();
        QVERIFY(labels.contains(QStringLiteral("English (United Kingdom)")));
        QVERIFY(labels.contains(QStringLiteral("English (United States)")));
        QVERIFY(labels.contains(QStringLiteral("Lithuanian (Lithuania)")));

        checker.setPreferredLanguage(QStringLiteral("xx-XX"));
        QVERIFY(!checker.available());
        QCOMPARE(checker.unavailableReason(), QStringLiteral("no-dictionary"));
        QVERIFY(checker.misspelledRanges(QStringLiteral("labas hello")).isEmpty());
        // The picker still offers what the platform CAN check, so the user
        // has a way out of a preference the machine cannot honour.
        QCOMPARE(checker.availableLanguages(),
                 (QStringList{ QStringLiteral("en-US"), QStringLiteral("lt-LT") }));
        QCOMPARE(checker.languageOptions().size(), 3);
        QCOMPARE(checker.preferredLanguage(), QStringLiteral("xx-XX"));
        const int before = requested.size();
        checker.setPreferredLanguage(QStringLiteral("xx_XX"));
        QCOMPARE(requested.size(), before);
    }

    void disablingRemovesEveryRangeAndEnablingBringsThemBack()
    {
        Fixture f;
        const QString text = QStringLiteral("teh hello teh");
        QCOMPARE(f.checker.misspelledRanges(text).size(), 2);
        f.checker.setEnabled(false);
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
        f.checker.setEnabled(true);
        QCOMPARE(f.checker.misspelledRanges(text).size(), 2);
    }

    void aPathologicalDraftIsNotCheckedAtAll()
    {
        Fixture f;
        const QString text =
            QStringLiteral("teh ").repeated(SpellChecker::kMaxCheckedChars / 4 + 8);
        QVERIFY(text.size() > SpellChecker::kMaxCheckedChars);
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
        QVERIFY(f.backend->m_asked.isEmpty());
    }

    void labelsNameLanguagesTheWayAPickerShould()
    {
        QCOMPARE(SpellChecker::labelForTag(QStringLiteral("lt")), QStringLiteral("Lithuanian"));
        QCOMPARE(SpellChecker::labelForTag(QStringLiteral("en_GB")),
                 QStringLiteral("English (United Kingdom)"));
        QCOMPARE(SpellChecker::labelForTag(QStringLiteral("de-DE")),
                 QStringLiteral("German (Germany)"));
        QCOMPARE(SpellChecker::labelForTag(QString()), QString());
    }

    void neitherComposerSuppressesPlatformPrediction()
    {
        const QStringList files = { QStringLiteral("MessageComposerBar.qml"),
                                    QStringLiteral("ThreadPanel.qml") };
        int found = 0;
        for (const QString &name : files) {
            const QString raw = qmlSource(name);
            QVERIFY2(!raw.isEmpty(),
                     qPrintable(QStringLiteral("could not read %1").arg(name)));
            const QString source = qmlCodeLines(raw);
            // Anchored on the EXPRESSION, never on a window of characters
            // after a name: a comment added above the declaration must not
            // be able to move the assertion off its target. The presence of
            // the declaration is also the stripper's own found>0 guard — a
            // stripper that had eaten the file would fail here rather than
            // pass the two bans vacuously.
            QVERIFY2(source.contains(QStringLiteral("inputMethodHints:")),
                     qPrintable(QStringLiteral(
                         "%1 no longer declares inputMethodHints; the ban "
                         "below would then be vacuous").arg(name)));
            ++found;
            QVERIFY2(!source.contains(QStringLiteral("ImhNoPredictiveText")),
                     qPrintable(QStringLiteral(
                         "%1 disables platform text prediction").arg(name)));
            QVERIFY2(!source.contains(QStringLiteral("ImhSensitiveData")),
                     qPrintable(QStringLiteral(
                         "%1 marks the composer sensitive, which also stops "
                         "prediction and IME learning").arg(name)));
        }
        QCOMPARE(found, static_cast<int>(files.size()));
    }

    // ---- 2: what gets asked, and what never does ------------------------

    void aMisspelledWordIsReportedWithItsExactRange()
    {
        Fixture f;
        const QString text = QStringLiteral("hello wrold");
        const QVariantList ranges = f.checker.misspelledRanges(text);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value("start").toInt(), 6);
        QCOMPARE(ranges.first().toMap().value("length").toInt(), 5);
        QCOMPARE(text.mid(6, 5), QStringLiteral("wrold"));
    }

    void thingsThatAreNotWordsAreNeverEvenAsked_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("token");
        // Each row is a real thing a Matrix message carries, containing one
        // token a dictionary would certainly reject. The token must never be
        // looked up at all — not merely go un-underlined.
        QTest::newRow("url")
            << QStringLiteral("see https://smetoniss.net/qq now")
            << QStringLiteral("smetoniss");
        QTest::newRow("mxid")
            << QStringLiteral("ping @rokazz:smetonis.net please")
            << QStringLiteral("rokazz");
        QTest::newRow("alias")
            << QStringLiteral("join #desiggn:smetonis.net today")
            << QStringLiteral("desiggn");
        QTest::newRow("room id")
            << QStringLiteral("the room !abcdefgh:server here")
            << QStringLiteral("abcdefgh");
        QTest::newRow("emoji shortcode")
            << QStringLiteral("nice :thumbsuup: yes")
            << QStringLiteral("thumbsuup");
        QTest::newRow("code span")
            << QStringLiteral("run `qmlformaat` first")
            << QStringLiteral("qmlformaat");
        QTest::newRow("path")
            << QStringLiteral("in docs/relaese/notes today")
            << QStringLiteral("relaese");
        QTest::newRow("home path")
            << QStringLiteral("open ~/lightnink/config now")
            << QStringLiteral("lightnink");
        QTest::newRow("version")
            << QStringLiteral("on 0.8.0-beeta now")
            << QStringLiteral("beeta");
        QTest::newRow("identifier")
            << QStringLiteral("the my_speling_flag member")
            << QStringLiteral("speling");
        QTest::newRow("e-mail")
            << QStringLiteral("mail rokkas@example.com now")
            << QStringLiteral("rokkas");
        QTest::newRow("acronym")
            << QStringLiteral("the HTTPP header")
            << QStringLiteral("HTTPP");
        QTest::newRow("CamelCase")
            << QStringLiteral("call refreshTrayyState now")
            << QStringLiteral("refreshTrayyState");
        QTest::newRow("single letter")
            << QStringLiteral("a b c q") << QStringLiteral("q");
    }

    void thingsThatAreNotWordsAreNeverEvenAsked()
    {
        QFETCH(QString, text);
        QFETCH(QString, token);
        // A dictionary that knows NOTHING, so the only reason a token can go
        // unreported is that the tokenizer refused to ask about it.
        Fixture f(QStringList{});
        // The control word is the found>0 guard: it proves this text was
        // actually walked, so a tokenizer that had quietly stopped producing
        // words could not pass every row.
        const QString probe = text + QStringLiteral(" wrold");
        const QVariantList ranges = f.checker.misspelledRanges(probe);
        QVERIFY2(f.backend->m_asked.contains(QStringLiteral("wrold")),
                 "the control word was not looked up: this row proves nothing");
        QVERIFY2(!f.backend->m_asked.contains(token),
                 qPrintable(QStringLiteral("looked up \"%1\"").arg(token)));
        for (const QVariant &range : ranges) {
            const QString word = probe.mid(range.toMap().value("start").toInt(),
                                           range.toMap().value("length").toInt());
            QVERIFY2(word != token,
                     qPrintable(QStringLiteral("underlined \"%1\"").arg(token)));
        }
    }

    void theWordUnderTheCaretIsNotUnderlinedWhileItIsBeingTyped()
    {
        Fixture f;
        const QString text = QStringLiteral("hello wrold");
        // Caret at the end of "wrold": still being typed.
        QVERIFY(f.checker.misspelledRanges(text, 11).isEmpty());
        // Caret inside it: same.
        QVERIFY(f.checker.misspelledRanges(text, 8).isEmpty());
        // Caret at its leading edge counts as inside, because a Backspace
        // from there is still editing this word.
        QVERIFY(f.checker.misspelledRanges(text, 6).isEmpty());
        // Caret elsewhere: reported.
        QCOMPARE(f.checker.misspelledRanges(text, 2).size(), 1);
        // And with no caret at all.
        QCOMPARE(f.checker.misspelledRanges(text, -1).size(), 1);
    }

    void aMentionRangeIsNeverSpellChecked()
    {
        Fixture f;
        // A display name is a person's name, not English, and the composer
        // keeps mentions as plain text with a semantic range over them.
        // The fixture's dictionary knows none of these, so all four words are
        // rejected without the mention range; with it, only the two outside
        // it are. Both halves are asserted, so a skip rule that dropped
        // EVERYTHING would fail here rather than look like a pass.
        const QString text = QStringLiteral("Rokas Smetoniss said so");
        QCOMPARE(f.checker.misspelledRanges(text).size(), 4);
        const QVariantList mention{ QVariantMap{ { "start", 0 },
                                                 { "length", 15 } } };
        const QVariantList kept = f.checker.misspelledRanges(text, -1, mention);
        QCOMPARE(kept.size(), 2);
        QCOMPARE(text.mid(kept.at(0).toMap().value("start").toInt(),
                          kept.at(0).toMap().value("length").toInt()),
                 QStringLiteral("said"));
    }

    void wordAtNamesTheWordUnderAPosition()
    {
        Fixture f;
        const QString text = QStringLiteral("hello wrold there");
        const QVariantMap hit = f.checker.wordAt(text, 8);
        QCOMPARE(hit.value("word").toString(), QStringLiteral("wrold"));
        QCOMPARE(hit.value("start").toInt(), 6);
        QCOMPARE(hit.value("length").toInt(), 5);
        // A position at a word's trailing edge still names that word — a
        // right-click just past the last letter means that word, and so does
        // a caret sitting there.
        QCOMPARE(f.checker.wordAt(text, 5).value("word").toString(),
                 QStringLiteral("hello"));
        // A position genuinely in whitespace names nothing rather than
        // guessing at the nearest word.
        const QString spaced = QStringLiteral("hello   wrold");
        QCOMPARE(f.checker.wordAt(spaced, 6).value("word").toString(),
                 QString{});
        QCOMPARE(f.checker.wordAt(spaced, 6).value("start").toInt(), -1);
    }

    void addingAWordConsultsThePlatformAndClearsTheStaleAnswer()
    {
        Fixture f;
        const QString text = QStringLiteral("hello Lightnink");
        QCOMPARE(f.checker.misspelledRanges(text).size(), 1);
        QSignalSpy spy(&f.checker, &SpellChecker::dictionaryChanged);
        f.checker.addToDictionary(QStringLiteral("Lightnink"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.backend->m_added,
                 QStringList{ QStringLiteral("Lightnink") });
        // The cached "wrong" must not survive: this is the whole reason the
        // cache is cleared rather than one key removed.
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
    }

    void ignoringAWordIsSessionOnlyAndTouchesNoDictionary()
    {
        Fixture f;
        const QString text = QStringLiteral("hello Lightnink");
        QCOMPARE(f.checker.misspelledRanges(text).size(), 1);
        f.checker.ignoreWord(QStringLiteral("Lightnink"));
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
        QVERIFY2(f.backend->m_added.isEmpty(),
                 "Ignore must not write the user's platform dictionary");
    }

    void oneWordIsLookedUpOnceHoweverOftenItIsTyped()
    {
        Fixture f;
        // A composer re-checks the whole draft on every keystroke; the
        // dictionary lookup is the expensive half of that.
        const QString text =
            QStringLiteral("wrold wrold wrold wrold wrold");
        f.checker.misspelledRanges(text);
        f.checker.misspelledRanges(text);
        QCOMPARE(f.backend->m_asked.count(QStringLiteral("wrold")), 1);
    }

    void aDisabledOrUnavailableCheckerReportsNothing()
    {
        Fixture f;
        f.checker.setEnabled(false);
        QVERIFY(f.checker.misspelledRanges(QStringLiteral("wrold")).isEmpty());
        f.checker.setEnabled(true);
        QCOMPARE(f.checker.misspelledRanges(QStringLiteral("wrold")).size(), 1);

        SpellChecker bare;
        QVERIFY(!bare.available());
        QVERIFY(bare.misspelledRanges(QStringLiteral("wrold")).isEmpty());
        QVERIFY(bare.suggestions(QStringLiteral("wrold")).isEmpty());
    }

    void suggestionsComeFromThePlatformAndNowhereElse()
    {
        Fixture f;
        QCOMPARE(f.checker.suggestions(QStringLiteral("teh")),
                 (QStringList{ QStringLiteral("the"), QStringLiteral("ten") }));
        QVERIFY(f.checker.suggestions(QStringLiteral("qqq")).isEmpty());
    }

    // ---- 3: the tray badge --------------------------------------------

    void theTrayBadgeSaysOnlyWhatIsKnown_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("anyUnread");
        QTest::addColumn<QString>("label");
        QTest::newRow("nothing") << 0 << false << QString{};
        QTest::newRow("countless") << 0 << true << QStringLiteral("•");
        QTest::newRow("one") << 1 << true << QStringLiteral("1");
        QTest::newRow("nine") << 9 << true << QStringLiteral("9");
        QTest::newRow("ten") << 10 << true << QStringLiteral("9+");
        QTest::newRow("many") << 412 << true << QStringLiteral("9+");
    }

    void theTrayBadgeSaysOnlyWhatIsKnown()
    {
        QFETCH(int, count);
        QFETCH(bool, anyUnread);
        QFETCH(QString, label);
        QCOMPARE(TrayIcon::badgeLabel(count, anyUnread), label);
    }

    void aBusyRoomDoesNotRepaintTheTrayIconPerMessage()
    {
        // The recorded objection to a badge was that it is "a per-message
        // rasterization for a number the tooltip already carries". The
        // answer is this: above nine, the drawn string stops changing, so
        // the repaint condition — the label changed — is false.
        QCOMPARE(TrayIcon::badgeLabel(40, true), TrayIcon::badgeLabel(41, true));
        QCOMPARE(TrayIcon::badgeLabel(10, true),
                 TrayIcon::badgeLabel(9999, true));
        // ...and it IS still true across the boundaries that matter.
        QVERIFY(TrayIcon::badgeLabel(9, true) != TrayIcon::badgeLabel(10, true));
        QVERIFY(TrayIcon::badgeLabel(0, true) != TrayIcon::badgeLabel(0, false));
        QVERIFY(TrayIcon::badgeLabel(0, true) != TrayIcon::badgeLabel(1, true));
    }
};

QTEST_GUILESS_MAIN(ComposerSpellTest)
#include "ComposerSpellTest.moc"
