// Swarm C17 regressions for the sanitizer's table flattening and the spoiler
// markup contract the delegate keys on.
#include "models/MessageHtml.h"

#include <QtTest/QtTest>

namespace {
QString sanitize(const QString &html, bool reveal = false,
                 const QString &cover = QStringLiteral("#303030"))
{
    MessageHtml::MentionStyle style;
    style.codeBackground = cover;
    return MessageHtml::sanitize(html, nullptr, QString(), style, reveal);
}
QString nested(const QString &innerText)
{
    // Three tables deep: the innermost is past kMaxTableDepth and is
    // flattened into the middle cell, which already holds text.
    return QStringLiteral(
               "<table><tr><td>outer<table><tr><td>inner<table><tr><td>")
        + innerText
        + QStringLiteral("</td></tr></table></td></tr></table></td></tr></table>");
}
} // namespace

class MessageHtmlC17Test : public QObject
{
    Q_OBJECT
private slots:
    // C17-F07: "inner" and the flattened "too deep" ran together.
    void aFlattenedNestedTableIsSeparatedFromTheTextBeforeIt()
    {
        const QString out = sanitize(nested(QStringLiteral("too deep")));
        QVERIFY2(!out.contains(QStringLiteral("innertoo")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("inner too deep")),
                 qPrintable(out));
    }

    // C17-F01 contract: a covered spoiler is the one run whose style sets the
    // text colour (to the slab colour); a revealed one never does. The
    // delegate disables selection while the covered form is present, since
    // the selection highlight repaints that run in clear.
    void coveredAndRevealedSpoilersAreDistinguishable()
    {
        const QString src = QStringLiteral(
            "a <span data-mx-spoiler>secret</span> b");
        const QString covered = sanitize(src, false);
        const QString revealed = sanitize(src, true);
        QVERIFY2(covered.contains(QStringLiteral(";color:#303030")),
                 qPrintable(covered));
        QVERIFY2(!revealed.contains(QStringLiteral(";color:")),
                 qPrintable(revealed));
        QVERIFY(covered.contains(QStringLiteral("spoiler:toggle")));
        QVERIFY(revealed.contains(QStringLiteral("spoiler:toggle")));
    }
};

QTEST_APPLESS_MAIN(MessageHtmlC17Test)
#include "MessageHtmlC17Test.moc"
