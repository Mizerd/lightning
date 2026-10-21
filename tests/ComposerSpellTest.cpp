#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>
#include <QRegularExpression>
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

// A dictionary that knows exactly what a case needs and counts lookups: most
// of the policy is about words the checker must never ask about, and "no
// range came back" is also what a broken tokenizer produces.
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
        m_suggestedFor << word;
        if (m_suggestions.contains(word))
            return m_suggestions.value(word);
        if (word == QStringLiteral("teh"))
            return { QStringLiteral("the"), QStringLiteral("ten") };
        return {};
    }
    QString language() const override { return m_language; }
    QStringList availableLanguages() const override
    { return { QStringLiteral("en-US"), QStringLiteral("lt-LT"), QStringLiteral("en-GB") }; }
    QString name() const override { return QStringLiteral("fake"); }
    QString autoCorrection(const QString &word) const override
    {
        return m_native.value(word);
    }
    bool hasNativeAutoCorrection() const override { return m_hasNative; }

    mutable QStringList m_asked;
    mutable QStringList m_suggestedFor;
    QStringList m_known;
    QHash<QString, QStringList> m_suggestions;
    QHash<QString, QString> m_native;
    bool m_hasNative = false;
    QString m_language = QStringLiteral("en-US");
};

// The checker plus a borrowed pointer to its backend.
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

// A platform with English and Lithuanian dictionaries; "" (Automatic) is the
// system language, English here.
SpellChecker::BackendFactory languageFactory(QStringList *requested = nullptr)
{
    return [requested](const QString &tag, QString *failure,
                       QStringList *offered) -> std::unique_ptr<SpellBackend> {
        if (requested)
            *requested << tag;
        *offered = { QStringLiteral("en-US"), QStringLiteral("lt-LT") };
        const QString lang = tag.section(QLatin1Char('-'), 0, 0);
        if (tag.isEmpty() || lang == QStringLiteral("en")) {
            auto b = std::make_unique<FakeBackend>(QStringList{ QStringLiteral("hello") });
            b->m_language = QStringLiteral("en-US");
            return b;
        }
        if (lang == QStringLiteral("lt")) {
            auto b = std::make_unique<FakeBackend>(QStringList{ QStringLiteral("labas") });
            b->m_language = QStringLiteral("lt-LT");
            return b;
        }
        *failure = QStringLiteral("no-dictionary");
        return {};
    };
}

QString qmlSource(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR) + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// Code lines only: the composer's own comment names the flag it must never
// set. Whole-line only, deliberately: a trailing-comment stripper is a parser
// (`//` appears in URLs), so this drops only lines that begin with a comment
// marker and can never swallow code.
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

