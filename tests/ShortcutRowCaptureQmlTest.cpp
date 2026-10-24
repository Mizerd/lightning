// ShortcutRow.qml loaded for real (through the "MatrixClient" module, with a
// real ShortcutRegistry behind a stub `app`) inside a Flickable whose content
// is taller than its viewport, the shape of Settings. Pins, by behaviour:
//   * C68-F02: a capture ends on a click on blank Flickable space and on a
//     click on an item that accepts the press (a sidebar row); the click still
//     reaches that item; the chord pressed afterwards rebinds nothing;
//   * clicking this row's own Cancel ends the capture and does not re-arm it;
//   * C68-F01: arming a capture, and focusing Change by keyboard, scroll the
//     row into the viewport;
//   * C68-F07: a stored chord hands focus back to the Change button.
#include <QtTest/QtTest>

#include <QSettings>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"

namespace {

class StubApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *shortcuts READ shortcuts CONSTANT)
public:
    explicit StubApp(QObject *s) : m_s(s) {}
    QObject *shortcuts() const { return m_s; }
private:
    QObject *m_s;
};

const char *kHarness = R"QML(
import QtQuick
Item {
    width: 800; height: 300
    property int sidebarPresses: 0
    Flickable {
        id: flick
        objectName: "flick"
        x: 0; y: 0; width: 600; height: 300
        contentWidth: 600; contentHeight: 1700
        clip: true
        Item { objectName: "page"; width: 600; height: 1700 }
    }
    // Stands in for a Settings sidebar row: accepts the press.
    Rectangle {
        objectName: "sidebar"
        x: 620; y: 100; width: 160; height: 40
        MouseArea {
            anchors.fill: parent
            onPressed: (m) => { sidebarPresses++; m.accepted = true }
        }
    }
}
)QML";

} // namespace

class ShortcutRowCaptureQmlTest : public QObject
{
    Q_OBJECT

    QQmlEngine engine;
    QTemporaryDir configDir;
    SettingsManager *settings = nullptr;
    ShortcutRegistry *registry = nullptr;
    StubApp *app = nullptr;
    QQuickWindow window;
    QQuickItem *harness = nullptr;
    QQuickItem *flick = nullptr;
    QQuickItem *row = nullptr;

    QQuickItem *child(const QString &name) const
    {
        return row->findChild<QQuickItem *>(name);
    }
    QPoint sceneCentre(QQuickItem *it) const
    {
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2))
            .toPoint();
    }
    bool inViewport(QQuickItem *it) const
    {
        const QPointF tl = it->mapToItem(flick, QPointF(0, 0));
        return tl.y() >= 0 && tl.y() + it->height() <= flick->height();
    }

