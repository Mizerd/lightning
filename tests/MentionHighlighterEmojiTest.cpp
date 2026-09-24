#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextCursor>
#include <QtTest>

#include "models/MentionHighlighter.h"

// The composer's emoji face, asserted on the format the document ends up
// with rather than on source text.
//
// Qt's per-character font fallback is version-dependent (6.8 prefers a
// monochrome font that claims the codepoint), so emoji must be named. The
// composer is mixed text, so the face is applied per range through this
// highlighter, and the applied format must carry a property the layout
// honours.
class MentionHighlighterEmojiTest : public QObject
{
    Q_OBJECT
private:
    // The format the renderer will use at a character position. Not
    // QTextCursor::charFormat(): a QSyntaxHighlighter publishes its runs
    // through QTextLayout::setFormats() and never writes into the document's
    // character formats.
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
        // The words around it are left alone, or the whole message would
        // render in an emoji font. Asked as "is there a families property at
        // all", because QTextCharFormat::fontFamily() asserts on an empty list.
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
