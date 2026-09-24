// One-line event summaries for the room list, DM list and notifications: a
// poll's multi-line MSC3381 fallback stays one line, and mention markdown
// ([label](https://matrix.to/...)) renders as the label.

#include "matrix/EventPreview.h"
#include "matrix/TimelineEvent.h"

#include <QtTest/QtTest>

using matrix::preview::normalizePreviewText;
using matrix::preview::oneLineSummary;

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
        // Multiple mentions in one body.
        QCOMPARE(normalizePreviewText(QStringLiteral(
                     "[@a](https://matrix.to/#/%40a%3Ax) and [@b](https://matrix.to/#/%40b%3Ax)")),
                 QStringLiteral("@a and @b"));
        // Non-matrix.to markdown links stay untouched (the preview is not a
        // markdown renderer).
        const QString other =
            QStringLiteral("[site](https://example.org/page)");
        QCOMPARE(normalizePreviewText(other), other);
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

        // No typed question: degrade to the fallback's first line, never
        // the answer list.
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

    // An MSC4274 gallery's media fields name its primary picture, which is not
    // a summary of the whole gallery.
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
        // State bodies pass through (already single-line from Rust).
        e = TimelineEvent{};
        e.type = TimelineEvent::StateChange;
        e.body = QStringLiteral("Alice joined the room.");
        QCOMPARE(oneLineSummary(e), QStringLiteral("Alice joined the room."));
    }
};

QTEST_GUILESS_MAIN(EventPreviewTest)
#include "EventPreviewTest.moc"
