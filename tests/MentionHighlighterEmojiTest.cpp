#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextCursor>
#include <QtTest>

#include "models/MentionHighlighter.h"

// THE COMPOSER'S EMOJI FACE, asserted on the FORMAT the document actually ends
// up with rather than on the source text.
//
// Background: Qt's automatic per-character fallback is version-dependent (Qt
// 6.8 prefers a MONOCHROME font that claims the codepoint where 6.11 picks the
// colour one), so emoji have to be NAMED. On a single-purpose Label that is a
// `font.family` binding; the composer is MIXED text, so the face is applied
// per-range through this highlighter instead.
//
// The first attempt set ONLY QTextCharFormat::setFontFamilies() -- the Qt 6
// list API -- and the composer went on rendering emoji in the default face
// while the emoji picker was already correct. It was reported as "emojis look
// good in catalog but bad when in text box", and nothing in the tree could have
// caught it: a source scan sees the call, and only reading back the applied
// format shows which property the layout will honour.
class MentionHighlighterEmojiTest : public QObject
{
    Q_OBJECT
private:
    // The format the RENDERER will use at a character position.
    //
    // Deliberately NOT QTextCursor::charFormat(): a QSyntaxHighlighter does not
    // write into the document's character formats at all. It publishes
    // presentation-only runs through QTextLayout::setFormats(), so a cursor
    // reads back nothing and a test built on one measures the wrong thing and
    // fails on correct code. That mistake cost a round here.
    static QTextCharFormat formatAt(QTextDocument &doc, int pos)
    {
        const QTextBlock block = doc.findBlock(pos);
        if (!block.isValid() || !block.layout())
            return {};
        const int inBlock = pos - block.position();
        for (const QTextLayout::FormatRange &r : block.layout()->formats()) {
            if (inBlock >= r.start && inBlock < r.start + r.length)
                return r.format;
        }
        return {};
    }

private Q_SLOTS:
    // U+1F600 is a surrogate PAIR in UTF-16, so this also pins that the run is
    // measured in UTF-16 units: indexing by codepoint would misplace the format
    // by one for every emoji already passed.
    void emojiRunCarriesTheNamedFaceAndTextDoesNot()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("hi \U0001F600 there"));

        MentionHighlighter highlighter;
        highlighter.setEmojiFontFamily(QStringLiteral("Noto Color Emoji"));
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();

        // The emoji occupies UTF-16 indices 3 and 4.
        const QTextCharFormat onEmoji = formatAt(doc, 3);
        QVERIFY2(onEmoji.fontFamilies().isValid(),
                 "the emoji run carries no font family at all");
        QCOMPARE(onEmoji.fontFamilies().toStringList(),
                 QStringList{ QStringLiteral("Noto Color Emoji") });
        // And the words around it must be left alone, or naming the face would
        // render the whole message in an emoji font. Asked as "is there a
        // families property at all": QTextCharFormat::fontFamily() ASSERTS on
        // an empty list rather than returning an empty string, so calling it
        // on an unformatted run aborts the process.
        const QTextCharFormat onLetter = formatAt(doc, 0);
        QVERIFY2(!onLetter.fontFamilies().isValid(),
                 "plain text was given the emoji face");
    }

    // Not gated on mentions: a composer with no mention in it still types
    // emoji, which is the common case an earlier early-return would have
    // skipped entirely.
    void emojiAreFormattedWithNoMentionRangesSet()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("\U0001F389"));
        MentionHighlighter highlighter;
        highlighter.setEmojiFontFamily(QStringLiteral("Noto Color Emoji"));
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();
        QVERIFY(highlighter.ranges().isEmpty());
        QCOMPARE(formatAt(doc, 0).fontFamilies().toStringList(),
                 QStringList{ QStringLiteral("Noto Color Emoji") });
    }

    // An empty family must leave every character untouched, so a host with no
    // emoji font keeps exactly the behaviour it had.
    void noFamilyLeavesTheDocumentAlone()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("\U0001F600"));
        MentionHighlighter highlighter;
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();
        QVERIFY(!formatAt(doc, 0).fontFamilies().isValid());
    }
};

QTEST_MAIN(MentionHighlighterEmojiTest)
#include "MentionHighlighterEmojiTest.moc"
