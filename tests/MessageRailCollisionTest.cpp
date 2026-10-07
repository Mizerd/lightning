// The row's right rail is shared: the hover action bar and the read-receipt
// facepile anchor to the same edge from opposite ends of a row, and on a
// short row the facepile (same z, later in the document) covered the bar's
// buttons.
//
// Also covered, all in Bubbles: the sender header, body text, media cards and
// reactions must stay inside (or under) their own bubble, and an own bubble
// must leave the receipt rail clear.
//
// And the body's right-to-left paragraphs (2026-09-30: an all-Arabic
// paragraph read left-aligned): each paragraph takes its own direction and
// starts at the edge it reads from.
//
// And the inks drawn ON an own bubble (#25: an own image's caption used the
// page's secondary grey on the accent fill): every one meets 4.5:1 against
// the bubble on all eleven presets.
//
// These are geometric assertions on the real delegate, not a source scan: a
// scan cannot see an overlap.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>

#include "app/AppController.h"
#include "app/SettingsManager.h"

#include <algorithm>
#include <cmath>

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
        f.insert(QStringLiteral("sentReceipt"), false);
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
        // Bubbles is a direct-room layout and the flag normally comes from the
        // host view. Set it before the first layout pass, or the content caps
        // lag a frame behind.
        if (direct)
            QQmlProperty::write(out.root, QStringLiteral("isDirectRoom"), true);
        out.window->show();
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        return true;
    }

    /// The action bar is created on hover, which needs a live timelineView.
    /// Forcing the Loader is enough for a geometry question: its x and width
    /// come from anchors and the reserve, not `visible`.
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

    /// "مرحبا بالعالم": short, so it never wraps and a body as wide as its
    /// own text stays narrower than the column.
    static QString arabic()
    {
        return QStringLiteral("\u0645\u0631\u062D\u0628\u0627 "
                              "\u0628\u0627\u0644\u0639\u0627\u0644\u0645");
    }

    static QTextDocument *documentOf(QQuickItem *textEdit)
    {
        auto *quick =
            textEdit->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }

    /// Where a paragraph's first character is drawn: the right end of its
    /// line when it reads right to left.
    static qreal paragraphStartIn(QQuickItem *textEdit, const QTextBlock &block,
                                  QQuickItem *reference)
    {
        QRectF r;
        QMetaObject::invokeMethod(textEdit, "positionToRectangle",
                                  Q_RETURN_ARG(QRectF, r),
                                  Q_ARG(int, block.position()));
        return textEdit->mapToItem(reference, r.topLeft()).x();
    }

    /// WCAG 2.1 relative luminance and contrast ratio.
    static double relativeLuminance(const QColor &c)
    {
        auto lin = [](double v) {
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF())
            + 0.0722 * lin(c.blueF());
    }
    static double contrastRatio(const QColor &a, const QColor &b)
    {
        const double la = relativeLuminance(a);
        const double lb = relativeLuminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }
    /// What `ink` drawn at `alpha` over the opaque `ground` renders as.
    static QColor over(const QColor &ink, double alpha, const QColor &ground)
    {
        return QColor::fromRgbF(
            float(alpha * ink.redF() + (1.0 - alpha) * ground.redF()),
            float(alpha * ink.greenF() + (1.0 - alpha) * ground.greenF()),
            float(alpha * ink.blueF() + (1.0 - alpha) * ground.blueF()));
    }
    /// The opacity an item is painted at, up to (not including) `ground`.
    static double opacityUpTo(QQuickItem *item, QQuickItem *ground)
    {
        double o = 1.0;
        for (QQuickItem *it = item; it && it != ground; it = it->parentItem())
            o *= it->opacity();
        return o;
    }
    /// First item in the VISUAL tree with this objectName: Loader content
    /// and Repeater delegates are not always QObject children.
    static QQuickItem *findVisual(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        for (QQuickItem *child : parent->childItems()) {
            if (child->objectName() == name)
                return child;
            if (QQuickItem *hit = findVisual(child, name))
                return hit;
        }
        return nullptr;
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

    // A short row with receipts: the bar must end before the facepile begins.
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

    // Element's "sent" check rides the same rail as the facepile, so it must
    // be arbitrated the same way: on a short row the action bar ends before
    // the check begins (the facepile lesson, applied to its sibling).
    void theSentCheckClearsTheActionBarOnAShortRow()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("sentReceipt"), true);

        Delegate d;
        QVERIFY(build(app, f, d));
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        QVERIFY2(check, "no sentReceiptCheck in the row");
        QVERIFY2(check->isVisible() && check->width() > 0,
                 "the sent check is not drawn on the newest own message");
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY2(bar, "no messageActionBarLoader in the row");
        QVERIFY2(bar->width() > 0, "the action bar measured empty");

        const qreal barRight = rightEdgeIn(bar, d.root);
        const qreal checkLeft = leftEdgeIn(check, d.root);
        QVERIFY2(barRight <= checkLeft + 1.0,
                 qPrintable(QStringLiteral(
                     "the action bar (ends at %1) runs into the sent check "
                     "(starts at %2)").arg(barRight).arg(checkLeft)));
        // Inside the row, not hanging off its edge.
        QVERIFY(rightEdgeIn(check, d.root) <= d.root->width() + 1.0);
    }

    // A row that does not carry the flag, or whose readers are shown, draws no
    // check; readers replace it rather than sitting beside it.
    void theSentCheckGivesWayToReaders()
    {
        AppController app(AppController::MockBackend);
        {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("isOwn"), true);
            Delegate d;
            QVERIFY(build(app, f, d));
            auto *check = d.root->findChild<QQuickItem *>(
                QStringLiteral("sentReceiptCheck"));
            QVERIFY(!check || !check->isVisible());
        }
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("sentReceipt"), true);
        f.insert(QStringLiteral("readReceipts"), receipts(2));
        f.insert(QStringLiteral("readReceiptsTotal"), 2);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        QVERIFY2(!check || !check->isVisible(),
                 "the sent check is drawn beside the readers it gives way to");
    }

    // Bubbles: an own bubble leaves the check's rail clear, as for the pile.
    void anOwnBubbleClearsTheSentCheck()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("sentReceipt"), true);

        Delegate d;
        QVERIFY(build(app, f, d));
        QQmlProperty::write(d.root, QStringLiteral("isDirectRoom"), true);
        QCoreApplication::processEvents();
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        QVERIFY(bubble);
        QVERIFY2(check && check->isVisible() && check->width() > 0,
                 "no sent check on the newest own bubble");
        const qreal bubbleRight = rightEdgeIn(bubble, d.root);
        const qreal checkLeft = leftEdgeIn(check, d.root);
        QVERIFY2(bubbleRight <= checkLeft + 1.0,
                 qPrintable(QStringLiteral(
                     "the own bubble (ends at %1) runs under the sent check "
                     "(starts at %2)").arg(bubbleRight).arg(checkLeft)));
    }

    // It is a reservation, not a permanent indent: on a tall row the bar keeps
    // the row's corner.
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

    // Bubbles: the identity header belongs inside the bubble. Its width cap
    // must not be derived from the bubble, which is sized from the header's
    // own column (Qt resolves that loop by pinning the header to one pixel).
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

    // Bubbles: an own bubble's placement must honour the receipt-rail inset
    // its width cap reserves, or the avatars clip its corner.
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

    // ── Bubbles geometry ─────────────────────────────────────────────────

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

    // Bubbles: reactions on an own message hang under that (right-aligned)
    // bubble, not at the row's left edge.
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

    // Bubbles: the body stays inside the bubble's padding (`bubblePad` is 10
    // in Bubbles, 0 elsewhere), so its cap must be the inner width.
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

    // Bubbles: a media card stays inside the bubble on a narrow row, so its
    // cap must be the inner width too.
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

    // Bubbles: the header cap mirrors the whole bubble cap, including
    // `bubbleReceiptInset`, so on a narrow row with a facepile the header
    // cannot be wider than the bubble.
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

    // An all-Arabic paragraph starts at the column's right edge. It read
    // left-aligned: right-aligned inside a body exactly as wide as its text.
    // A leading emoji changes nothing (TextEdit reads its surrogate as left to
    // right). A left-to-right line with an Arabic run keeps its own width.
    void aRightToLeftParagraphStartsAtTheColumnsRightEdge()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);   // Modern

        const QStringList bodies = {
            arabic(),
            QString::fromUtf8("\xf0\x9f\x98\x80 ") + arabic(),
        };
        for (const QString &text : bodies) {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("body"), text);
            Delegate d;
            QVERIFY(build(app, f, d));
            auto *column = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageContentColumn"));
            auto *body = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(column);
            QVERIFY2(body && body->isVisible(), "no message body in the row");
            QTextDocument *doc = documentOf(body);
            QVERIFY(doc);
            QCOMPARE(doc->firstBlock().textDirection(), Qt::RightToLeft);
            const qreal start = paragraphStartIn(body, doc->firstBlock(), d.root);
            QVERIFY2(qAbs(start - rightEdgeIn(column, d.root)) <= 1.5,
                     qPrintable(QStringLiteral(
                         "the Arabic paragraph starts at %1 and the column "
                         "ends at %2 — it reads left-aligned")
                         .arg(start).arg(rightEdgeIn(column, d.root))));
        }

        // Control: left to right, and not widened.
        QVariantMap g = baseFixture();
        g.insert(QStringLiteral("body"), QStringLiteral("Mixed: hello 123 ")
                                             + arabic() + QStringLiteral(" world"));
        Delegate e;
        QVERIFY(build(app, g, e));
        auto *mixed = e.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(mixed);
        QTextDocument *mixedDoc = documentOf(mixed);
        QVERIFY(mixedDoc);
        QCOMPARE(mixedDoc->firstBlock().textDirection(), Qt::LeftToRight);
        QVERIFY2(qAbs(paragraphStartIn(mixed, mixedDoc->firstBlock(), e.root)
                      - leftEdgeIn(mixed, e.root)) <= 1.5,
                 "a left-to-right line does not start at the body's left edge");
        QVERIFY2(mixed->width() <= mixed->implicitWidth() + 1.0,
                 qPrintable(QStringLiteral(
                     "a left-to-right body was widened to %1 (its text is %2)")
                     .arg(mixed->width()).arg(mixed->implicitWidth())));
    }

    // Each paragraph reads in its own direction, whichever comes first. The
    // TextEdit took one direction for the whole message from its first
    // paragraph.
    void eachParagraphTakesItsOwnDirection()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);   // Modern

        const QString english = QStringLiteral("Hello world");
        const QList<QStringList> orders = {
            { english, arabic() },
            { arabic(), english },
        };
        for (const QStringList &order : orders) {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("body"), order.join(QStringLiteral("\n\n")));
            f.insert(QStringLiteral("formattedBody"),
                     QStringLiteral("<p>%1</p><p>%2</p>")
                         .arg(order.at(0), order.at(1)));
            Delegate d;
            QVERIFY(build(app, f, d));
            auto *column = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageContentColumn"));
            auto *body = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(column && body);
            QTextDocument *doc = documentOf(body);
            QVERIFY(doc);
            QCOMPARE(doc->blockCount(), 2);
            int checked = 0;
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
                const bool rtl = b.text() == arabic();
                QCOMPARE(b.textDirection(),
                         rtl ? Qt::RightToLeft : Qt::LeftToRight);
                const qreal edge = rtl ? rightEdgeIn(column, d.root)
                                       : leftEdgeIn(column, d.root);
                const qreal start = paragraphStartIn(body, b, d.root);
                QVERIFY2(qAbs(start - edge) <= 1.5,
                         qPrintable(QStringLiteral(
                             "paragraph %1 (%2) starts at %3, not at %4")
                             .arg(b.blockNumber())
                             .arg(rtl ? QStringLiteral("Arabic")
                                      : QStringLiteral("English"))
                             .arg(start).arg(edge)));
                ++checked;
            }
            QCOMPARE(checked, 2);
        }
    }

    // Bubbles: a right-to-left body starts at the bubble's inner right edge
    // (here the sender header, not the text, sets the bubble's width) and
    // stays inside the bubble.
    void aRightToLeftBodyStartsAtItsBubblesInnerRightEdge()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("body"), arabic());
        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(bubble);
        QVERIFY2(body && body->isVisible(), "no message body in the row");
        QTextDocument *doc = documentOf(body);
        QVERIFY(doc);
        QVERIFY2(body->implicitWidth() + 20 < bubble->width(),
                 "the fixture's header no longer outgrows its text");
        const qreal start = paragraphStartIn(body, doc->firstBlock(), d.root);
        QVERIFY2(qAbs(start - innerRight(d.root, bubble)) <= 1.5,
                 qPrintable(QStringLiteral(
                     "the Arabic paragraph starts at %1, the bubble's inner "
                     "edge is at %2")
                     .arg(start).arg(innerRight(d.root, bubble))));
        QVERIFY(rightEdgeIn(body, d.root) <= innerRight(d.root, bubble) + 0.6);
    }

    // Bubbles: the secondary text drawn ON an own bubble is readable on every
    // preset (#25). An own image's caption took the page's textSecondary over
    // the accent fill: 1.04:1 on Lightning Light, 1.16 on Moss Light, 1.27 on
    // Warm, 4.19 on Purple Dusk. The quoted reply line (onAccentMuted under a
    // 0.85 fade: 4.00 on Moss Light, 4.14 on Purple Dusk) and the "(edited)"
    // status line are held to the same floor. Measures the ink the real
    // delegate resolves, at the opacity it is painted with, over the fill the
    // bubble resolves, and asserts the count of distinct fills it saw.
    void textOnAnOwnBubbleIsReadableOnEveryPreset()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);   // Bubbles

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("isImage"), true);
        f.insert(QStringLiteral("mediaWidth"), 320);
        f.insert(QStringLiteral("mediaHeight"), 200);
        f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("image/png"));
        f.insert(QStringLiteral("mediaFilename"), QStringLiteral("lake.png"));
        // Differs from the filename, so it renders once as a caption.
        f.insert(QStringLiteral("body"),
                 QStringLiteral("Sunset over the lake"));
        f.insert(QStringLiteral("replyToEventId"), QStringLiteral("$quoted"));
        f.insert(QStringLiteral("replyToSender"), QStringLiteral("Bob"));
        f.insert(QStringLiteral("replyToPreview"),
                 QStringLiteral("the line being answered"));
        f.insert(QStringLiteral("edited"), true);

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool(),
                 "the fixture is not in Bubbles mode; the rest proves nothing");
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("mediaCaptionBody"))
                     .toBool(),
                 "the fixture's body is not read as a caption");
        // Bubbles is switched on after creation (isDirectRoom), and the quote
        // line's opacity has a 90 ms Behavior: let it settle, or the case
        // measures the animation's start.
        QTest::qWait(300);

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubble);
        struct Probe {
            const char *what;
            QQuickItem *item;
        };
        const QList<Probe> probes = {
            { "the caption",
              findVisual(d.root, QStringLiteral("messageBody")) },
            { "the quoted reply line",
              findVisual(d.root, QStringLiteral("replyQuoteBody")) },
            { "the status line",
              findVisual(d.root, QStringLiteral("messageStatusText")) },
        };
        for (const Probe &p : probes) {
            QVERIFY2(p.item && p.item->isVisible(),
                     qPrintable(QStringLiteral("%1 is not on the bubble")
                                    .arg(QLatin1String(p.what))));
            QVERIFY2(p.item->mapToItem(bubble, QPointF(0, 0)).y() >= 0,
                     qPrintable(QStringLiteral("%1 is not inside the bubble")
                                    .arg(QLatin1String(p.what))));
        }

        QSet<QRgb> fills;
        int checked = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            QQmlExpression set(qmlContext(d.root), d.root,
                               QStringLiteral("AppTheme.mode = %1").arg(mode));
            set.evaluate();
            QVERIFY2(!set.hasError(), qPrintable(set.error().toString()));
            QCoreApplication::processEvents();

            const QColor fill = bubble->property("color").value<QColor>();
            QVERIFY2(fill.isValid() && fill.alpha() == 255,
                     qPrintable(QStringLiteral("theme %1: the own bubble is "
                                               "not an opaque fill (%2)")
                                    .arg(mode).arg(fill.name(QColor::HexArgb))));
            fills.insert(fill.rgb());
            for (const Probe &p : probes) {
                const QColor ink = p.item->property("color").value<QColor>();
                QVERIFY(ink.isValid());
                const double alpha = ink.alphaF() * opacityUpTo(p.item, bubble);
                const double ratio = contrastRatio(over(ink, alpha, fill), fill);
                QVERIFY2(ratio >= 4.5,
                         qPrintable(QStringLiteral(
                             "theme %1: %2 is %3 at %4 opacity on the own "
                             "bubble %5 — %6:1, under the 4.5:1 text floor")
                                        .arg(mode)
                                        .arg(QLatin1String(p.what))
                                        .arg(ink.name(QColor::HexRgb))
                                        .arg(alpha, 0, 'f', 2)
                                        .arg(fill.name(QColor::HexRgb))
                                        .arg(ratio, 0, 'f', 2)));
            }
            ++checked;
        }
        QQmlExpression reset(qmlContext(d.root), d.root,
                             QStringLiteral("AppTheme.mode = 0"));
        reset.evaluate();
        QCOMPARE(checked, 11);
        // Eleven presets share nine own-bubble fills (the three Lightning
        // palettes use one blue). Fewer means the palette never changed.
        QVERIFY2(fills.size() >= 8,
                 qPrintable(QStringLiteral("only %1 distinct own-bubble fills "
                                           "were measured")
                                .arg(fills.size())));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MessageRailCollisionTest)
#include "MessageRailCollisionTest.moc"
