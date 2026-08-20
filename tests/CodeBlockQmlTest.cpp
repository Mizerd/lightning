// v0.7.4: behavioural suite for qml/CodeBlock.qml.
//
// The defect this component exists for is a GEOMETRY defect: Qt's rich-text
// engine does not wrap <pre>, so one long terminal line laid the message
// TextEdit out far past its own width and — because MessageDelegate's root is
// deliberately clip:false so the hover action bar can overhang — escaped the
// bubble and the timeline. A source scan cannot see that, and neither can a
// unit test of the parser: it is only visible in real instantiated geometry.
// So every case here loads the production component offscreen and measures it.
//
// The four contracts pinned:
//   * a line far wider than any pane never widens the component (the overflow
//     becomes contentX inside a clipping Flickable, never geometry);
//   * the gutter has exactly one number per source line, and is not selectable
//     text that could ride along into a copy;
//   * Copy yields the program EXACTLY — no line numbers, no gutter padding;
//   * the height is bounded and the content still scrolls past the bound.
//
// Plus the nested-scroll hazard, which is the one that would be found last and
// hurt most: the room timeline's WheelHandler is the single pointer-wheel
// owner, and wheel events are delivered innermost-first. A code block that
// accepted a vertical notch would trap a reader mid-conversation.

#include <QtTest/QtTest>

#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>

#include <memory>

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
        // Declared FIRST so it is destroyed LAST: the warning lambda below
        // captures it by reference and the engine can still emit during its
        // own teardown.
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

    // The code is injected through setProperty rather than baked into the QML
    // source, so no test has to escape tabs, newlines or quotes into a string
    // literal inside a string literal — the fixture text stays readable and,
    // more importantly, stays EXACTLY what the assertion compares against.
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

private Q_SLOTS:
    // THE defect. A 5000-character line must not become 5000 characters of
    // geometry: the component stays inside the width it was given, its
    // implicit width stays bounded (so it cannot inflate the bubble or the
    // enclosing layout either), and the overflow lives in a CLIPPING
    // Flickable's contentWidth instead.
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
        // width (tens of thousands of pixels) and drags the enclosing layout
        // with it — that is precisely the escape this clamp prevents.
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

    // Short code must not scroll horizontally at all, or every ordinary
    // snippet would show a scrollbar it does not need.
    void shortCodeDoesNotOverflowHorizontally()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("ls -la")));
        QTRY_VERIFY_WITH_TIMEOUT(h.block->width() > 0.0, kTimeoutMs);
        QVERIFY(!h.block->property("horizontalOverflow").toBool());
        QVERIFY(!h.block->property("verticalOverflow").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

    // One gutter number per source line, in order. wrapMode is NoWrap, so a
    // source line is exactly one visual line and the two cannot drift.
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

        // A Text, never a TextEdit: if the gutter were selectable, a
        // select-all inside the block would carry line numbers into the paste.
        QVERIFY2(!gutter->property("selectByMouse").isValid(),
                 "the gutter is selectable text");
        QCOMPARE(h.warnings, QStringList{});
    }

    // Copy yields the program and nothing else. The fixture deliberately
    // starts a line with a digit and uses a tab, so gutter contamination or
    // whitespace normalisation both show up as an inequality.
    void copyYieldsExactlyTheCodeTextWithNoGutter()
    {
        const QString code =
            QStringLiteral("#!/bin/sh\n\techo one\n2nd line\n\t\tdeep");
        Harness h;
        QVERIFY(build(h, code));

        QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
        QVERIFY(QMetaObject::invokeMethod(h.block, "copyCode"));

        const QString pasted = QGuiApplication::clipboard()->text();
        // QTextCursor::selectedText() reports a block break as U+2029, so the
        // hidden-TextEdit relay is only correct if QQuickTextControl::copy()
        // normalises it. The production "copy message text" path uses the same
        // relay on bodies that are routinely multi-line, so a failure here is a
        // pre-existing defect in that path too — and the fix is a C++ clipboard
        // helper, not a change to this component's structure.
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

    // The height is bounded, and the content that does not fit is reachable
    // by scrolling rather than simply lost.
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

    // The nested-scroll hazard. A plain vertical notch over a code block must
    // reach the outer wheel owner (in production, the timeline's
    // timelineWheelHandler), and must NOT move the block's own content.
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

    // Shift+wheel is the block's own gesture: it scrolls the code sideways
    // and does NOT reach the timeline. This is the only wheel the block takes.
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
        // Negative delta = "toward the end of the line", matching the
        // wheel-down-scrolls-forward convention.
        sendWheel(*h.window, centre, QPoint(0, -120), Qt::ShiftModifier);

        QTRY_VERIFY_WITH_TIMEOUT(
            scroll->property("contentX").toReal() > 0.0, kTimeoutMs);
        QCOMPARE(h.outer->property("outerWheelNotches").toInt(), 0);
        QCOMPARE(h.warnings, QStringList{});
    }

    // The language token is re-validated here, not trusted: this label is
    // user-visible and part of an accessible name, and a class attribute is
    // sender-chosen text.
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
            // The label must not EXIST, not merely be invisible. A QQuickText
            // is born carrying ItemObservesViewport and only
            // QQuickText::setText clears it — and that function early-returns
            // on an unchanged value BEFORE the clearing line, so a Label
            // created holding "" keeps the flag for its whole life and
            // defeats subtree pruning on every contentY change. Inside a
            // timeline row that is the single most expensive QML mistake this
            // codebase knows (2026-08-19: 3000 observers, 33.89 -> 10.39 ms
            // per wheel notch once removed). Visibility is irrelevant to the
            // mechanism, so a `visible: false` gate would NOT be a fix — the
            // component uses a Loader, and this asserts that.
            QVERIFY2(bad.find(QStringLiteral("codeBlockLanguageLabel"))
                         == nullptr,
                     qPrintable(QStringLiteral(
                         "a rejected language (%1) still created the label")
                                    .arg(hostile)));
        }
    }

    // The code is rendered as PLAIN text. The segment text is already
    // entity-decoded, so RichText here would re-interpret a program's own
    // angle brackets as markup — the one thing this whole path exists to
    // prevent.
    void codeIsRenderedAsPlainTextAndNeverWrapped()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("<b>not bold</b> && <script>x</script>")));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(text != nullptr);
        // Text.PlainText == 0, Text.NoWrap == 0 in the QML enums.
        QCOMPARE(text->property("textFormat").toInt(), 0);
        QCOMPARE(text->property("wrapMode").toInt(), 0);
        QVERIFY(text->property("readOnly").toBool());
        QVERIFY(text->property("selectByMouse").toBool());
        QCOMPARE(text->property("text").toString(),
                 QStringLiteral("<b>not bold</b> && <script>x</script>"));
        QCOMPARE(h.warnings, QStringList{});
    }

    // "A visible focus state" is a requirement, not decoration: the block is
    // keyboard-reachable (it owns horizontal scrolling and Ctrl+C), so a
    // keyboard user must be able to see where they are. The focus ring is the
    // frame border itself, in the shared focus ink — one outline, not two.
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
};

QTEST_MAIN(CodeBlockQmlTest)
#include "CodeBlockQmlTest.moc"
