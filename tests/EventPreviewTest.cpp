
#include "matrix/EventPreview.h"
#include "matrix/TimelineEvent.h"

#include <QCoreApplication>
#include <QTranslator>
#include <QtTest/QtTest>

using matrix::preview::normalizePreviewText;
using matrix::preview::oneLineSummary;
using matrix::preview::stripReplyFallback;

class EventPreviewTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void newlinesCollapseToOneLine()
    {
        QCOMPARE(normalizePreviewText(QStringLiteral("a\nb\r\nc\n\nd")),
                 QStringLiteral("a b c d"));
        QCOMPARE(normalizePreviewText(QStringLiteral("a b c")),
                 QStringLiteral("a b c"));
        QCOMPARE(normalizePreviewText(QStringLiteral("  spaced   out  ")),
                 QStringLiteral("spaced out"));
    }

    void mentionMarkdownReducesToLabel()
    {
        QCOMPARE(normalizePreviewText(QStringLiteral(
                     "[@test](https://matrix.to/#/%40test%3Amatrix.example.org) hello")),
                 QStringLiteral("@test hello"));
        QCOMPARE(normalizePreviewText(QStringLiteral(
                     "[@a](https://matrix.to/#/%40a%3Ax) and [@b](https://matrix.to/#/%40b%3Ax)")),
                 QStringLiteral("@a and @b"));
        const QString other =
            QStringLiteral("[site](https://example.org/page)");
        QCOMPARE(normalizePreviewText(other), other);
    }

    void replyFallbackIsNotPartOfThePreview()
    {
        QCOMPARE(normalizePreviewText(QStringLiteral(
                     "> <@moosix:cutefunny.art> You know what\n\nyes I do")),
                 QStringLiteral("yes I do"));
        QCOMPARE(normalizePreviewText(QStringLiteral(
                     "> <@a:b.c> one\n>\n> two\n\nanswer")),
                 QStringLiteral("answer"));
        QCOMPARE(normalizePreviewText(QStringLiteral(
                     "> <@a:b.c> > <@d:e.f> older\n> \n> newer\n\nanswer")),
                 QStringLiteral("answer"));
        QCOMPARE(stripReplyFallback(QStringLiteral(
                     "> <@a:b.c> q\r\n\r\nanswer")),
                 QStringLiteral("answer"));
        QCOMPARE(stripReplyFallback(QStringLiteral("> * <@a:b.c> waves\n\nok")),
                 QStringLiteral("ok"));
        QCOMPARE(stripReplyFallback(QStringLiteral(
                     "<mx-reply><blockquote>q</blockquote></mx-reply>hello")),
                 QStringLiteral("hello"));
    }

    void aMessageThatMerelyStartsWithAQuoteIsKept()
    {
        for (const QString &b : {
                 QStringLiteral("> <@a:b.c> quoting\nreply with no separator"),
                 QStringLiteral("> <@a:b.c> only a quote"),
                 QStringLiteral("> markdown quote\n\ntext"),
                 QStringLiteral("> not a mention <@a:b.c>\n\ntext"),
                 QStringLiteral("hi\n\n> <@a:b.c> later\n\nx"),
             }) {
            QCOMPARE(stripReplyFallback(b), b);
        }
        QCOMPARE(normalizePreviewText(QStringLiteral("> just a quote")),
                 QStringLiteral("> just a quote"));
    }

    void longPreviewsAreBounded()
    {
        const QString longBody(500, QLatin1Char('x'));
        const QString out = normalizePreviewText(longBody);
        QCOMPARE(out.size(), 120);
        QVERIFY(out.endsWith(QChar(0x2026)));
    }

    void pollSummarizesQuestionOnly()
    {
        TimelineEvent e;
        e.type = TimelineEvent::Poll;
        e.pollQuestion = QStringLiteral("Best answer?");
        e.body = QStringLiteral("Best answer?\n1. Yes\n2. No\n3. Big Money");
        QCOMPARE(oneLineSummary(e), QStringLiteral("Poll: Best answer?"));

        e.pollQuestion.clear();
        QCOMPARE(oneLineSummary(e), QStringLiteral("Poll: Best answer?"));

        e.body.clear();
        QCOMPARE(oneLineSummary(e), QStringLiteral("Poll"));
    }

    void mediaTypesSummarizeSemantically()
    {
        TimelineEvent e;
        e.type = TimelineEvent::Image;
        QCOMPARE(oneLineSummary(e), QStringLiteral("Image"));
        e.mediaFilename = QStringLiteral("cat.png");
        QCOMPARE(oneLineSummary(e), QStringLiteral("cat.png"));
        e.mediaMimetype = QStringLiteral("image/gif");
        QCOMPARE(oneLineSummary(e), QStringLiteral("GIF"));

        e = TimelineEvent{};
        e.type = TimelineEvent::File;
        e.mediaFilename = QStringLiteral("archive.zip");
        QCOMPARE(oneLineSummary(e), QStringLiteral("File: archive.zip"));
        e.mediaFilename.clear();
        QCOMPARE(oneLineSummary(e), QStringLiteral("File"));

        e = TimelineEvent{};
        e.type = TimelineEvent::Audio;
        e.mediaIsVoice = true;
        QCOMPARE(oneLineSummary(e), QStringLiteral("Voice message"));

        e = TimelineEvent{};
        e.type = TimelineEvent::Sticker;
        QCOMPARE(oneLineSummary(e), QStringLiteral("Sticker"));
    }

    void galleriesSummarizeAsWhatTheyHold()
    {
        TimelineEvent e;
        e.type = TimelineEvent::Image;
        e.mediaFilename = QStringLiteral("before.png");
        GalleryItem a;
        a.mediaKey = QStringLiteral("$g");
        a.kind = QStringLiteral("image");
        GalleryItem b = a;
        b.mediaKey = QStringLiteral("$g#item1");
        e.galleryItems = { a, b };
        QCOMPARE(oneLineSummary(e), QStringLiteral("2 images"));
        e.body = QStringLiteral("left is\n0.9.8");
        QCOMPARE(oneLineSummary(e), QStringLiteral("left is 0.9.8"));
        e.body.clear();
        e.galleryItems[1].kind = QStringLiteral("file");
        QCOMPARE(oneLineSummary(e), QStringLiteral("2 attachments"));
    }

    void summariesGoThroughTheTranslator()
    {
        struct Stub : QTranslator {
            QString translate(const char *ctx, const char *src, const char *,
                              int n) const override
            {
                if (QByteArray(ctx) != "EventPreview")
                    return {};
                const QByteArray s(src);
                if (s == "Message removed") return QStringLiteral("PANAIKINTA");
                if (s == "Poll: %1") return QStringLiteral("Apklausa: %1");
                if (s == "%n image(s)")
                    return n == 1 ? QStringLiteral("%n paveikslas")
                                  : QStringLiteral("%n paveikslai");
                return {};
            }
            bool isEmpty() const override { return false; }
        } stub;
        QCoreApplication::installTranslator(&stub);
        TimelineEvent e;
        e.redacted = true;
        QCOMPARE(oneLineSummary(e), QStringLiteral("PANAIKINTA"));
        TimelineEvent p;
        p.type = TimelineEvent::Poll;
        p.pollQuestion = QStringLiteral("Q?");
        QCOMPARE(oneLineSummary(p), QStringLiteral("Apklausa: Q?"));
        TimelineEvent g;
        g.type = TimelineEvent::Image;
        GalleryItem a;
        a.kind = QStringLiteral("image");
        g.galleryItems = { a, a };
        QCOMPARE(oneLineSummary(g), QStringLiteral("2 paveikslai"));
        QCoreApplication::removeTranslator(&stub);
        QCOMPARE(oneLineSummary(e), QStringLiteral("Message removed"));
        QCOMPARE(oneLineSummary(g), QStringLiteral("2 images"));
    }

    void aLiveLocationWithoutADescriptionIsNamed()
    {
        TimelineEvent live;
        live.type = TimelineEvent::Location;
        live.locationLive = true;
        QCOMPARE(oneLineSummary(live), QStringLiteral("Live location"));
        live.body = QStringLiteral("Heading home");
        QCOMPARE(oneLineSummary(live), QStringLiteral("Heading home"));
        TimelineEvent pin;
        pin.type = TimelineEvent::Location;
        pin.body = QStringLiteral("Vilnius");
        QCOMPARE(oneLineSummary(pin), QStringLiteral("Vilnius"));
    }

    void redactedAndUndecryptableAreHonest()
    {
        TimelineEvent e;
        e.type = TimelineEvent::TextMessage;
        e.redacted = true;
        e.body = QStringLiteral("should never appear");
        QCOMPARE(oneLineSummary(e), QStringLiteral("Message removed"));

        e.redacted = false;
        e.undecryptable = true;
        QCOMPARE(oneLineSummary(e), QStringLiteral("Unable to decrypt"));
    }

    void textBodiesNormalize()
    {
        TimelineEvent e;
        e.type = TimelineEvent::TextMessage;
        e.body = QStringLiteral(
            "[@test](https://matrix.to/#/%40test%3Ax) hi\nthere");
        QCOMPARE(oneLineSummary(e), QStringLiteral("@test hi there"));
        e = TimelineEvent{};
        e.type = TimelineEvent::StateChange;
        e.body = QStringLiteral("Alice joined the room.");
        QCOMPARE(oneLineSummary(e), QStringLiteral("Alice joined the room."));
    }

    void aStateRowNamesThePersonNotTheMxid()
    {
        const QString mxid = QStringLiteral("@lightningtest5:matrix.smetonis.net");
        TimelineEvent e;
        e.type = TimelineEvent::StateChange;
        e.sender = mxid;
        e.body = mxid + QStringLiteral(" updated room settings.");

        e.senderDisplayName = QStringLiteral("Test Five");
        QCOMPARE(oneLineSummary(e), QStringLiteral("Test Five updated room settings."));
        e.senderDisplayName.clear();
        QCOMPARE(oneLineSummary(e, QStringLiteral("From Cache")),
                 QStringLiteral("From Cache updated room settings."));
        QCOMPARE(oneLineSummary(e), QStringLiteral("lightningtest5 updated room settings."));
        QVERIFY(!oneLineSummary(e).contains(QLatin1Char(':')));
        QCOMPARE(oneLineSummary(e, mxid),
                 QStringLiteral("lightningtest5 updated room settings."));
    }

    void actorSentenceReplacesOnlyTheWholeLeadingSender()
    {
        using matrix::preview::actorSentence;
        const QString alice = QStringLiteral("@alice:example.org");
        QCOMPARE(actorSentence(alice + QStringLiteral(" invited Bob."), alice,
                               QStringLiteral("Alice")),
                 QStringLiteral("Alice invited Bob."));
        QCOMPARE(actorSentence(QStringLiteral("Bob joined the room."), alice,
                               QStringLiteral("Alice")),
                 QStringLiteral("Bob joined the room."));
        QCOMPARE(actorSentence(QStringLiteral("@alice:example.org.evil changed the room name."),
                               alice, QStringLiteral("Alice")),
                 QStringLiteral("@alice:example.org.evil changed the room name."));
        QCOMPARE(actorSentence(QStringLiteral("Encryption was enabled."), alice,
                               QStringLiteral("Alice")),
                 QStringLiteral("Encryption was enabled."));
        QCOMPARE(actorSentence(alice + QStringLiteral(" left."), QString(),
                               QStringLiteral("Alice")),
                 alice + QStringLiteral(" left."));
    }

    // Review M1: the display name is member-chosen room state. As a name,
    // "@admin:example.org" would make a power-level row read exactly like
    // the id-phrased sentence it replaced, so an address-shaped name (NFKC,
    // fullwidth too) or one another member uses carries the localpart, and
    // bidi/invisible characters are dropped. The incoming-call card's rule.
    void anActorNameCannotImpersonateAnAddressOrHideCharacters()
    {
        using matrix::preview::actorSentence;
        const QString mallory = QStringLiteral("@mallory:evil.example");
        const QString sentence = mallory + QStringLiteral(" changed the power levels.");

        QCOMPARE(actorSentence(sentence, mallory, QStringLiteral("@admin:example.org")),
                 QStringLiteral("@admin:example.org (mallory) changed the power levels."));
        const QString fullwidth = QString(QChar(0xFF20)) + QStringLiteral("admin")
            + QChar(0xFF1A) + QStringLiteral("example.org");
        QVERIFY(actorSentence(sentence, mallory, fullwidth)
                    .contains(QStringLiteral("(mallory) changed")));
        const QString bidi = QStringLiteral("Ad") + QChar(0x202E) + QStringLiteral("min")
            + QChar(0x200B) + QChar(0x2066);
        QCOMPARE(actorSentence(sentence, mallory, bidi),
                 QStringLiteral("Admin changed the power levels."));
        QCOMPARE(actorSentence(sentence, mallory, QString(QChar(0x200B)) + QChar(0x202E)),
                 QStringLiteral("mallory changed the power levels."));
        QCOMPARE(actorSentence(sentence, mallory, mallory),
                 QStringLiteral("mallory changed the power levels."));
        QCOMPARE(actorSentence(sentence, mallory, QStringLiteral("Admin"), true),
                 QStringLiteral("Admin (mallory) changed the power levels."));

        TimelineEvent e;
        e.type = TimelineEvent::StateChange;
        e.sender = mallory;
        e.body = sentence;
        e.senderDisplayName = QStringLiteral("@admin:example.org");
        QCOMPARE(oneLineSummary(e),
                 QStringLiteral("@admin:example.org (mallory) changed the power levels."));
        e.senderDisplayName = QStringLiteral("Admin");
        e.senderNameAmbiguous = true;
        QCOMPARE(oneLineSummary(e),
                 QStringLiteral("Admin (mallory) changed the power levels."));
    }
};

QTEST_GUILESS_MAIN(EventPreviewTest)
#include "EventPreviewTest.moc"
