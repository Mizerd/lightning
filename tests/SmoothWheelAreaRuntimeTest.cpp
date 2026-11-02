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

class AppStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
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
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    }

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
        QCOMPARE(wheel->property("maxContentY").toReal(), 1800.0);
    }

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
        QCOMPARE(wheel->property("maxContentY").toReal(), 1800.0);
        QCOMPARE(wheel->property("viewportExtent").toReal(), 200.0);

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

    static qreal notchDistanceFor(QQuickItem *view)
    {
        TimelineScrollController controller;
        return controller.notchDistance(view->height());
    }

    void turningOffSmoothScrollingLandsTheNotchImmediately()
    {
        QQmlEngine engine;
        SettingsManager settings;
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

        const QPointF pos(100.0, 50.0);
        QWheelEvent notch(pos, window.mapToGlobal(pos.toPoint()), QPoint(0, 0),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &notch);
        QCoreApplication::processEvents();

        const qreal landed = root->property("contentY").toReal();
        QVERIFY2(landed > 0.0,
                 "the notch did not land synchronously — it is still gliding");
        QVERIFY2(qAbs(landed - notchDistanceFor(view)) < 1.0,
                 qPrintable(QStringLiteral("landed %1, expected one notch %2")
                                .arg(landed).arg(notchDistanceFor(view))));
        QCOMPARE(wheel->property("glideDirection").toInt(), 0);
    }

    void aPhasedTouchpadFrameWithNoWholePixelNeverGlides()
    {
        QQmlEngine engine;
        SettingsManager settings;
        settings.setSmoothScrolling(true);
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
        QTest::qWait(150);
        QCOMPARE(root->property("contentY").toReal(), 0.0);

        QWheelEvent notch(pos, window.mapToGlobal(pos.toPoint()), QPoint(0, 0),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &notch);
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(root->property("contentY").toReal() > 0.0,
                                 2000);
    }

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

        const QPointF pos(100.0, 20.0);
        QWheelEvent pixelWheel(pos, window.mapToGlobal(pos.toPoint()),
                               QPoint(0, -120), QPoint(0, 0), Qt::NoButton,
                               Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &pixelWheel);
        QCoreApplication::processEvents();

        QCOMPARE(root->property("contentX").toReal(), 120.0);
        QCOMPARE(root->property("contentY").toReal(), 0.0);

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
