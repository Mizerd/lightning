// Behavioural suite for qml/CodeBlock.qml. Qt's rich-text engine does not
// wrap <pre>, and MessageDelegate's root is clip:false (for the hover bar),
// so a long line could escape the bubble; only real instantiated geometry
// shows that. Pinned:
//   * a line wider than any pane never widens the component (the overflow is
//     contentX inside a clipping Flickable);
//   * the gutter has one number per source line and is not selectable;
//   * Copy yields exactly the program, with no line numbers or padding;
//   * the height is bounded and the content scrolls past the bound;
//   * a vertical wheel notch is left to the timeline's WheelHandler (wheel
//     events go innermost-first, so accepting it would trap the reader).

#include <QtTest/QtTest>

#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QWheelEvent>

#include <memory>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kTimeoutMs = 3000;

QString repeatedLines(int count, const QString &prefix)
{
    QStringList lines;
    lines.reserve(count);
    for (int i = 1; i <= count; ++i)
        lines << prefix + QString::number(i);
    return lines.join(QLatin1Char('\n'));
}
} // namespace

class CodeBlockQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Harness {
        // Declared first so it is destroyed last: the warning lambda captures
        // it and the engine can still emit during teardown.
        QStringList warnings;
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QObject> rootOwner;
        QQuickItem *outer = nullptr;
        QQuickItem *column = nullptr;
        QQuickItem *block = nullptr;

