
#include <QtTest/QtTest>

#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/MouseNavigationFilter.h"

using lightning::MouseNavigationFilter;

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    width: 600
    height: 400
    visible: true

    TextArea {
        objectName: "area"
        x: 10; y: 10; width: 280; height: 120
        text: "a message being written"
        font.pixelSize: 16
        selectByMouse: true
    }
    TextField {
        objectName: "field"
        x: 10; y: 150; width: 280; height: 40
        text: "general"
    }
    MouseArea {
        objectName: "swallower"
        x: 300; y: 10; width: 280; height: 120
        acceptedButtons: Qt.AllButtons
        property int presses: 0
        onPressed: presses++
    }
    // An AppMenu is a Menu: non-modal, closes on a press outside.
    Menu {
        objectName: "menu"
        MenuItem { text: "One" }
        MenuItem { text: "Two" }
    }
    Popup {
        objectName: "picker"
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: 20; y: 220; width: 100; height: 50
    }
    Popup {
        objectName: "dialog"
        modal: true
        x: 100; y: 100; width: 200; height: 100
    }
}
)QML";

const QPoint kEmpty(500, 350);

}

class MouseNavigationFilterTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    MouseNavigationFilter *m_filter = nullptr;
    int m_backs = 0;
    int m_forwards = 0;
    bool m_allowed = true;
    QList<bool> m_gateAnswers;
    int m_gateAsked = 0;

    QQuickItem *item(const char *name) const
    {
        QQuickItem *found =
            m_window->findChild<QQuickItem *>(QLatin1String(name));
        if (!found)
            qFatal("no item %s in the scene", name);
        return found;
    }
    QObject *popup(const char *name) const
    {
        return m_window->findChild<QObject *>(QLatin1String(name));
    }
    QPoint centerOf(QQuickItem *it) const
    {
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2))
            .toPoint();
    }
    void click(Qt::MouseButton button, const QPoint &at)
    {
        QTest::mouseClick(m_window, button, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    }
    void openPopup(const char *name)
    {
        QObject *p = popup(name);
        QVERIFY2(p, name);
        QVERIFY(QMetaObject::invokeMethod(p, "open"));
        QTRY_VERIFY(p->property("opened").toBool());
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_engine = new QQmlEngine(this);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("mousenavigationfilter.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_filter = new MouseNavigationFilter(
            m_window,
            [this] {
                ++m_gateAsked;
                return m_gateAnswers.isEmpty() ? m_allowed
                                               : m_gateAnswers.takeFirst();
            },
            [this](MouseNavigationFilter::Direction d) {
                if (d == MouseNavigationFilter::Direction::Back)
                    ++m_backs;
                else if (d == MouseNavigationFilter::Direction::Forward)
                    ++m_forwards;
                return true;
            },
            this);
    }

    void cleanupTestCase()
    {
        delete m_filter;
        delete m_window;
    }

    void init()
    {
        for (const char *name : { "menu", "picker", "dialog" }) {
            QObject *p = popup(name);
            QVERIFY(p);
            QMetaObject::invokeMethod(p, "close");
            QTRY_VERIFY(!p->property("visible").toBool());
        }
        QVERIFY(!MouseNavigationFilter::windowHasOpenPopup(m_window));
        m_backs = 0;
        m_forwards = 0;
        m_allowed = true;
        m_gateAnswers.clear();
        m_gateAsked = 0;
    }

    void theButtonMapping()
    {
        using D = MouseNavigationFilter::Direction;
        QCOMPARE(MouseNavigationFilter::directionFor(Qt::BackButton), D::Back);
        QCOMPARE(MouseNavigationFilter::directionFor(Qt::ExtraButton1), D::Back);
        QCOMPARE(MouseNavigationFilter::directionFor(Qt::ExtraButton4), D::Back);
        QCOMPARE(MouseNavigationFilter::directionFor(Qt::ForwardButton),
                 D::Forward);
        QCOMPARE(MouseNavigationFilter::directionFor(Qt::ExtraButton2),
                 D::Forward);
        QCOMPARE(MouseNavigationFilter::directionFor(Qt::ExtraButton3),
                 D::Forward);
        for (Qt::MouseButton other :
             { Qt::LeftButton, Qt::RightButton, Qt::MiddleButton,
               Qt::ExtraButton5, Qt::ExtraButton6, Qt::NoButton })
            QCOMPARE(MouseNavigationFilter::directionFor(other), D::None);
    }

    void backAndForwardNavigate_data()
    {
        QTest::addColumn<QString>("where");
        QTest::newRow("empty area") << QString();
        QTest::newRow("over a TextArea") << QStringLiteral("area");
        QTest::newRow("over a TextField") << QStringLiteral("field");
        QTest::newRow("over an AllButtons MouseArea")
            << QStringLiteral("swallower");
    }
    void backAndForwardNavigate()
    {
        QFETCH(QString, where);
        const QPoint at = where.isEmpty()
            ? kEmpty
            : centerOf(item(where.toLatin1().constData()));
        click(Qt::BackButton, at);
        QCOMPARE(m_backs, 1);
        QCOMPARE(m_forwards, 0);
        click(Qt::ForwardButton, at);
        QCOMPARE(m_forwards, 1);
        QCOMPARE(m_backs, 1);
    }

    void theKernelBackAndForwardCodesNavigate()
    {
        click(Qt::ExtraButton4, kEmpty);
        QCOMPARE(m_backs, 1);
        click(Qt::ExtraButton3, kEmpty);
        QCOMPARE(m_forwards, 1);
    }

    void otherButtonsDoNotNavigate()
    {
        for (Qt::MouseButton b : { Qt::LeftButton, Qt::RightButton,
                                   Qt::MiddleButton, Qt::ExtraButton5 })
            click(b, kEmpty);
        QCOMPARE(m_backs, 0);
        QCOMPARE(m_forwards, 0);
    }

    void theItemBeneathStillGetsThePress()
    {
        QQuickItem *swallower = item("swallower");
        const int before = swallower->property("presses").toInt();
        click(Qt::BackButton, centerOf(swallower));
        QCOMPARE(swallower->property("presses").toInt(), before + 1);
        QCOMPARE(m_backs, 1);
    }

    void aTextAreaKeepsItsFocusAndCaret()
    {
        QQuickItem *area = item("area");
        click(Qt::LeftButton, area->mapToScene(QPointF(5, 5)).toPoint());
        QVERIFY(area->hasActiveFocus());
        QQuickItem *focused = m_window->activeFocusItem();
        QVERIFY(area->setProperty("cursorPosition", 2));
        click(Qt::BackButton, centerOf(area));
        QCOMPARE(m_backs, 1);
        QCOMPARE(m_window->activeFocusItem(), focused);
        QCOMPARE(area->property("cursorPosition").toInt(), 2);
        QCOMPARE(area->property("selectedText").toString(), QString());
    }

    void aLeftClickStillMovesTheCaret()
    {
        QQuickItem *area = item("area");
        QVERIFY(area->setProperty("cursorPosition", 0));
        const int length = area->property("length").toInt();
        QRectF end;
        QVERIFY(QMetaObject::invokeMethod(area, "positionToRectangle",
                                          Q_RETURN_ARG(QRectF, end),
                                          Q_ARG(int, length)));
        click(Qt::LeftButton,
              area->mapToScene(QPointF(end.x() + 2, end.center().y()))
                  .toPoint());
        QVERIFY(area->hasActiveFocus());
        QCOMPARE(area->property("cursorPosition").toInt(), length);
        QCOMPARE(m_backs, 0);
    }

    void aPressThatDismissesAPopupOnlyDismissesIt_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("button");
        QTest::newRow("click-away Menu, Back") << QStringLiteral("menu")
                                               << int(Qt::BackButton);
        QTest::newRow("click-away Menu, Forward") << QStringLiteral("menu")
                                                  << int(Qt::ForwardButton);
        QTest::newRow("click-away Menu, BTN_BACK") << QStringLiteral("menu")
                                                   << int(Qt::ExtraButton4);
        QTest::newRow("non-modal picker, Back") << QStringLiteral("picker")
                                                << int(Qt::BackButton);
        QTest::newRow("modal dialog, Back") << QStringLiteral("dialog")
                                            << int(Qt::BackButton);
    }
    void aPressThatDismissesAPopupOnlyDismissesIt()
    {
        QFETCH(QString, name);
        QFETCH(int, button);
        const QByteArray n = name.toLatin1();
        openPopup(n.constData());
        QVERIFY(MouseNavigationFilter::windowHasOpenPopup(m_window));
        click(Qt::MouseButton(button), kEmpty);
        QTRY_VERIFY(!popup(n.constData())->property("visible").toBool());
        QCOMPARE(m_backs + m_forwards, 0);
        click(Qt::MouseButton(button), kEmpty);
        QCOMPARE(m_backs + m_forwards, 1);
    }

    void aDragIsNotATap()
    {
        QTest::mousePress(m_window, Qt::BackButton, Qt::NoModifier, kEmpty);
        QTest::mouseMove(m_window, kEmpty + QPoint(60, 30));
        QTest::mouseRelease(m_window, Qt::BackButton, Qt::NoModifier,
                            kEmpty + QPoint(60, 30));
        QCoreApplication::processEvents();
        QCOMPARE(m_backs, 0);
    }

    void theGateIsReadOnThePress()
    {
        m_allowed = false;
        QTest::mousePress(m_window, Qt::BackButton, Qt::NoModifier, kEmpty);
        m_allowed = true;
        QTest::mouseRelease(m_window, Qt::BackButton, Qt::NoModifier, kEmpty);
        QCoreApplication::processEvents();
        QCOMPARE(m_backs, 0);
        click(Qt::BackButton, kEmpty);
        QCOMPARE(m_backs, 1);
    }

    void theGateIsReadAgainWhenTheNavigationRuns()
    {
        m_gateAnswers = { true, false };
        click(Qt::BackButton, kEmpty);
        QCOMPARE(m_gateAsked, 2);
        QCOMPARE(m_backs, 0);
        click(Qt::BackButton, kEmpty);
        QCOMPARE(m_backs, 1);
    }

    void onlyAMouseOrTouchpadDeviceNavigates_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("expected");
        QTest::newRow("mouse (control)") << 0 << 1;
        QTest::newRow("touchscreen") << 1 << 0;
        QTest::newRow("stylus") << 2 << 0;
        QTest::newRow("touchpad") << 3 << 1;
    }
    void onlyAMouseOrTouchpadDeviceNavigates()
    {
        QFETCH(int, kind);
        QFETCH(int, expected);
        static QPointingDevice stylus(
            QStringLiteral("fx6r test stylus"), 0x5f6a1,
            QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position, 1, 3);
        static QPointingDevice touchpad(
            QStringLiteral("fx6r test touchpad"), 0x5f6a2,
            QInputDevice::DeviceType::TouchPad,
            QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position, 1, 3);
        static QPointingDevice *touchscreen = QTest::createTouchDevice();
        const QPointingDevice *device =
            kind == 1 ? touchscreen
            : kind == 2 ? &stylus
            : kind == 3 ? &touchpad
                        : QPointingDevice::primaryPointingDevice();
        QVERIFY(device);
        if (kind == 0)
            QCOMPARE(device->type(), QInputDevice::DeviceType::Mouse);
        const QPointF at(kEmpty);
        const QPointF global = m_window->mapToGlobal(at);
        QMouseEvent press(QEvent::MouseButtonPress, at, at, global,
                          Qt::BackButton, Qt::BackButton, Qt::NoModifier,
                          device);
        QCoreApplication::sendEvent(m_window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, at, global,
                            Qt::BackButton, Qt::NoButton, Qt::NoModifier,
                            device);
        QCoreApplication::sendEvent(m_window, &release);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(m_backs, expected);
    }

    void aChordIsNotATap()
    {
        QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, kEmpty);
        QTest::mousePress(m_window, Qt::BackButton, Qt::NoModifier, kEmpty);
        QTest::mouseRelease(m_window, Qt::BackButton, Qt::NoModifier, kEmpty);
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, kEmpty);
        QCoreApplication::processEvents();
        QCOMPARE(m_backs, 0);

        QTest::mousePress(m_window, Qt::ForwardButton, Qt::NoModifier, kEmpty);
        QTest::mousePress(m_window, Qt::RightButton, Qt::NoModifier, kEmpty);
        QTest::mouseRelease(m_window, Qt::RightButton, Qt::NoModifier, kEmpty);
        QTest::mouseRelease(m_window, Qt::ForwardButton, Qt::NoModifier,
                            kEmpty);
        QCoreApplication::processEvents();
        QCOMPARE(m_forwards, 0);

        click(Qt::BackButton, kEmpty);
        QCOMPARE(m_backs, 1);
    }

    void aFingerTapDoesNotNavigate()
    {
        QPointingDevice *touch = QTest::createTouchDevice();
        QTest::touchEvent(m_window, touch).press(0, kEmpty);
        QTest::touchEvent(m_window, touch).release(0, kEmpty);
        QCoreApplication::processEvents();
        QCOMPARE(m_backs, 0);
        QCOMPARE(m_forwards, 0);
    }
};

QTEST_MAIN(MouseNavigationFilterTest)
#include "MouseNavigationFilterTest.moc"
