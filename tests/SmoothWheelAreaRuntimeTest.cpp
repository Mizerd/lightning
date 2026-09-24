// SmoothWheelArea must actually resolve the view it scrolls. If
// `scrollTarget` resolves to null, onWheel accepts and drops every wheel
// event and the pane cannot scroll at all, which a source scan cannot see.
// This instantiates the real component inside a real Flickable.

#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include "app/SettingsManager.h"
#include "models/TimelineScrollController.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWheelEvent>


// QML resolves `app.settings` through the meta-object, so a dynamic property
// set with QObject::setProperty() would be invisible. This holder declares the
// property and hands over the real SettingsManager.
class AppStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    // The component asks `app.timelineScroll` for the per-notch distance and
    // falls back to a flat 120 without it; provide it so the real tuning is
    // measured.
    Q_PROPERTY(QObject *timelineScroll READ timelineScroll CONSTANT)
public:
    explicit AppStub(SettingsManager *s, TimelineScrollController *c,
                     QObject *parent = nullptr)
        : QObject(parent), m_settings(s), m_scroll(c) {}
    QObject *settings() const { return m_settings; }
    QObject *timelineScroll() const { return m_scroll; }
private:
    SettingsManager *m_settings = nullptr;
    TimelineScrollController *m_scroll = nullptr;
};

class SmoothWheelAreaRuntimeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // SettingsManager persists, so isolate it from the developer's own
    // configuration and from values an earlier run left behind.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    }

    // Declared as a direct child of a Flickable, `parent as Flickable` must
    // give back that Flickable. Qt routes declared children of a Flickable
    // through flickableData, so this is exactly the assumption worth pinning.
    void resolvesTheEnclosingFlickable()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    objectName: "flick"
    width: 200; height: 200
    contentWidth: 200; contentHeight: 2000
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inline.qml")));

        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);

        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY2(wheel != nullptr, "SmoothWheelArea was not instantiated");

        QObject *target =
            wheel->property("scrollTarget").value<QObject *>();
        QVERIFY2(target != nullptr,
                 "scrollTarget resolved to null — onWheel would swallow every "
                 "wheel event and the pane would not scroll at all");
        QCOMPARE(target, root.get());
    }

    // The bounds must reflect the view, or a glide clamps to nothing and the
    // pane appears frozen even with a correct target.
    void reportsTheScrollableRange()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 200
    contentWidth: 200; contentHeight: 2000
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inline2.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);

        QCOMPARE(wheel->property("minContentY").toReal(), 0.0);
        // contentHeight - height.
        QCOMPARE(wheel->property("maxContentY").toReal(), 1800.0);
    }

    // Content shorter than the viewport must clamp to zero rather than
    // producing a negative range.
    void shortContentClampsToZeroRange()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 500
    contentWidth: 200; contentHeight: 100
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inline3.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);
        QCOMPARE(wheel->property("maxContentY").toReal(), 0.0);
    }

    // ---- Horizontal axis ------------------------------------------------
    //
    // On a horizontal view maxContentY is 0, so a vertical-only component
    // would accept and drop every wheel event.

    void anExplicitHorizontalAxisMeasuresTheHorizontalRange()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
