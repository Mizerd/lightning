#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextCursor>
#include <QtTest>

#include "models/MentionHighlighter.h"

class MentionHighlighterEmojiTest : public QObject
{
    Q_OBJECT
private:
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
    void emojiRunCarriesTheNamedFaceAndTextDoesNot()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("hi \U0001F600 there"));

        MentionHighlighter highlighter;
        highlighter.setEmojiFontFamily(QStringLiteral("Noto Color Emoji"));
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();

        const QTextCharFormat onEmoji = formatAt(doc, 3);
        QVERIFY2(onEmoji.fontFamilies().isValid(),
                 "the emoji run carries no font family at all");
        QCOMPARE(onEmoji.fontFamilies().toStringList(),
                 QStringList{ QStringLiteral("Noto Color Emoji") });
        const QTextCharFormat onLetter = formatAt(doc, 0);
        QVERIFY2(!onLetter.fontFamilies().isValid(),
                 "plain text was given the emoji face");
    }

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
