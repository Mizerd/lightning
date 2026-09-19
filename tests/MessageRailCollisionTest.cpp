// THE ROW'S RIGHT RAIL IS SHARED, AND THREE THINGS WERE FIGHTING OVER IT.
//
// Reported 2026-09-14 with a screenshot: "this is a bit messy and hard to
// click on stuff". Read-receipt avatars sat ON TOP of the hover action bar,
// over its Edit and overflow buttons. The reporter guessed at their display
// scaling; it is not the scaling, it is that the bar and the facepile are
// anchored to the same edge from opposite ends of the row and nothing
// arbitrated between them. Reproduced live on this machine before the fix,
// and the avatars win: both carry z 3 and the receipt strip is later in the
// document, so the buttons underneath are not merely ugly, they are
// unclickable.
//
// Two more of the same shape turned up in the Bubbles audit that followed:
//   * the sender identity header rendered OUTSIDE its bubble, because the
//     header's width cap was derived from the bubble that is itself sized
//     from the header — a loop Qt resolves by pinning the header to one
//     pixel of contributed width;
//   * the facepile clipped the bottom-right corner of an own bubble, because
//     the bubble's width cap reserves a 40px rail that its PLACEMENT then
//     ignored.
//
// A 2026-09-19 GUI audit added four more, all of them in Bubbles and all of
// them found by the same method — a geometric sweep over the real delegate,
// 23 fixture variants x 3 layouts x own/other x two row widths x two text
// scales x hover/idle. Modern and Compact produced zero violations; every
// failure was a child of the Bubbles bubble being sized against the bubble's
// OUTER width, or the reactions Flow packing from the wrong edge.
//
// These are GEOMETRIC assertions on the real delegate, not a source scan.
// A scan cannot see an overlap: every one of these defects was present in a
// file that read as though it handled the case, and two of them are sitting
// underneath comments that describe the collision being handled.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;
constexpr qreal kRowWidth = 640;
}