ListView {
    width: 200; height: 40
    orientation: ListView.Horizontal
    model: 40
    delegate: Item { width: 50; height: 40 }
    SmoothWheelArea { objectName: "wheel"; axis: "horizontal" }
}
)", QUrl(QStringLiteral("qrc:/inlineH1.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);
        QVERIFY(wheel->property("scrollTarget").value<QObject *>() != nullptr);
        QVERIFY(wheel->property("horizontal").toBool());
        // 40 delegates of 50 = 2000 wide, viewport 200.
        QCOMPARE(wheel->property("maxContentY").toReal(), 1800.0);
        QCOMPARE(wheel->property("viewportExtent").toReal(), 200.0);

        // And the write path really moves the HORIZONTAL axis. Without this
        // the range could be right while every write still went to contentY,
        // which is the failure the whole case exists to rule out.
        QVERIFY(QMetaObject::invokeMethod(wheel, "setScrollPosition",
                                          Q_ARG(QVariant, QVariant(120.0))));
        QCOMPARE(root->property("contentX").toReal(), 120.0);
        QCOMPARE(root->property("contentY").toReal(), 0.0);
        QVariant position;
        QVERIFY(QMetaObject::invokeMethod(wheel, "scrollPosition",
                                          Q_RETURN_ARG(QVariant, position)));
        QCOMPARE(position.toReal(), 120.0);
    }

    void autoPicksHorizontalOnlyWhenTheVerticalAxisHasNoOverflow()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        // Overflows BOTH ways: the vertical axis wins, so every existing
        // caller is untouched by the new default.
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 200
    contentWidth: 2000; contentHeight: 2000
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inlineH2.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> both(component.create());
        QVERIFY(both != nullptr);
        QObject *wheel = both->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);
        QVERIFY(!wheel->property("horizontal").toBool());
        QCOMPARE(wheel->property("maxContentY").toReal(), 1800.0);

        // Overflows horizontally only: previously the wheel event was accepted
        // and ignored there, so answering it breaks nobody.
        QQmlComponent wide(&engine);
        wide.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 200
    contentWidth: 900; contentHeight: 200
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inlineH3.qml")));
        QVERIFY2(wide.errors().isEmpty(), qPrintable(wide.errorString()));
        std::unique_ptr<QObject> wideRoot(wide.create());
        QVERIFY(wideRoot != nullptr);
        QObject *wideWheel =
            wideRoot->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wideWheel != nullptr);
        QVERIFY(wideWheel->property("horizontal").toBool());
        QCOMPARE(wideWheel->property("maxContentY").toReal(), 700.0);
    }

    void aVerticalViewStillWritesTheVerticalAxis()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 200
    contentWidth: 200; contentHeight: 2000
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inlineH4.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);
        QVERIFY(!wheel->property("horizontal").toBool());
        QVERIFY(QMetaObject::invokeMethod(wheel, "setScrollPosition",
                                          Q_ARG(QVariant, QVariant(300.0))));
        QCOMPARE(root->property("contentY").toReal(), 300.0);
        QCOMPARE(root->property("contentX").toReal(), 0.0);
    }

    // Smooth scrolling off must land the notch, not glide it, and travel the
    // same distance. Driven through the real `app.settings.smoothScrolling`
    // property via a stub context object.
    static qreal notchDistanceFor(QQuickItem *view)
    {
        // ScrollTuning's stateless per-notch distance is what BOTH paths use;
        // reading it here rather than hardcoding a number keeps the test true
        // if the tuning changes.
        TimelineScrollController controller;
        return controller.notchDistance(view->height());
    }

    void turningOffSmoothScrollingLandsTheNotchImmediately()
    {
        QQmlEngine engine;
        SettingsManager settings;
        // Default must be ON — every build so far has glided, and an absent
        // key must not silently change how the wheel feels.
        QVERIFY2(settings.smoothScrolling(),
                 "smooth scrolling must default to on");
        settings.setSmoothScrolling(false);
        TimelineScrollController scroll;
        AppStub app(&settings, &scroll);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);

        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 100
    contentWidth: 200; contentHeight: 4000
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inlineNoSmooth.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        auto *view = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(view != nullptr);

        QQuickWindow window;
        window.resize(200, 100);
        view->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);
        QVERIFY2(!wheel->property("smoothScrollingEnabled").toBool(),
                 "the component did not read the setting at all");
        QCOMPARE(root->property("contentY").toReal(), 0.0);

        // A DISCRETE notch — the branch that normally glides. With the
        // setting off the position must move on this very event, before any
        // ticker could have run.
        const QPointF pos(100.0, 50.0);
        QWheelEvent notch(pos, window.mapToGlobal(pos.toPoint()), QPoint(0, 0),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &notch);
        QCoreApplication::processEvents();

        const qreal landed = root->property("contentY").toReal();
        QVERIFY2(landed > 0.0,
                 "the notch did not land synchronously — it is still gliding");
        // The DISTANCE must be the notch distance, not some other number:
        // turning off an animation is not a request to scroll a different
        // amount. 120 units is exactly one notch by Qt's own convention.
        QVERIFY2(qAbs(landed - notchDistanceFor(view)) < 1.0,
                 qPrintable(QStringLiteral("landed %1, expected one notch %2")
                                .arg(landed).arg(notchDistanceFor(view))));
        // And no glide may be left running behind it.
        QCOMPARE(wheel->property("glideDirection").toInt(), 0);
    }

    // A touchpad frame with no whole pixel is not a wheel notch. Qt Wayland
    // rounds finger-scroll frames to whole pixels and also sends angleDelta,
    // so a slow swipe arrives as `pixelDelta 0, angleDelta ±1` frames with a
    // scroll phase. The phase tells a touchpad from a wheel (a wheel is always
    // NoScrollPhase).
    void aPhasedTouchpadFrameWithNoWholePixelNeverGlides()
    {
        QQmlEngine engine;
        SettingsManager settings;
        // SET it, never assume the default: the case above turns smooth
        // scrolling OFF through QSettings, which persists across cases.
        settings.setSmoothScrolling(true); // the glide path is the one at risk
        TimelineScrollController scroll;
        AppStub app(&settings, &scroll);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);

        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Flickable {
    width: 200; height: 100
    contentWidth: 200; contentHeight: 4000
    SmoothWheelArea { objectName: "wheel" }
}
)", QUrl(QStringLiteral("qrc:/inlinePhased.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        auto *view = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(view != nullptr);
        QQuickWindow window;
        window.resize(200, 100);
        view->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();
        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);

        const QPointF pos(100.0, 50.0);
        for (int i = 0; i < 8; ++i) {
            QWheelEvent frame(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, -1), Qt::NoButton,
                              Qt::NoModifier,
                              i == 0 ? Qt::ScrollBegin : Qt::ScrollUpdate,
                              false);
            QCoreApplication::sendEvent(&window, &frame);
        }
        QCoreApplication::processEvents();
        QCOMPARE(wheel->property("glideDirection").toInt(), 0);
        QTest::qWait(150); // long enough for any glide to have moved
        QCOMPARE(root->property("contentY").toReal(), 0.0);

        // CONTROL: the same component still glides for a real wheel notch,
        // so the assertions above cannot pass by scrolling being disabled.
        QWheelEvent notch(pos, window.mapToGlobal(pos.toPoint()), QPoint(0, 0),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &notch);
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(root->property("contentY").toReal() > 0.0,
                                 2000);
    }

    // A mouse reports its one wheel on the Y axis, so a horizontal strip must
    // answer a vertical wheel. A real QWheelEvent into a real window; the
    // horizontal axis must move.
    void aVerticalWheelEventScrollsAHorizontalStrip()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
