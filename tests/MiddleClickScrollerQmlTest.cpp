// MiddleClickScroller driven by a real pointer, as browsers do autoscroll: a
// quick middle click latches, a press that travels is a hold, the next click
// ends a latch and goes no further, and nothing starts over a link, editable
// text or a MouseArea that takes the middle button. The 2026-08-21 defects
// that removed the first latch (82794ed9) each have a case: a click that turns
// into a drag, a latch that outlives the pane or the window, and rows losing
// their clicks and hover. The last case drives the room list's real scroller
// over the mock backend.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWheelEvent>

#include <memory>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "models/RoomListModel.h"

namespace {

constexpr int kTimeoutMs = 4000;

// A 400x600 pane over a 500-wide window, so x 400..500 is outside the pane
// but inside the window. At contentY 1000: link y 20, input y 80, owner y 140,
// row y 200..500, empty content below.
const char *kFixture = R"QML(
import QtQuick
import MatrixClient
Item {
    id: host
    width: 500; height: 600
    property int rowLeftClicks: 0
    property int rowRightClicks: 0
    property int ownerMiddleClicks: 0
    Flickable {
        id: flick
        objectName: "flick"
        width: 400; height: 600
        contentWidth: 400; contentHeight: 5000
        contentY: 1000
        Item {
            width: 400; height: 5000
            Text {
                objectName: "link"
                x: 10; y: 1020
                textFormat: Text.RichText
                font.pixelSize: 24
                text: "<a href='https://example.org/probe'>a link to example.org</a>"
            }
            TextInput {
                objectName: "input"
                x: 10; y: 1080; width: 300; height: 32
                font.pixelSize: 20
                text: "editable"
            }
            MouseArea {
                objectName: "owner"
                x: 10; y: 1140; width: 300; height: 40
                acceptedButtons: Qt.MiddleButton
                onClicked: host.ownerMiddleClicks++
            }
            MouseArea {
                objectName: "row"
                x: 0; y: 1200; width: 400; height: 300
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true
                onClicked: (mouse) => {
                    if (mouse.button === Qt.RightButton)
                        host.rowRightClicks++
                    else
                        host.rowLeftClicks++
                }
            }
        }
    }
    MiddleClickScroller {
        objectName: "scroller"
        width: 400; height: 600
        view: flick
    }
}
)QML";

} // namespace

class MiddleClickScrollerQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Fixture {
        QQmlEngine engine;
        QQuickWindow window;
        std::unique_ptr<QQuickItem> root;
        QQuickItem *scroller = nullptr;
        QQuickItem *flick = nullptr;
        QQuickItem *marker = nullptr;
        QQuickItem *area = nullptr;
    };

    static bool load(Fixture &f)
    {
        QQmlComponent component(&f.engine);
        component.setData(kFixture, QUrl(QStringLiteral("qrc:/mcs.qml")));
        if (!component.errors().isEmpty()) {
            qWarning() << component.errorString();
            return false;
        }
        f.root.reset(qobject_cast<QQuickItem *>(component.create()));
        if (!f.root)
            return false;
        f.window.resize(500, 600);
        f.root->setParentItem(f.window.contentItem());
        f.window.show();
        if (!QTest::qWaitForWindowExposed(&f.window))
            return false;
        f.window.requestActivate();
        if (!QTest::qWaitForWindowActive(&f.window))
            return false;
        f.scroller = f.root->findChild<QQuickItem *>(QStringLiteral("scroller"));
        f.flick = f.root->findChild<QQuickItem *>(QStringLiteral("flick"));
        if (!f.scroller || !f.flick)
            return false;
        f.marker = f.scroller->findChild<QQuickItem *>(
            QStringLiteral("autoscrollAnchorMarker"));
        f.area = f.scroller->findChild<QQuickItem *>(
            QStringLiteral("autoscrollMouseArea"));
        return f.marker && f.area;
    }

    static bool active(const Fixture &f)
    {
        return f.scroller->property("active").toBool();
    }
    static bool latched(const Fixture &f)
    {
        return f.scroller->property("latched").toBool();
    }
    static qreal contentY(const Fixture &f)
    {
        return f.flick->property("contentY").toReal();
    }
    static int hostInt(const Fixture &f, const char *name)
    {
        return f.root->property(name).toInt();
    }
    static QPoint centreOf(QQuickItem *item)
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
            .toPoint();
    }
    static void click(Fixture &f, Qt::MouseButton button, const QPoint &at)
    {
        QTest::mouseClick(&f.window, button, Qt::NoModifier, at);
        QCoreApplication::processEvents();
    }
    // A middle click at `at` that must leave a latched scroll behind.
    static bool latchAt(Fixture &f, const QPoint &at)
    {
        click(f, Qt::MiddleButton, at);
        return active(f) && latched(f) && f.marker->isVisible();
    }
    static void wheel(Fixture &f, const QPoint &at)
    {
        QWheelEvent notch(QPointF(at), f.window.mapToGlobal(at), QPoint(0, 0),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&f.window, &notch);
        QCoreApplication::processEvents();
    }
    // Still for `ms`: nothing is writing contentY.
    static bool holdsStill(const Fixture &f, int ms)
    {
        const qreal before = contentY(f);
        QTest::qWait(ms);
        return qAbs(contentY(f) - before) < 0.5;
    }

    // Every item below this one in the visual tree (findChild cannot see
    // delegates, which are parented to the view's model).
    static void visualTree(QQuickItem *item, QList<QQuickItem *> &out)
    {
        for (QQuickItem *child : item->childItems()) {
            out.append(child);
            visualTree(child, out);
        }
    }

    // Over plain content, in the pane, no owner under it.
    const QPoint spot{200, 350};

    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
    }

    // The browser rule: a middle click that does not travel past the slop
    // latches, however long it was held, and a fresh latch does not move.
    void aQuickMiddleClickLatchesAndSitsStill()
    {
        Fixture f;
        QVERIFY(load(f));
        const qreal slop = f.scroller->property("latchSlop").toReal();
        QVERIFY2(slop >= 5 && slop <= f.scroller->property("deadZone").toReal(),
                 qPrintable(QStringLiteral("latchSlop %1").arg(slop)));

        // Jitter inside the slop is still a click.
        const QPoint jitter = spot + QPoint(4, 4);
        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QTest::mouseMove(&f.window, jitter);
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier, jitter);
        QCoreApplication::processEvents();
        QVERIFY2(active(f) && latched(f), "a quick middle click did not latch");
        QVERIFY2(f.marker->isVisible(), "a latched scroll shows no marker");
        QVERIFY2(holdsStill(f, 200), "a fresh latch scrolled on its own");

        // No time limit: a long still press latches too.
        QTest::keyClick(&f.window, Qt::Key_Escape);
        QVERIFY(!active(f));
        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QTest::qWait(700);
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QCoreApplication::processEvents();
        QVERIFY2(latched(f), "a long still middle press did not latch");
    }

    // A late timer must not make the scroll slower: each step covers the real
    // time since the previous one (bounded), not a nominal 16 ms.
    void aSlowFrameAdvancesByTheTimeThatPassed()
    {
        Fixture f;
        QVERIFY(load(f));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        f.scroller->setProperty("lastStepMs", double(now - 60));
        QVariant dt;
        QVERIFY(QMetaObject::invokeMethod(f.scroller, "elapsedSinceLastStep",
                                          Q_RETURN_ARG(QVariant, dt),
                                          Q_ARG(QVariant, 16)));
        QVERIFY2(dt.toReal() >= 55 && dt.toReal() <= 100,
                 qPrintable(QStringLiteral("a 60 ms gap stepped %1 ms")
                                .arg(dt.toReal())));
        f.scroller->setProperty("lastStepMs", double(now - 5000));
        QVERIFY(QMetaObject::invokeMethod(f.scroller, "elapsedSinceLastStep",
                                          Q_RETURN_ARG(QVariant, dt),
                                          Q_ARG(QVariant, 16)));
        QCOMPARE(dt.toReal(), f.scroller->property("maxStepMs").toReal());
    }

    // Pinned against the end of the range while the pointer still asks for
    // more, the host keeps hearing about it (that is what asks for older
    // history at the top), and nothing throws or moves.
    void pinnedAtTheEdgeStillReportsTheGesture()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(latchAt(f, spot));
        QTest::mouseMove(&f.window, spot + QPoint(0, 200));
        const qreal bottom = f.flick->property("contentHeight").toReal()
                             - f.flick->height();
        QTRY_VERIFY_WITH_TIMEOUT(contentY(f) >= bottom - 0.5, kTimeoutMs);
        QSignalSpy spy(f.scroller, SIGNAL(scrolled()));
        QTRY_VERIFY2_WITH_TIMEOUT(spy.count() >= 3,
                                  "no scrolled() once pinned at the end",
                                  kTimeoutMs);
        QVERIFY(contentY(f) <= bottom + 0.5);
        QVERIFY(latched(f));
    }

    // Latched, the pointer steers with no button held, both ways.
    void aLatchScrollsTowardThePointerWithNoButtonHeld()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(latchAt(f, spot));
        const qreal start = contentY(f);
        QTest::mouseMove(&f.window, spot + QPoint(0, 150));
        QTRY_VERIFY2_WITH_TIMEOUT(contentY(f) > start + 40,
                                  "moving below the anchor did not scroll down",
                                  kTimeoutMs);
        QTest::mouseMove(&f.window, spot + QPoint(0, -150));
        const qreal turned = contentY(f);
        QTRY_VERIFY2_WITH_TIMEOUT(contentY(f) < turned - 40,
                                  "moving above the anchor did not scroll up",
                                  kTimeoutMs);
        QVERIFY(latched(f));
    }

    // The next click of any button ends a latch and reaches nothing below;
    // afterwards the rows get their clicks again.
    void theNextClickEndsALatchAndGoesNoFurther()
    {
        Fixture f;
        QVERIFY(load(f));
        // Keep the row under the pointer.
        f.scroller->setProperty("maxSpeed", 0);

        QVERIFY(latchAt(f, spot));
        click(f, Qt::LeftButton, spot);
        QVERIFY2(!active(f), "a left click did not end the latch");
        QCOMPARE(hostInt(f, "rowLeftClicks"), 0);

        QVERIFY(latchAt(f, spot));
        click(f, Qt::RightButton, spot);
        QVERIFY2(!active(f), "a right click did not end the latch");
        QCOMPARE(hostInt(f, "rowRightClicks"), 0);

        // A middle click ends it too, and its release starts no new latch.
        QVERIFY(latchAt(f, spot));
        click(f, Qt::MiddleButton, spot);
        QVERIFY2(!active(f) && !latched(f),
                 "the exit middle click latched again");
        QVERIFY(!f.marker->isVisible());

        // Idle again: middle only, no hover, and the rows get their clicks.
        QVERIFY(f.area->property("acceptedButtons").value<Qt::MouseButtons>()
                == Qt::MouseButtons(Qt::MiddleButton));
        QVERIFY(!f.area->property("hoverEnabled").toBool());
        click(f, Qt::LeftButton, spot);
        click(f, Qt::RightButton, spot);
        QCOMPARE(hostInt(f, "rowLeftClicks"), 1);
        QCOMPARE(hostInt(f, "rowRightClicks"), 1);
    }

    // Idle, the area is middle-only with hover off: rows keep every other
    // button, hover and the wheel. (Guard; proven by mutants.)
    void rowsKeepTheirClicksHoverAndWheelWhenIdle()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(f.area->property("acceptedButtons").value<Qt::MouseButtons>()
                == Qt::MouseButtons(Qt::MiddleButton));
        QVERIFY(!f.area->property("hoverEnabled").toBool());

        // The wheel first: a press on a Flickable child stamps the Flickable
        // with QTest's synthetic mouse clock, and the wheel event here has no
        // timestamp, so after a click Flickable finds no elapsed time and drops
        // the notch whoever delivered it.
        const qreal before = contentY(f);
        wheel(f, spot);
        QTRY_VERIFY2_WITH_TIMEOUT(qAbs(contentY(f) - before) > 1,
                                  "an idle scroller swallowed the wheel",
                                  kTimeoutMs);
        // Back to the fixture's layout, at rest, so the row is under `spot`.
        QVERIFY(QMetaObject::invokeMethod(f.flick, "cancelFlick"));
        f.flick->setProperty("contentY", 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.flick->property("moving").toBool(),
                                 kTimeoutMs);

        auto *row = f.root->findChild<QQuickItem *>(QStringLiteral("row"));
        QVERIFY(row);
        click(f, Qt::LeftButton, spot);
        click(f, Qt::RightButton, spot);
        QCOMPARE(hostInt(f, "rowLeftClicks"), 1);
        QCOMPARE(hostInt(f, "rowRightClicks"), 1);
        QTest::mouseMove(&f.window, spot + QPoint(10, 10));
        QTRY_VERIFY2_WITH_TIMEOUT(row->property("containsMouse").toBool(),
                                  "the row lost hover to an idle scroller",
                                  kTimeoutMs);
    }

    // 2026-08-21: an ordinary click that turns into a drag must not latch,
    // even when it comes back to where it started.
    void aPressThatTravelsIsAHoldNotALatch()
    {
        Fixture f;
        QVERIFY(load(f));
        const qreal start = contentY(f);
        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QTest::mouseMove(&f.window, spot + QPoint(0, 100));
        QTRY_VERIFY2_WITH_TIMEOUT(contentY(f) > start + 20,
                                  "a held middle drag did not scroll",
                                  kTimeoutMs);
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier,
                            spot + QPoint(0, 100));
        QCoreApplication::processEvents();
        QVERIFY2(!active(f) && !latched(f), "a middle drag latched");
        QVERIFY(!f.marker->isVisible());
        QVERIFY(holdsStill(f, 200));

        // Out and back to the anchor is still a drag.
        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QTest::mouseMove(&f.window, spot + QPoint(60, 0));
        QTest::mouseMove(&f.window, spot + QPoint(1, 0));
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier,
                            spot + QPoint(1, 0));
        QCoreApplication::processEvents();
        QVERIFY2(!active(f) && !latched(f),
                 "a drag that came back to its anchor latched");
    }

    void escapeEndsALatch()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(latchAt(f, spot));
        QTest::keyClick(&f.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY2(!active(f) && !latched(f), "Escape did not end a latch");
        QVERIFY(!f.marker->isVisible());
    }

    // 2026-08-21: a latched pointer that left the pane kept scrolling at its
    // last speed. Leaving the pane ends it.
    void leavingThePaneEndsALatch()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(latchAt(f, spot));
        const QPoint below = spot + QPoint(0, 150);
        const qreal start = contentY(f);
        QTest::mouseMove(&f.window, below);
        QTRY_VERIFY_WITH_TIMEOUT(contentY(f) > start + 20, kTimeoutMs);
        // Out of the pane, still in the window.
        QTest::mouseMove(&f.window, QPoint(450, below.y()));
        QTRY_VERIFY2_WITH_TIMEOUT(!active(f),
                                  "a latch outlived the pointer leaving the pane",
                                  kTimeoutMs);
        QVERIFY2(holdsStill(f, 200), "the view kept scrolling after the latch");
    }

    void leavingTheWindowEndsALatch()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(latchAt(f, spot));
        QTest::mouseMove(&f.window, spot + QPoint(0, 60));
        QCoreApplication::processEvents();
        QVERIFY(latched(f));
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&f.window, &leave);
        QTRY_VERIFY2_WITH_TIMEOUT(!active(f),
                                  "a latch outlived the pointer leaving the window",
                                  kTimeoutMs);
    }

    void losingFocusEndsALatch()
    {
        Fixture f;
        QVERIFY(load(f));
        QVERIFY(latchAt(f, spot));
        QWindow other;
        other.resize(100, 100);
        other.show();
        other.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTRY_VERIFY2_WITH_TIMEOUT(!active(f),
                                  "a latch outlived the window losing focus",
                                  kTimeoutMs);
    }

    // Firefox's rule: the wheel ends a latch and is swallowed, except for a
    // short cooldown after latching, when a stray notch is only swallowed.
    void theWheelEndsALatchAfterTheCooldownAndIsSwallowed()
    {
        Fixture f;
        QVERIFY(load(f));
        f.scroller->setProperty("maxSpeed", 0);
        QVERIFY(latchAt(f, spot));
        const qreal start = contentY(f);
        wheel(f, spot);
        QVERIFY2(latched(f), "a wheel notch inside the cooldown ended the latch");
        QVERIFY2(holdsStill(f, 150), "a wheel notch reached the view while latched");
        QCOMPARE(contentY(f), start);

        QTest::qWait(f.scroller->property("wheelCooldownMs").toInt() + 100);
        wheel(f, spot);
        QVERIFY2(!active(f), "the wheel did not end the latch");
        QVERIFY2(holdsStill(f, 150), "the ending wheel notch reached the view");
        QCOMPARE(contentY(f), start);
    }

    // No hold and no latch over a link: middle click is the link's
    // (Firefox and Chromium both refuse to start there).
    void nothingStartsOverALink()
    {
        Fixture f;
        QVERIFY(load(f));
        auto *link = f.root->findChild<QQuickItem *>(QStringLiteral("link"));
        QVERIFY(link);
        const QPointF local(12, link->height() / 2);
        QString href;
        QVERIFY(QMetaObject::invokeMethod(link, "linkAt",
                                          Q_RETURN_ARG(QString, href),
                                          Q_ARG(qreal, local.x()),
                                          Q_ARG(qreal, local.y())));
        QVERIFY2(!href.isEmpty(), "premise: the probe point is not on the link");
        const QPoint at = link->mapToScene(local).toPoint();

        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!active(f), "a middle press on a link started autoscroll");
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!latched(f), "a middle click on a link latched");

        // The fixture can latch beside it.
        QVERIFY(latchAt(f, spot));
    }

    // Editable text owns middle click (primary-selection paste).
    void nothingStartsOverEditableText()
    {
        Fixture f;
        QVERIFY(load(f));
        auto *input = f.root->findChild<QQuickItem *>(QStringLiteral("input"));
        QVERIFY(input);
        const QPoint at = centreOf(input);
        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!active(f), "a middle press on editable text started autoscroll");
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY(!latched(f));
    }

    // A MouseArea that takes the middle button gets its click.
    void aMiddleButtonOwnerGetsItsClick()
    {
        Fixture f;
        QVERIFY(load(f));
        auto *owner = f.root->findChild<QQuickItem *>(QStringLiteral("owner"));
        QVERIFY(owner);
        click(f, Qt::MiddleButton, centreOf(owner));
        QCOMPARE(hostInt(f, "ownerMiddleClicks"), 1);
        QVERIFY(!active(f));
    }

    // Browsers start nothing when there is nothing to scroll.
    void nothingStartsWhenTheViewCannotScroll()
    {
        Fixture f;
        QVERIFY(load(f));
        f.flick->setProperty("contentY", 0);
        f.flick->setProperty("contentHeight", 600);
        QTest::mousePress(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QCoreApplication::processEvents();
        QVERIFY2(!active(f), "autoscroll started on a view that cannot scroll");
        QTest::mouseRelease(&f.window, Qt::MiddleButton, Qt::NoModifier, spot);
        QCoreApplication::processEvents();
        QVERIFY(!latched(f));
    }

    // The real room list: a latch swallows the click on a row, the next click
    // opens it, and a room switch ends a latch.
    void theRoomListLatchesSwallowsTheExitClickAndStopsOnARoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(kTimeoutMs));
        QTRY_VERIFY_WITH_TIMEOUT(controller.roomList()->rowCount() >= 2,
                                 kTimeoutMs);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                                 &controller);
        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick
