// Behavioural suite for qml/CodeBlock.qml. Qt's rich-text engine does not
// wrap <pre>, and MessageDelegate's root is clip:false (for the hover bar),
// so a long line could escape the bubble; only real instantiated geometry
// shows that. Pinned:
//   * a line wider than any pane never widens the component (the overflow is
//     contentX inside a clipping Flickable);
//   * the gutter has one number per source line and is not selectable;
//   * Copy yields exactly the program, with no line numbers or padding;
//   * the height is bounded and the content scrolls past the bound;
//   * the wheel: sideways scrolls the code; vertical scrolls a tall block
//     to its edge and then reaches the timeline's WheelHandler, but only once
//     the pointer has rested on the block, so a gesture scrolling the
//     conversation is never captured by a block passing under the pointer
//     (wheel events go innermost-first). Checked against the real
//     TimelinePane as well as a stand-in owner.

#include <QtTest/QtTest>

#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPointingDevice>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <memory>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/TimelineEvent.h"
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
    // pointer-wheel owner an ancestor away from the message row, restricted
    // to the same devices as production's.
    property int outerWheelNotches: 0
    // Of those, the ones that carried motion (a touchpad's ScrollBegin and
    // ScrollEnd carry none).
    property int outerMotionEvents: 0
    // The hook TimelinePane provides (see CodeBlock's wheelRouter): called
    // for every wheel event with motion that the block keeps for itself.
    property int nestedTaken: 0
    function nestedWheelTaken() { outer.nestedTaken += 1 }
    WheelHandler {
        objectName: "outerWheel"
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            outer.outerWheelNotches += 1
            if (event.angleDelta.x !== 0 || event.angleDelta.y !== 0
                    || event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0)
                outer.outerMotionEvents += 1
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

    // A mouse wheel notch from an explicit mouse device: production's wheel
    // owner accepts Mouse | TouchPad only, so the device is part of what is
    // tested.
    static void sendWheel(QQuickWindow &window, const QPointF &scenePos,
                          const QPoint &angleDelta,
                          Qt::KeyboardModifiers modifiers)
    {
        QWheelEvent wheel(scenePos, window.mapToGlobal(scenePos.toPoint()),
                          QPoint(0, 0), angleDelta, Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false,
                          Qt::MouseEventNotSynthesized, mouseDevice());
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
    }

    static const QPointingDevice *mouseDevice()
    {
        const QPointingDevice *device = QPointingDevice::primaryPointingDevice();
        Q_ASSERT(device->type() == QInputDevice::DeviceType::Mouse);
        return device;
    }

    // A touchpad that reports pixel deltas and scroll phases, as on Wayland
    // and macOS.
    static const QPointingDevice *touchpadDevice()
    {
        // Owned by the application, so it goes before Qt's device registry.
        static const QPointingDevice *device = new QPointingDevice(
            QStringLiteral("code-block-test touchpad"), 0x7c0b,
            QInputDevice::DeviceType::TouchPad,
            QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::Scroll
                | QInputDevice::Capability::PixelScroll,
            1, 3, QString(), QPointingDeviceUniqueId(), qApp);
        return device;
    }

    static void sendTouchpad(QQuickWindow &window, const QPointF &scenePos,
                             Qt::ScrollPhase phase, const QPoint &pixelDelta)
    {
        // Qt sends an angle delta beside the pixels, at twice their size on
        // macOS: a pixel-reading owner must use the pixels, on both axes.
        QWheelEvent wheel(scenePos, window.mapToGlobal(scenePos.toPoint()),
                          pixelDelta, pixelDelta * 2, Qt::NoButton,
                          Qt::NoModifier,
                          phase, false, Qt::MouseEventNotSynthesized,
                          touchpadDevice());
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
    }

    // The block's rest interval. The fallback keeps a component without the
    // property failing on behaviour rather than on a missing name.
    static int restMsOf(QQuickItem *block)
    {
        const QVariant v = block->property("wheelRestMs");
        return v.isValid() ? v.toInt() : 300;
    }

    static int trainMsOf(QQuickItem *block)
    {
        const QVariant v = block->property("wheelTrainMs");
        return v.isValid() ? v.toInt() : 500;
    }

    // Brings the pointer to `scenePos` and leaves it there past the block's
    // rest interval, so the next wheel event belongs to whatever is under it.
    // It first moves off the block: Qt delivers no hover for a move to where
    // the pointer already is, which made a second resting case in one
    // process start from a stale hover state. Returns whether the block's
    // hover state matches where the pointer ended up.
    [[nodiscard]] static bool restPointerAt(QQuickWindow &window,
                                            QQuickItem *block,
                                            const QPointF &scenePos)
    {
        const QRectF blockRect = block->mapRectToScene(block->boundingRect());
        QPointF away(window.width() - 2.0, window.height() - 2.0);
        if (blockRect.contains(away))
            away = QPointF(window.width() - 2.0, 2.0);
        if (blockRect.contains(away))
            away = QPointF(2.0, window.height() - 2.0);
        QTest::mouseMove(&window, away.toPoint());
        QCoreApplication::processEvents();
        QTest::mouseMove(&window, scenePos.toPoint());
        QCoreApplication::processEvents();
        QTest::qWait(restMsOf(block) + 80);

        auto *hover = block->findChild<QObject *>(
            QStringLiteral("codeBlockBodyHover"));
        auto *body = block->findChild<QQuickItem *>(
            QStringLiteral("codeBlockBody"));
        auto *router = block->findChild<QObject *>(
            QStringLiteral("codeBlockWheelRouter"));
        if (!hover || !body || !router) {
            // A component without the latch (the old code) has no state to
            // check; the behaviour is what the case asserts.
            return true;
        }
        const bool onBody =
            body->mapRectToScene(body->boundingRect()).contains(scenePos);
        if (hover->property("hovered").toBool() != onBody) {
            qWarning("restPointerAt: hovered=%d but the pointer is %s the "
                     "body", hover->property("hovered").toBool(),
                     onBody ? "on" : "off");
            return false;
        }
        return !onBody || router->property("restSinceMs").toReal() >= 0.0;
    }

    // Runs a timing-sensitive scenario up to three times. The scenario sets
    // `stalled` when the machine paused for longer than the interval it
    // measures, which makes that run say nothing either way; only a run that
    // was not stalled counts.
    static void retryOnStall(const std::function<void(bool *stalled)> &scenario)
    {
        for (int attempt = 1; attempt <= 3; ++attempt) {
            bool stalled = false;
            scenario(&stalled);
            if (QTest::currentTestFailed() || !stalled)
                return;
            qWarning("attempt %d stalled past the latch's interval; retrying",
                     attempt);
        }
        QFAIL("the machine stalled past the latch's interval on three "
              "attempts; the latch could not be measured");
    }

    static QPointF bodyCentre(QQuickItem *body)
    {
        return body->mapToScene(
            QPointF(body->width() / 2.0, body->height() / 2.0));
    }

    // Taller than the body cap and wider than any pane.
    static QString tallWideCode()
    {
        QStringList lines;
        for (int i = 1; i <= 60; ++i)
            lines << (i == 30 ? QString(300, QLatin1Char('w'))
                              : QStringLiteral("line %1").arg(i));
        return lines.join(QLatin1Char('\n'));
    }

    static qreal maxContentY(QQuickItem *scroll)
    {
        return std::max<qreal>(0.0, scroll->property("contentHeight").toReal()
                                        + scroll->property("bottomMargin").toReal()
                                        - scroll->height());
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

    // The production wheel stack: the REAL TimelinePane (rotated timeline
    // Flickable, timelineWheelHandler restricted to Mouse | TouchPad, the
    // MiddleClickScroller MouseArea drawn over the timeline) with one
    // code-block message as the newest row. The CodeBlock harness's stand-in
    // owner shares none of that stack, so it cannot show a wheel that never
    // reaches the block's router.
    struct PaneHarness {
        QStringList warnings;
        std::unique_ptr<AppController> controller;
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QObject> rootOwner;
        QQuickItem *root = nullptr;
        QQuickItem *timeline = nullptr;
        QQuickItem *block = nullptr;
        QQuickItem *body = nullptr;
        QQuickItem *scroll = nullptr;

        ~PaneHarness()
        {
            // The QML tree goes before the engine and the controller its
            // bindings reach.
            rootOwner.reset();
            window.reset();
            engine.reset();
            controller.reset();
        }
    };

    // A Repeater parents its delegates with setParentItem() only, so they are
    // not QObject descendants and findChildren() cannot see them.
    static QQuickItem *findVisual(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        const auto kids = parent->childItems();
        for (QQuickItem *child : kids) {
            if (child->objectName() == name)
                return child;
            if (QQuickItem *hit = findVisual(child, name))
                return hit;
        }
        return nullptr;
    }

    bool buildPane(PaneHarness &h, const QString &code)
    {
        h.controller =
            std::make_unique<AppController>(AppController::MockBackend);
        auto *mock = h.controller->findChild<MockMatrixClient *>();
        if (!mock)
            return false;
        const QString roomId = QStringLiteral("!general:mock.local");
        h.controller->setCurrentRoomId(roomId);

        h.engine = std::make_unique<QQmlApplicationEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        h.engine->rootContext()->setContextProperty("app", h.controller.get());
        QSignalSpy createdSpy(h.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        h.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kTimeoutMs))
            return false;
        h.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!h.root)
            return false;
        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(760, 900);
        h.root->setParentItem(h.window->contentItem());
        h.root->setSize(QSizeF(h.window->width(), h.window->height()));
        h.window->show();
        // Not asserted: the offscreen platform may decline.
        (void)QTest::qWaitForWindowExposed(h.window.get(), 2000);
        QCoreApplication::processEvents();

        // Plain rows first, so the timeline itself has range to scroll; the
        // code block is the newest message and sits on screen at the bottom.
        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-9000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$plain%1").arg(i);
            e.itemId = QStringLiteral("uid-plain%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("plain row %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        TimelineEvent codeEvent;
        codeEvent.eventId = QStringLiteral("$code");
        codeEvent.itemId = QStringLiteral("uid-code");
        codeEvent.roomId = roomId;
        codeEvent.sender = QStringLiteral("@alice:mock.local");
        codeEvent.senderDisplayName = QStringLiteral("Alice");
        codeEvent.body = QStringLiteral("```\n") + code
                         + QStringLiteral("\n```");
        codeEvent.formattedBody = QStringLiteral("<pre><code>")
                                  + code.toHtmlEscaped()
                                  + QStringLiteral("</code></pre>");
        codeEvent.timestamp = base.addSecs(60 * 30);
        events.append(codeEvent);
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        h.timeline = h.root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!h.timeline)
            return false;
        if (!QTest::qWaitFor([&] {
                return h.timeline->property("presentationReady").toBool();
            }, 5000))
            return false;
        if (!QTest::qWaitFor([&] {
                h.body = findVisual(h.root, QStringLiteral("codeBlockBody"));
                return h.body != nullptr && h.body->height() > 4.0
                       && h.body->width() > 4.0;
            }, 5000))
            return false;
        h.scroll = findVisual(h.body, QStringLiteral("codeBlockScroll"));
        for (QQuickItem *p = h.body; p; p = p->parentItem()) {
            if (p->objectName() == QLatin1String("codeBlockWheelRouter"))
                continue;
            if (p->property("lineCount").isValid()
                && p->property("verticalOverflow").isValid()) {
                h.block = p;
                break;
            }
        }
        // Settle the bottom pin before measuring.
        QTest::qWait(400);
        return h.scroll != nullptr && h.block != nullptr;
    }

    // The pane's warnings minus the one that belongs to the mock fixture
    // (image rows resolving `mock.local`), as TimelinePaneQmlTest filters it.
    static QStringList paneWarnings(const PaneHarness &h)
    {
        QStringList out;
        for (const QString &w : h.warnings) {
            if (w.contains(QLatin1String("QQuickImage: Host mock.local"))
                && w.contains(QLatin1String("not found")))
                continue;
            out << w;
        }
        return out;
    }

    // Wheel with an explicit mouse device: the production WheelHandler
    // accepts Mouse | TouchPad only, so the device is part of what is tested.
    static void sendPaneWheel(QQuickWindow &window, const QPointF &scenePos,
                              const QPoint &pixelDelta,
                              const QPoint &angleDelta,
                              Qt::KeyboardModifiers modifiers,
                              Qt::ScrollPhase phase = Qt::NoScrollPhase)
    {
        QWheelEvent wheel(scenePos, window.mapToGlobal(scenePos.toPoint()),
                          pixelDelta, angleDelta, Qt::NoButton, modifiers,
                          phase, false, Qt::MouseEventNotSynthesized,
                          mouseDevice());
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
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

    // Reads the harness's stand-in timeline owner.
    static int outerNotches(const Harness &h)
    {
        return h.outer->property("outerWheelNotches").toInt();
    }
    static int outerMotion(const Harness &h)
    {
        return h.outer->property("outerMotionEvents").toInt();
    }
    static int nestedTaken(const Harness &h)
    {
        return h.outer->property("nestedTaken").toInt();
    }

    // Taller than the body cap; with `wide`, wider than any pane too.
    static QString tallCode(int count, bool wide)
    {
        QStringList lines;
        for (int i = 1; i <= count; ++i)
            lines << (wide && i == 7 ? QString(300, QLatin1Char('w'))
                                     : QStringLiteral("row %1").arg(i));
        return lines.join(QLatin1Char('\n'));
    }

    // Moves the block by `dy`, as the timeline does when it scrolls a block
    // under a still pointer.
    static void carryBlockBy(Harness &h, qreal dy)
    {
        h.column->setY(h.column->y() + dy);
        QCoreApplication::processEvents();
        // A frame, so Qt re-delivers hover to what is now under the pointer.
        QTest::qWait(30);
    }

    // Moves the block so its body's centre sits under `scenePos`.
    static void carryBlockUnder(Harness &h, QQuickItem *body,
                                const QPointF &scenePos)
    {
        carryBlockBy(h, scenePos.y() - bodyCentre(body).y());
    }

    // (a) A pointer at rest over a tall block: the notch scrolls the code,
    // by the system's lines per notch, the timeline gets nothing, and the
    // timeline's hook still hears that the reader scrolled.
    void aRestingPointerScrollsATallBlockAndTheTimelineGetsNothing()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QVERIFY(text != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
        QVERIFY(h.block->property("verticalOverflow").toBool());

        const QPointF centre = bodyCentre(body);
        QVERIFY(restPointerAt(*h.window, h.block, centre));
        sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);

        const qreal afterOne = scroll->property("contentY").toReal();
        QVERIFY2(afterOne > 0.0,
                 "a notch over a tall block the pointer rests on did not "
                 "scroll it");
        QCOMPARE(outerNotches(h), 0);
        const qreal lineHeight = text->implicitHeight() / 400.0;
        QCOMPARE(qRound(afterOne),
                 qRound(QGuiApplication::styleHints()->wheelScrollLines()
                        * lineHeight));

        sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);
        QCOMPARE(outerNotches(h), 0);
        QCOMPARE(nestedTaken(h), 2);
        QCOMPARE(h.warnings, QStringList{});
    }

    // A fine-grained source (an X11 libinput touchpad, a Windows precision
    // touchpad) sends notches of a few 1/120ths, each well under a pixel of
    // code. Mid-block they are the block's and add up; none of them reads
    // as "at the edge".
    void aOneUnitWheelMidBlockMovesTheCodeAndIsKept()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QVERIFY(text != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);

        const QPointF centre = bodyCentre(body);
        QVERIFY(restPointerAt(*h.window, h.block, centre));
        sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
        QVERIFY(scroll->property("contentY").toReal() > 0.0);
        // Past the stream's end, so the next event is decided afresh.
        QTest::qWait(trainMsOf(h.block) + 80);

        const qreal before = scroll->property("contentY").toReal();
        const qreal unit = QGuiApplication::styleHints()->wheelScrollLines()
                           * (text->implicitHeight() / 400.0) / 120.0;
        QVERIFY2(unit < 0.5, "one unit is a pixel or more here; the case "
                             "would not exercise a sub-pixel step");
        for (int i = 0; i < 21; ++i)
            sendWheel(*h.window, centre, QPoint(0, -1), Qt::NoModifier);
        QCOMPARE(outerNotches(h), 0);
        QVERIFY2(qAbs(scroll->property("contentY").toReal() - before
                      - 21 * unit) < 0.01,
                 qPrintable(QStringLiteral("21 one-unit notches moved the code "
                                           "%1 px, expected %2")
                                .arg(scroll->property("contentY").toReal()
                                     - before)
                                .arg(21 * unit)));
        QCOMPARE(h.warnings, QStringList{});
    }

    // (b) Browser-style latching. A stream the block has taken stays the
    // block's at its edge, so the conversation never lurches when the code
    // runs out; after a pause the next notch at the edge goes to the
    // timeline, the rest of that stream stays there, and after another pause
    // the block takes the reversal, up to its top, and so on.
    void atTheBlocksEdgeTheStreamStaysAndThePauseHandsOn()
    {
        Harness h;
        QVERIFY(build(h, tallCode(400, true)));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
        QVERIFY(h.block->property("verticalOverflow").toBool());
        QVERIFY2(scroll->property("bottomMargin").toReal() > 0.0,
                 "no horizontal bar band, so the edge cannot show whether the "
                 "last line clears it");
        const qreal maxY = maxContentY(scroll);
        QVERIFY(maxY > 0.0);
        const int restMs = restMsOf(h.block);
        const int trainMs = trainMsOf(h.block);
        const QPointF centre = bodyCentre(body);
        const auto contentY = [scroll] {
            return scroll->property("contentY").toReal();
        };

        retryOnStall([&](bool *stalled) {
            scroll->setProperty("contentY", 0.0);
            const int base = outerNotches(h);
            QVERIFY(restPointerAt(*h.window, h.block, centre));
            QElapsedTimer gap;
            gap.start();
            int notches = 0;
            while (contentY() < maxY && notches < 500) {
                sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
                ++notches;
                if (gap.restart() >= trainMs) { *stalled = true; return; }
            }
            QCOMPARE(contentY(), maxY);
            // The same stream, at the edge: still the block's.
            for (int i = 0; i < 3; ++i) {
                sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
                if (gap.restart() >= trainMs) { *stalled = true; return; }
            }
            QCOMPARE(outerNotches(h), base);
            QCOMPARE(contentY(), maxY);

            // A pause: a stream starting at the edge is the conversation's...
            QTest::qWait(trainMs + 80);
            gap.restart();
            sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), base + 1);
            // ...both ways, while it runs.
            sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
            if (gap.elapsed() >= restMs) { *stalled = true; return; }
            QCOMPARE(outerNotches(h), base + 2);
            QCOMPARE(contentY(), maxY);

            // Another pause: the block takes the reversal, up to its top,
            // and keeps the stream there too.
            QTest::qWait(std::max(restMs, trainMs) + 80);
            gap.restart();
            notches = 0;
            while (contentY() > 0.0 && notches < 500) {
                sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
                ++notches;
                if (gap.restart() >= trainMs) { *stalled = true; return; }
            }
            sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
            if (gap.restart() >= trainMs) { *stalled = true; return; }
            QCOMPARE(outerNotches(h), base + 2);
            QCOMPARE(contentY(), 0.0);
            QTest::qWait(trainMs + 80);
            sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), base + 3);
        });
        QCOMPARE(h.warnings, QStringList{});
    }

    // (c) A block without vertical overflow never takes a vertical notch,
    // rested pointer or not, and a sideways overflow does not change that.
    void aShortBlockLeavesEveryVerticalNotchToTheTimeline()
    {
        for (const QString &code :
             {QStringLiteral("ls -la\ncd /tmp\nmake"),
              QStringLiteral("ls -la\n") + QString(300, QLatin1Char('x'))
                  + QStringLiteral("\nmake")}) {
            Harness h;
            QVERIFY(build(h, code));
            auto *body = h.find(QStringLiteral("codeBlockBody"));
            auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
            QVERIFY(body != nullptr);
            QVERIFY(scroll != nullptr);
            QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
            QVERIFY(!h.block->property("verticalOverflow").toBool());

            const QPointF centre = bodyCentre(body);
            QVERIFY(restPointerAt(*h.window, h.block, centre));
            sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
            sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
            sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), 3);
            QCOMPARE(scroll->property("contentY").toReal(), 0.0);
            QCOMPARE(scroll->property("contentX").toReal(), 0.0);
            QCOMPARE(h.warnings, QStringList{});
        }
    }

    // (d) The latch. A notch train scrolling the timeline carries a tall
    // block under the still pointer: the train stays on the timeline, however
    // long it runs, and only a notch after a pause is the block's.
    void aBlockArrivingUnderAStillPointerDoesNotTakeTheTrain()
    {
        const int trainGapMs = 40;
        retryOnStall([&](bool *stalled) {
            Harness h;
            QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
            auto *body = h.find(QStringLiteral("codeBlockBody"));
            auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
            QVERIFY(body != nullptr);
            QVERIFY(scroll != nullptr);
            QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
            QVERIFY(h.block->property("verticalOverflow").toBool());
            const int restMs = restMsOf(h.block);

            // The pointer rests on the timeline, below the block.
            const qreal blockBottom =
                h.block->mapToScene(QPointF(0.0, h.block->height())).y();
            const QPointF pointer(bodyCentre(body).x(), blockBottom + 40.0);
            QVERIFY(pointer.y() < h.outer->height());
            QVERIFY(restPointerAt(*h.window, h.block, pointer));
            sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), 1);

            // Each gap is timed from before the previous event, so a stall
            // cannot pass for a pause.
            QElapsedTimer gap;
            gap.start();
            carryBlockUnder(h, body, pointer);
            QVERIFY(body->mapRectToScene(body->boundingRect())
                        .contains(pointer));
            // The train runs on for twice the rest interval.
            const int train = (restMs * 2) / trainGapMs + 1;
            for (int i = 0; i < train; ++i) {
                QTest::qWait(trainGapMs);
                sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
                if (gap.restart() >= restMs) { *stalled = true; return; }
            }
            QCOMPARE(outerNotches(h), 1 + train);
            QCOMPARE(scroll->property("contentY").toReal(), 0.0);

            // A pause: the block under the pointer takes the next notch.
            QTest::qWait(restMs + 80);
            sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), 1 + train);
            QVERIFY2(scroll->property("contentY").toReal() > 0.0,
                     "after a pause the block under the pointer did not take "
                     "the notch");
            QCOMPARE(h.warnings, QStringList{});
        });
    }

    // The latch's other way in: the pointer rests on the block, then the
    // block moves under it (a timeline glide still running, a row above
    // resizing). A moving block is not a resting one, so the next notch is
    // the timeline's; once the block is still again, the block's.
    void aBlockMovingUnderARestingPointerIsNotRested()
    {
        retryOnStall([&](bool *stalled) {
            Harness h;
            QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
            auto *body = h.find(QStringLiteral("codeBlockBody"));
            auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
            QVERIFY(body != nullptr);
            QVERIFY(scroll != nullptr);
            QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
            const int restMs = restMsOf(h.block);

            const QPointF pointer = bodyCentre(body);
            QVERIFY(restPointerAt(*h.window, h.block, pointer));
            QElapsedTimer gap;
            gap.start();
            carryBlockBy(h, 30.0);
            QVERIFY(body->mapRectToScene(body->boundingRect())
                        .contains(pointer));
            sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
            if (gap.elapsed() >= restMs) { *stalled = true; return; }
            QCOMPARE(outerNotches(h), 1);
            QCOMPARE(scroll->property("contentY").toReal(), 0.0);

            QTest::qWait(restMs + 80);
            sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), 1);
            QVERIFY(scroll->property("contentY").toReal() > 0.0);
            QCOMPARE(h.warnings, QStringList{});
        });
    }

    // Touchpad swipes (pixel deltas with scroll phases) are owned whole: one
    // that began on the timeline stays there when a block slides under the
    // fingers, and one that begins on a rested block moves it pixel for
    // pixel, to its edge and no further, with nothing reaching the timeline.
    void aTouchpadSwipeBelongsWhollyToWhereItBegan()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
        const int restMs = restMsOf(h.block);

        const qreal blockBottom =
            h.block->mapToScene(QPointF(0.0, h.block->height())).y();
        const QPointF pointer(bodyCentre(body).x(), blockBottom + 40.0);
        QVERIFY(restPointerAt(*h.window, h.block, pointer));
        sendTouchpad(*h.window, pointer, Qt::ScrollBegin, QPoint());
        sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -30));
        QCOMPARE(outerMotion(h), 1);

        // Mid-swipe the block slides under the fingers, and the fingers keep
        // moving for longer than the rest interval: still the timeline's.
        carryBlockUnder(h, body, pointer);
        QTest::qWait(restMs + 80);
        for (int i = 0; i < 5; ++i)
            sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -30));
        sendTouchpad(*h.window, pointer, Qt::ScrollEnd, QPoint());
        QCOMPARE(outerMotion(h), 6);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);

        // A new swipe over the rested block is the block's, in its own pixels.
        QVERIFY(restPointerAt(*h.window, h.block, pointer));
        sendTouchpad(*h.window, pointer, Qt::ScrollBegin, QPoint());
        sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -40));
        QCOMPARE(scroll->property("contentY").toReal(), 40.0);
        const qreal maxY = maxContentY(scroll);
        for (int i = 0; i < 200 && scroll->property("contentY").toReal() < maxY;
             ++i)
            sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -400));
        QCOMPARE(scroll->property("contentY").toReal(), maxY);
        // Past the edge, still inside the same swipe: the conversation does
        // not jump.
        sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -400));
        sendTouchpad(*h.window, pointer, Qt::ScrollEnd, QPoint());
        QCOMPARE(outerMotion(h), 6);
        QCOMPARE(scroll->property("contentY").toReal(), maxY);
        QCOMPARE(h.warnings, QStringList{});
    }

    // A swipe that starts sideways claims only the sideways axis unless the
    // pointer had rested: a block carried under the fingers, then a swipe
    // starting (-2,-1) and going straight down, scrolls the code 2 px
    // sideways and leaves every vertical frame to the timeline.
    void aSidewaysStartBeforeRestOwnsOnlyTheSidewaysAxis()
    {
        retryOnStall([&](bool *stalled) {
            Harness h;
            QVERIFY(build(h, tallCode(400, true)));
            auto *body = h.find(QStringLiteral("codeBlockBody"));
            auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
            QVERIFY(body != nullptr);
            QVERIFY(scroll != nullptr);
            QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
            QVERIFY(h.block->property("verticalOverflow").toBool());
            QVERIFY(h.block->property("horizontalOverflow").toBool());
            const int restMs = restMsOf(h.block);

            const qreal blockBottom =
                h.block->mapToScene(QPointF(0.0, h.block->height())).y();
            const QPointF pointer(bodyCentre(body).x(), blockBottom + 40.0);
            QVERIFY(restPointerAt(*h.window, h.block, pointer));
            QElapsedTimer gap;
            gap.start();
            carryBlockUnder(h, body, pointer);
            sendTouchpad(*h.window, pointer, Qt::ScrollBegin, QPoint());
            if (gap.elapsed() >= restMs) { *stalled = true; return; }
            sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(-2, -1));
            for (int i = 0; i < 5; ++i)
                sendTouchpad(*h.window, pointer, Qt::ScrollUpdate,
                             QPoint(0, -30));
            sendTouchpad(*h.window, pointer, Qt::ScrollEnd, QPoint());
            QCOMPARE(scroll->property("contentY").toReal(), 0.0);
            QCOMPARE(scroll->property("contentX").toReal(), 2.0);
            QCOMPARE(outerMotion(h), 5);
            QCOMPARE(h.warnings, QStringList{});
        });
    }

    // macOS momentum. A ScrollBegin carrying motion right after a block-owned
    // ScrollEnd, and momentum frames after it, continue that gesture, even at
    // the block's edge where a new gesture would go to the timeline; a swipe
    // that starts later is decided afresh.
    void momentumRightAfterABlockOwnedSwipeStaysWithTheBlock()
    {
        retryOnStall([&](bool *stalled) {
            Harness h;
            QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
            auto *body = h.find(QStringLiteral("codeBlockBody"));
            auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
            QVERIFY(body != nullptr);
            QVERIFY(scroll != nullptr);
            QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
            const qreal maxY = maxContentY(scroll);
            const QPointF centre = bodyCentre(body);

            QVERIFY(restPointerAt(*h.window, h.block, centre));
            sendTouchpad(*h.window, centre, Qt::ScrollBegin, QPoint());
            sendTouchpad(*h.window, centre, Qt::ScrollUpdate,
                         QPoint(0, -100000));
            QCOMPARE(scroll->property("contentY").toReal(), maxY);
            QElapsedTimer sinceEnd;
            sendTouchpad(*h.window, centre, Qt::ScrollEnd, QPoint());
            sinceEnd.start();
            sendTouchpad(*h.window, centre, Qt::ScrollBegin, QPoint(0, -30));
            if (sinceEnd.elapsed() >= 100) { *stalled = true; return; }
            sendTouchpad(*h.window, centre, Qt::ScrollUpdate, QPoint(0, -30));
            sendTouchpad(*h.window, centre, Qt::ScrollEnd, QPoint());
            sendTouchpad(*h.window, centre, Qt::ScrollMomentum, QPoint(0, -30));
            QCOMPARE(outerMotion(h), 0);
            QCOMPARE(scroll->property("contentY").toReal(), maxY);

            // Later, a new swipe starting at the edge is the conversation's.
            QTest::qWait(1100);
            sendTouchpad(*h.window, centre, Qt::ScrollBegin, QPoint(0, -30));
            sendTouchpad(*h.window, centre, Qt::ScrollEnd, QPoint());
            QCOMPARE(outerMotion(h), 1);
            QCOMPARE(h.warnings, QStringList{});
        });
    }

    // A swipe over a wide but short block that starts sideways is the
    // block's sideways, and its vertical part still scrolls the conversation:
    // owning a swipe never swallows motion the block has no range for.
    void aSwipeOverAWideShortBlockStillScrollsTheTimelineVertically()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("ls -la\n") + QString(300, QLatin1Char('x'))));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        auto *scroll = h.find(QStringLiteral("codeBlockScroll"));
        QVERIFY(body != nullptr);
        QVERIFY(scroll != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > 4.0, kTimeoutMs);
        QVERIFY(h.block->property("horizontalOverflow").toBool());
        QVERIFY(!h.block->property("verticalOverflow").toBool());

        const QPointF centre = bodyCentre(body);
        QVERIFY(restPointerAt(*h.window, h.block, centre));
        sendTouchpad(*h.window, centre, Qt::ScrollBegin, QPoint());
        sendTouchpad(*h.window, centre, Qt::ScrollUpdate, QPoint(-20, -4));
        QCOMPARE(scroll->property("contentX").toReal(), 20.0);
        QCOMPARE(outerMotion(h), 0);
        sendTouchpad(*h.window, centre, Qt::ScrollUpdate, QPoint(0, -30));
        sendTouchpad(*h.window, centre, Qt::ScrollUpdate, QPoint(-1, -30));
        sendTouchpad(*h.window, centre, Qt::ScrollEnd, QPoint());
        QCOMPARE(outerMotion(h), 2);
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

    // Code reads left to right: a block whose first line is an Arabic comment
    // was laid out right to left, every line (TextEdit takes one direction for
    // the whole document). Prose beside it keeps its own directions.
    void codeReadsLeftToRightAndProseBesideItKeepsItsOwn()
    {
        const QString arabic = QStringLiteral("\u0645\u0631\u062D\u0628\u0627");
        const auto documentOf = [](QQuickItem *textEdit) -> QTextDocument * {
            auto *quick =
                textEdit->property("textDocument").value<QQuickTextDocument *>();
            return quick ? quick->textDocument() : nullptr;
        };

        Harness h;
        QVERIFY(build(h, QStringLiteral("// ") + arabic
                             + QStringLiteral("\nint x = 1; // done")));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(text != nullptr);
        QTextDocument *doc = documentOf(text);
        QVERIFY(doc);
        QCOMPARE(doc->blockCount(), 2);
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            QCOMPARE(b.textDirection(), Qt::LeftToRight);
            QRectF r;
            QVERIFY(QMetaObject::invokeMethod(text, "positionToRectangle",
                                              Q_RETURN_ARG(QRectF, r),
                                              Q_ARG(int, b.position())));
            QVERIFY2(qAbs(r.x()) < 0.5,
                     qPrintable(QStringLiteral("line %1 starts at x %2")
                                    .arg(b.blockNumber() + 1).arg(r.x())));
        }
        QCOMPARE(h.warnings, QStringList{});

        RowHarness row;
        QVERIFY(buildRow(row, QVariantList{
                             richSegment(QStringLiteral("<p>Hello world</p><p>")
                                         + arabic + QStringLiteral("</p>")),
                             codeSegment(QStringLiteral("int x = 1;"), QString())},
                         QStringLiteral("body"), false));
        const QList<QQuickItem *> rows = segmentRows(row);
        QCOMPARE(rows.size(), 2);
        auto *prose = rows.at(0)->findChild<QQuickItem *>(
            QStringLiteral("messageSegmentText"));
        QVERIFY(prose != nullptr);
        QTextDocument *proseDoc = documentOf(prose);
        QVERIFY(proseDoc);
        QCOMPARE(proseDoc->blockCount(), 2);
        QCOMPARE(proseDoc->firstBlock().textDirection(), Qt::LeftToRight);
        QCOMPARE(proseDoc->lastBlock().textDirection(), Qt::RightToLeft);
        QCOMPARE(row.bindingLoops(), QStringList{});
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

    // The production stack: in the REAL TimelinePane every sideways wheel
    // gesture over an overflowing block reaches the block's router (the
    // MiddleClickScroller over the timeline passes it on, and
    // timelineWheelHandler is never offered it), and the conversation does
    // not move.
    void theRealTimelineDeliversSidewaysWheelToTheBlock()
    {
        PaneHarness h;
        QVERIFY(buildPane(h, tallWideCode()));
        QVERIFY(h.block->property("horizontalOverflow").toBool());
        const QPointF centre = bodyCentre(h.body);
        const qreal timelineY = h.timeline->property("contentY").toReal();

        sendPaneWheel(*h.window, centre, QPoint(), QPoint(-120, 0),
                      Qt::NoModifier);
        QCOMPARE(h.scroll->property("contentX").toReal(), 120.0);
        sendPaneWheel(*h.window, centre, QPoint(), QPoint(0, -120),
                      Qt::ShiftModifier);
        QCOMPARE(h.scroll->property("contentX").toReal(), 240.0);
        sendTouchpad(*h.window, centre, Qt::ScrollBegin, QPoint());
        sendTouchpad(*h.window, centre, Qt::ScrollUpdate, QPoint(-30, 0));
        sendTouchpad(*h.window, centre, Qt::ScrollUpdate, QPoint(-30, 0));
        sendTouchpad(*h.window, centre, Qt::ScrollEnd, QPoint());
        QCOMPARE(h.scroll->property("contentX").toReal(), 300.0);

        QTest::qWait(300);
        QCOMPARE(h.timeline->property("contentY").toReal(), timelineY);
        QCOMPARE(h.scroll->property("contentY").toReal(), 0.0);
        QCOMPARE(paneWarnings(h), QStringList{});
    }

    // The production stack, vertically: a rested pointer's notches scroll
    // the block and the conversation stays put; at the block's top the next
    // notch reaches timelineWheelHandler and the conversation moves.
    void theRealTimelineLeavesARestedBlockItsNotchesUntilItsEdge()
    {
        PaneHarness h;
        QVERIFY(buildPane(h, tallWideCode()));
        QVERIFY(h.block->property("verticalOverflow").toBool());
        const QPointF centre = bodyCentre(h.body);
        const qreal timelineY = h.timeline->property("contentY").toReal();

        QVERIFY(restPointerAt(*h.window, h.block, centre));
        sendPaneWheel(*h.window, centre, QPoint(), QPoint(0, -120),
                      Qt::NoModifier);
        QVERIFY2(h.scroll->property("contentY").toReal() > 0.0,
                 "in the real timeline a rested pointer's notch did not "
                 "scroll the block");
        sendPaneWheel(*h.window, centre, QPoint(), QPoint(0, 120),
                      Qt::NoModifier);
        QCOMPARE(h.scroll->property("contentY").toReal(), 0.0);
        // A pause longer than the block's stream latch, which also lets any
        // glide the timeline might have started finish.
        QTest::qWait(trainMsOf(h.block) + 80);
        QCOMPARE(h.timeline->property("contentY").toReal(), timelineY);

        // At the top, a new stream: the conversation's.
        sendPaneWheel(*h.window, centre, QPoint(), QPoint(0, 120),
                      Qt::NoModifier);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.timeline->property("contentY").toReal() > timelineY + 1.0,
            kTimeoutMs);
        QCOMPARE(h.scroll->property("contentY").toReal(), 0.0);
        QCOMPARE(paneWarnings(h), QStringList{});
    }
};

QTEST_MAIN(CodeBlockQmlTest)
#include "CodeBlockQmlTest.moc"
