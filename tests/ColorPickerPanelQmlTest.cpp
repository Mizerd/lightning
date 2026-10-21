// qml/ColorPickerPanel.qml on the real component. Pins:
//   * a host that assigns `selectedColor` directly (Settings' name colour
//     picker) gets a picker that SHOWS it: hex field, HSV and the "before"
//     half all follow, on first open and on every reopen;
//   * the SV square, the hue strip, the close button and the reset button take
//     keyboard focus and are operable from it (arrows move, Enter closes).
// And a source-order guard (documented limit: ImageCropDialog needs the whole
// app to load standalone) that it ignores a horizontal-only wheel.

#include <QAccessible>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <memory>

class ColorPickerPanelQmlTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem *m_panel = nullptr;

    void build()
    {
        m_engine = std::make_unique<QQmlEngine>();
        QQmlComponent c(m_engine.get());
        c.setData(R"(
import QtQuick
import MatrixClient
ColorPickerPanel {
    width: 288
    canReset: true
}
)", QUrl());
        QVERIFY2(c.isReady(), qPrintable(c.errorString()));
        m_panel = qobject_cast<QQuickItem *>(c.create());
        QVERIFY(m_panel);
        m_window = std::make_unique<QQuickWindow>();
        m_window->resize(320, 700);
        m_panel->setParentItem(m_window->contentItem());
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    QQuickItem *item(const char *name) const
    {
        return m_panel->findChild<QQuickItem *>(QString::fromLatin1(name));
    }
    QString hex() const
    {
        return item("colorHexField")->property("text").toString();
    }

private slots:
    void init() { build(); }
    void cleanup()
    {
        m_window.reset();
        m_engine.reset();
        m_panel = nullptr;
    }

    void anExternalAssignmentIsShown()
    {
        m_panel->setProperty("selectedColor", QColor("#B81425"));
        QCOMPARE(hex(), QStringLiteral("#B81425"));
        QVERIFY(m_panel->property("val").toReal() > 0.7);
        QVERIFY(m_panel->property("sat").toReal() > 0.8);
        QCOMPARE(m_panel->property("originalColor").value<QColor>().name(),
                 QStringLiteral("#b81425"));

        // Reopen with another colour: not the previous one.
        m_panel->setProperty("selectedColor", QColor("#115E9A"));
        QCOMPARE(hex(), QStringLiteral("#115E9A"));
        QCOMPARE(m_panel->property("originalColor").value<QColor>().name(),
                 QStringLiteral("#115e9a"));
    }

    void reopenWithTheSameColourRebasesBefore()
    {
        m_panel->setProperty("selectedColor", QColor("#B81425"));
        // Simulate a drag that the host mirrored into its draft.
        QMetaObject::invokeMethod(m_panel, "applyColor",
                                  Q_ARG(QVariant, QColor("#336699")));
        QCOMPARE(m_panel->property("selectedColor").value<QColor>().name(),
                 QStringLiteral("#336699"));
        m_panel->setProperty("visible", false);
        // The host re-assigns the colour the panel already holds.
        m_panel->setProperty("selectedColor", QColor("#336699"));
        m_panel->setProperty("visible", true);
        QCOMPARE(m_panel->property("originalColor").value<QColor>().name(),
                 QStringLiteral("#336699"));
    }

    void theResetButtonIsOneAccessibleButton()
    {
        // Behaviour, not a source count: the reset button itself is the
        // accessible Button carrying the label, and has no accessible child
        // repeating it (the old MouseArea child was the only one named).
        QQuickItem *btn = item("colorResetButton");
        QVERIFY(btn);
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(btn);
        QVERIFY(iface);
        QCOMPARE(iface->role(), QAccessible::Button);
        QCOMPARE(iface->text(QAccessible::Name),
                 m_panel->property("resetLabel").toString());
        for (int i = 0; i < iface->childCount(); ++i) {
            QAccessibleInterface *c = iface->child(i);
            QVERIFY(c);
            QVERIFY2(!(c->role() == QAccessible::Button
                       && c->text(QAccessible::Name)
                              == m_panel->property("resetLabel").toString()),
                     "a nested accessible button repeats the reset button");
        }
    }

    void theSlidersExposeIncreaseAndDecreaseActions()
    {
        m_panel->setProperty("selectedColor", QColor("#808080"));
        QQuickItem *hue = item("colorHueStrip");
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(hue);
        QVERIFY(iface);
        QVERIFY(QAccessibleActionInterface::increaseAction().size() > 0);
        QAccessibleActionInterface *act = iface->actionInterface();
        QVERIFY2(act, "hue strip exposes no accessible actions");
        QVERIFY(act->actionNames().contains(
            QAccessibleActionInterface::increaseAction()));
        const qreal before = m_panel->property("hue").toReal();
        act->doAction(QAccessibleActionInterface::increaseAction());
        QVERIFY(m_panel->property("hue").toReal() > before);
    }

    void theControlsTakeFocusAndKeys()
    {
        m_panel->setProperty("selectedColor", QColor("#808080"));
        for (const char *n : {"colorSvField", "colorHueStrip",
                              "colorPickerCloseButton", "colorResetButton"}) {
            QQuickItem *it = item(n);
            QVERIFY2(it, n);
            QVERIFY2(it->activeFocusOnTab(), n);
        }

        QSignalSpy picked(m_panel, SIGNAL(picked(QColor)));
        QQuickItem *sv = item("colorSvField");
        sv->forceActiveFocus();
        QVERIFY(sv->hasActiveFocus());
        const qreal satBefore = m_panel->property("sat").toReal();
        QTest::keyClick(m_window.get(), Qt::Key_Right);
        QVERIFY(m_panel->property("sat").toReal() > satBefore);
        QVERIFY(picked.count() >= 1);

        QQuickItem *hue = item("colorHueStrip");
        hue->forceActiveFocus();
        const qreal hueBefore = m_panel->property("hue").toReal();
        QTest::keyClick(m_window.get(), Qt::Key_Right);
        QVERIFY(m_panel->property("hue").toReal() > hueBefore);

        QSignalSpy closed(m_panel, SIGNAL(closed()));
        item("colorPickerCloseButton")->forceActiveFocus();
        QTest::keyClick(m_window.get(), Qt::Key_Return);
        QCOMPARE(closed.count(), 1);

        QSignalSpy reset(m_panel, SIGNAL(resetRequested()));
        item("colorResetButton")->forceActiveFocus();
        QTest::keyClick(m_window.get(), Qt::Key_Space);
        QCOMPARE(reset.count(), 1);
    }

    void cropIgnoresAHorizontalOnlyWheel()
    {
        QFile f(QStringLiteral(QML_DIR "/ImageCropDialog.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString src = QString::fromUtf8(f.readAll());
        const int h = src.indexOf(QStringLiteral("onWheel: function (wheel)"));
        QVERIFY(h > 0);
        const int guard = src.indexOf(
            QStringLiteral("wheel.angleDelta.y === 0 && wheel.pixelDelta.y === 0"), h);
        const int zoom = src.indexOf(QStringLiteral("_zoomTo("), h);
        QVERIFY2(guard > h && guard < zoom,
                 "the wheel handler must return on y==0 before it zooms");
    }
};

QTEST_MAIN(ColorPickerPanelQmlTest)
#include "ColorPickerPanelQmlTest.moc"