        QQuickItem *find(const QString &name) const
        {
            return block ? block->findChild<QQuickItem *>(name) : nullptr;
        }
    };

    // The code is injected through setProperty, so fixtures need no escaping
    // and stay exactly what the assertions compare against.
    bool build(Harness &h, const QString &code,
               const QString &language = QString())
    {
        h.engine = std::make_unique<QQmlEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        QQmlComponent component(h.engine.get());
        component.setData(R"(
import QtQuick
import QtQuick.Layouts
import MatrixClient
Item {
    id: outer
    objectName: "outer"
    width: 420
    height: 600
    // Stands in for TimelinePane's timelineWheelHandler: the single
    // pointer-wheel owner an ancestor away from the message row.
    property int outerWheelNotches: 0
    WheelHandler {
        objectName: "outerWheel"
        target: null
        onWheel: (event) => {
            outer.outerWheelNotches += 1
            event.accepted = true
        }
    }
    ColumnLayout {
        id: col
        objectName: "col"
        width: 420
        CodeBlock {
            objectName: "block"
            Layout.fillWidth: true
        }
    }
}
)", QUrl(QStringLiteral("qrc:/codeblocktest.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        h.rootOwner.reset(component.create());
        h.outer = qobject_cast<QQuickItem *>(h.rootOwner.get());
        if (!h.outer)
            return false;
        h.column = h.outer->findChild<QQuickItem *>(QStringLiteral("col"));
        h.block = h.outer->findChild<QQuickItem *>(QStringLiteral("block"));
        if (!h.column || !h.block)
            return false;

        h.block->setProperty("language", language);
        h.block->setProperty("code", code);

        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(520, 700);
        h.outer->setParentItem(h.window->contentItem());
        h.window->show();
        QCoreApplication::processEvents();
        // Layouts and text metrics settle on a polish pass, not on creation.
        h.block->polish();
        QCoreApplication::processEvents();
        return true;
    }

    static void sendWheel(QQuickWindow &window, const QPointF &scenePos,
                          const QPoint &angleDelta,
                          Qt::KeyboardModifiers modifiers)
    {
        QWheelEvent wheel(scenePos, window.mapToGlobal(scenePos.toPoint()),
                          QPoint(0, 0), angleDelta, Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
    }

    // The segment host: MessageDelegate replaces the body TextEdit with a
    // Repeater of segment rows, which is where a code block's geometry lands.
    // These cases load the real MessageDelegate with the TimelineModel role
    // schema.
    struct RowHarness {
        // Declared first so it is destroyed last (see the CodeBlock harness).
        QStringList warnings;
        std::unique_ptr<AppController> controller;
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        // Owned explicitly and destroyed first: component.create() returns an
        // unparented object, and the QML tree must go before the engine and the
        // AppController its bindings reach.
        std::unique_ptr<QObject> hostOwner;
        QQuickItem *host = nullptr;
        QQuickItem *row = nullptr;
        QQuickItem *segments = nullptr;

        QStringList bindingLoops() const
        {
            QStringList out;
            for (const QString &w : warnings) {
                if (w.contains(QLatin1String("Binding loop")))
                    out << w;
            }
            return out;
        }
    };

    // TimelineModel::MessageSegmentsRole's exact payload shape: kind 0 is
    // rich text, kind 1 a fenced block (see TimelineModel.cpp).
    static QVariantMap segment(int kind, const QString &text,
                               const QString &language)
    {
        QVariantMap m;
        m.insert(QStringLiteral("kind"), kind);
        m.insert(QStringLiteral("text"), text);
        m.insert(QStringLiteral("language"), language);
        return m;
    }

    static QVariantMap richSegment(const QString &text)
    {
        return segment(0, text, QString());
    }

    static QVariantMap codeSegment(const QString &code,
                                   const QString &language)
    {
        return segment(1, code, language);
    }

    // Long enough to exceed the cap at every width these cases use.
    static QString longProse()
    {
        return QStringLiteral(
            "Here is the thing I meant, and it runs on for a good while so "
            "that it certainly has to wrap inside the bubble at any sane cap "
            "width whatsoever, twice over if need be.");
    }

    // The Repeater's delegates in layout order; findChildren() promises no
    // order, the layout's child list does.
    static QList<QQuickItem *> segmentRows(const RowHarness &h)
    {
        QList<QQuickItem *> rows;
        auto *inner = h.segments
            ? qobject_cast<QQuickItem *>(
                  h.segments->property("item").value<QObject *>())
            : nullptr;
        if (!inner)
            return rows;
        for (QQuickItem *child : inner->childItems()) {
            if (child->objectName() == QLatin1String("messageSegmentRow"))
                rows << child;
        }
        return rows;
    }

    bool buildRow(RowHarness &h, const QVariantList &segments,
                  const QString &body, bool bubbles)
    {
        h.controller =
            std::make_unique<AppController>(AppController::MockBackend);
        h.controller->settings()->setMessageLayout(bubbles ? 1 : 0);

        QVariantMap fixture;
        const auto roles = h.controller->timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QStringLiteral("$fix:mock"));
        fixture.insert(QStringLiteral("eventId"), QStringLiteral("$fix:mock"));
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), body);
        fixture.insert(QStringLiteral("messageSegments"), segments);
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), true);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        h.engine = std::make_unique<QQmlApplicationEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        h.engine->rootContext()->setContextProperty("app", h.controller.get());
        h.engine->rootContext()->setContextProperty("model", fixture);

        QQmlComponent component(h.engine.get());
        component.setData(R"(
import QtQuick
import MatrixClient
Item {
    id: host
    objectName: "segmentHost"
    width: 640
    height: 900
    property bool direct: false
    MessageDelegate {
        objectName: "segmentMessageRow"
        width: host.width
        isDirectRoom: host.direct
    }
}
)", QUrl(QStringLiteral("qrc:/segmenthosttest.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        h.hostOwner.reset(component.create());
        h.host = qobject_cast<QQuickItem *>(h.hostOwner.get());
        if (!h.host)
            return false;
        h.host->setProperty("direct", bubbles);

        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(700, 900);
        h.host->setParentItem(h.window->contentItem());
        h.window->show();
        QCoreApplication::processEvents();

        h.row = h.host->findChild<QQuickItem *>(
            QStringLiteral("segmentMessageRow"));
        if (!h.row)
            return false;
        h.row->polish();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        h.segments = h.row->findChild<QQuickItem *>(
            QStringLiteral("messageSegments"));
        return true;
    }

