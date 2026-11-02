
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
#include <QScopeGuard>
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
}

class CodeBlockQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Harness {
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

    static const QPointingDevice *touchpadDevice()
    {
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
        QWheelEvent wheel(scenePos, window.mapToGlobal(scenePos.toPoint()),
                          pixelDelta, pixelDelta * 2, Qt::NoButton,
                          Qt::NoModifier,
                          phase, false, Qt::MouseEventNotSynthesized,
                          touchpadDevice());
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
    }

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

    struct RowHarness {
        QStringList warnings;
        std::unique_ptr<AppController> controller;
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
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

    static QString longProse()
    {
        return QStringLiteral(
            "Here is the thing I meant, and it runs on for a good while so "
            "that it certainly has to wrap inside the bubble at any sane cap "
            "width whatsoever, twice over if need be.");
    }

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
            rootOwner.reset();
            window.reset();
            engine.reset();
            controller.reset();
        }
    };

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
        (void)QTest::qWaitForWindowExposed(h.window.get(), 2000);
        QCoreApplication::processEvents();

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
        mock->resetTimelineForTest(roomId, events,0);

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

    void oneVeryLongLineNeverWidensTheComponent()
    {
        Harness h;
        QVERIFY(build(h, QString(5000, QLatin1Char('x'))));

        QTRY_VERIFY_WITH_TIMEOUT(h.block->width() > 0.0, kTimeoutMs);
        QVERIFY2(h.block->width() <= 420.5,
                 qPrintable(QStringLiteral("block width %1 exceeded the 420 "
                                           "it was given")
                                .arg(h.block->width())));
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

    void shortCodeDoesNotOverflowHorizontally()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("ls -la")));
        QTRY_VERIFY_WITH_TIMEOUT(h.block->width() > 0.0, kTimeoutMs);
        QVERIFY(!h.block->property("horizontalOverflow").toBool());
        QVERIFY(!h.block->property("verticalOverflow").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

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

        QVERIFY2(!gutter->property("selectByMouse").isValid(),
                 "the gutter is selectable text");
        QCOMPARE(h.warnings, QStringList{});
    }

    void copyYieldsExactlyTheCodeTextWithNoGutter()
    {
        const QString code =
            QStringLiteral("#!/bin/sh\n\techo one\n2nd line\n\t\tdeep");
        Harness h;
        QVERIFY(build(h, code));

        QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
        QVERIFY(QMetaObject::invokeMethod(h.block, "copyCode"));

        const QString pasted = QGuiApplication::clipboard()->text();
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

        QCOMPARE(band, h.block->property("verticalBarSpace").toReal());

        Harness shortBlock;
        QVERIFY(build(shortBlock, QStringLiteral("one line")));
        QTRY_VERIFY_WITH_TIMEOUT(shortBlock.block->height() > 0.0, kTimeoutMs);
        QVERIFY(!shortBlock.block->property("verticalOverflow").toBool());
        QCOMPARE(shortBlock.block->property("verticalBarSpace").toReal(), 0.0);
    }

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

    static QString tallCode(int count, bool wide)
    {
        QStringList lines;
        for (int i = 1; i <= count; ++i)
            lines << (wide && i == 7 ? QString(300, QLatin1Char('w'))
                                     : QStringLiteral("row %1").arg(i));
        return lines.join(QLatin1Char('\n'));
    }

    static void carryBlockBy(Harness &h, qreal dy)
    {
        h.column->setY(h.column->y() + dy);
        QCoreApplication::processEvents();
        QTest::qWait(30);
    }

    static void carryBlockUnder(Harness &h, QQuickItem *body,
                                const QPointF &scenePos)
    {
        carryBlockBy(h, scenePos.y() - bodyCentre(body).y());
    }

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
            for (int i = 0; i < 3; ++i) {
                sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
                if (gap.restart() >= trainMs) { *stalled = true; return; }
            }
            QCOMPARE(outerNotches(h), base);
            QCOMPARE(contentY(), maxY);

            QTest::qWait(trainMs + 80);
            gap.restart();
            sendWheel(*h.window, centre, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), base + 1);
            sendWheel(*h.window, centre, QPoint(0, 120), Qt::NoModifier);
            if (gap.elapsed() >= restMs) { *stalled = true; return; }
            QCOMPARE(outerNotches(h), base + 2);
            QCOMPARE(contentY(), maxY);

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

            const qreal blockBottom =
                h.block->mapToScene(QPointF(0.0, h.block->height())).y();
            const QPointF pointer(bodyCentre(body).x(), blockBottom + 40.0);
            QVERIFY(pointer.y() < h.outer->height());
            QVERIFY(restPointerAt(*h.window, h.block, pointer));
            sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), 1);

            QElapsedTimer gap;
            gap.start();
            carryBlockUnder(h, body, pointer);
            QVERIFY(body->mapRectToScene(body->boundingRect())
                        .contains(pointer));
            const int train = (restMs * 2) / trainGapMs + 1;
            for (int i = 0; i < train; ++i) {
                QTest::qWait(trainGapMs);
                sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
                if (gap.restart() >= restMs) { *stalled = true; return; }
            }
            QCOMPARE(outerNotches(h), 1 + train);
            QCOMPARE(scroll->property("contentY").toReal(), 0.0);

            QTest::qWait(restMs + 80);
            sendWheel(*h.window, pointer, QPoint(0, -120), Qt::NoModifier);
            QCOMPARE(outerNotches(h), 1 + train);
            QVERIFY2(scroll->property("contentY").toReal() > 0.0,
                     "after a pause the block under the pointer did not take "
                     "the notch");
            QCOMPARE(h.warnings, QStringList{});
        });
    }

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

        carryBlockUnder(h, body, pointer);
        QTest::qWait(restMs + 80);
        for (int i = 0; i < 5; ++i)
            sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -30));
        sendTouchpad(*h.window, pointer, Qt::ScrollEnd, QPoint());
        QCOMPARE(outerMotion(h), 6);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);

        QVERIFY(restPointerAt(*h.window, h.block, pointer));
        sendTouchpad(*h.window, pointer, Qt::ScrollBegin, QPoint());
        sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -40));
        QCOMPARE(scroll->property("contentY").toReal(), 40.0);
        const qreal maxY = maxContentY(scroll);
        for (int i = 0; i < 200 && scroll->property("contentY").toReal() < maxY;
             ++i)
            sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -400));
        QCOMPARE(scroll->property("contentY").toReal(), maxY);
        sendTouchpad(*h.window, pointer, Qt::ScrollUpdate, QPoint(0, -400));
        sendTouchpad(*h.window, pointer, Qt::ScrollEnd, QPoint());
        QCOMPARE(outerMotion(h), 6);
        QCOMPARE(scroll->property("contentY").toReal(), maxY);
        QCOMPARE(h.warnings, QStringList{});
    }

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

            QTest::qWait(1100);
            sendTouchpad(*h.window, centre, Qt::ScrollBegin, QPoint(0, -30));
            sendTouchpad(*h.window, centre, Qt::ScrollEnd, QPoint());
            QCOMPARE(outerMotion(h), 1);
            QCOMPARE(h.warnings, QStringList{});
        });
    }

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
        sendWheel(*h.window, centre, QPoint(0, -120), Qt::ShiftModifier);

        QTRY_VERIFY_WITH_TIMEOUT(
            scroll->property("contentX").toReal() > 0.0, kTimeoutMs);
        QCOMPARE(h.outer->property("outerWheelNotches").toInt(), 0);
        QCOMPARE(h.warnings, QStringList{});
    }

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
            QVERIFY2(bad.find(QStringLiteral("codeBlockLanguageLabel"))
                         == nullptr,
                     qPrintable(QStringLiteral(
                         "a rejected language (%1) still created the label")
                                    .arg(hostile)));
        }
    }

    void codeIsRenderedAsPlainTextAndNeverWrapped()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("<b>not bold</b> && <script>x</script>")));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(text != nullptr);
        QCOMPARE(text->property("textFormat").toInt(), 0);
        QCOMPARE(text->property("wrapMode").toInt(), 0);
        QVERIFY(text->property("readOnly").toBool());
        QVERIFY(text->property("selectByMouse").toBool());
        QCOMPARE(text->property("text").toString(),
                 QStringLiteral("<b>not bold</b> && <script>x</script>"));
        QCOMPARE(h.warnings, QStringList{});
    }

    void theCaretIsScrolledIntoViewWhenItMovesPastTheEdge()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *flick = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(flick && text);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);

        const int lastLine = text->property("text").toString().size();
        text->setProperty("cursorPosition", lastLine);
        QCoreApplication::processEvents();
        const qreal y = flick->property("contentY").toReal();
        const QRectF caret = text->property("cursorRectangle").toRectF();
        QVERIFY2(y > 0.0, "the view stayed at the top while the caret moved "
                          "400 lines down");
        QVERIFY2(caret.bottom() <= y + flick->height() + 1.0
                     && caret.top() >= y - 1.0,
                 "the caret is outside the visible part of the code");

        text->setProperty("cursorPosition", 0);
        QCoreApplication::processEvents();
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        QCOMPARE(h.warnings, QStringList{});
    }

    void aDragHeldPastTheBottomEdgeScrollsAndExtendsTheSelection()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *flick = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        auto *body = h.find(QStringLiteral("codeBlockBody"));
        QVERIFY(flick && text && body);

        const QPointF start = body->mapToScene(QPointF(
            flick->x() + 30, 20));
        const QPointF below = body->mapToScene(QPointF(
            flick->x() + 30, body->height() + 120));
        QTest::mousePress(h.window.get(), Qt::LeftButton, Qt::NoModifier,
                          start.toPoint());
        QTest::mouseMove(h.window.get(), below.toPoint());
        const qreal before = flick->property("contentY").toReal();
        for (int i = 0; i < 12; ++i)
            QVERIFY(QMetaObject::invokeMethod(h.block, "autoscrollTick"));
        QVERIFY2(flick->property("contentY").toReal() > before + 50.0,
                 "holding the drag below the block did not scroll it");
        QTest::mouseRelease(h.window.get(), Qt::LeftButton, Qt::NoModifier,
                            below.toPoint());
        const int end = text->property("selectionEnd").toInt();
        QVERIFY2(end > 100, "the selection did not grow past the first screen");
        QCOMPARE(h.warnings, QStringList{});
    }

    void aWheelScrollIsNotUndoneByARelayoutUnderAStillCaret()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *flick = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(flick);
        QVERIFY(text);
        QVERIFY(text->setProperty("cursorPosition", text->property("length")));
        QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentY").toReal() > 600.0,
                                 kTimeoutMs);
        QVERIFY(flick->setProperty("contentY", 0.0));
        QCoreApplication::processEvents();
        const int pos = text->property("cursorPosition").toInt();
        auto *theme = h.engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY(theme);
        const qreal scale = theme->property("textScale").toReal();
        const auto restore = qScopeGuard([&] {
            theme->setProperty("textScale", scale);
        });
        text->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(text->hasActiveFocus(), kTimeoutMs);
        QVERIFY(theme->setProperty("textScale", scale * 1.5));
        (void)text->property("cursorRectangle");
        QTest::qWait(100);
        QCOMPARE(text->property("cursorPosition").toInt(), pos);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        QCOMPARE(h.warnings, QStringList{});
    }

    void selectAllDoesNotJumpToTheBottom()
    {
        Harness h;
        QVERIFY(build(h, repeatedLines(400, QStringLiteral("row "))));
        auto *flick = h.find(QStringLiteral("codeBlockScroll"));
        auto *text = h.find(QStringLiteral("codeBlockText"));
        QVERIFY(flick);
        QVERIFY(text);
        QVERIFY(QMetaObject::invokeMethod(text, "selectAll"));
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        QVERIFY(text->property("selectionEnd").toInt() > 0);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        QCOMPARE(h.warnings, QStringList{});
    }

    void theDragHandlerIgnoresTouch()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("echo hi")));
        auto *ph = h.block->findChild<QObject *>(
            QStringLiteral("codeBlockDragPoint"));
        QVERIFY(ph);
        const auto devices = ph->property("acceptedDevices").toInt();
        QVERIFY2(!(devices & int(QInputDevice::DeviceType::TouchScreen)),
                 "the drag handler accepts touch");
        QVERIFY(devices & int(QInputDevice::DeviceType::Mouse));
    }

    void returnAndEnterActivateTheFocusedCopyButton()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("echo hi")));
        auto *button = h.find(QStringLiteral("codeBlockCopyButton"));
        QVERIFY(button);
        for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Enter}) {
            QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
            button->forceActiveFocus(Qt::TabFocusReason);
            QTRY_VERIFY_WITH_TIMEOUT(button->hasActiveFocus(), kTimeoutMs);
            QTest::keyClick(h.window.get(), key);
            QCOMPARE(QGuiApplication::clipboard()->text(),
                     QStringLiteral("echo hi"));
        }
        QCOMPARE(h.warnings, QStringList{});
    }

    void theCopyButtonStaysClearOfAHostReservedTrailingBand()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("echo hi")));
        auto *button = h.find(QStringLiteral("codeBlockCopyButton"));
        QVERIFY(button);
        const qreal before = button->mapToItem(h.block, QPointF(button->width(), 0)).x();
        QVERIFY(h.block->setProperty("toolbarClearance", 130.0));
        QCoreApplication::processEvents();
        const qreal after = button->mapToItem(h.block, QPointF(button->width(), 0)).x();
        QVERIFY2(before - after > 129.0 && before - after < 131.0,
                 "toolbarClearance does not move the Copy button");
        QVERIFY(after <= h.block->width() - 130.0);
        QCOMPARE(h.warnings, QStringList{});
    }

    void theCopyButtonShowsATooltipOnHover()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral("echo hi")));
        auto *button = h.find(QStringLiteral("codeBlockCopyButton"));
        QVERIFY(button);
        auto *tip = h.block->findChild<QObject *>(
            QStringLiteral("codeBlockCopyTip"));
        QVERIFY2(tip, "the Copy button has no tooltip");
        QCOMPARE(tip->property("text").toString(), QStringLiteral("Copy code"));
        QVERIFY(!tip->property("visible").toBool());
        const QPointF c = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2));
        QTest::mouseMove(h.window.get(), c.toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(tip->property("visible").toBool(), kTimeoutMs);
        QCOMPARE(h.warnings, QStringList{});
    }

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

        auto *paragraph = rows.at(0)->findChild<QQuickItem *>(
            QStringLiteral("messageSegmentText"));
        QVERIFY(paragraph != nullptr);
        QCOMPARE(paragraph->width(), cap);
        QVERIFY(paragraph->property("contentWidth").toReal() <= cap + 0.5);
        QVERIFY2(paragraph->property("lineCount").toInt() > 1,
                 "the paragraph did not wrap at the cap");
    }

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
        QTest::qWait(trainMsOf(h.block) + 80);
        QCOMPARE(h.timeline->property("contentY").toReal(), timelineY);

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
