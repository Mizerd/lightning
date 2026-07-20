// v0.7.1: sanitizer for incoming Matrix formatted bodies. Proves the
// allowlist is fail-closed (dangerous tags/attributes/schemes are stripped),
// that matrix.to mentions become internal "mention:" links with resolved
// display names and self-mention emphasis, and that ordinary formatting and
// http(s) links survive.

#include "models/MessageHtml.h"

#include <QtTest/QtTest>

namespace {
QString sanitize(const QString &html)
{
    return MessageHtml::sanitize(html, nullptr, QString());
}
} // namespace

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

    void attributesAreStripped()
    {
        // style / class / data-* and every other attribute are dropped.
        QCOMPARE(sanitize(QStringLiteral(
                     "<span data-mx-color=\"#ff0000\" style=\"color:red\">x</span>")),
                 QStringLiteral("<span>x</span>"));
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
        // No formatted body may cause a remote image fetch.
        QCOMPARE(sanitize(QStringLiteral(
                     "hi <img src=\"https://tracker/x.png\" onerror=\"e()\">")),
                 QStringLiteral("hi "));
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

    void matrixToMentionBecomesInternalLinkWithLocalpart()
    {
        // No resolver -> localpart fallback, never a bare MXID.
        const QString out = sanitize(QStringLiteral(
            "<a href=\"https://matrix.to/#/@bob:example.org\">"
            "@bob:example.org</a> hey"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@bob</a> hey"));
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

    void malformedInputDoesNotCrashAndFailsClosed()
    {
        // Unterminated tag: nothing after the '<' is emitted as markup.
        QVERIFY(!sanitize(QStringLiteral("text <b unclosed")).contains(
            QStringLiteral("<b")));
        // Empty and plain inputs round-trip safely.
        QCOMPARE(sanitize(QString()), QString());
        QCOMPARE(sanitize(QStringLiteral("just text")),
                 QStringLiteral("just text"));
    }
};

QTEST_APPLESS_MAIN(MessageHtmlTest)
#include "MessageHtmlTest.moc"