private Q_SLOTS:
    void initTestCase()
    {
        // Bindings are stored: a fresh settings root keeps one case's chord
        // (or a previous run's) out of the next.
        QVERIFY(configDir.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           configDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           configDir.path());
    }
    void init()
    {
        QFile::remove(configDir.path() + QStringLiteral("/Unknown Organization/rowtest.conf"));
        settings = new SettingsManager;
        registry = new ShortcutRegistry(settings);
        app = new StubApp(registry);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), app);

        QQmlComponent hc(&engine);
        hc.setData(kHarness, QUrl());
        harness = qobject_cast<QQuickItem *>(hc.create());
        QVERIFY2(harness, qPrintable(hc.errorString()));
        harness->setParentItem(window.contentItem());
        window.resize(800, 300);
        flick = harness->findChild<QQuickItem *>(QStringLiteral("flick"));
        QVERIFY(flick);

        QQmlComponent rc(&engine,
                         QUrl::fromLocalFile(QStringLiteral(QML_DIR "/ShortcutRow.qml")));
        QVERIFY2(rc.status() == QQmlComponent::Ready, qPrintable(rc.errorString()));
        QVariantMap props{ { QStringLiteral("actionId"), QStringLiteral("app.quit") },
                           { QStringLiteral("description"), QStringLiteral("Quit") },
                           { QStringLiteral("currentSequence"), QStringLiteral("Ctrl+Q") },
                           { QStringLiteral("defaultSequence"), QStringLiteral("Ctrl+Q") } };
        row = qobject_cast<QQuickItem *>(rc.createWithInitialProperties(props));
        QVERIFY2(row, qPrintable(rc.errorString()));
        // Below the fold of the 300 px viewport.
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        row->setParentItem(content);
        row->setPosition(QPointF(20, 1400));
        row->setWidth(560);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(child(QStringLiteral("shortcutChange_app.quit")));
    }
    void cleanup()
    {
        window.hide();
        delete row; row = nullptr;
        delete harness; harness = nullptr;
        delete app; delete registry; delete settings;
        app = nullptr; registry = nullptr; settings = nullptr;
    }

    // C68-F01.
    void armingACaptureScrollsTheChipIntoView()
    {
        QCOMPARE(flick->property("contentY").toDouble(), 0.0);
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QVERIFY(row->property("capturing").toBool());
        QVERIFY(flick->property("contentY").toDouble() > 0);
        QVERIFY(inViewport(child(QStringLiteral("shortcutChip_app.quit"))));
    }
    void keyboardFocusOnChangeScrollsItIntoView()
    {
        QQuickItem *change = child(QStringLiteral("shortcutChange_app.quit"));
        QVERIFY(!inViewport(change));
        change->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(inViewport(change));
    }

    // C68-F02.
    void aClickOnBlankFlickableSpaceEndsTheCapture()
    {
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QVERIFY(row->property("capturing").toBool());
        // Blank page space in the viewport, away from the row's controls.
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(30, 20));
        QVERIFY(!row->property("capturing").toBool());
        QTest::keyClick(&window, Qt::Key_7, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(registry->sequenceFor(QStringLiteral("app.quit")),
                 QStringLiteral("Ctrl+Q"));
    }
    void dialogKeysCanBeCapturedWithoutAConflictWarning()
    {
        const QList<QPair<QString, Qt::Key>> actions = {
            { QStringLiteral("dialog.previousItem"), Qt::Key_K },
            { QStringLiteral("dialog.nextItem"), Qt::Key_J },
            { QStringLiteral("dialog.close"), Qt::Key_C },
        };
        for (const auto &action : actions) {
            row->setProperty("actionId", action.first);
            QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
            QTest::keyClick(&window, action.second, Qt::ControlModifier);
            QVERIFY(!row->property("capturing").toBool());
            QVERIFY(row->property("captureError").toString().isEmpty());
            QCOMPARE(registry->sequenceFor(action.first),
                     registry->defaultSequenceFor(action.first));
        }
        QCOMPARE(registry->conflictCount(), 0);
    }

    void aClickOnAPressAcceptingItemEndsTheCaptureAndStillReachesIt()
    {
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QVERIFY(row->property("capturing").toBool());
        QQuickItem *sidebar = harness->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QTest::mouseClick(&window, Qt::LeftButton, {}, sceneCentre(sidebar));
        QVERIFY(!row->property("capturing").toBool());
        QCOMPARE(harness->property("sidebarPresses").toInt(), 1);
        QTest::keyClick(&window, Qt::Key_7, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(registry->sequenceFor(QStringLiteral("app.quit")),
                 QStringLiteral("Ctrl+Q"));
    }
    void clickingCancelEndsTheCaptureAndDoesNotRearmIt()
    {
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QQuickItem *change = child(QStringLiteral("shortcutChange_app.quit"));
        QTRY_VERIFY(inViewport(change));
        QTest::mouseClick(&window, Qt::LeftButton, {}, sceneCentre(change));
        QVERIFY(!row->property("capturing").toBool());
    }
    void aChordWhileCapturingStillRebinds()
    {
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QTest::keyClick(&window, Qt::Key_7, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(!row->property("capturing").toBool());
        QCOMPARE(registry->sequenceFor(QStringLiteral("app.quit")),
                 QStringLiteral("Ctrl+Alt+7"));
        // C68-F07: focus returns to Change, not to the zero-size sink.
        QVERIFY(child(QStringLiteral("shortcutChange_app.quit"))->hasActiveFocus());
    }
    void escapeEndsTheCaptureAndFocusesChange()
    {
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QTest::keyClick(&window, Qt::Key_Escape);
        QVERIFY(!row->property("capturing").toBool());
        QVERIFY(child(QStringLiteral("shortcutChange_app.quit"))->hasActiveFocus());
    }
};

QTEST_MAIN(ShortcutRowCaptureQmlTest)
#include "ShortcutRowCaptureQmlTest.moc"