class MessageRailCollisionTest : public QObject
{
    Q_OBJECT

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
    };

    /// A complete role map, so the production delegate binds without
    /// undefined-property warnings. Same shape MediaPlaceholderQmlTest uses.
    static QVariantMap baseFixture()
    {
        QVariantMap f;
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("isCallEvent"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), QStringLiteral("$rail"));
        f.insert(QStringLiteral("itemId"), QStringLiteral("rail-item"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@a:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"),
                 QStringLiteral("A Sender With A Fairly Long Name"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("AS"));
        f.insert(QStringLiteral("body"), QStringLiteral("ok"));
        f.insert(QStringLiteral("eventType"), 0);
        f.insert(QStringLiteral("status"), 0);
        f.insert(QStringLiteral("isOwn"), false);
        f.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
        f.insert(QStringLiteral("redacted"), false);
        f.insert(QStringLiteral("edited"), false);
        f.insert(QStringLiteral("isEncrypted"), false);
        f.insert(QStringLiteral("isDecrypted"), true);
        f.insert(QStringLiteral("undecryptable"), false);
        f.insert(QStringLiteral("errorKind"), QString{});
        f.insert(QStringLiteral("isImage"), false);
        f.insert(QStringLiteral("isFile"), false);
        f.insert(QStringLiteral("isVideo"), false);
        f.insert(QStringLiteral("isAudio"), false);
        f.insert(QStringLiteral("isSticker"), false);
        f.insert(QStringLiteral("mediaIsVoice"), false);
        f.insert(QStringLiteral("mediaDurationMs"), 0);
        f.insert(QStringLiteral("mediaWidth"), 0);
        f.insert(QStringLiteral("mediaHeight"), 0);
        f.insert(QStringLiteral("mediaSize"), 0);
        f.insert(QStringLiteral("mediaSourceAvailable"), false);
        f.insert(QStringLiteral("mediaMimetype"), QString{});
        f.insert(QStringLiteral("reactions"), QVariantList{});
        f.insert(QStringLiteral("replyToEventId"), QString{});
        f.insert(QStringLiteral("isThreadRoot"), false);
        f.insert(QStringLiteral("mentionsMe"), false);
        f.insert(QStringLiteral("mentionsRoom"), false);
        f.insert(QStringLiteral("isLocalEcho"), false);
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        return f;
    }

    /// `count` other readers, in the shape the strip's chips expect.
    static QVariantList receipts(int count)
    {
        QVariantList out;
        for (int i = 0; i < count; ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"),
                     QStringLiteral("@r%1:mock.local").arg(i));
            r.insert(QStringLiteral("displayName"),
                     QStringLiteral("Reader %1").arg(i));
            r.insert(QStringLiteral("initials"), QStringLiteral("R%1").arg(i));
            r.insert(QStringLiteral("avatarUrl"), QString{});
            out.append(r);
        }
        return out;
    }

    /// `count` reaction pills, in the shape the Flow's chips expect.
    static QVariantList reactions(int count)
    {
        static const char *keys[] = {"\xf0\x9f\x91\x8d", "\xf0\x9f\x8e\x89",
                                     "\xf0\x9f\x94\xa5"};
        QVariantList out;
        for (int i = 0; i < count; ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("key"),
                     QString::fromUtf8(keys[i % 3]));
            r.insert(QStringLiteral("count"), i + 1);
            r.insert(QStringLiteral("byMe"), i == 0);
            r.insert(QStringLiteral("senders"),
                     QVariantList{QStringLiteral("@r:mock.local")});
            r.insert(QStringLiteral("names"),
                     QVariantList{QStringLiteral("Reader")});
            out.append(r);
        }
        return out;
    }

    bool build(AppController &controller, const QVariantMap &fixture,
               Delegate &out, qreal rowWidth = kRowWidth,
               bool direct = false)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy created(out.engine.get(),
                           &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (created.isEmpty() && !created.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(800, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(rowWidth);
        // Bubbles is a DIRECT-room layout and the flag normally comes from
        // the host view this delegate has none of. Set it BEFORE the first
        // layout pass: switching a laid-out row from Modern to Bubbles
        // leaves the content caps a frame behind and the measurement is of
        // neither layout.
        if (direct)
            QQmlProperty::write(out.root, QStringLiteral("isDirectRoom"), true);
        out.window->show();
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        return true;
    }

    /// The action bar is created on hover, which needs a live timelineView.
    /// Forcing the Loader is enough for a GEOMETRY question: its x and width
    /// come from anchors and the reserve, neither of which consults
    /// `visible`.
    static QQuickItem *forceActionBar(QQuickItem *root)
    {
        auto *loader = root->findChild<QQuickItem *>(
            QStringLiteral("messageActionBarLoader"));
        if (!loader)
            return nullptr;
        QQmlProperty::write(loader, QStringLiteral("active"), true);
        QQmlProperty::write(loader, QStringLiteral("visible"), true);
        QCoreApplication::processEvents();
        return loader;
    }

    static qreal rightEdgeIn(QQuickItem *item, QQuickItem *reference)
    {
        return item->mapToItem(reference, QPointF(item->width(), 0)).x();
    }
    static qreal leftEdgeIn(QQuickItem *item, QQuickItem *reference)
    {
        return item->mapToItem(reference, QPointF(0, 0)).x();
    }

private Q_SLOTS:
    // Isolate the settings store: these build a real AppController, hence a
    // real SettingsManager, which resolves its file from the application
    // identity. Same two guards MediaPlaceholderQmlTest documents.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("message-rail-collision-test"));
    }

    // THE REPORTED DEFECT. A short row with receipts: the bar must end
    // before the facepile begins.
    //
    // FAIL-ON-OLD: drop `+ root.actionBarReceiptReserve` from the loader's
    // rightMargin and the bar's right edge lands past the pile's left edge
    // by the pile's full width.
    void theActionBarClearsTheReceiptPileOnAShortRow()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);

        Delegate d;
        QVERIFY(build(app, f, d));
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY2(bar, "no messageActionBarLoader in the row");
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY2(pile, "no readReceiptRow in the row");
        QVERIFY2(pile->width() > 0, "the receipt facepile measured empty");
        QVERIFY2(bar->width() > 0, "the action bar measured empty");

        const qreal barRight = rightEdgeIn(bar, d.root);
        const qreal pileLeft = leftEdgeIn(pile, d.root);
        QVERIFY2(barRight <= pileLeft + 1.0,
                 qPrintable(QStringLiteral(
                     "the action bar (ends at %1) runs into the read-receipt "
                     "facepile (starts at %2) — the avatars paint over its "
                     "buttons and the buttons cannot be pressed")
                     .arg(barRight).arg(pileLeft)));
    }

    // AND IT IS A RESERVATION, NOT A PERMANENT INDENT. On a tall row the bar
    // is nowhere near the pile and must keep the row's corner, or every
    // message in a room with receipts pays for a collision it never has.
    void aTallRowKeepsTheBarInTheCorner()
    {
        AppController app(AppController::MockBackend);
        QVariantMap shortRow = baseFixture();
        shortRow.insert(QStringLiteral("readReceipts"), receipts(4));
        shortRow.insert(QStringLiteral("readReceiptsTotal"), 4);
        QVariantMap tallRow = shortRow;
        tallRow.insert(
            QStringLiteral("body"),
            QStringLiteral("a body long enough to wrap over several lines so "
                           "that the row is far taller than the action bar "
                           "and the facepile at its bottom edge cannot reach "
                           "the bar at its top edge, which is the ordinary "
                           "case and must not be indented"));

        Delegate shortD;
        QVERIFY(build(app, shortRow, shortD));
        QQuickItem *shortBar = forceActionBar(shortD.root);
        QVERIFY(shortBar);
        const qreal shortRight = rightEdgeIn(shortBar, shortD.root);

        Delegate tallD;
        QVERIFY(build(app, tallRow, tallD));
        QQuickItem *tallBar = forceActionBar(tallD.root);
        QVERIFY(tallBar);
        auto *tallPile = tallD.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(tallPile);
        const qreal tallRight = rightEdgeIn(tallBar, tallD.root);

        QVERIFY2(tallD.root->height() > shortD.root->height() + 20,
                 "the fixture did not actually produce a taller row");
        QVERIFY2(tallRight > shortRight + 1.0,
                 qPrintable(QStringLiteral(
                     "a tall row indented the bar anyway (tall ends at %1, "
                     "short at %2): the reservation is unconditional")
                     .arg(tallRight).arg(shortRight)));
    }

    // BUBBLES: THE IDENTITY HEADER BELONGS INSIDE THE BUBBLE.
    //
    // The header's width cap was `bubble.width - 112`, and in Bubbles the
    // bubble is SIZED FROM the column the header is in — so the cap fed the
    // header's own input. Qt resolves that with whatever the bubble measured
    // last, which for a short body is under 112, so `Math.max(1, …)` pinned
    // the header's contributed width to ONE PIXEL: the bubble sized itself to
    // the body alone and the sender name and timestamp rendered outside it,
    // over the timeline background. Seen live 2026-09-14 on a DM reply of
    // "got it".
    //
    // FAIL-ON-OLD: restore the unconditional `bubble.width - 112` cap.
    void aBubbleContainsItsOwnSenderHeader()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        Delegate d;
        QVERIFY(build(app, f, d));
        // Bubbles applies to DIRECT rooms only, and the flag normally comes
        // from the host view this delegate has none of.
        QQmlProperty::write(d.root, QStringLiteral("isDirectRoom"), true);
        QCoreApplication::processEvents();
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool(),
                 "the fixture is not in Bubbles mode; the rest proves nothing");

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *header = d.root->findChild<QQuickItem *>(
            QStringLiteral("senderIdentityHeader"));
        QVERIFY2(bubble, "no bubble in the row");
        QVERIFY2(header, "no sender identity header in the row");
        QVERIFY2(header->width() > 40,
                 qPrintable(QStringLiteral(
                     "the identity header measured %1px wide — it was pinned "
                     "to its minimum by a cap derived from the bubble it "
                     "sizes").arg(header->width())));

        const qreal headerRight = rightEdgeIn(header, d.root);
        const qreal bubbleRight = rightEdgeIn(bubble, d.root);
        QVERIFY2(headerRight <= bubbleRight + 1.0,
                 qPrintable(QStringLiteral(
                     "the sender header (ends at %1) spills past its bubble "
                     "(ends at %2) and renders on the timeline background")
                     .arg(headerRight).arg(bubbleRight)));
    }

    // BUBBLES: AN OWN BUBBLE MUST LEAVE THE RECEIPT RAIL ALONE.
    //
    // The width cap subtracts 40 for a rail; the PLACEMENT right-aligned to
    // `parent.width` and ignored it, so a short own bubble was pushed flush
    // to the row edge and the avatars clipped its bottom-right corner.
    //
    // FAIL-ON-OLD: drop `- root.bubbleReceiptInset` from the bubble's x.
    void anOwnBubbleClearsTheReceiptPile()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("readReceipts"), receipts(3));
        f.insert(QStringLiteral("readReceiptsTotal"), 3);

        Delegate d;
        QVERIFY(build(app, f, d));
        QQmlProperty::write(d.root, QStringLiteral("isDirectRoom"), true);
        QCoreApplication::processEvents();
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(bubble);
        QVERIFY(pile);
        QVERIFY2(pile->width() > 0, "the receipt facepile measured empty");

        const qreal bubbleRight = rightEdgeIn(bubble, d.root);
        const qreal pileLeft = leftEdgeIn(pile, d.root);
        QVERIFY2(bubbleRight <= pileLeft + 1.0,
                 qPrintable(QStringLiteral(
                     "the own bubble (ends at %1) runs under the read-receipt "
                     "facepile (starts at %2), which clips its corner")
                     .arg(bubbleRight).arg(pileLeft)));
    }

    // ── 2026-09-19 GUI AUDIT ────────────────────────────────────────────

    /// The union of every reaction pill and the add chip, in row coords.
    static QRectF chipBand(QQuickItem *root)
    {
        QRectF acc;
        const auto chips = root->findChildren<QQuickItem *>(
            QStringLiteral("reactionChip"));
        auto add = root->findChildren<QQuickItem *>(
            QStringLiteral("reactionAddChip"));
        QList<QQuickItem *> all = chips;
        all += add;
        for (auto *c : all) {
            if (!c->isVisible() || c->width() <= 0)
                continue;
            const QPointF tl = c->mapToItem(root, QPointF(0, 0));
            const QRectF r(tl, QSizeF(c->width(), c->height()));
            acc = acc.isNull() ? r : acc.united(r);
        }
        return acc;
    }

    /// Everything `bubbleContent` lays out is inset by `bubblePad`, so the
    /// bubble's INNER right edge is what a child may reach.
    static qreal innerRight(QQuickItem *root, QQuickItem *bubble)
    {
        const qreal pad =
            QQmlProperty::read(root, QStringLiteral("bubblePad")).toReal();
        return bubble->mapToItem(root, QPointF(bubble->width(), 0)).x() - pad;
    }

    // BUBBLES: A REACTION ON YOUR OWN MESSAGE BELONGS UNDER YOUR OWN MESSAGE.
    //
    // The reactions Flow fills the row and packs from its own left edge,
    // and an own bubble is right-aligned — so the chips were laid out at
    // the row's LEFT edge with the bubble they annotate at the right.
    // Measured live 2026-09-19 in a DM on a 1920px window: chips at
    // x 444..520, the bubble at x 1156..1883, which reads as a reaction on
    // the OTHER person's side of the conversation.
    //
    // FAIL-ON-OLD: restore `Layout.leftMargin: root.avatarGutterWidth` on
    // reactionsFlow and the band lands at the row's left edge again.
    void ownBubbleReactionsHangUnderTheirBubble()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("reactions"), reactions(3));

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubble);
        const QRectF band = chipBand(d.root);
        QVERIFY2(!band.isNull(), "the fixture produced no reaction chips");

        const qreal bubbleLeft = leftEdgeIn(bubble, d.root);
        // One chip run (220px, the Flow's own minimum band) is the widest
        // separation that still reads as "these belong to that message".
        QVERIFY2(band.right() >= bubbleLeft - 240.0,
                 qPrintable(QStringLiteral(
                     "the reaction chips end at %1 but the own bubble starts "
                     "at %2 — they are stranded at the row's left edge, on "
                     "the incoming side of the conversation")
                     .arg(band.right()).arg(bubbleLeft)));
    }

    // BUBBLES: THE BODY MUST STAY INSIDE THE BUBBLE'S PADDING.
    //
    // `bubbleContent` insets every child by `bubblePad` (10 in Bubbles, 0 in
    // Modern/Compact), but the body's cap was `bubble.width - 8` — the
    // bubble's OUTER width. Measured 2026-09-19 at a 640px row: bubble
    // 84..640, messageBody 94..642, i.e. 12px past the bubble's inner edge
    // and 2px past the ROW. Invisible in Modern/Compact, where the padding
    // is zero and the same expression is 8px conservative.
    //
    // FAIL-ON-OLD: put `Math.min(720, bubble.width - 8)` back on bodyLabel.
    void aBubbleContainsItsOwnBodyText()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        // One unbroken run, so the body is laid out AT its cap rather than
        // at a word boundary below it.
        f.insert(QStringLiteral("body"), QString(220, QLatin1Char('W')));

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(bubble);
        QVERIFY2(body && body->width() > 0, "no message body in the row");

        const qreal bodyRight = rightEdgeIn(body, d.root);
        QVERIFY2(bodyRight <= innerRight(d.root, bubble) + 0.6,
                 qPrintable(QStringLiteral(
                     "the body ends at %1, past the bubble's inner edge %2 "
                     "(bubble ends at %3) — the text is drawn on and over "
                     "the bubble's own border")
                     .arg(bodyRight).arg(innerRight(d.root, bubble))
                     .arg(rightEdgeIn(bubble, d.root))));
        QVERIFY2(bodyRight <= d.root->width() + 0.6,
                 qPrintable(QStringLiteral(
                     "the body ends at %1, past the row's own width %2")
                     .arg(bodyRight).arg(d.root->width())));
    }

    // BUBBLES: AND SO MUST A MEDIA CARD, WHICH IS THE VISIBLE HALF.
    //
    // Every card sized itself `Math.min(N, bubble.width)` — again the OUTER
    // width — so on a row narrow enough for the bubble's cap to bind, the
    // card was drawn 10px past the bubble on the right and, for an own
    // message, 10px past the row. Measured 2026-09-19 at a 360px row:
    // bubble 84..360, fileCard 94..370.
    //
    // FAIL-ON-OLD: put `Math.min(340, bubble.width)` back on fileCard.
    void aNarrowBubbleContainsItsFileCard()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("isFile"), true);
        f.insert(QStringLiteral("mediaSize"), 4500000);
        f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("application/pdf"));
        f.insert(QStringLiteral("mediaKey"), QStringLiteral("k5"));
        f.insert(QStringLiteral("mediaFilename"),
                 QStringLiteral("a-really-long-attachment-filename.pdf"));
        f.insert(QStringLiteral("body"),
                 QStringLiteral("a-really-long-attachment-filename.pdf"));

        Delegate d;
        QVERIFY(build(app, f, d, 360, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *card = d.root->findChild<QQuickItem *>(
            QStringLiteral("fileCard"));
        QVERIFY(bubble);
        QVERIFY2(card && card->width() > 0, "no file card in the row");

        const qreal cardRight = rightEdgeIn(card, d.root);
        QVERIFY2(cardRight <= innerRight(d.root, bubble) + 0.6,
                 qPrintable(QStringLiteral(
                     "the file card ends at %1, past the bubble's inner edge "
                     "%2 (bubble ends at %3) — the card is painted outside "
                     "the bubble that is supposed to hold it")
                     .arg(cardRight).arg(innerRight(d.root, bubble))
                     .arg(rightEdgeIn(bubble, d.root))));
        QVERIFY2(cardRight <= d.root->width() + 0.6,
                 qPrintable(QStringLiteral(
                     "the file card ends at %1, past the row's own width %2")
                     .arg(cardRight).arg(d.root->width())));
    }

    // BUBBLES: THE HEADER CAP HAS TO MIRROR THE **WHOLE** BUBBLE CAP.
    //
    // `aBubbleContainsItsOwnSenderHeader` above fixed the loop; the cap it
    // installed mirrors the bubble's width EXCEPT for `bubbleReceiptInset`,
    // which was added later. So with a facepile on a narrow row the header
    // may be the pile's width wider than the bubble can ever become.
    // Measured 2026-09-19 at a 360px row with four receipts: bubble
    // 44..256, senderIdentityHeader 54..310, and its timestamp at 293..319
    // — outside the bubble and underneath the avatars.
    //
    // FAIL-ON-OLD: drop `- root.bubbleReceiptInset` from contentInnerCap.
    void aNarrowBubbleWithReceiptsContainsItsSenderHeader()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);

        Delegate d;
        QVERIFY(build(app, f, d, 360, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *header = d.root->findChild<QQuickItem *>(
            QStringLiteral("senderIdentityHeader"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(bubble);
        QVERIFY2(header && header->width() > 0, "no sender header in the row");
        QVERIFY(pile);

        const qreal headerRight = rightEdgeIn(header, d.root);
        QVERIFY2(headerRight <= innerRight(d.root, bubble) + 0.6,
                 qPrintable(QStringLiteral(
                     "the sender header ends at %1, past the bubble's inner "
                     "edge %2 (bubble ends at %3) — the name and timestamp "
                     "render on the timeline background")
                     .arg(headerRight).arg(innerRight(d.root, bubble))
                     .arg(rightEdgeIn(bubble, d.root))));
        QVERIFY2(headerRight <= leftEdgeIn(pile, d.root) + 0.6,
                 qPrintable(QStringLiteral(
                     "the sender header ends at %1 and the receipt facepile "
                     "starts at %2 — the timestamp is under the avatars")
                     .arg(headerRight).arg(leftEdgeIn(pile, d.root))));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MessageRailCollisionTest)
#include "MessageRailCollisionTest.moc"
