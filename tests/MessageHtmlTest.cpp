// MessageHtml, the sanitizer for incoming Matrix formatted bodies: the
// allowlist is fail-closed (dangerous tags, attributes and schemes are
// stripped), matrix.to mentions become internal "mention:" links with resolved
// names, and ordinary formatting and http(s) links survive.
//
// Also code-block segmentation (MessageHtml::segments): terminal transcripts,
// very long lines, many lines, significant whitespace, non-Latin and emoji,
// HTML-looking text that must stay literal, interleaved prose, the
// segment/byte bounds, and the language token grammar. The last is a security
// boundary: `class` is sender-chosen, so only a validated token may reach a
// label or an accessible name.

#include "models/MessageHtml.h"

#include <QElapsedTimer>
#include <QSize>
#include <QtTest/QtTest>

namespace {
QString sanitize(const QString &html)
{
    return MessageHtml::sanitize(html, nullptr, QString());
}

QList<MessageHtml::Segment> segments(const QString &html)
{
    return MessageHtml::segments(html, nullptr, QString());
}

QString codeAt(const QList<MessageHtml::Segment> &segs, int nth)
{
    int seen = 0;
    for (const auto &s : segs) {
        if (s.kind != MessageHtml::SegmentKind::CodeBlock)
            continue;
        if (seen++ == nth)
            return s.text;
    }
    return {};
}

QString languageAt(const QList<MessageHtml::Segment> &segs, int nth)
{
    int seen = 0;
    for (const auto &s : segs) {
        if (s.kind != MessageHtml::SegmentKind::CodeBlock)
            continue;
        if (seen++ == nth)
            return s.language;
    }
    return {};
}

int countOf(const QList<MessageHtml::Segment> &segs,
            MessageHtml::SegmentKind kind)
{
    int n = 0;
    for (const auto &s : segs)
        if (s.kind == kind)
            ++n;
    return n;
}

// The first tag of sanitizer output that leaves the table it was written in,
// or an empty string. Qt's HTML parser walks a close tag up to the nearest
// open element of that name through any <table>, and puts a <td> in the
// nearest <tr> above it, in whichever table that is. So inside a table every
// close tag must match an element opened inside that same (innermost) table,
// and every cell must have a row of that table open; otherwise the rows that
// follow are built into a table the sanitizer did not count.
QString tagLeavingItsTable(const QString &out)
{
    static const QRegularExpression tag(
        QStringLiteral("<(/?)([a-z0-9]+)[^>]*>"));
    QStringList open;
    QList<qsizetype> tables;
    auto it = tag.globalMatch(out);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString where = m.captured(0) + QStringLiteral(" at ")
            + QString::number(m.capturedStart());
        const bool closing = !m.captured(1).isEmpty();
        const QString name = m.captured(2);
        const qsizetype base = tables.isEmpty() ? -1 : tables.last();
        if (name == QLatin1String("br") || name == QLatin1String("hr")
            || name == QLatin1String("img")) {
            // A void close tag is not inert: Qt's parseCloseTag closes
            // everything opened after the <br>/<hr> just before it.
            if (closing && base >= 0)
                return where;
            continue;
        }
        if (closing && name == QLatin1String("table")) {
            if (tables.isEmpty())
                return where;
            open.resize(tables.takeLast());
            continue;
        }
        if (closing || name == QLatin1String("td")
            || name == QLatin1String("th")) {
            const QString want = closing ? name : QStringLiteral("tr");
            qsizetype j = open.size() - 1;
            while (j > base && open[j] != want)
                --j;
            if (j == base) {
                if (!closing || base >= 0)
                    return where;
                continue;
            }
            if (closing) {
                open.resize(j);
                continue;
            }
        }
        if (name == QLatin1String("table"))
            tables.append(open.size());
        open.append(name);
    }
    return {};
}
}

class MessageHtmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void allowedFormattingSurvives()
    {
        QCOMPARE(sanitize(QStringLiteral("<strong>hi</strong> <em>there</em>")),
                 QStringLiteral("<strong>hi</strong> <em>there</em>"));
        QCOMPARE(sanitize(QStringLiteral("a<br>b")),
                 QStringLiteral("a<br>b"));
        QCOMPARE(sanitize(QStringLiteral("<code>x = 1</code>")),
                 QStringLiteral("<code>x = 1</code>"));
    }

    void aTableSurvivesWithOnlyTheSanitizersOwnAttributes()
    {
        const QString out = sanitize(QStringLiteral(
            "<table border=\"9\" width=\"5000\" bgcolor=\"red\" "
            "style=\"position:fixed\" onclick=\"x()\">"
            "<thead><tr class=\"h\"><th align=\"right\" style=\"color:red\">"
            "a</th><th>b</th></tr></thead>"
            "<tbody><tr><td width=\"999\" bgcolor=\"#f00\">1</td>"
            "<td colspan=\"500\">2</td></tr></tbody></table>"));
        QCOMPARE(out,
                 QStringLiteral(
                     "<table border=\"1\" cellspacing=\"0\" cellpadding=\"6\" "
                     "style=\"border-collapse:collapse\">"
                     "<thead><tr><th>a</th><th>b</th></tr></thead>"
                     "<tbody><tr><td>1</td><td>2</td></tr></tbody></table>"));
        const QString themed = MessageHtml::sanitize(
            QStringLiteral("<table><tr><td>x</td></tr></table>"), nullptr,
            QString(),
            MessageHtml::MentionStyle{QString(), QString(),
                                      QStringLiteral("#203040"), false});
        QVERIFY2(themed.startsWith(QStringLiteral(
                     "<table border=\"1\" cellspacing=\"0\" cellpadding=\"6\" "
                     "style=\"border-collapse:collapse; border-color:#203040\">")),
                 qPrintable(themed));
    }

    void tableStructureIsBoundedAndBalanced()
    {
        QCOMPARE(sanitize(QStringLiteral("<tr><td>a</td><td>b</td></tr>c")),
                 QStringLiteral("a b <br>c"));
        QString deep;
        for (int i = 0; i < 50; ++i)
            deep += QStringLiteral("<table><tr><td>");
        deep += QStringLiteral("core");
        for (int i = 0; i < 50; ++i)
            deep += QStringLiteral("</td></tr></table>");
        const QString out = sanitize(deep);
        QCOMPARE(out.count(QStringLiteral("<table")), 2);
        QCOMPARE(out.count(QStringLiteral("</table>")), 2);
        QVERIFY(out.contains(QStringLiteral("core")));
        const QString open = sanitize(QStringLiteral("<table><tr><td>x"));
        QCOMPARE(open.count(QStringLiteral("<table")), 1);
        QVERIFY(open.endsWith(QStringLiteral("</table>")));
        QCOMPARE(sanitize(QStringLiteral("x</table>y")), QStringLiteral("xy"));
    }

    void aTablePastTheColumnOrCellBoundIsFlattened()
    {
        const auto row = [](int cells) {
            QString r = QStringLiteral("<tr>");
            for (int c = 0; c < cells; ++c)
                r += QStringLiteral("<td>x</td>");
            return r + QStringLiteral("</tr>");
        };
        const auto table = [](const QString &rows) {
            return QStringLiteral("<table>") + rows + QStringLiteral("</table>");
        };
        const QString sixteen = sanitize(table(row(16)));
        QCOMPARE(sixteen.count(QStringLiteral("<td>")), 16);
        QCOMPARE(sixteen.count(QStringLiteral("</table>")), 1);
        const QString seventeen =
            sanitize(table(row(17)) + QStringLiteral("<b>after</b>"));
        QCOMPARE(seventeen,
                 QStringLiteral("x ").repeated(17) + QStringLiteral("<br>")
                     + QStringLiteral("<b>after</b>"));
        const QString wide = sanitize(table(row(300)));
        QVERIFY2(!wide.contains(QStringLiteral("<t")), qPrintable(wide.left(200)));
        QCOMPARE(wide.count(QLatin1Char('x')), 300);
        QCOMPARE(sanitize(table(row(16).repeated(25)))
                     .count(QStringLiteral("<td>")), 400);
        QVERIFY(!sanitize(table(row(16).repeated(25) + row(1)))
                     .contains(QStringLiteral("<table")));
        QCOMPARE(sanitize(QStringLiteral("<table><tr>")
                          + QStringLiteral("<td><em>y</em></td>").repeated(17)),
                 QStringLiteral("<em>y</em> ").repeated(17));
        const QString innerWide = sanitize(table(
            QStringLiteral("<tr><td>") + table(row(17))
            + QStringLiteral("</td><td>z</td></tr>")));
        QCOMPARE(innerWide.count(QStringLiteral("<table")), 1);
        QCOMPARE(innerWide.count(QStringLiteral("</table>")), 1);
        QCOMPARE(innerWide.count(QStringLiteral("<td>")), 2);
        const QString inner = table(row(16).repeated(13));
        const QString parentFull = sanitize(table(
            QStringLiteral("<tr><td>") + inner + QStringLiteral("</td><td>")
            + inner + QStringLiteral("</td></tr>")));
        QVERIFY2(!parentFull.contains(QStringLiteral("<t")),
                 qPrintable(parentFull.left(200)));
        QCOMPARE(parentFull.count(QLatin1Char('x')), 416);

        const QString emptyRows30 = row(16) + QStringLiteral("<tr>").repeated(30);
        const QString emptyRows24 = row(16) + QStringLiteral("<tr>").repeated(24);
        QVERIFY2(!sanitize(table(emptyRows30)).contains(QStringLiteral("<t")),
                 qPrintable(sanitize(table(emptyRows30)).left(200)));
        const QString kept24 = sanitize(table(emptyRows24));
        QCOMPARE(kept24.count(QStringLiteral("<table")), 1);
        QCOMPARE(kept24.count(QStringLiteral("<td>")), 16);
        QCOMPARE(kept24.count(QStringLiteral("<tr>")), 25);
        const auto inBody = [](const QString &rows) {
            return QStringLiteral("<table><tbody>") + rows
                + QStringLiteral("</tbody></table>");
        };
        QVERIFY(!sanitize(inBody(emptyRows30)).contains(QStringLiteral("<t")));
        QCOMPARE(sanitize(inBody(emptyRows24)).count(QStringLiteral("<table")), 1);
        QVERIFY(!sanitize(table(row(1) + QStringLiteral("<tr>").repeated(400)))
                     .contains(QStringLiteral("<t")));
        const QString tall =
            sanitize(table(row(16) + QStringLiteral("<tr>").repeated(15000)));
        QVERIFY2(!tall.contains(QStringLiteral("<t")), qPrintable(tall.left(200)));
        QCOMPARE(tall.count(QLatin1Char('x')), 16);
        const QString nestedTall = sanitize(table(
            QStringLiteral("<tr><td>") + table(emptyRows24)
            + QStringLiteral("</td><td>z</td></tr>")));
        QVERIFY2(!nestedTall.contains(QStringLiteral("<t")),
                 qPrintable(nestedTall.left(200)));
        QCOMPARE(nestedTall.count(QLatin1Char('x')), 16);
        QVERIFY(nestedTall.contains(QLatin1Char('z')));
    }

    void noTagLeavesTheTableItWasWrittenIn_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<int>("tables");
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("cells");
        const QString cells15 = QStringLiteral("<td>x</td>").repeated(15);
        const QString rows30 = QStringLiteral("<tr>").repeated(30);
        QTest::newRow("</td> ends the inner table")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><table></td></tr>") + rows30
                   + QStringLiteral("</table></table>")
            << 1 << 1 << 16;
        QTest::newRow("</tr> ends the inner table")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><table></tr>") + rows30
                   + QStringLiteral("</table></table>")
            << 1 << 1 << 16;
        QTest::newRow("</tbody> ends the inner table")
            << QStringLiteral("<table><tbody><tr>") + cells15
                   + QStringLiteral("<td><table></tbody>") + rows30
                   + QStringLiteral("</table></table>")
            << 1 << 1 << 16;
        QTest::newRow("</b> opened in the outer cell")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><b><table></b></td></tr>") + rows30
                   + QStringLiteral("</table></table>")
            << 1 << 1 << 16;
        QTest::newRow("a cell in no row of the inner table")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><table><td>y")
                   + QStringLiteral("<tr>").repeated(383)
                   + QStringLiteral("</table></td></tr></table>")
            << 1 << 1 << 16;
        QTest::newRow("</br> closes what follows a <br>")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><b><table><tr><td><br><b></br></b>")
                   + rows30
                   + QStringLiteral("</table></b></td></tr></table>")
            << 2 << 32 << 17;
        QTest::newRow("</hr> closes what follows an <hr>")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><b><table><tr><td><hr><b></hr></b>")
                   + rows30
                   + QStringLiteral("</table></b></td></tr></table>")
            << 2 << 32 << 17;
    }
    void noTagLeavesTheTableItWasWrittenIn()
    {
        QFETCH(QString, html);
        QFETCH(int, tables);
        QFETCH(int, rows);
        QFETCH(int, cells);
        const QString out = sanitize(html);
        const QString leaving = tagLeavingItsTable(out);
        QVERIFY2(leaving.isEmpty(),
                 qPrintable(leaving + QStringLiteral(" in ") + out.left(400)));
        QCOMPARE(out.count(QStringLiteral("<table")), tables);
        QCOMPARE(out.count(QStringLiteral("<tr>")), rows);
        QCOMPARE(out.count(QStringLiteral("<td>")), cells);
        QVERIFY(!out.contains(QStringLiteral("</br>")));
        QVERIFY(!out.contains(QStringLiteral("</hr>")));
    }

    void anImplicitlyClosedParagraphOrItemDoesNotOpenAnEscape()
    {
        const QString cells15 = QStringLiteral("<td>x</td>").repeated(15);
        const QString rows30 = QStringLiteral("<tr>").repeated(30);
        QCOMPARE(sanitize(QStringLiteral("<table><tr>") + cells15
                          + QStringLiteral("<td><p><table><tr><td><p>a<p>b"
                                           "</p></p></td></tr>")
                          + rows30 + QStringLiteral("</table></td></tr></table>")),
                 QStringLiteral("x ").repeated(15)
                     + QStringLiteral("<p><p>a<p>b</p></p> <br> <br>"));
        QCOMPARE(sanitize(QStringLiteral("<table><tr>") + cells15
                          + QStringLiteral("<td><ul><li><table><tr><td><ul>"
                                           "<li><li>a</li></li></td></tr>")
                          + rows30
                          + QStringLiteral("</table></li></ul></td></tr></table>")),
                 QStringLiteral("x ").repeated(15)
                     + QStringLiteral("<ul><li><ul><li><li>a</li></li> <br>"
                                      "</li></ul> <br>"));
        const QString gfm = sanitize(QStringLiteral(
            "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n"
            "<tbody>\n<tr>\n<td><strong>1</strong> https://example.org</td>\n"
            "<td><code>2</code></td>\n</tr>\n</tbody>\n</table>\n"));
        QCOMPARE(gfm.count(QStringLiteral("<table")), 1);
        QVERIFY2(tagLeavingItsTable(gfm).isEmpty(), qPrintable(gfm));
        const QString blocks = sanitize(QStringLiteral(
            "<table><tr><td><p>a</p><p>b</p><ul><li>c</li><li>d</li></ul>"
            "</td><td>e</td></tr></table>"));
        QCOMPARE(blocks.count(QStringLiteral("<table")), 1);
        QCOMPARE(blocks.count(QStringLiteral("<td>")), 2);
    }

    void oneCellBudgetCoversTheWholeBody()
    {
        const auto row = [](int cells) {
            return QStringLiteral("<tr>")
                + QStringLiteral("<td>x</td>").repeated(cells)
                + QStringLiteral("</tr>");
        };
        const auto table = [](const QString &rows) {
            return QStringLiteral("<table>") + rows + QStringLiteral("</table>");
        };
        const QString full =
            table(row(16) + QStringLiteral("<tr>").repeated(24));
        const QString many = sanitize(full.repeated(300));
        QCOMPARE(many.count(QStringLiteral("<table")), 1);
        QCOMPARE(many.count(QStringLiteral("</table>")), 1);
        const QString halves = sanitize(table(row(10).repeated(20)).repeated(3));
        QCOMPARE(halves.count(QStringLiteral("<table")), 2);
        QCOMPARE(halves.count(QStringLiteral("<td>")), 400);
        QCOMPARE(halves.count(QLatin1Char('x')), 600);
        QCOMPARE(sanitize(full + table(row(1))).count(QStringLiteral("<table")),
                 1);
        QCOMPARE(sanitize(table(row(1)) + full).count(QStringLiteral("<table")),
                 1);
        const QString nested = sanitize(
            table(row(10).repeated(20))
            + table(QStringLiteral("<tr><td>") + table(row(10).repeated(30))
                    + QStringLiteral("</td><td>z</td></tr>")));
        QCOMPARE(nested.count(QStringLiteral("<table")), 2);
        QCOMPARE(nested.count(QStringLiteral("<td>")), 202);
    }

    void attributesAreStripped()
    {
        QCOMPARE(sanitize(QStringLiteral(
                     "<span data-mx-color=\"#ff0000\" style=\"color:red\">x</span>")),
                 QStringLiteral("<span>x</span>"));
    }

    void aCoveredSpoilerIsASolidSlabBehindItsOwnToggleAnchor()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<span data-mx-spoiler>the twist</span>"),
            nullptr, QString(), style,false);
        QVERIFY(out.contains(QStringLiteral("href=\"spoiler:toggle\"")));
        QVERIFY(out.contains(
            QStringLiteral("background-color:#22262e;color:#22262e")));
        QVERIFY(out.contains(QStringLiteral("the twist")));
        QVERIFY(out.contains(QStringLiteral("</span></a>")));
    }

    void aRevealedSpoilerKeepsTheBackgroundButShowsTheText()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<span data-mx-spoiler=\"reason\">t</span>"),
            nullptr, QString(), style,true);
        QVERIFY(out.contains(QStringLiteral("href=\"spoiler:toggle\"")));
        QVERIFY(out.contains(QStringLiteral("background-color:#22262e")));
        QVERIFY(!out.contains(
            QStringLiteral("background-color:#22262e;color:#22262e")));
    }

    void aPlainSpanBesideASpoilerCLosesItsOwnTagOnly()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<span>a</span><span data-mx-spoiler>b</span><span>c</span>"),
            nullptr, QString(), style, false);
        QCOMPARE(out.count(QStringLiteral("<a ")), 1);
        QCOMPARE(out.count(QStringLiteral("</a>")), 1);
        QVERIFY(out.indexOf(QStringLiteral("</a>"))
                < out.indexOf(QStringLiteral("<span>c")));
    }

    void anUnclosedSpoilerSpanIsClosedAtTheEnd()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<span data-mx-spoiler>never closed"),
            nullptr, QString(), style, false);
        QVERIFY(out.endsWith(QStringLiteral("</span></a>")));
    }

    void spoilerAttributeVariantsAreRecognisedAndFakesAreNot()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const auto covered = [&](const QString &html) {
            return MessageHtml::sanitize(html, nullptr, QString(), style,
                                         false)
                .contains(QStringLiteral("spoiler:toggle"));
        };
        QVERIFY(covered(QStringLiteral("<span data-mx-spoiler>x</span>")));
        QVERIFY(covered(
            QStringLiteral("<span data-mx-spoiler=\"why\">x</span>")));
        QVERIFY(covered(
            QStringLiteral("<span DATA-MX-SPOILER>x</span>")));
        QVERIFY(!covered(
            QStringLiteral("<span data-mx-spoilerish>x</span>")));
        QVERIFY(!covered(
            QStringLiteral("<span notdata-mx-spoiler=\"1\">x</span>")));
    }

    void scriptAndStyleContentIsDropped()
    {
        QCOMPARE(sanitize(QStringLiteral("a<script>alert(1)</script>b")),
                 QStringLiteral("ab"));
        QCOMPARE(sanitize(QStringLiteral("a<style>body{}</style>b")),
                 QStringLiteral("ab"));
    }

    void imagesAreDropped()
    {
        QCOMPARE(sanitize(QStringLiteral(
                     "hi <img src=\"https://tracker/x.png\" onerror=\"e()\">")),
                 QStringLiteral("hi "));
    }

    // MSC2545 inline custom emoji are the one image this sanitizer emits: only
    // when marked as an emoticon and addressed by `mxc:`, which can only be
    // fetched through the authenticated media path.
    void onlyMarkedMxcEmoticonsSurvive()
    {
        const QString kept = sanitize(QStringLiteral(
            "a <img data-mx-emoticon src=\"mxc://e.org/blob\" "
            "alt=\":blob:\" title=\":blob:\" height=\"32\" /> b"));
        QVERIFY2(kept.contains(QStringLiteral("data-mx-emoticon")),
                 qPrintable(kept));
        QVERIFY2(kept.contains(QStringLiteral("mxc://e.org/blob")),
                 qPrintable(kept));
        QVERIFY2(kept.contains(QStringLiteral("alt=\":blob:\"")),
                 qPrintable(kept));
        QVERIFY2(!kept.contains(QStringLiteral("height=\"32\"")),
                 qPrintable(kept));

        for (const QString &hostile : {
                 QStringLiteral("<img src=\"mxc://e.org/blob\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"https://t/x.png\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"http://t/x.png\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"//t/x.png\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"javascript:e()\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"data:image/png;base64,AA\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"file:///etc/passwd\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"mxc://\">"),
                 QStringLiteral("<img data-mx-emoticon alt=\":x:\">"),
             }) {
            const QString out = sanitize(hostile);
            QVERIFY2(!out.contains(QStringLiteral("<img")), qPrintable(out));
        }

        const QString rebuilt = sanitize(QStringLiteral(
            "<img data-mx-emoticon src=\"mxc://e.org/b\" onerror=\"e()\" "
            "onload=\"e()\" style=\"position:fixed\" class=\"x\">"));
        QVERIFY2(!rebuilt.contains(QStringLiteral("onerror")), qPrintable(rebuilt));
        QVERIFY2(!rebuilt.contains(QStringLiteral("onload")), qPrintable(rebuilt));
        QVERIFY2(!rebuilt.contains(QStringLiteral("style")), qPrintable(rebuilt));
        QVERIFY2(!rebuilt.contains(QStringLiteral("class")), qPrintable(rebuilt));
    }

    void anEmoticonKeepsItsDescriptionAndShortcodeApart()
    {
        const QString kept = sanitize(QStringLiteral(
            "<img data-mx-emoticon src=\"mxc://packs.example.org/catWave\" "
            "alt=\"A cat &quot;waving&quot; hello\" title=\"Cat_Wave\" height=\"32\" />"));
        QVERIFY2(kept.contains(QStringLiteral("alt=\"A cat &quot;waving&quot; hello\"")),
                 qPrintable(kept));
        QVERIFY2(kept.contains(QStringLiteral("title=\"Cat_Wave\"")), qPrintable(kept));
        const QString pending = MessageHtml::resolveInlineImages(
            kept, [](const QString &) { return QString(); });
        QVERIFY2(pending.contains(QStringLiteral(":Cat_Wave:")), qPrintable(pending));
        QVERIFY2(!pending.contains(QStringLiteral("waving")), qPrintable(pending));
        for (const QString &marker : { QStringLiteral("data-mx-emoticon"),
                                       QStringLiteral("data-mx-emoticon=\"\""),
                                       QStringLiteral("data-mx-emoticon=\"no\"") }) {
            const QString out = sanitize(QStringLiteral("<img ") + marker
                + QStringLiteral(" src=\"mxc://e.org/b\" alt=\"b\">"));
            QVERIFY2(out.contains(QStringLiteral("mxc://e.org/b")), qPrintable(out));
        }
        for (const QString &bad : { QStringLiteral("mxc://e.org/a/b"),
                                    QStringLiteral("mxc://e.org/a?x=1"),
                                    QStringLiteral("mxc://e.org/a#f"),
                                    QStringLiteral("mxc://e.org/.."),
                                    QStringLiteral("mxc://user@e.org/a") }) {
            const QString out = sanitize(QStringLiteral("<img data-mx-emoticon src=\"")
                + bad + QStringLiteral("\">"));
            QVERIFY2(!out.contains(QStringLiteral("<img")), qPrintable(bad + " -> " + out));
        }
    }

    void anInlineEmojiKeepsItsAspectRatioWithinBounds()
    {
        QCOMPARE(MessageHtml::inlineEmojiWidth(1.0), 20);
        QCOMPARE(MessageHtml::inlineEmojiWidth(2.0), 40);
        QCOMPARE(MessageHtml::inlineEmojiWidth(0.5), 10);
        QCOMPARE(MessageHtml::inlineEmojiWidth(1000.0), 60);
        QCOMPARE(MessageHtml::inlineEmojiWidth(0.001), 6);
        QCOMPARE(MessageHtml::inlineEmojiWidth(0.0), 20);
        const QString safe = sanitize(QStringLiteral(
            "<img data-mx-emoticon src=\"mxc://e.org/wide\" alt=\"w\" title=\"w\">"));
        const auto resolve = [](const QString &mxc) {
            return QStringLiteral("file:///cache/") + mxc.section(QLatin1Char('/'), -1);
        };
        const QString wide = MessageHtml::resolveInlineImages(
            safe, resolve, [](const QString &) { return 3.0; });
        QVERIFY2(wide.contains(QStringLiteral("width=\"60\"")), qPrintable(wide));
        QVERIFY2(wide.contains(QStringLiteral("height=\"20\"")), qPrintable(wide));
        const QString unknown = MessageHtml::resolveInlineImages(
            safe, resolve, [](const QString &) { return 0.0; });
        QVERIFY2(unknown.contains(QStringLiteral("width=\"20\"")), qPrintable(unknown));
        QVERIFY(safe.contains(QStringLiteral("width=\"20\"")));
    }

    void anUnknownInlineAspectIsAskedAgainOnceTheFileArrives()
    {
        bool cached = false;
        int asked = 0;
        const auto aspectOf = MessageHtml::rememberingAspect([&](const QString &) {
            ++asked;
            return cached ? QSize(256, 128) : QSize();
        });
        const QString source = QStringLiteral("image://lightning-media/mxc%3A%2F%2Fe.org%2Fw");
        QCOMPARE(aspectOf(source), 0.0);
        cached = true;
        QCOMPARE(aspectOf(source), 2.0);
        cached = false;
        QCOMPARE(aspectOf(source), 2.0);
        QCOMPARE(asked, 2);
    }

    void resolvingAnEmojiNeverChangesWhatAnEditWouldSend()
    {
        const QString safe = sanitize(QStringLiteral(
            "x <img data-mx-emoticon src=\"mxc://e.org/b\" alt=\":b:\"> y"));
        QVERIFY(safe.contains(QStringLiteral("mxc://e.org/b")));

        const QString rendered = MessageHtml::resolveInlineImages(
            safe, [](const QString &mxc) {
                return QStringLiteral("image://lightning-media/") + mxc;
            });
        QVERIFY(rendered.contains(QStringLiteral("image://lightning-media/")));
        QVERIFY2(!rendered.contains(QStringLiteral("data-mx-emoticon src=\"mxc")),
                 qPrintable(rendered));
        QVERIFY(safe.contains(QStringLiteral("mxc://e.org/b")));

        const QString pending = MessageHtml::resolveInlineImages(
            safe, [](const QString &) { return QString(); });
        QVERIFY2(!pending.contains(QStringLiteral("<img")), qPrintable(pending));
        QVERIFY2(pending.contains(QStringLiteral(":b:")), qPrintable(pending));
    }

    void mxReplyFallbackIsDropped()
    {
        QCOMPARE(sanitize(QStringLiteral(
                     "<mx-reply><blockquote>old</blockquote></mx-reply>real")),
                 QStringLiteral("real"));
    }

    void unknownTagsAreDroppedButTextKept()
    {
        QCOMPARE(sanitize(QStringLiteral("<marquee>hello</marquee>")),
                 QStringLiteral("hello"));
    }

    void strayLessThanIsEscaped()
    {
        QCOMPARE(sanitize(QStringLiteral("a < b and c")),
                 QStringLiteral("a &lt; b and c"));
    }

    void javascriptLinksBecomePlainText()
    {
        const QString out = sanitize(QStringLiteral(
            "<a href=\"javascript:alert(1)\">click</a>"));
        QVERIFY(!out.contains(QStringLiteral("<a")));
        QVERIFY(!out.contains(QStringLiteral("javascript")));
        QCOMPARE(out, QStringLiteral("click"));
    }

    void httpLinksSurvive()
    {
        QCOMPARE(sanitize(QStringLiteral(
                     "<a href=\"https://example.com/x\">site</a>")),
                 QStringLiteral("<a href=\"https://example.com/x\">site</a>"));
    }

    void aBareUrlInAFormattedBodyBecomesALink()
    {
        const QString out = sanitize(QStringLiteral(
            "<a href=\"https://matrix.to/#/@bob:example.org\">Bob</a> look\n"
            "https://github.com/matrix-org/matrix.org/pull/3654"));
        QVERIFY2(out.contains(QStringLiteral(
                     "<a href=\"https://github.com/matrix-org/matrix.org/pull/"
                     "3654\">https://github.com/matrix-org/matrix.org/pull/3654"
                     "</a>")),
                 qPrintable(out));
        const QString punct = sanitize(QStringLiteral(
            "see https://example.com/a?x=1&amp;y=2, ok."));
        QVERIFY2(punct.contains(QStringLiteral(
                     "<a href=\"https://example.com/a?x=1&amp;y=2\">"
                     "https://example.com/a?x=1&amp;y=2</a>, ok.")),
                 qPrintable(punct));
    }

    void aBareUrlIsLeftAloneInsideLinksCodeAndSpoilers()
    {
        const QString link = sanitize(QStringLiteral(
            "<a href=\"https://a.example/\">https://a.example/</a>"));
        QCOMPARE(link.count(QStringLiteral("<a ")), 1);
        const QString code = sanitize(QStringLiteral(
            "<code>curl https://a.example/x</code>"));
        QVERIFY2(!code.contains(QStringLiteral("<a ")), qPrintable(code));
        const QString js = sanitize(QStringLiteral("javascript://a.example/x"));
        QVERIFY2(!js.contains(QStringLiteral("<a ")), qPrintable(js));
        const QString spoiler = sanitize(QStringLiteral(
            "<span data-mx-spoiler>https://a.example/x</span>"));
        QCOMPARE(spoiler.count(QStringLiteral("<a ")), 1);
    }

    void matrixToMentionBecomesInternalLinkWithLocalpart()
    {
        const QString out = sanitize(QStringLiteral(
            "<a href=\"https://matrix.to/#/@bob:example.org\">"
            "@bob:example.org</a> hey"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@bob</a> hey"));
    }

    void underlineLinksAddsAnUnderlineToMentionsOnly()
    {
        const QString html = QStringLiteral(
            "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a> "
            "<a href=\"https://example.org/x\">site</a>");
        MessageHtml::MentionStyle style{QStringLiteral("#112233"),
                                        QStringLiteral("#445566"), QString()};
        const QString flat = MessageHtml::sanitize(html, nullptr, QString(), style);
        QVERIFY(flat.contains(QStringLiteral("href=\"mention:@bob:example.org\"")));
        QVERIFY(flat.contains(QStringLiteral(";text-decoration:none\"")));

        style.underlineMentions = true;
        const QString underlined = MessageHtml::sanitize(html, nullptr, QString(), style);
        QVERIFY2(underlined.contains(QStringLiteral(";text-decoration:underline\"")),
                 qPrintable(underlined));
        QVERIFY(!underlined.contains(QStringLiteral("text-decoration:none")));
        for (const QString &out : {flat, underlined}) {
            const int at = out.indexOf(QStringLiteral("href=\"https://example.org/x\""));
            QVERIFY(at >= 0);
            const QString tag = out.mid(at, out.indexOf(QLatin1Char('>'), at) - at);
            QVERIFY2(!tag.contains(QStringLiteral("text-decoration")), qPrintable(tag));
        }
    }

    void mentionUsesResolvedDisplayName()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            [](const QString &id) {
                return id == QStringLiteral("@bob:example.org")
                    ? QStringLiteral("Bob Builder") : QString();
            },
            QString());
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@Bob Builder</a>"));
    }

    void theSendersAnchorLabelNeverNamesThePill()
    {
        const QString out = sanitize(QStringLiteral(
            "<a href=\"https://matrix.to/#/@attacker:evil.org\">@admin</a> hi"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@attacker:evil.org\">@attacker</a> hi"));
    }

    void resolvedNameWinsOverTheAnchorText()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">@old name</a>"),
            [](const QString &id) {
                return id == QStringLiteral("@bob:example.org")
                    ? QStringLiteral("Bob Builder") : QString();
            },
            QString());
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@Bob Builder</a>"));
    }

    void selfMentionIsEmphasised()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@me:example.org\">me</a>"),
            nullptr,
            QStringLiteral("@me:example.org"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@me:example.org\"><b>@me</b></a>"));
    }

    void mentionStyleIsInkAndWeightNotABox()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#7c7ff2"),
                                      QStringLiteral("#9295f5")});
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\" "
            "style=\"color:#9295f5;font-weight:600;"
            "text-decoration:none\">@bob</a>"));
        QVERIFY(!out.contains(QStringLiteral("background-color")));
    }

    void theAccentIsSpentOnlyOnAMentionOfYou()
    {
        const MessageHtml::MentionStyle style{QStringLiteral("#ffd447"),
                                              QStringLiteral("#9295f5")};
        const QString me = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@me:example.org\">me</a>"),
            nullptr, QStringLiteral("@me:example.org"), style);
        QVERIFY(me.contains(QStringLiteral("color:#ffd447")));
        QVERIFY(me.contains(QStringLiteral("<b>@me</b>")));

        const QString them = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QStringLiteral("@me:example.org"), style);
        QVERIFY(them.contains(QStringLiteral("color:#9295f5")));
        QVERIFY(!them.contains(QStringLiteral("#ffd447")));
    }

    void mentionInkFallsBackToTheAccentWhenNoLinkInkIsPushed()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#7c7ff2")});
        QVERIFY(out.contains(QStringLiteral("color:#7c7ff2")));
    }

    void externalLinksCarryTheThemeInk()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral("see <a href=\"https://matrix.org/\">spec</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#ffd447"),
                                      QStringLiteral("#9295f5")});
        QCOMPARE(out, QStringLiteral(
            "see <a href=\"https://matrix.org/\" "
            "style=\"color:#9295f5\">spec</a>"));
        QVERIFY(!out.contains(QStringLiteral("text-decoration")));
    }

    void unstyledBodiesAreUnchangedByTheInkPath()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral("see <a href=\"https://matrix.org/\">spec</a> and "
                           "<a href=\"https://matrix.to/#/@bob:e.org\">b</a>"),
            nullptr, QString());
        QCOMPARE(out, QStringLiteral(
            "see <a href=\"https://matrix.org/\">spec</a> and "
            "<a href=\"mention:@bob:e.org\">@bob</a>"));
    }

    void mentionStyleCannotBreakOutOfTheAttribute()
    {
        const QString mention = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{
                QStringLiteral("\"><script>bad</script>"),
                QStringLiteral("\"><script>bad</script>")});
        QVERIFY(!mention.contains(QStringLiteral("<script")));

        const QString link = MessageHtml::sanitize(
            QStringLiteral("<a href=\"https://matrix.org/\">spec</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{
                QStringLiteral("#ffd447"),
                QStringLiteral("\"><script>bad</script>")});
        QVERIFY(!link.contains(QStringLiteral("<script")));
    }

    void malformedInputDoesNotCrashAndFailsClosed()
    {
        QVERIFY(!sanitize(QStringLiteral("text <b unclosed")).contains(
            QStringLiteral("<b")));
        QCOMPARE(sanitize(QString()), QString());
        QCOMPARE(sanitize(QStringLiteral("just text")),
                 QStringLiteral("just text"));
    }

    void bodyWithoutACodeBlockIsExactlyOneSanitizedRichTextSegment()
    {
        const QStringList bodies = {
            QString(),
            QStringLiteral("just text"),
            QStringLiteral("<strong>hi</strong> <em>there</em>"),
            QStringLiteral("run <code>ls -la</code> please"),
            QStringLiteral("<blockquote>quoted</blockquote><p>after</p>"),
            QStringLiteral("<a href=\"https://matrix.to/#/@bob:example.org\">"
                           "bob</a> hey"),
            QStringLiteral("a < b and c"),
        };
        for (const QString &body : bodies) {
            const auto segs = segments(body);
            QCOMPARE(segs.size(), 1);
            QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
            QCOMPARE(segs.at(0).text, sanitize(body));
            QVERIFY(segs.at(0).language.isEmpty());
        }
    }

    void terminalTranscriptBecomesOnePlainTextBlock()
    {
        const QString html = QStringLiteral(
            "<pre><code>; &lt;&lt;&gt;&gt; DiG 9.18.24 &lt;&lt;&gt;&gt; "
            "example.com A\n"
            ";; global options: +cmd\n"
            ";; Got answer:\n"
            ";; -&gt;&gt;HEADER&lt;&lt;- opcode: QUERY, status: NOERROR, "
            "id: 4242\n"
            ";; flags: qr rd ra; QUERY: 1, ANSWER: 1\n"
            "\n"
            ";; ANSWER SECTION:\n"
            "example.com.\t\t3600\tIN\tA\t93.184.216.34\n"
            "</code></pre>");
        const auto segs = segments(html);
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 1);
        const QString code = codeAt(segs, 0);
        const QStringList lines = code.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 8);
        QVERIFY(lines.at(0).startsWith(QStringLiteral("; <<>> DiG")));
        QCOMPARE(lines.at(3),
                 QStringLiteral(";; ->>HEADER<<- opcode: QUERY, status: "
                                "NOERROR, id: 4242"));
        QCOMPARE(lines.at(5), QString());
        QCOMPARE(lines.at(7),
                 QStringLiteral("example.com.\t\t3600\tIN\tA\t93.184.216.34"));
        QVERIFY(!code.endsWith(QLatin1Char('\n')));
    }

    void oneVeryLongLineSurvivesWholeAndUnbroken()
    {
        const QString line(5000, QLatin1Char('x'));
        const auto segs = segments(
            QStringLiteral("<pre><code>") + line
            + QStringLiteral("</code></pre>"));
        const QString code = codeAt(segs, 0);
        QCOMPARE(code.size(), 5000);
        QVERIFY(!code.contains(QLatin1Char('\n')));
    }

    void hundredsOfLinesAllSurviveInOrder()
    {
        QStringList lines;
        for (int i = 0; i < 150; ++i)
            lines << QStringLiteral("line %1").arg(i);
        const auto segs = segments(QStringLiteral("<pre><code>")
                                   + lines.join(QLatin1Char('\n'))
                                   + QStringLiteral("</code></pre>"));
        const QString code = codeAt(segs, 0);
        const QStringList out = code.split(QLatin1Char('\n'));
        QCOMPARE(out.size(), 150);
        QCOMPARE(out.first(), QStringLiteral("line 0"));
        QCOMPARE(out.last(), QStringLiteral("line 149"));
    }

    void whitespaceIsPreservedExactly()
    {
        const QString html = QStringLiteral(
            "<pre><code class=\"language-python\">\n"
            "def f(x):\n"
            "\tif x:\n"
            "        return    x\n"
            "&nbsp;&nbsp;# padded\n"
            "</code></pre>");
        const auto segs = segments(html);
        const QStringList lines = codeAt(segs, 0).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 4);
        QCOMPARE(lines.at(0), QStringLiteral("def f(x):"));
        QCOMPARE(lines.at(1), QStringLiteral("\tif x:"));
        QCOMPARE(lines.at(2), QStringLiteral("        return    x"));
        QCOMPARE(lines.at(3), QStringLiteral("  # padded"));
        QCOMPARE(languageAt(segs, 0), QStringLiteral("python"));
    }

    void nonLatinTextAndEmojiSurviveInsideCode()
    {
        const QString html = QStringLiteral(
            "<pre><code># \u30b3\u30e1\u30f3\u30c8 \U0001F389 \u2014 "
            "\u043f\u0440\u0438\u0432\u0435\u0442\n"
            "print(\"h\u00e9llo \U0001F60A\")</code></pre>");
        const QStringList lines =
            codeAt(segments(html), 0).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(0),
                 QStringLiteral("# \u30b3\u30e1\u30f3\u30c8 \U0001F389 "
                                "\u2014 \u043f\u0440\u0438\u0432\u0435\u0442"));
        QCOMPARE(lines.at(1),
                 QStringLiteral("print(\"h\u00e9llo \U0001F60A\")"));
    }

    void numericEntitiesDecodeToRealCharacters()
    {
        const QString code = codeAt(
            segments(QStringLiteral(
                "<pre><code>a&#128512;b&#x41;c</code></pre>")), 0);
        QCOMPARE(code, QStringLiteral("a\U0001F600bAc"));
    }

    void htmlLookingCodeArrivesAsLiteralText()
    {
        const QString code = codeAt(
            segments(QStringLiteral(
                "<pre><code>&lt;div class=\"x\"&gt;hi&lt;/div&gt;\n"
                "&lt;script&gt;alert(1)&lt;/script&gt;\n"
                "a &amp;&amp; b</code></pre>")), 0);
        QCOMPARE(code,
                 QStringLiteral("<div class=\"x\">hi</div>\n"
                                "<script>alert(1)</script>\n"
                                "a && b"));
    }

    void entityDecodingDoesNotRunTwice()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre><code>&amp;lt;b&amp;gt;</code></pre>")), 0),
                 QStringLiteral("&lt;b&gt;"));
    }

    void dropWithContentStillDropsInsideACodeBlock()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre>a<script>bad()</script>b</pre>")), 0),
                 QStringLiteral("ab"));
    }

    void brInsideACodeBlockBecomesANewlineAndOtherTagsAreDropped()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre>one<br>t<b>w</b>o<br/>three</pre>")), 0),
                 QStringLiteral("one\ntwo\nthree"));
    }

    void textBeforeBetweenAndAfterBlocksBecomesRichTextInOrder()
    {
        const auto segs = segments(QStringLiteral(
            "before<pre><code>one</code></pre>"
            "between<pre><code>two</code></pre>after"));
        QCOMPARE(segs.size(), 5);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
        QCOMPARE(segs.at(0).text, QStringLiteral("before"));
        QVERIFY(segs.at(1).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(1).text, QStringLiteral("one"));
        QCOMPARE(segs.at(2).text, QStringLiteral("between"));
        QVERIFY(segs.at(3).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(3).text, QStringLiteral("two"));
        QCOMPARE(segs.at(4).text, QStringLiteral("after"));
    }

    void adjacentBlocksDoNotEmitAnEmptyRichTextSegmentBetweenThem()
    {
        const auto segs = segments(QStringLiteral(
            "<pre><code>one</code></pre>\n<p> </p>\n"
            "<pre><code>two</code></pre>"));
        QCOMPARE(segs.size(), 2);
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 2);
    }

    void inlineCodeAdjacentToAFencedBlockStaysInline()
    {
        const auto segs = segments(QStringLiteral(
            "try <code>ls -la</code> then:"
            "<pre><code>ls -la /tmp</code></pre>"
            "and <code>cd</code> back"));
        QCOMPARE(segs.size(), 3);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
        QVERIFY(segs.at(0).text.contains(QStringLiteral("<code>ls -la</code>")));
        QVERIFY(segs.at(1).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(1).text, QStringLiteral("ls -la /tmp"));
        QVERIFY(segs.at(2).text.contains(QStringLiteral("<code>cd</code>")));
    }

    void barePreIsOneBlock()
    {
        const auto segs = segments(QStringLiteral("<pre>plain\nblock</pre>"));
        QCOMPARE(segs.size(), 1);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(0).text, QStringLiteral("plain\nblock"));
        QVERIFY(segs.at(0).language.isEmpty());
    }

    void preInsideDroppedContentIsNotACodeBlock()
    {
        const QString html = QStringLiteral(
            "<mx-reply><blockquote><pre>old code</pre></blockquote>"
            "</mx-reply>real");
        const auto segs = segments(html);
        QCOMPARE(segs.size(), 1);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
        QCOMPARE(segs.at(0).text, sanitize(html));
        QVERIFY(!segs.at(0).text.contains(QStringLiteral("old code")));
    }

    void validLanguageTokensAreAccepted()
    {
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre><code class=\"language-rust\">fn main(){}"
                     "</code></pre>")), 0),
                 QStringLiteral("rust"));
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre class=\"language-go\">package main</pre>")), 0),
                 QStringLiteral("go"));
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre><code class=\"lang-c++\">x</code></pre>")), 0),
                 QStringLiteral("c++"));
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre><code class=\"highlight language-objective-c\">x"
                     "</code></pre>")), 0),
                 QStringLiteral("objective-c"));
    }

    void hostileOrOversizedClassesYieldNoLanguage()
    {
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code class=\"onclick=x\">x</code></pre>")), 0)
                    .isEmpty());
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code class=\"language-onclick=x\">x</code></pre>")),
                    0).isEmpty());
        const QString long40(40, QLatin1Char('a'));
        QVERIFY(languageAt(segments(
                    QStringLiteral("<pre><code class=\"language-") + long40
                    + QStringLiteral("\">x</code></pre>")), 0).isEmpty());
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code class='language-ru\"st'>x</code></pre>")), 0)
                    .isEmpty());
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code data-class=\"language-rust\">x</code></pre>")),
                    0).isEmpty());
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code>x</code></pre>")), 0).isEmpty());
    }

    void theRawClassStringNeverLeavesTheParser()
    {
        const auto segs = segments(QStringLiteral(
            "<pre><code class=\"language-rust theme-dracula tabindex\">x"
            "</code></pre>"));
        QCOMPARE(languageAt(segs, 0), QStringLiteral("rust"));
        for (const auto &s : segs) {
            QVERIFY(!s.language.contains(QStringLiteral("theme-dracula")));
            QVERIFY(!s.text.contains(QStringLiteral("theme-dracula")));
        }
    }

    void segmentCountIsBounded()
    {
        QString html;
        for (int i = 0; i < 100; ++i) {
            html += QStringLiteral("t%1").arg(i);
            html += QStringLiteral("<pre><code>c%1</code></pre>").arg(i);
        }
        const auto segs = segments(html);
        QCOMPARE(segs.size(), 64);
    }

    void codeTextVolumeIsBounded()
    {
        const QString first(200000, QLatin1Char('a'));
        const QString second(100000, QLatin1Char('b'));
        const auto segs = segments(
            QStringLiteral("<pre><code>") + first
            + QStringLiteral("</code></pre><pre><code>") + second
            + QStringLiteral("</code></pre>"));
        QCOMPARE(segs.size(), 1);
        QCOMPARE(codeAt(segs, 0).size(), 200000);
    }

    void malformedCodeMarkupFailsClosed()
    {
        QCOMPARE(codeAt(segments(QStringLiteral("<pre><code>one\ntwo")), 0),
                 QStringLiteral("one\ntwo"));
        const auto stray = segments(QStringLiteral("hi</pre>there"));
        QCOMPARE(stray.size(), 1);
        QVERIFY(stray.at(0).kind == MessageHtml::SegmentKind::RichText);
        const auto nested =
            segments(QStringLiteral("<pre>a<pre>b</pre>c</pre>"));
        QCOMPARE(countOf(nested, MessageHtml::SegmentKind::CodeBlock), 1);
        QCOMPARE(codeAt(nested, 0), QStringLiteral("abc"));
        QCOMPARE(segments(QStringLiteral("<pre><code>  </code></pre>")).size(),
                 0);
    }

    void aTagStormWithNoClosingBracketIsScannedInLinearTime()
    {
        const QString hostile =
            QStringLiteral("<pre") + QStringLiteral("<a").repeated(600000);
        QElapsedTimer clock;
        clock.start();
        const auto segs = segments(hostile);
        const qint64 ms = clock.elapsed();
        QVERIFY2(ms < 1500,
                 qPrintable(QStringLiteral("tag storm took %1 ms — the scan "
                                           "is quadratic again").arg(ms)));
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 0);
    }

    void aTagStormWithADistantClosingBracketIsAlsoLinear()
    {
        const QString hostile = QStringLiteral("<script><pre></script>")
            + QStringLiteral("</").repeated(300000) + QStringLiteral(">");
        QElapsedTimer clock;
        clock.start();
        const auto segs = segments(hostile);
        const qint64 ms = clock.elapsed();
        QVERIFY2(ms < 1500,
                 qPrintable(QStringLiteral("closing-tag storm took %1 ms — "
                                           "the scan is quadratic again")
                                .arg(ms)));
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 0);
    }

    void malformedTagShapesKeepTheirExactTextOutput()
    {
        QCOMPARE(sanitize(QStringLiteral("<a<a<a")),
                 QStringLiteral("&lt;a&lt;a&lt;a"));
        QCOMPARE(sanitize(QStringLiteral("</ >")), QStringLiteral("&lt;/ >"));
        QCOMPARE(sanitize(QStringLiteral("</></>")),
                 QStringLiteral("&lt;/>&lt;/>"));
        QCOMPARE(sanitize(QStringLiteral("<a<b>x")), QStringLiteral("x"));
        QCOMPARE(sanitize(QStringLiteral("a < b > c")),
                 QStringLiteral("a &lt; b > c"));
        QCOMPARE(sanitize(QStringLiteral("</</<b>hi</b>")),
                 QStringLiteral("&lt;/&lt;/<b>hi</b>"));
    }

    void anOrderedListKeepsTheNumberItContinuesFrom()
    {
        const QString out = sanitize(QStringLiteral(
            "<ol><li>one</li></ol><ul><li>sub</li></ul>"
            "<ol start=\"2\"><li>two</li></ol>"));
        QVERIFY2(out.contains(QStringLiteral("<ol start=\"2\">")),
                 qPrintable(QStringLiteral(
                     "the continuation lost its start, so it renders as 1 "
                     "again: %1").arg(out)));
        QVERIFY(out.startsWith(QStringLiteral("<ol>")));

        for (const QString &bad : { QStringLiteral("-4"),
                                    QStringLiteral("0"),
                                    QStringLiteral("99999999999"),
                                    QStringLiteral("1e3"),
                                    QStringLiteral(""),
                                    QStringLiteral("abc") }) {
            const QString got = sanitize(
                QStringLiteral("<ol start=\"%1\"><li>x</li></ol>").arg(bad));
            QVERIFY2(got.startsWith(QStringLiteral("<ol>"))
                         && !got.contains(QStringLiteral("start=")),
                     qPrintable(QStringLiteral("accepted start=%1 -> %2")
                                    .arg(bad, got)));
        }

        const QString hostile = sanitize(QStringLiteral(
            "<ol start=\"2\" onclick=\"evil()\" style=\"x\"><li>x</li></ol>"));
        QCOMPARE(hostile, QStringLiteral("<ol start=\"2\"><li>x</li></ol>"));
    }

    void roomMentionIsInkedWithoutBecomingALink()
    {
        const QString ink = QStringLiteral("#ff8800");
        const QString out =
            MessageHtml::markRoomMention(QStringLiteral("hey @room look"), ink);
        QVERIFY(out.contains(QStringLiteral("color:#ff8800")));
        QVERIFY(out.contains(QStringLiteral("background-color:#30ff8800")));
        const QString solid = MessageHtml::markRoomMention(
            QStringLiteral("@room"), QStringLiteral("#ffffff"),
            QStringLiteral("#e13239"));
        QVERIFY(solid.contains(QStringLiteral("background-color:#e13239")));
        QVERIFY(solid.contains(QStringLiteral("color:#ffffff")));
        QVERIFY(out.contains(QStringLiteral("font-weight:600")));
        QVERIFY(out.contains(QStringLiteral("@room")));
        QVERIFY(!out.contains(QStringLiteral("<a ")));
        QVERIFY(!out.contains(QStringLiteral("href")));
        QVERIFY(out.startsWith(QStringLiteral("hey ")));
        QVERIFY(out.endsWith(QStringLiteral(" look")));
        QCOMPARE(MessageHtml::markRoomMention(QStringLiteral("@room"),
                                              QString()),
                 QStringLiteral("@room"));
    }

    void roomMentionNeedsWordBoundariesAndSkipsMarkupAndCode()
    {
        const QString ink = QStringLiteral("#ff8800");
        const auto mark = [&ink](const QString &s) {
            return MessageHtml::markRoomMention(s, ink);
        };
        QCOMPARE(mark(QStringLiteral("@roomba")), QStringLiteral("@roomba"));
        QCOMPARE(mark(QStringLiteral("bot@room.example")),
                 QStringLiteral("bot@room.example"));
        const QString tag =
            QStringLiteral("<a href=\"https://x/@room\">click</a> @room");
        const QString marked = mark(tag);
        QVERIFY(marked.contains(QStringLiteral("href=\"https://x/@room\"")));
        QCOMPARE(marked.count(QStringLiteral("<span")), 1);
        QVERIFY(mark(QStringLiteral("&amp; @room")).contains(
            QStringLiteral("&amp;")));
        QCOMPARE(mark(QStringLiteral("<code>@room</code>")),
                 QStringLiteral("<code>@room</code>"));
        QCOMPARE(mark(QStringLiteral("<pre>@room</pre>")),
                 QStringLiteral("<pre>@room</pre>"));
        QVERIFY(mark(QStringLiteral("<code>@room</code> @room"))
                    .contains(QStringLiteral("<span")));
    }
};

QTEST_APPLESS_MAIN(MessageHtmlTest)
#include "MessageHtmlTest.moc"