import MatrixClient
RoomListClassicPresenter { objectName: "presenter"; width: 320; height: 90 }
)QML", QUrl(QStringLiteral("qrc:/mcs-roomlist.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QQuickItem> presenter(
            qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(presenter);
        QQuickWindow window;
        window.resize(320, 90);
        presenter->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        auto *scroller = presenter->findChild<QQuickItem *>(
            QStringLiteral("roomListMiddleClickScroller"));
        QVERIFY(scroller);
        // The first row delegate inside the viewport.
        QQuickItem *row = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                QList<QQuickItem *> items;
                visualTree(presenter.get(), items);
                for (QQuickItem *item : items) {
                    if (!item->property("showGroupDivider").isValid()
                        || item->property("roomId").toString().isEmpty()
                        || !item->isVisible())
                        continue;
                    const QPointF c = item->mapToItem(
                        presenter.get(),
                        QPointF(item->width() / 2, item->height() / 2));
                    if (c.y() > 30 && c.y() < presenter->height() - 10) {
                        row = item;
                        return true;
                    }
                }
                return false;
            }(), kTimeoutMs);
        const QPoint at = row->mapToScene(QPointF(row->width() / 2,
                                                  row->height() / 2))
                              .toPoint();
        QSignalSpy activated(presenter.get(), SIGNAL(roomActivated(QString)));

        QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(scroller->property("latched").toBool(),
                 "a middle click did not latch the room list");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "a click did not end the room list's latch");
        QCOMPARE(activated.count(), 0);

        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QCOMPARE(activated.count(), 1);

        QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY(scroller->property("latched").toBool());
        const QString other =
            controller.currentRoomId() == QStringLiteral("!devs:mock.local")
                ? QStringLiteral("!general:mock.local")
                : QStringLiteral("!devs:mock.local");
        controller.setCurrentRoomId(other);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "a room switch did not end the room list's latch");
    }
};

QTEST_MAIN(MiddleClickScrollerQmlTest)
#include "MiddleClickScrollerQmlTest.moc"