ListView {
    width: 200; height: 40
    orientation: ListView.Horizontal
    model: 40
    delegate: Item { width: 50; height: 40 }
    SmoothWheelArea { objectName: "wheel"; axis: "horizontal" }
}
)", QUrl(QStringLiteral("qrc:/inlineH5.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root != nullptr);
        auto *view = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(view != nullptr);

        QQuickWindow window;
        window.resize(200, 40);
        view->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        QObject *wheel = root->findChild<QObject *>(QStringLiteral("wheel"));
        QVERIFY(wheel != nullptr);
        QVERIFY(wheel->property("horizontal").toBool());
        QCOMPARE(root->property("contentX").toReal(), 0.0);

        // The pixel branch writes the position synchronously, so it can be
        // asserted without waiting on the glide. pixelDelta is the third
        // constructor argument; angleDelta the fourth.
        const QPointF pos(100.0, 20.0);
        QWheelEvent pixelWheel(pos, window.mapToGlobal(pos.toPoint()),
                               QPoint(0, -120), QPoint(0, 0), Qt::NoButton,
                               Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &pixelWheel);
        QCoreApplication::processEvents();

        // Scrolling "down" on a horizontal strip moves it RIGHT, and the
        // vertical axis must not have moved at all.
        QCOMPARE(root->property("contentX").toReal(), 120.0);
        QCOMPARE(root->property("contentY").toReal(), 0.0);

        // The notch branch glides, but sets its target synchronously; a
        // notch the handler ignored would leave the target where the pixel
        // branch left it.
        QWheelEvent notch(pos, window.mapToGlobal(pos.toPoint()), QPoint(0, 0),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &notch);
        QCoreApplication::processEvents();
        QCOMPARE(wheel->property("glideDirection").toInt(), 1);
        QVERIFY(wheel->property("glideTargetY").toReal() > 120.0);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(SmoothWheelAreaRuntimeTest)
#include "SmoothWheelAreaRuntimeTest.moc"