private:
    QTemporaryDir m_configHome;

private Q_SLOTS:
    // The segment-host cases build a real AppController with QSettings, so the
    // suite uses a temporary config home.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("code-block-qml-test"));
        QSettings().clear();
    }

    // A 5000-character line must not become 5000 characters of geometry: the
    // component stays inside its width, its implicit width stays bounded, and
    // the overflow is a clipping Flickable's contentWidth.
    void oneVeryLongLineNeverWidensTheComponent()
    {
        Harness h;
        QVERIFY(build(h, QString(5000, QLatin1Char('x'))));

        QTRY_VERIFY_WITH_TIMEOUT(h.block->width() > 0.0, kTimeoutMs);
        QVERIFY2(h.block->width() <= 420.5,
                 qPrintable(QStringLiteral("block width %1 exceeded the 420 "
                                           "it was given")
                                .arg(h.block->width())));
        // A plain non-wrapping TextEdit reports the whole line as its implicit
        // width and drags the layout with it; this clamp prevents that.
        const qreal implicit = h.block->implicitWidth();
        QVERIFY2(implicit <= 761.0,
                 qPrintable(QStringLiteral("implicitWidth %1 is unbounded")
                                .arg(implicit)));
        QVERIFY2(h.column->implicitWidth() <= 761.0,
                 qPrintable(QStringLiteral("the enclosing layout grew to %1")
                                .arg(h.column->implicitWidth())));

        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(scroll != nullptr);
        QVERIFY(scroll->clip());
        QVERIFY(scroll->width() > 0.0);
        QVERIFY2(scroll->property("contentWidth").toReal() > scroll->width(),
                 "the long line did not become horizontal scroll range");
        QVERIFY(h.block->property("horizontalOverflow").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

    // Short code does not scroll horizontally, so ordinary snippets show no
    // scrollbar.
    void shortCodeDoesNotOverflowHorizontally()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("ls -la")));
        QTRY_VERIFY_WITH_TIMEOUT(h.block->width() > 0.0, kTimeoutMs);
        QVERIFY(!h.block->property("horizontalOverflow").toBool());
        QVERIFY(!h.block->property("verticalOverflow").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

    // One gutter number per source line, in order. wrapMode is NoWrap, so
    // source and visual lines cannot drift.
    void gutterHasExactlyOneNumberPerLine()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(120, QStringLiteral("line "))));
        QCOMPARE(h.block->property("lineCount").toInt(), 120);

        auto *gutter = h.find(QStringLiteral("codeBlockGutter"));
        QVERIFY(gutter != nullptr);
        const QStringList numbers =
            gutter->property("text").toString().split(QLatin1Char('\n'));
        QCOMPARE(numbers.size(), 120);
        QCOMPARE(numbers.first(), QStringLiteral("1"));
        QCOMPARE(numbers.last(), QStringLiteral("120"));

        // A Text, not a TextEdit: a selectable gutter would put line numbers
        // into a select-all paste.
        QVERIFY2(!gutter->property("selectByMouse").isValid(),
                 "the gutter is selectable text");
        QCOMPARE(h.warnings, QStringList{});
    }

    // Copy yields the program and nothing else. The fixture starts a line with
    // a digit and uses a tab, so gutter contamination or whitespace
    // normalisation both show.
    void copyYieldsExactlyTheCodeTextWithNoGutter()
    {
        const QString code =
            QStringLiteral("#!/bin/sh\n\techo one\n2nd line\n\t\tdeep");
        Harness h;
        QVERIFY(build(h, code));

        QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
        QVERIFY(QMetaObject::invokeMethod(h.block, "copyCode"));

        const QString pasted = QGuiApplication::clipboard()->text();
        // QTextCursor::selectedText() reports block breaks as U+2029, so the
        // hidden-TextEdit relay relies on QQuickTextControl::copy() normalising
        // it. The "copy message text" path uses the same relay.
        QVERIFY2(!pasted.contains(QChar(0x2029)),
                 "the clipboard carries U+2029 paragraph separators instead of "
                 "newlines");
        QCOMPARE(pasted, code);
        QVERIFY2(!pasted.startsWith(QLatin1Char('1')),
                 "a leading gutter number reached the clipboard");
        QVERIFY(!pasted.endsWith(QLatin1Char(' ')));
        QVERIFY(!pasted.endsWith(QLatin1Char('\t')));
        QVERIFY(h.block->property("copied").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

    // The vertical scrollbar never paints over the widest line. An attached
    // ScrollBar takes no layout width, so contentWidth must reserve a band
    // (not a rightMargin: the overflow predicate, Right key and wheel router
    // all clamp on contentWidth).
    void theVerticalBarNeverPaintsOverTheWidestLine()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        QTRY_VERIFY_WITH_TIMEOUT(h.block->height() > 0.0, kTimeoutMs);

        QVERIFY2(h.block->property("verticalOverflow").toBool(),
                 "the fixture is not tall enough to raise a vertical bar, so "
                 "this case would prove nothing");

        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(scroll != nullptr);
        QVERIFY(text != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(text->implicitWidth() > 0.0, kTimeoutMs);

        const qreal contentWidth = scroll->property("contentWidth").toReal();
        const qreal band = contentWidth - text->implicitWidth();
        QVERIFY2(band >= 6.0,
                 qPrintable(QStringLiteral(
                     "the scrollable extent is %1 and the widest line is %2, "
                     "leaving %3px for a 6px scrollbar — the bar is painting "
                     "over the code")
                         .arg(contentWidth).arg(text->implicitWidth())
                         .arg(band)));

        // The band is exactly the reserve, so an unrelated widening of
        // contentWidth cannot satisfy this.
        QCOMPARE(band, h.block->property("verticalBarSpace").toReal());

        // A block with nothing to scroll vertically reserves nothing.
        Harness shortBlock;
        QVERIFY(build(shortBlock, QStringLiteral("one line")));
        QTRY_VERIFY_WITH_TIMEOUT(shortBlock.block->height() > 0.0, kTimeoutMs);
        QVERIFY(!shortBlock.block->property("verticalOverflow").toBool());
        QCOMPARE(shortBlock.block->property("verticalBarSpace").toReal(), 0.0);
    }

    // The height is bounded and the rest is reachable by scrolling.
    void boundedHeightCapsAndTheContentStillScrollsPastIt()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        QTRY_VERIFY_WITH_TIMEOUT(h.block->height() > 0.0, kTimeoutMs);

        auto *body = h.find(QStringLiteral("codeBlockBody"));
        QVERIFY(body != nullptr);
        const qreal maxBody = h.block->property("maxBodyHeight").toReal();
        const qreal barSpace =
            h.block->property("horizontalBarSpace").toReal();
        QVERIFY(maxBody > 0.0);
        QVERIFY2(body->height() <= maxBody + barSpace + 1.0,
                 qPrintable(QStringLiteral("body height %1 exceeded the %2 "
                                           "bound")
                                .arg(body->height()).arg(maxBody)));

        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(scroll != nullptr);
        QVERIFY2(scroll->property("contentHeight").toReal() > scroll->height(),
                 "400 lines did not produce vertical scroll range");
        QVERIFY(h.block->property("verticalOverflow").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

    // A plain vertical notch over a code block reaches the outer wheel owner
    // (timelineWheelHandler in production) and does not move the block.
    void verticalWheelIsLeftToTheTimeline()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);

        const QPointF centre = body->mapToScene(
            QPointF(body->width() / 2.0, body->height() / 2.0));
        QCOMPARE(h.outer->property("outerWheelNotches").toInt(), 0);
        sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);

        QTRY_COMPARE_WITH_TIMEOUT(
            h.outer->property("outerWheelNotches").toInt(), 1, kTimeoutMs);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);
        QCOMPARE(h.warnings, QStringList{});
    }

    // Shift+wheel scrolls the code sideways and stops there; it is the only
    // wheel gesture the block takes.
    void shiftWheelScrollsTheCodeSidewaysAndStopsThere()
    {
        Harness h;
        QVERIFY(build(h, QString(5000, QLatin1Char('x'))));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            scroll->property("contentWidth").toReal() > scroll->width(),
            kTimeoutMs);

        const QPointF centre = body->mapToScene(
            QPointF(body->width() / 2.0, body->height() / 2.0));
        // Negative delta moves toward the end of the line (wheel-down scrolls
        // forward).
        sendWheel(*h.window, centre, QPoint(0, -120), Qt::ShiftModifier);

        QTRY_VERIFY_WITH_TIMEOUT(
            scroll->property("contentX").toReal() > 0.0, kTimeoutMs);
        QCOMPARE(h.outer->property("outerWheelNotches").toInt(), 0);
        QCOMPARE(h.warnings, QStringList{});
    }

    // The language token is re-validated: it is sender-chosen text shown to
    // the user and used in an accessible name.
    void onlyAValidatedLanguageTokenIsShown()
    {
        Harness valid;
        QVERIFY(build(valid, QStringLiteral("fn main() {}"),
                      QStringLiteral("rust")));
        auto *label = valid.find(QStringLiteral("codeBlockLanguageLabel"));
        QVERIFY(label != nullptr);
        QVERIFY(label->isVisible());
        QCOMPARE(label->property("text").toString(), QStringLiteral("rust"));
        QCOMPARE(valid.block->property("safeLanguage").toString(),
                 QStringLiteral("rust"));

        for (const QString &hostile :
             {QStringLiteral("onclick=x"),
              QStringLiteral("ru\"st"),
              QStringLiteral("<b>rust</b>"),
              QString(40, QLatin1Char('a'))}) {
            Harness bad;
            QVERIFY(build(bad, QStringLiteral("x"), hostile));
            QVERIFY2(bad.block->property("safeLanguage").toString().isEmpty(),
                     qPrintable(QStringLiteral("accepted %1").arg(hostile)));
            // The label must not exist, not merely be invisible: a Label
            // created holding "" keeps ItemObservesViewport for its lifetime
            // (QQuickText::setText early-returns before clearing it), which
            // defeats subtree pruning in every timeline row. Hence a Loader.
            QVERIFY2(bad.find(QStringLiteral("codeBlockLanguageLabel"))
                         == nullptr,
                     qPrintable(QStringLiteral(
                         "a rejected language (%1) still created the label")
                                    .arg(hostile)));
        }
    }

    // The code renders as plain text: the segment text is already
    // entity-decoded, so RichText would reinterpret the program's angle
    // brackets as markup.
    void codeIsRenderedAsPlainTextAndNeverWrapped()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("<b>not bold</b> && <script>x</script>")));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(text != nullptr);
        // Text.PlainText == 0 and Text.NoWrap == 0 in the QML enums.
        QCOMPARE(text->property("textFormat").toInt(), 0);
        QCOMPARE(text->property("wrapMode").toInt(), 0);
        QVERIFY(text->property("readOnly").toBool());
        QVERIFY(text->property("selectByMouse").toBool());
        QCOMPARE(text->property("text").toString(),
                 QStringLiteral("<b>not bold</b> && <script>x</script>"));
        QCOMPARE(h.warnings, QStringList{});
    }

    // The block is keyboard-reachable (horizontal scrolling, Ctrl+C), so it
    // must show focus; the ring is the frame border in the shared focus ink.
    void focusIsReachableByTabAndVisiblyMarked()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("ls -la")));
        QVERIFY2(h.block->property("activeFocusOnTab").toBool(),
                 "the block cannot be reached with the keyboard");

        auto *theme = h.engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY(theme != nullptr);
        auto *pen = qvariant_cast<QObject *>(h.block->property("border"));
        QVERIFY(pen != nullptr);

        QVERIFY(!h.block->property("focusWithin").toBool());
        QCOMPARE(pen->property("color").value<QColor>(),
                 theme->property("border").value<QColor>());

        h.block->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(h.block->hasActiveFocus(), kTimeoutMs);
        QVERIFY(h.block->property("focusWithin").toBool());
        QCOMPARE(pen->property("color").value<QColor>(),
                 theme->property("focusRing").value<QColor>());
        QCOMPARE(h.warnings, QStringList{});
    }

    // A real MessageDelegate over the TimelineModel role schema must produce
    // zero QML warnings, including binding loops (which are only warnings).
    // Reading QQuickTextEdit::implicitWidth from a binding runs updateSize()
    // and emits implicitWidthChanged mid-binding, and a wrapping text sized
    // from min(cap, its own implicit width) measures itself; code segments do
    // neither.
    void segmentedMessageRowsReportNoBindingLoop()
    {
        const QString prose = longProse();
        const QVariantList mixed{
            richSegment(prose),
            codeSegment(QStringLiteral("ls -la\ncd /tmp"),
                        QStringLiteral("bash")),
            richSegment(QStringLiteral("Short tail."))};
        const QVariantList codeOnly{
            codeSegment(QStringLiteral("ls -la"), QStringLiteral("bash"))};
        const QVariantList shortAndCode{
            richSegment(QStringLiteral("Short tail.")),
            codeSegment(QStringLiteral("ls -la"), QStringLiteral("bash"))};
        const QVariantList longAndCode{
            richSegment(prose),
            codeSegment(QString(400, QLatin1Char('x')), QString())};

        struct Case { const char *name; QVariantList segments; QString body; };
        const QList<Case> cases{
            // (a) and (b) have no fenced block and render through the single
            // body TextEdit, the common case, which must not loop either.
            {"shortPlainMessage", QVariantList{}, QStringLiteral("ok")},
            {"longPlainMessage", QVariantList{}, prose},
            {"codeOnlyMessage", codeOnly, QStringLiteral("```\nls -la\n```")},
            {"shortProseAndCode", shortAndCode, QStringLiteral("body")},
            {"longProseAndWideCode", longAndCode, QStringLiteral("body")},
            {"proseCodeProse", mixed, QStringLiteral("body")},
        };

        for (bool bubbles : {false, true}) {
            for (const Case &c : cases) {
                RowHarness h;
                QVERIFY2(buildRow(h, c.segments, c.body, bubbles), c.name);
                const QStringList loops = h.bindingLoops();
                QVERIFY2(loops.isEmpty(),
                         qPrintable(QStringLiteral("%1 (bubbles=%2) reported "
                                                   "%3 binding loop(s):\n%4")
                                        .arg(QLatin1String(c.name))
                                        .arg(bubbles)
                                        .arg(loops.size())
                                        .arg(loops.join(QLatin1Char('\n')))));
            }
        }
    }

    // A segment reports its natural width upward (it sizes a DM bubble): a
    // short tail stays under the cap, a paragraph exceeds it and is laid out
    // at the cap so it wraps.
    void aShortSegmentStaysNarrowAndAParagraphReachesTheCap()
    {
        RowHarness h;
        QVERIFY(buildRow(h, QVariantList{
                                richSegment(longProse()),
                                codeSegment(QStringLiteral("ls -la"),
                                            QStringLiteral("bash")),
                                richSegment(QStringLiteral("Hi."))},
                         QStringLiteral("body"), false));
        const QList<QQuickItem *> rows = segmentRows(h);
        QCOMPARE(rows.size(), 3);
        const qreal cap = h.segments->property("segmentCap").toReal();
        QVERIFY(cap > 100.0);

        QVERIFY2(rows.at(0)->implicitWidth() > cap,
                 qPrintable(QStringLiteral("a wrapping paragraph reported %1, "
                                           "under the %2 cap -- it can no "
                                           "longer be said to reach it")
                                .arg(rows.at(0)->implicitWidth()).arg(cap)));
        QVERIFY2(rows.at(2)->implicitWidth() < cap / 2.0,
                 qPrintable(QStringLiteral("a two-word segment reported %1 "
                                           "against a %2 cap -- a short "
                                           "message would stretch its bubble")
                                .arg(rows.at(2)->implicitWidth()).arg(cap)));

        // contentWidth is the widest laid-out line, so a wrapped paragraph
        // cannot exceed its given width; an unwrapped one would report its
        // implicit width.
        auto *paragraph = rows.at(0)->findChild<QQuickItem *>(
            QStringLiteral("messageSegmentText"));
        QVERIFY(paragraph != nullptr);
        QCOMPARE(paragraph->width(), cap);
        QVERIFY(paragraph->property("contentWidth").toReal() <= cap + 0.5);
        QVERIFY2(paragraph->property("lineCount").toInt() > 1,
                 "the paragraph did not wrap at the cap");
    }

    // In Bubbles the bubble width is its content's implicit width, so a
    // code-only message must report one (or collapse to the 60px floor) while
    // the block keeps its own width rather than filling the row.
    void aCodeOnlyMessageKeepsItsOwnWidthInsideABubble()
    {
        RowHarness h;
        QVERIFY(buildRow(h, QVariantList{codeSegment(
                                QStringLiteral("ls -la"),
                                QStringLiteral("bash"))},
                         QStringLiteral("```\nls -la\n```"), true));
        const QList<QQuickItem *> rows = segmentRows(h);
        QCOMPARE(rows.size(), 1);
        const qreal cap = h.segments->property("segmentCap").toReal();

        QVERIFY2(rows.at(0)->implicitWidth() > 60.0,
                 qPrintable(QStringLiteral("the code row reported %1 -- a DM "
                                           "bubble would collapse to its floor")
                                .arg(rows.at(0)->implicitWidth())));
        QVERIFY2(rows.at(0)->implicitWidth() < cap / 2.0,
                 qPrintable(QStringLiteral("a six-character snippet reported "
                                           "%1 against a %2 cap -- the block "
                                           "is stretching edge to edge")
                                .arg(rows.at(0)->implicitWidth()).arg(cap)));

        auto *bubbleItem = h.row->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubbleItem != nullptr);
        QVERIFY2(bubbleItem->width() > 60.0,
                 qPrintable(QStringLiteral("the bubble collapsed to %1")
                                .arg(bubbleItem->width())));
        QVERIFY(bubbleItem->width() < cap);
    }

    // Inside the delegate, a line wider than any pane clamps at the cap and
    // overflows into horizontal scroll range.
    void aWideCodeBlockClampsAtTheCapAndScrollsInstead()
    {
        RowHarness h;
        QVERIFY(buildRow(h, QVariantList{codeSegment(
                                QString(400, QLatin1Char('x')), QString())},
                         QStringLiteral("body"), false));
        const QList<QQuickItem *> rows = segmentRows(h);
        QCOMPARE(rows.size(), 1);
        const qreal cap = h.segments->property("segmentCap").toReal();

        auto *scroll = rows.at(0)->findChild<QQuickItem *>(
            QStringLiteral("codeBlockScroll"));
        QVERIFY(scroll != nullptr);
        QVERIFY2(scroll->width() <= cap + 0.5,
                 qPrintable(QStringLiteral("the block was laid out %1 wide "
                                           "against a %2 cap")
                                .arg(scroll->width()).arg(cap)));
        QVERIFY2(scroll->property("contentWidth").toReal() > scroll->width(),
                 "the long line did not become horizontal scroll range");
    }
};

QTEST_MAIN(CodeBlockQmlTest)
#include "CodeBlockQmlTest.moc"