// Two different things are defended:
// 1. The composers never set `Qt.ImhNoPredictiveText` or
//    `Qt.ImhSensitiveData`, which switch off the platform's prediction,
//    autocorrect and IME learning. A source scan, so it holds for the
//    composers as written.
// 2. What the spell checker may ask about: mxids, aliases, URLs, code and
//    versions are correct but not words, and underlining them makes users
//    turn the checker off.
class ComposerSpellTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // ---- 1: input method hints ----

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
        // '\n'; a lone backtick in one paragraph must not swallow the next.
        const QString rich = QStringLiteral("a ` stray") + QChar::ParagraphSeparator
            + QStringLiteral("teh");
        const QVariantList ranges = f.checker.misspelledRanges(rich);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value(QStringLiteral("start")).toInt(),
                 rich.indexOf(QStringLiteral("teh")));
        // ```teh``` on its own line is an inline span (a CommonMark info
        // string may not contain backticks), not a fence.
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
        // `_teh_` is not checked: underscores mark identifiers more often than
        // emphasis in chat.
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
        // The picker still offers what the platform can check, so the user has
        // a way out of a preference the machine cannot honour.
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

    // The command popup is a clipped, scrollable list whose height covers its
    // rows; a Column overflowed below the panel.
    void theCommandPopupClipsItsRowsAndCanScroll()
    {
        const QString popup = qmlCodeLines(qmlSource(QStringLiteral("SlashCommandPopup.qml")));
        QVERIFY2(!popup.isEmpty(), "SlashCommandPopup.qml missing");
        // A real list, not a Column that can overflow.
        QVERIFY(popup.contains(QStringLiteral("ListView {")));
        QVERIFY(popup.contains(QStringLiteral("id: commandList")));
        QVERIFY(popup.contains(QStringLiteral("clip: true")));
        QVERIFY(popup.contains(QStringLiteral("ScrollBar.vertical: AppScrollBar")));
        // The keyboard drives the list, and the list follows it.
        QVERIFY(popup.contains(QStringLiteral(
            "commandList.positionViewAtIndex(currentIndex, ListView.Contain)")));
        // The row height covers both lines a row draws.
        const QRegularExpression rowH(
            QStringLiteral("rowH:\\s*AppTheme\\.scaled\\((\\d+)\\)"));
        const QRegularExpressionMatch m = rowH.match(popup);
        QVERIFY2(m.hasMatch(), "rowH is not declared as a scaled constant");
        QVERIFY2(m.captured(1).toInt() >= 48,
                 qPrintable(QStringLiteral("rowH %1 is too short for a two-line row")
                                .arg(m.captured(1))));
    }

    // The two rich-only toolbar chips are chips, not 72px AppButton gaps.
    void theRichToolbarChipsAreChipsNotButtonWidthGaps()
    {
        const QString bar = qmlCodeLines(qmlSource(QStringLiteral("MessageComposerBar.qml")));
        QVERIFY2(!bar.isEmpty(), "MessageComposerBar.qml missing");
        for (const QString &name : { QStringLiteral("composerFormat_underline"),
                                     QStringLiteral("composerFormat_orderedlist") }) {
            const int at = bar.indexOf(QStringLiteral("objectName: \"") + name);
            QVERIFY2(at > 0, qPrintable(name));
            // Anchored on the declaration, not a fixed window.
            const int end = bar.indexOf(QStringLiteral("onClicked"), at);
            QVERIFY(end > at);
            QVERIFY2(bar.mid(at, end - at).contains(QStringLiteral("minWidth: 0")),
                     qPrintable(name + QStringLiteral(" still takes the 72px button minimum")));
        }
    }

    // The rich editor's placeholder hides as soon as the document draws
    // anything; an empty list item has no characters but still shows "1.".
    void bothRichComposersGateTheirPlaceholderOnTheDocument()
    {
        const QString bar = qmlCodeLines(qmlSource(QStringLiteral("MessageComposerBar.qml")));
        const QString thread = qmlCodeLines(qmlSource(QStringLiteral("ThreadPanel.qml")));
        QVERIFY(bar.contains(QStringLiteral("property bool richBlank: true")));
        QVERIFY(bar.contains(QStringLiteral(
            "app.richComposer.documentIsBlank(richInput.textDocument)")));
        QVERIFY(bar.contains(QStringLiteral("placeholderText: root.richBlank")));
        QVERIFY(thread.contains(QStringLiteral("property bool threadRichBlank: true")));
        QVERIFY(thread.contains(QStringLiteral(
            "app.richComposer.documentIsBlank(threadRichInput.textDocument)")));
        QVERIFY(thread.contains(QStringLiteral("placeholderText: panel.threadRichBlank")));
        // A format toggle changes no characters, so it refreshes the flag
        // itself.
        QVERIFY(bar.contains(QStringLiteral("root.refreshRichBlank()")));
        QVERIFY(thread.contains(QStringLiteral("panel.refreshThreadRichBlank()")));
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
            // Anchored on the expression, so a comment above cannot move the
            // assertion. The declaration's presence is also the stripper's
            // found>0 guard, so the two bans cannot pass vacuously.
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

    // ---- 2: what gets asked, and what never does ----

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
        // Each row is something a Matrix message carries, containing one token
        // a dictionary would reject. The token must never be looked up, not
        // merely go un-underlined.
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
        // A dictionary that knows nothing, so a token goes unreported only if
        // the tokenizer refused to ask.
        Fixture f(QStringList{});
        // The control word is the found>0 guard: it proves the text was walked.
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
        // Caret at its leading edge counts as inside: Backspace from there
        // still edits this word.
        QVERIFY(f.checker.misspelledRanges(text, 6).isEmpty());
        // Caret elsewhere: reported.
        QCOMPARE(f.checker.misspelledRanges(text, 2).size(), 1);
        // And with no caret at all.
        QCOMPARE(f.checker.misspelledRanges(text, -1).size(), 1);
    }

    void aMentionRangeIsNeverSpellChecked()
    {
        Fixture f;
        // A display name is not English; mentions are plain text with a
        // semantic range. Without the range all four words are rejected, with
        // it only the two outside; both halves are asserted so a rule that
        // drops everything fails.
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
        // A position at a word's trailing edge names that word (a right-click
        // just past the last letter, or a caret there).
        QCOMPARE(f.checker.wordAt(text, 5).value("word").toString(),
                 QStringLiteral("hello"));
        // A position in whitespace names nothing.
        const QString spaced = QStringLiteral("hello   wrold");
        QCOMPARE(f.checker.wordAt(spaced, 6).value("word").toString(),
                 QString{});
        QCOMPARE(f.checker.wordAt(spaced, 6).value("start").toInt(), -1);
    }

    void addingAWordWritesThePersonalDictionaryAndClearsTheStaleAnswer()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("spelling/personal-dictionary.txt"));
        Fixture f;
        f.checker.setPersonalDictionaryPath(path);
        const QString text = QStringLiteral("hello Lightnink");
        QCOMPARE(f.checker.misspelledRanges(text).size(), 1);
        QSignalSpy spy(&f.checker, &SpellChecker::dictionaryChanged);
        QSignalSpy words(&f.checker, &SpellChecker::personalDictionaryChanged);
        f.checker.addToDictionary(QStringLiteral("Lightnink"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(words.count(), 1);
        // The cached "wrong" must not survive.
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
        QCOMPARE(f.checker.personalWords(), QStringList{ QStringLiteral("Lightnink") });

        // Lightning's own file, one word per line, read back by a new
        // checker: the word list belongs to the user, not to the session.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), QStringLiteral("Lightnink\n"));
        file.close();
        Fixture again;
        QCOMPARE(again.checker.misspelledRanges(text).size(), 1);
        again.checker.setPersonalDictionaryPath(path);
        QVERIFY(again.checker.misspelledRanges(text).isEmpty());

        // And it can be taken back, which a platform dictionary written
        // behind the user's back could not offer.
        again.checker.removeFromDictionary(QStringLiteral("Lightnink"));
        QCOMPARE(again.checker.misspelledRanges(text).size(), 1);
        QVERIFY(again.checker.personalWords().isEmpty());
        Fixture third;
        third.checker.setPersonalDictionaryPath(path);
        QCOMPARE(third.checker.misspelledRanges(text).size(), 1);
    }

    void aPersonalLowercaseWordAlsoCoversItsCapitalisedForm()
    {
        Fixture f;
        f.checker.addToDictionary(QStringLiteral("matrixy"));
        QVERIFY(f.checker.misspelledRanges(QStringLiteral("hello matrixy")).isEmpty());
        QVERIFY(f.checker.misspelledRanges(QStringLiteral("Matrixy hello")).isEmpty());
        // The other way round is not implied: "Smetonis" does not make
        // "smetonis" right, as a dictionary would not.
        f.checker.addToDictionary(QStringLiteral("Smetonisx"));
        QCOMPARE(f.checker.misspelledRanges(QStringLiteral("hello smetonisx")).size(), 1);
    }

    void ignoringAWordIsSessionOnlyAndTouchesNoDictionary()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("personal-dictionary.txt"));
        Fixture f;
        f.checker.setPersonalDictionaryPath(path);
        const QString text = QStringLiteral("hello Lightnink");
        QCOMPARE(f.checker.misspelledRanges(text).size(), 1);
        f.checker.ignoreWord(QStringLiteral("Lightnink"));
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
        QVERIFY2(!QFile::exists(path),
                 "Ignore must not write the personal dictionary");
        QVERIFY(f.checker.personalWords().isEmpty());
    }

    void oneWordIsLookedUpOnceHoweverOftenItIsTyped()
    {
        Fixture f;
        // The whole draft is re-checked per keystroke; lookups are the
        // expensive half.
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

    // ---- 2b: several languages at once ----

    void severalLanguagesAcceptAWordAnyOfThemKnows()
    {
        SpellChecker checker;
        checker.setBackendFactoryForTest(languageFactory());
        checker.configure(QString(), { QStringLiteral("lt_LT") }, QString());
        QCOMPARE(checker.languages(),
                 (QStringList{ QStringLiteral("en-US"), QStringLiteral("lt-LT") }));
        QCOMPARE(checker.extraLanguages(), QStringList{ QStringLiteral("lt-LT") });
        // English and Lithuanian in one draft: only the word NEITHER knows.
        const QString text = QStringLiteral("labas hello wrold");
        const QVariantList ranges = checker.misspelledRanges(text);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value("start").toInt(), 12);
        QCOMPARE(checker.languageLabel(),
                 QStringLiteral("English (United States), Lithuanian (Lithuania)"));
        QVERIFY(checker.missingLanguages().isEmpty());
    }

    void automaticAlsoChecksTheInterfaceLanguageWhenItHasADictionary()
    {
        SpellChecker checker;
        QStringList requested;
        checker.setBackendFactoryForTest(languageFactory(&requested));
        // System English, Lithuanian UI: both are checked.
        checker.configure(QString(), {}, QStringLiteral("lt"));
        QCOMPARE(checker.languages(),
                 (QStringList{ QStringLiteral("en-US"), QStringLiteral("lt-LT") }));
        QVERIFY(checker.misspelledRanges(QStringLiteral("labas hello")).isEmpty());
        // A UI language with no dictionary is silent: Automatic never reports
        // a language the user did not pick.
        checker.setInterfaceLanguage(QStringLiteral("zh"));
        QCOMPARE(checker.languages(), QStringList{ QStringLiteral("en-US") });
        QVERIFY(checker.missingLanguages().isEmpty());
        QVERIFY(checker.available());
        // The same dictionary twice is one dictionary, and it is not even
        // opened a second time.
        requested.clear();
        checker.setInterfaceLanguage(QStringLiteral("en"));
        QCOMPARE(checker.languages(), QStringList{ QStringLiteral("en-US") });
        QCOMPARE(requested, QStringList{ QString() });
        // An explicit primary language does not take the UI language along.
        requested.clear();
        checker.configure(QStringLiteral("en-US"), {}, QStringLiteral("lt"));
        QCOMPARE(requested, QStringList{ QStringLiteral("en-US") });
        QCOMPARE(checker.languages(), QStringList{ QStringLiteral("en-US") });
    }

    // C47-F01: a dictionary with no word data accepts every word, and "any
    // active language accepts it" would then switch all checking off.
    void anExtraLanguageThatAcceptsEverythingIsReportedMissing()
    {
        class AcceptAll : public FakeBackend
        {
        public:
            AcceptAll() : FakeBackend({}) {}
            bool isCorrect(const QString &) const override { return true; }
        };
        SpellChecker checker;
        checker.setBackendFactoryForTest(
            [](const QString &tag, QString *failure,
               QStringList *offered) -> std::unique_ptr<SpellBackend> {
                *offered = { QStringLiteral("en-US"), QStringLiteral("he-IL") };
                if (tag.startsWith(QStringLiteral("he"))) {
                    auto b = std::make_unique<AcceptAll>();
                    b->m_language = QStringLiteral("he-IL");
                    return b;
                }
                if (tag.isEmpty() || tag.startsWith(QStringLiteral("en"))) {
                    auto b = std::make_unique<FakeBackend>(
                        QStringList{ QStringLiteral("hello") });
                    b->m_language = QStringLiteral("en-US");
                    return b;
                }
                *failure = QStringLiteral("no-dictionary");
                return {};
            });
        checker.configure(QString(), { QStringLiteral("he-IL") }, QString());
        QCOMPARE(checker.missingLanguages(), QStringList{ QStringLiteral("he-IL") });
        QCOMPARE(checker.misspelledRanges(QStringLiteral("hello zzqq")).size(), 1);
    }

    // MASTER-1 M4: a working dictionary that passes words outside its own
    // script unjudged (a Russian one handed Latin text) is NOT one that
    // accepts everything. Old probe (one Latin nonsense word): the Russian
    // dictionary was dropped as "missing".
    void aDictionaryThatOnlyJudgesItsOwnScriptStaysActive()
    {
        class CyrillicOnly : public FakeBackend
        {
        public:
            CyrillicOnly() : FakeBackend({ QStringLiteral("\u043f\u0440\u0438\u0432\u0435\u0442") }) {}
            bool isCorrect(const QString &word) const override
            {
                bool cyrillic = false;
                for (const QChar c : word)
                    cyrillic = cyrillic || (c.unicode() >= 0x0400 && c.unicode() <= 0x04FF);
                return !cyrillic || FakeBackend::isCorrect(word);
            }
        };
        SpellChecker checker;
        checker.setBackendFactoryForTest(
            [](const QString &tag, QString *failure,
               QStringList *offered) -> std::unique_ptr<SpellBackend> {
                *offered = { QStringLiteral("en-US"), QStringLiteral("ru-RU") };
                if (tag.startsWith(QStringLiteral("ru"))) {
                    auto b = std::make_unique<CyrillicOnly>();
                    b->m_language = QStringLiteral("ru-RU");
                    return b;
                }
                if (tag.isEmpty() || tag.startsWith(QStringLiteral("en"))) {
                    auto b = std::make_unique<FakeBackend>(
                        QStringList{ QStringLiteral("hello") });
                    b->m_language = QStringLiteral("en-US");
                    return b;
                }
                *failure = QStringLiteral("no-dictionary");
                return {};
            });
        checker.configure(QString(), { QStringLiteral("ru-RU") }, QString());
        QVERIFY2(checker.missingLanguages().isEmpty(),
                 qPrintable(checker.missingLanguages().join(QLatin1Char(','))));
        QVERIFY(checker.languages().contains(QStringLiteral("ru-RU")));
    }

    // C47 r2: the accept-everything refusal sits in resolve(), so the
    // interface-language and primary paths get it too.
    void aUiOrPrimaryDictionaryThatAcceptsEverythingIsNotActivated()
    {
        class AcceptAll : public FakeBackend
        {
        public:
            AcceptAll() : FakeBackend({}) {}
            bool isCorrect(const QString &) const override { return true; }
        };
        auto factory = [](const QString &tag, QString *failure,
                          QStringList *offered) -> std::unique_ptr<SpellBackend> {
            *offered = { QStringLiteral("en-US"), QStringLiteral("he-IL") };
            if (tag.startsWith(QStringLiteral("he"))) {
                auto b = std::make_unique<AcceptAll>();
                b->m_language = QStringLiteral("he-IL");
                return b;
            }
            if (tag.isEmpty() || tag.startsWith(QStringLiteral("en"))) {
                auto b = std::make_unique<FakeBackend>(
                    QStringList{ QStringLiteral("hello") });
                b->m_language = QStringLiteral("en-US");
                return b;
            }
            *failure = QStringLiteral("no-dictionary");
            return {};
        };
        {
            SpellChecker checker;
            checker.setBackendFactoryForTest(factory);
            checker.configure(QString(), {}, QStringLiteral("he"));
            QCOMPARE(checker.misspelledRanges(QStringLiteral("hello zzqq")).size(), 1);
        }
        {
            SpellChecker checker;
            checker.setBackendFactoryForTest(factory);
            checker.configure(QStringLiteral("he-IL"), {}, QString());
            QVERIFY(checker.missingLanguages().contains(QStringLiteral("he-IL")));
            QCOMPARE(checker.misspelledRanges(QStringLiteral("zzqq")).size(), 0);
        }
    }

    // C47-F05: a closing bracket or a closing quote finishes a word like a
    // comma does. The period stays out on purpose (domains, file names;
    // docs/spell-checking.md), pinned below.
    void autocorrectFiresBeforeClosingPunctuation()
    {
        {
            SpellChecker qmlSide;
            QVERIFY(qmlSide.isAutocorrectBoundaryText(QStringLiteral(")")));
            QVERIFY(qmlSide.isAutocorrectBoundaryText(QString(QChar(0x201D))));
            QVERIFY(!qmlSide.isAutocorrectBoundaryText(QStringLiteral(".")));
            QVERIFY(!qmlSide.isAutocorrectBoundaryText(QStringLiteral("ab")));
        }
        Fixture f;
        f.checker.setAutocorrectEnabled(true);
        const QStringList closers = { QStringLiteral(")"), QStringLiteral("\""),
                                      QString(QChar(0x201D)) };
        for (const QString &c : closers) {
            const QString text = QStringLiteral("hi teh") + c;
            QVERIFY(SpellChecker::isAutocorrectBoundary(c.at(0)));
            const QVariantMap fix = f.checker.autocorrection(text, text.size() - 1);
            QVERIFY2(!fix.isEmpty(), qPrintable(text));
            QCOMPARE(fix.value("replacement").toString(), QStringLiteral("the"));
        }
        QVERIFY(!SpellChecker::isAutocorrectBoundary(QLatin1Char('.')));
        // The word is found even with an opening bracket in front of it.
        QVERIFY(!f.checker.autocorrection(QStringLiteral("(teh)"), 4).isEmpty());
    }

    void aChosenLanguageWithoutADictionaryIsReportedNotDropped()
    {
        SpellChecker checker;
        checker.setBackendFactoryForTest(languageFactory());
        checker.configure(QString(), { QStringLiteral("xx-XX"), QStringLiteral("lt-LT") },
                          QString());
        QCOMPARE(checker.missingLanguages(), QStringList{ QStringLiteral("xx-XX") });
        QCOMPARE(checker.languages(),
                 (QStringList{ QStringLiteral("en-US"), QStringLiteral("lt-LT") }));
        // A missing extra does not switch checking off.
        QVERIFY(checker.available());
        QCOMPARE(checker.unavailableReason(), QString());
    }

    void suggestionsInterleaveEveryActiveLanguage()
    {
        std::vector<std::unique_ptr<SpellBackend>> list;
        auto en = std::make_unique<FakeBackend>(QStringList{});
        en->m_suggestions.insert(QStringLiteral("lbas"),
                                 { QStringLiteral("lbs"), QStringLiteral("labs") });
        auto lt = std::make_unique<FakeBackend>(QStringList{});
        lt->m_language = QStringLiteral("lt-LT");
        lt->m_suggestions.insert(QStringLiteral("lbas"),
                                 { QStringLiteral("labas"), QStringLiteral("labs") });
        list.push_back(std::move(en));
        list.push_back(std::move(lt));
        SpellChecker checker;
        checker.setBackendsForTest(std::move(list));
        // Each language's best guess before anyone's second, no duplicates.
        QCOMPARE(checker.suggestions(QStringLiteral("lbas")),
                 (QStringList{ QStringLiteral("lbs"), QStringLiteral("labas"),
                               QStringLiteral("labs") }));
    }

    // ---- 2c: autocorrect ----

    void autocorrectIsOffUnlessSomethingTurnsItOn()
    {
        Fixture f;
        // Off on a fresh checker, and the platform default is off everywhere
        // but macOS (where it follows "Correct spelling automatically").
#if !defined(Q_OS_MACOS)
        QVERIFY(!SpellChecker::autocorrectDefault());
#endif
        QVERIFY(!f.checker.autocorrectEnabled());
        QVERIFY(f.checker.autocorrection(QStringLiteral("say teh "), 7).isEmpty());
        f.checker.setAutocorrectEnabled(true);
        QVERIFY(!f.checker.autocorrection(QStringLiteral("say teh "), 7).isEmpty());
        // Spell checking off switches autocorrect off with it.
        f.checker.setEnabled(false);
        QVERIFY(f.checker.autocorrection(QStringLiteral("say teh "), 7).isEmpty());
    }

    void aKnownTypoIsCorrectedOnlyOnceTheWordIsFinished()
    {
        Fixture f;
        f.checker.setAutocorrectEnabled(true);
        const QVariantMap fix = f.checker.autocorrection(QStringLiteral("say teh "), 7);
        QCOMPARE(fix.value("start").toInt(), 4);
        QCOMPARE(fix.value("length").toInt(), 3);
        QCOMPARE(fix.value("original").toString(), QStringLiteral("teh"));
        QCOMPARE(fix.value("replacement").toString(), QStringLiteral("the"));
        // Capitalisation follows what was typed.
        QCOMPARE(f.checker.autocorrection(QStringLiteral("Teh end"), 3)
                     .value("replacement").toString(),
                 QStringLiteral("The"));
        // Comma, semicolon, ! and ? end a word too.
        QCOMPARE(f.checker.autocorrection(QStringLiteral("teh, "), 3)
                     .value("replacement").toString(),
                 QStringLiteral("the"));
        // Not a boundary: '.' (a domain or file name in progress), ':' (a
        // shortcode), a letter (still typing), the end of the text.
        QVERIFY(f.checker.autocorrection(QStringLiteral("teh.org"), 3).isEmpty());
        QVERIFY(f.checker.autocorrection(QStringLiteral("teh:"), 3).isEmpty());
        QVERIFY(f.checker.autocorrection(QStringLiteral("tehx"), 3).isEmpty());
        QVERIFY(f.checker.autocorrection(QStringLiteral("teh"), 3).isEmpty());
        // A position that is not the end of the word.
        QVERIFY(f.checker.autocorrection(QStringLiteral("teh hello "), 9).isEmpty());
    }

    void autocorrectNeverTouchesCodeUrlsMentionsShortcodesOrCommands_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QVariantList>("skip");
        const QVariantList none;
        QTest::newRow("inline code being typed") << QStringLiteral("run `teh ") << none;
        QTest::newRow("unterminated fence")
            << QStringLiteral("```\nfoo teh ") << none;
        QTest::newRow("url") << QStringLiteral("https://x.org/teh ") << none;
        QTest::newRow("path") << QStringLiteral("cd docs/teh ") << none;
        QTest::newRow("shortcode") << QStringLiteral(":teh ") << none;
        QTest::newRow("mxid") << QStringLiteral("@teh ") << none;
        QTest::newRow("command") << QStringLiteral("/me teh ") << none;
        QTest::newRow("link destination") << QStringLiteral("[a](teh ") << none;
        QTest::newRow("mention range")
            << QStringLiteral("hi teh ")
            << QVariantList{ QVariantMap{ { "start", 3 }, { "length", 3 } } };
    }

    void autocorrectNeverTouchesCodeUrlsMentionsShortcodesOrCommands()
    {
        QFETCH(QString, text);
        QFETCH(QVariantList, skip);
        Fixture f;
        f.checker.setAutocorrectEnabled(true);
        // The control: the same typo in prose IS corrected, so an empty
        // answer below is the rule and not a dead checker.
        QVERIFY(!f.checker.autocorrection(QStringLiteral("hi teh "), 6).isEmpty());
        const int boundary = text.size() - 1;
        QVERIFY(SpellChecker::isAutocorrectBoundary(text.at(boundary)));
        QVERIFY2(f.checker.autocorrection(text, boundary, skip).isEmpty(),
                 qPrintable(QStringLiteral("corrected inside: %1").arg(text)));
    }

    void autocorrectLeavesKnownIgnoredPersonalAndRevertedWordsAlone()
    {
        Fixture f({ QStringLiteral("the"), QStringLiteral("hello") });
        f.checker.setAutocorrectEnabled(true);
        // A word the dictionary knows is never "corrected".
        QVERIFY(f.checker.autocorrection(QStringLiteral("hello "), 5).isEmpty());
        f.checker.ignoreWord(QStringLiteral("teh"));
        QVERIFY(f.checker.autocorrection(QStringLiteral("teh "), 3).isEmpty());

        Fixture p({ QStringLiteral("the") });
        p.checker.setAutocorrectEnabled(true);
        p.checker.addToDictionary(QStringLiteral("teh"));
        QVERIFY(p.checker.autocorrection(QStringLiteral("teh "), 3).isEmpty());

        // Undoing a correction once means "I meant that": not again this
        // session.
        Fixture r({ QStringLiteral("the") });
        r.checker.setAutocorrectEnabled(true);
        QVERIFY(!r.checker.autocorrection(QStringLiteral("teh "), 3).isEmpty());
        r.checker.rejectAutocorrection(QStringLiteral("teh"));
        QVERIFY(r.checker.autocorrection(QStringLiteral("teh "), 3).isEmpty());
        QVERIFY(r.checker.autocorrection(QStringLiteral("so teh "), 6).isEmpty());
    }

    void theTypoTableNeedsEnglishAndAReplacementTheDictionaryAccepts()
    {
        // A dictionary that does not know "the" vetoes "teh" -> "the".
        Fixture unknown({ QStringLiteral("hello") });
        unknown.checker.setAutocorrectEnabled(true);
        unknown.backend->m_suggestions.insert(QStringLiteral("teh"), {});
        QVERIFY(unknown.checker.autocorrection(QStringLiteral("teh "), 3).isEmpty());

        // A Lithuanian-only checker never applies the English table.
        Fixture lt({ QStringLiteral("the") });
        lt.backend->m_language = QStringLiteral("lt-LT");
        lt.backend->m_suggestions.insert(QStringLiteral("teh"), {});
        lt.checker.setAutocorrectEnabled(true);
        QVERIFY(lt.checker.autocorrection(QStringLiteral("teh "), 3).isEmpty());

        // Contractions, and a typographic apostrophe survives.
        Fixture en({ QStringLiteral("don't"), QString::fromUtf8("don’t") });
        en.checker.setAutocorrectEnabled(true);
        QCOMPARE(en.checker.autocorrection(QStringLiteral("dont "), 4)
                     .value("replacement").toString(),
                 QStringLiteral("don't"));
    }

    void aLoneIBecomesCapitalOnlyWhenEveryLanguageIsEnglish()
    {
        Fixture en({ QStringLiteral("I") });
        en.checker.setAutocorrectEnabled(true);
        const QVariantMap fix = en.checker.autocorrection(QStringLiteral("so i "), 4);
        QCOMPARE(fix.value("start").toInt(), 3);
        QCOMPARE(fix.value("replacement").toString(), QStringLiteral("I"));
        QCOMPARE(en.checker.autocorrection(QStringLiteral("i "), 1)
                     .value("replacement").toString(),
                 QStringLiteral("I"));
        // Part of something else, or inside code: left alone.
        QVERIFY(en.checker.autocorrection(QStringLiteral("f(i "), 3).isEmpty());
        QVERIFY(en.checker.autocorrection(QStringLiteral("`i "), 2).isEmpty());
        // "i" is a Lithuanian word ("į" typed without the ogonek): with
        // Lithuanian active it stays.
        std::vector<std::unique_ptr<SpellBackend>> list;
        list.push_back(std::make_unique<FakeBackend>(QStringList{ QStringLiteral("I") }));
        auto lt = std::make_unique<FakeBackend>(QStringList{});
        lt->m_language = QStringLiteral("lt-LT");
        list.push_back(std::move(lt));
        SpellChecker both;
        both.setBackendsForTest(std::move(list));
        both.setAutocorrectEnabled(true);
        QVERIFY(both.autocorrection(QStringLiteral("so i "), 4).isEmpty());
    }

    void withoutAPlatformRuleOnlyAUniqueTranspositionIsApplied()
    {
        Fixture f({ QStringLiteral("message"), QStringLiteral("test"),
                    QStringLiteral("stet"), QStringLiteral("world") });
        f.checker.setAutocorrectEnabled(true);
        // Two neighbouring letters swapped, and that swap is a word: applied.
        QCOMPARE(f.checker.autocorrection(QStringLiteral("messgae "), 7)
                     .value("replacement").toString(),
                 QStringLiteral("message"));
        QCOMPARE(f.checker.autocorrection(QStringLiteral("wrold "), 5)
                     .value("replacement").toString(),
                 QStringLiteral("world"));
        // Two different swaps are words ("tset" -> test, stet): ambiguous.
        QVERIFY(f.checker.autocorrection(QStringLiteral("tset "), 4).isEmpty());
        // Too short to be sure, even when the swap is a word.
        f.backend->m_known << QStringLiteral("for");
        QVERIFY(f.checker.autocorrection(QStringLiteral("fro "), 3).isEmpty());
        // A Capitalised word is usually a name: never, even for a clean swap.
        QVERIFY(f.checker.autocorrection(QStringLiteral("Messgae "), 7).isEmpty());
        // A mixed-case word is a name or an identifier.
        QVERIFY(f.checker.autocorrection(QStringLiteral("MeSsgae "), 7).isEmpty());
        // And suggest() is never called for it: it costs 18-200 ms per
        // dictionary on the GUI thread.
        QVERIFY(f.backend->m_suggestedFor.isEmpty());
    }

    // Measured with a real en_US dictionary (review round 1): the engine's
    // best one-edit suggestion turned each of these into another word. The
    // fake engine reproduces exactly that suggestion and knows exactly that
    // word, so the old "unique one-edit best guess" rule corrects every row
    // and the transposition-only rule corrects none.
    void wordsTheOldOneEditRuleMangledAreLeftAlone_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("eager");
        QTest::newRow("haha") << QStringLiteral("haha") << QStringLiteral("aha");
        QTest::newRow("Lukas") << QStringLiteral("Lukas") << QStringLiteral("Lucas");
        QTest::newRow("async") << QStringLiteral("async") << QStringLiteral("sync");
        QTest::newRow("hmmm") << QStringLiteral("hmmm") << QStringLiteral("hmm");
        QTest::newRow("lolz") << QStringLiteral("lolz") << QStringLiteral("loll");
        QTest::newRow("Matrixy") << QStringLiteral("Matrixy") << QStringLiteral("Matrix");
    }

    void wordsTheOldOneEditRuleMangledAreLeftAlone()
    {
        QFETCH(QString, typed);
        QFETCH(QString, eager);
        Fixture f({ eager, QStringLiteral("so") });
        f.backend->m_suggestions.insert(typed.toLower(), { eager });
        f.backend->m_suggestions.insert(typed, { eager });
        f.checker.setAutocorrectEnabled(true);
        const QString text = QStringLiteral("so ") + typed + QLatin1Char(' ');
        // Control: the word is rejected, so autocorrect does consider it.
        QCOMPARE(f.checker.misspelledRanges(text.trimmed()).size(), 1);
        const QVariantMap fix = f.checker.autocorrection(text, text.size() - 1);
        QVERIFY2(fix.isEmpty(),
                 qPrintable(QStringLiteral("%1 became %2").arg(typed,
                     fix.value("replacement").toString())));
    }

    void anAutocorrectAnswerIsWorkedOutOncePerWord()
    {
        Fixture f({ QStringLiteral("world") });
        f.checker.setAutocorrectEnabled(true);
        const QString text = QStringLiteral("wrold ");
        QCOMPARE(f.checker.autocorrection(text, 5).value("replacement").toString(),
                 QStringLiteral("world"));
        const int after = f.checker.backendLookupsForTest();
        // Typing the same word again (or another draft with it) asks nothing.
        for (int i = 0; i < 5; ++i)
            f.checker.autocorrection(text, 5);
        QCOMPARE(f.checker.backendLookupsForTest(), after);
        // "No correction" is remembered too.
        f.checker.autocorrection(QStringLiteral("qzxv "), 4);
        const int none = f.checker.backendLookupsForTest();
        f.checker.autocorrection(QStringLiteral("qzxv "), 4);
        QCOMPARE(f.checker.backendLookupsForTest(), none);
        QVERIFY(f.backend->m_suggestedFor.isEmpty());
    }

    void aPlatformWithItsOwnAutocorrectIsTrustedAndNotSecondGuessed()
    {
        Fixture f({ QStringLiteral("receive"), QStringLiteral("message") });
        f.checker.setAutocorrectEnabled(true);
        f.backend->m_hasNative = true;
        f.backend->m_native.insert(QStringLiteral("recieev"), QStringLiteral("receive"));
        QCOMPARE(f.checker.autocorrection(QStringLiteral("recieev "), 7)
                     .value("replacement").toString(),
                 QStringLiteral("receive"));
        // The platform declines: no rule of ours on top -- "messgae" is a
        // clean transposition of a known word and is still left alone, and
        // the swap is never even looked up.
        QVERIFY(f.checker.autocorrection(QStringLiteral("messgae "), 7).isEmpty());
        QVERIFY(!f.backend->m_asked.contains(QStringLiteral("message")));
    }

    // ---- 2d: a large paste ----

    void aLargePasteLooksUpOnlyTheVisibleWindow()
    {
        Fixture f(QStringList{});
        // ~300k characters of DISTINCT misspellings, so the cache cannot
        // hide how many lookups a pass costs.
        QString text;
        int n = 0;
        while (text.size() < 300000) {
            QString word = QStringLiteral("zq");
            for (int v = n++; v > 0; v /= 26)
                word += QChar(u'a' + v % 26);
            text += word + QLatin1Char(' ');
            if (n % 50 == 0)
                text += QStringLiteral("`code span here` ");
        }
        QVERIFY(text.size() > SpellChecker::kMaxCheckedChars);
        QVERIFY(text.size() < SpellChecker::kMaxScannedChars);

        // Without a window a draft this long is not checked at all.
        QVERIFY(f.checker.misspelledRanges(text).isEmpty());
        QCOMPARE(f.checker.backendLookupsForTest(), 0);

        // With one, only the window's words reach the dictionary.
        const int from = 150000;
        const int to = 152000;
        QElapsedTimer timer;
        timer.start();
        const QVariantList ranges = f.checker.misspelledRanges(text, -1, {}, from, to);
        const qint64 elapsed = timer.elapsed();
        QVERIFY(!ranges.isEmpty());
        // ~2000 characters hold a few hundred words; the whole draft holds
        // ~40000. The bound is the property; it fails by two orders of
        // magnitude if the window is ignored.
        QVERIFY2(f.checker.backendLookupsForTest() <= 400,
                 qPrintable(QStringLiteral("%1 lookups").arg(f.checker.backendLookupsForTest())));
        QVERIFY(ranges.size() <= SpellChecker::kMaxReportedRanges);
        for (const QVariant &v : ranges) {
            const int start = v.toMap().value("start").toInt();
            const int length = v.toMap().value("length").toInt();
            QVERIFY(start + length > from && start < to);
            // Code spans are skipped in the window too.
            QVERIFY(text.mid(start, length) != QStringLiteral("code"));
        }
        // A catastrophic-regression guard only (the tokenizer is linear and the
        // span lookup logarithmic); the lookup bound above is the real
        // assertion. Generous so a loaded machine does not flake it.
        QVERIFY2(elapsed < 3000, qPrintable(QStringLiteral("%1 ms").arg(elapsed)));
    }

    void aWindowNeverReportsMoreRangesThanTheCap()
    {
        Fixture f(QStringList{});
        const QString text = QStringLiteral("wrold ").repeated(3000);
        const QVariantList ranges =
            f.checker.misspelledRanges(text, -1, {}, 0, text.size());
        QCOMPARE(ranges.size(), SpellChecker::kMaxReportedRanges);
    }

    // ---- 2e: the real Hunspell engine (Linux) ----

    void hunspellChecksWithARealDictionaryInAnyEncoding()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A tiny UTF-8 English dictionary and an ISO-8859-13 Lithuanian one
        // (the encoding older lt dictionaries ship in).
        auto write = [&dir](const QString &name, const QByteArray &bytes) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
        };
        write(QStringLiteral("en_US.aff"),
              "SET UTF-8\nTRY esianrtolcdugmphbyfvkwzESIANRTOLCDUGMPHBYFVKWZ'\n");
        write(QStringLiteral("en_US.dic"), "4\nthe\nhello\nreceive\nworld\n");
        write(QStringLiteral("lt_LT.aff"), "SET ISO8859-13\nTRY aisuoe\n");
        // "ąžuolas" in ISO-8859-13: ą = 0xE0, ž = 0xFE.
        write(QStringLiteral("lt_LT.dic"), QByteArray("2\nlabas\n\xE0\xFEuolas\n"));
        // A hyphenation file is not a language.
        write(QStringLiteral("hyph_en_US.dic"), "ISO8859-1\n");

        QString failure;
        QStringList languages;
        auto en = createHunspellSpellBackend(QStringLiteral("en-US"), { dir.path() },
                                             &failure, &languages);
        if (!en && failure == QStringLiteral("no-library"))
            QSKIP("libhunspell-1.7 is not loadable here (the Nix dev shell puts it "
                  "on LD_LIBRARY_PATH; a bare shell does not)");
        QVERIFY2(en, qPrintable(failure));
        QCOMPARE(en->name(), QStringLiteral("hunspell"));
        QCOMPARE(en->language(), QStringLiteral("en-US"));
        QCOMPARE(languages, (QStringList{ QStringLiteral("en-US"), QStringLiteral("lt-LT") }));
        QVERIFY(en->isCorrect(QStringLiteral("hello")));
        QVERIFY(!en->isCorrect(QStringLiteral("teh")));
        QVERIFY(en->suggest(QStringLiteral("teh")).contains(QStringLiteral("the")));

        auto lt = createHunspellSpellBackend(QStringLiteral("lt"), { dir.path() },
                                             &failure, nullptr);
        QVERIFY2(lt, qPrintable(QStringLiteral("ISO-8859-13 dictionary refused: %1")
                                    .arg(failure)));
        QCOMPARE(lt->language(), QStringLiteral("lt-LT"));
        QVERIFY(lt->isCorrect(QString::fromUtf8("ąžuolas")));
        QVERIFY(!lt->isCorrect(QString::fromUtf8("ąžuolsa")));
        // A character the encoding cannot hold is not this dictionary's to
        // judge, never a crash or a mangled lookup.
        QVERIFY(lt->isCorrect(QString::fromUtf8("日本")));
        QVERIFY(lt->suggest(QString::fromUtf8("ąžuols")).contains(QString::fromUtf8("ąžuolas")));

        // Regional aliases shipped as symlinks (Debian's layout): the real
        // file wins even where an alias sorts first (de_BE < de_DE), each
        // dictionary is listed once, and a regional request still resolves
        // through its language.
        QTemporaryDir aliases;
        QVERIFY(aliases.isValid());
        auto writeIn = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
        };
        writeIn(aliases.filePath(QStringLiteral("de_DE.aff")), "SET UTF-8\n");
        writeIn(aliases.filePath(QStringLiteral("de_DE.dic")), "1\nhallo\n");
        writeIn(aliases.filePath(QStringLiteral("es_ES.aff")), "SET UTF-8\n");
        writeIn(aliases.filePath(QStringLiteral("es_ES.dic")), "1\nhola\n");
        for (const char *alias : { "de_BE", "es_AR", "es_MX" }) {
            const QString a = QString::fromLatin1(alias);
            const QString base = a.startsWith(QLatin1String("de")) ? QStringLiteral("de_DE")
                                                                   : QStringLiteral("es_ES");
            QVERIFY(QFile::link(base + QStringLiteral(".aff"),
                                aliases.filePath(a + QStringLiteral(".aff"))));
            QVERIFY(QFile::link(base + QStringLiteral(".dic"),
                                aliases.filePath(a + QStringLiteral(".dic"))));
        }
        QStringList aliasLanguages;
        auto de = createHunspellSpellBackend(QStringLiteral("de-DE"), { aliases.path() },
                                             &failure, &aliasLanguages);
        QVERIFY2(de, qPrintable(failure));
        QCOMPARE(de->language(), QStringLiteral("de-DE"));
        QCOMPARE(aliasLanguages, (QStringList{ QStringLiteral("de-DE"), QStringLiteral("es-ES") }));
        auto mx = createHunspellSpellBackend(QStringLiteral("es-MX"), { aliases.path() },
                                             &failure, nullptr);
        QVERIFY2(mx, qPrintable(failure));
        QVERIFY(mx->isCorrect(QStringLiteral("hola")));

        // No dictionary for the language: said so, and the list still offered.
        failure.clear();
        languages.clear();
        QVERIFY(!createHunspellSpellBackend(QStringLiteral("de-DE"), { dir.path() },
                                            &failure, &languages));
        QCOMPARE(failure, QStringLiteral("no-dictionary"));
        QCOMPARE(languages.size(), 2);

        // Through the platform factory: DICPATH is honoured first, and the
        // engine pin is what makes this deterministic on a machine with
        // enchant.
        qputenv("DICPATH", QFile::encodeName(dir.path()));
        qputenv("LIGHTNING_SPELL_ENGINE", "hunspell");
        QVERIFY(spellDictionaryDirectories().value(0)
                == QFileInfo(dir.path()).canonicalFilePath());
        SpellChecker checker;
        checker.configure(QStringLiteral("en-US"), { QStringLiteral("lt-LT") }, QString());
        qunsetenv("DICPATH");
        qunsetenv("LIGHTNING_SPELL_ENGINE");
        QCOMPARE(checker.backendName(), QStringLiteral("hunspell"));
        QCOMPARE(checker.languages(),
                 (QStringList{ QStringLiteral("en-US"), QStringLiteral("lt-LT") }));
        const QString draft = QString::fromUtf8("hello ąžuolas teh");
        const QVariantList ranges = checker.misspelledRanges(draft);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value("start").toInt(), 14);
#else
        QSKIP("Hunspell is the Linux/BSD engine");
#endif
    }

    // ---- 3: the tray badge ----

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
        // Above nine the drawn label stops changing, so the repaint condition
        // (label changed) is false and no per-message rasterization happens.
        QCOMPARE(TrayIcon::badgeLabel(40, true), TrayIcon::badgeLabel(41, true));
        QCOMPARE(TrayIcon::badgeLabel(10, true),
                 TrayIcon::badgeLabel(9999, true));
        // ...and it still changes across the boundaries that matter.
        QVERIFY(TrayIcon::badgeLabel(9, true) != TrayIcon::badgeLabel(10, true));
        QVERIFY(TrayIcon::badgeLabel(0, true) != TrayIcon::badgeLabel(0, false));
        QVERIFY(TrayIcon::badgeLabel(0, true) != TrayIcon::badgeLabel(1, true));
    }
};

QTEST_GUILESS_MAIN(ComposerSpellTest)
#include "ComposerSpellTest.moc"
