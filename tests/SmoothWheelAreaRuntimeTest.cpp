// v0.7.x: SmoothWheelArea must actually RESOLVE the view it scrolls.
//
// scroll-consistency-contract only scans QML SOURCE for the string
// "SmoothWheelArea", which proves the component was declared and nothing
// else. It cannot see the failure that matters: if `scrollTarget` resolves
// to null, onWheel takes its early return, sets `event.accepted = true`, and
// the pane becomes COMPLETELY unscrollable — the wheel event is swallowed
// and nothing moves. That is worse than not having the component at all,
// and a source scan reports it as covered.
//
// This instantiates the real component inside a real Flickable and asserts
// the resolution, which is the one thing the contract test cannot do.

#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>

class SmoothWheelAreaRuntimeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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
};

QTEST_MAIN(SmoothWheelAreaRuntimeTest)
#include "SmoothWheelAreaRuntimeTest.moc"
