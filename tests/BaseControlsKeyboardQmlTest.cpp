
#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 520
    height: 760
    visible: true

    property int btnClicks: 0
    property int iconClicks: 0
    property string segPicked: ""
    property int swToggles: 0
    property int fontPicks: 0
    property string fontPicked: ""
    readonly property string chipName: chip.Accessible.name

    Column {
        x: 20
        y: 20
        spacing: 12
        AppButton { id: btn; objectName: "btn"; text: "Go"; onClicked: win.btnClicks++ }
        IconButton { id: ib; objectName: "ib"; iconName: "close"; onClicked: win.iconClicks++ }
        SegmentedControl {
            objectName: "seg"
            model: ["a", "b"]
            current: "a"
            onActivated: (v) => win.segPicked = v
        }
        AppComboBox {
            id: combo
            objectName: "combo"
            width: 220
            model: { var l = []; for (var i = 0; i < 40; ++i) l.push("Row " + i); return l }
        }
        AppComboBox {
            id: fontCombo
            objectName: "fontCombo"
            width: 220
            filterable: true
            filterPlaceholder: "Search fonts"
            model: ["Noto Sans", "Inter", "Fira Code", "Noto Serif", "DejaVu Sans Mono"]
            onActivated: (i) => { win.fontPicks++; win.fontPicked = currentText }
        }
        AppSwitch { id: sw; objectName: "sw"; onToggled: win.swToggles++ }
        AppTextField { id: tf; objectName: "tf"; width: 220; text: "hello" }
        StatusChip { id: chip; label: "Sent" }
    }
}
)QML";

}

class BaseControlsKeyboardQmlTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    QPoint center(QQuickItem *it) const
    {
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint();
    }

    void type(const QString &text)
    {
        for (const QChar c : text)
            QTest::keyClick(m_window, c.toLatin1());
    }

    void focus(QQuickItem *it)
    {
        it->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(it->hasActiveFocus());
    }

private slots:
    void initTestCase()
    {
        QQmlComponent component(&m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("basekeys.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase() { delete m_root; }

    void returnAndEnterPressAFocusedButton()
    {
        auto *btn = item("btn");
        QVERIFY(btn);
        focus(btn);
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(m_root->property("btnClicks").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_Enter);
        QCOMPARE(m_root->property("btnClicks").toInt(), 2);
        QTest::keyClick(m_window, Qt::Key_Space);
        QTRY_COMPARE(m_root->property("btnClicks").toInt(), 3);
        btn->setProperty("enabled", false);
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(m_root->property("btnClicks").toInt(), 3);
        btn->setProperty("enabled", true);
    }

    void returnAndEnterPressAFocusedIconButton()
    {
        auto *ib = item("ib");
        QVERIFY(ib);
        focus(ib);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTest::keyClick(m_window, Qt::Key_Enter);
        QCOMPARE(m_root->property("iconClicks").toInt(), 2);
    }

    void returnSelectsAFocusedSegment()
    {
        auto *seg = item("seg_b");
        QVERIFY(seg);
        focus(seg);
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(m_root->property("segPicked").toString(), QStringLiteral("b"));
        m_root->setProperty("segPicked", QString());
        QTest::keyClick(m_window, Qt::Key_Enter);
        QCOMPARE(m_root->property("segPicked").toString(), QStringLiteral("b"));
    }

    void returnOpensAClosedComboAndPageKeysMoveTheHighlight()
    {
        auto *combo = item("combo");
        QVERIFY(combo);
        focus(combo);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(combo->property("popup").value<QObject *>()
                        ->property("visible").toBool());
        const int start = combo->property("highlightedIndex").toInt();
        QTest::keyClick(m_window, Qt::Key_PageDown);
        const int down = combo->property("highlightedIndex").toInt();
        QVERIFY2(down >= start + 3,
                 qPrintable(QStringLiteral("PageDown moved %1 -> %2")
                                .arg(start).arg(down)));
        QTest::keyClick(m_window, Qt::Key_PageUp);
        const int up = combo->property("highlightedIndex").toInt();
        QVERIFY2(up < down, qPrintable(QStringLiteral("PageUp %1 -> %2")
                                           .arg(down).arg(up)));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!combo->property("popup").value<QObject *>()
                         ->property("visible").toBool());
        QTRY_VERIFY(combo->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Enter);
        QTRY_VERIFY(combo->property("popup").value<QObject *>()
                        ->property("visible").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!combo->property("popup").value<QObject *>()
                         ->property("visible").toBool());
    }

    void autoRepeatedReturnDoesNotRepeatAClick()
    {
        auto *btn = item("btn");
        auto *ib = item("ib");
        QVERIFY(btn && ib);
        for (QQuickItem *target : {btn, ib}) {
            focus(target);
            const char *prop = target == btn ? "btnClicks" : "iconClicks";
            const int before = m_root->property(prop).toInt();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                            QStringLiteral("\r"), true);
            QCoreApplication::sendEvent(m_window, &press);
            QKeyEvent press2(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier,
                             QString(), true);
            QCoreApplication::sendEvent(m_window, &press2);
            QCOMPARE(m_root->property(prop).toInt(), before);
            QTest::keyClick(m_window, Qt::Key_Return);
            QCOMPARE(m_root->property(prop).toInt(), before + 1);
        }
    }

    void returnChoosesTheHighlightedRowOfAnOpenCombo()
    {
        auto *combo = item("combo");
        QVERIFY(combo);
        QObject *popup = combo->property("popup").value<QObject *>();
        focus(combo);
        combo->setProperty("currentIndex", 0);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(popup->property("visible").toBool());
        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Down);
        const int hi = combo->property("highlightedIndex").toInt();
        QVERIFY(hi > 0);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(combo->property("currentIndex").toInt(), hi);
        combo->setProperty("currentIndex", 0);
    }

    void aReleaseThatNeverArrivesDoesNotEatTheNextChoice()
    {
        auto *combo = item("combo");
        QVERIFY(combo);
        QObject *popup = combo->property("popup").value<QObject *>();
        focus(combo);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                        QStringLiteral("\r"));
        QCoreApplication::sendEvent(m_window, &press);
        QTRY_VERIFY(popup->property("visible").toBool());
        QVERIFY(combo->property("_swallowKeyRelease").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        focus(item("btn"));
        QVERIFY2(!combo->property("_swallowKeyRelease").toBool(),
                 "a stale swallow flag would eat the next row choice");
        focus(combo);
    }

    void typingInAFilterableComboNarrowsAndPicksTheOriginalRow()
    {
        auto *combo = item("fontCombo");
        QVERIFY(combo);
        auto *popup = combo->property("popup").value<QObject *>();
        auto *content = popup->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        auto *field = findItem(content, QStringLiteral("comboFilterField"));
        auto *list = findItem(content, QStringLiteral("comboFilteredList"));
        auto *empty = findItem(content, QStringLiteral("comboFilterEmpty"));
        QVERIFY(field && list && empty);
        const auto open = [&] {
            QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(combo));
            QTRY_VERIFY(popup->property("visible").toBool());
            QTRY_VERIFY(field->hasActiveFocus());
        };

        combo->setProperty("currentIndex", 0);
        open();
        QCOMPARE(list->property("count").toInt(), 5);
        type(QStringLiteral("SERIF"));
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QVERIFY(popup->property("visible").toBool());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(combo->property("currentIndex").toInt(), 3);
        QCOMPARE(m_root->property("fontPicks").toInt(), 1);
        QCOMPARE(m_root->property("fontPicked").toString(), QStringLiteral("Noto Serif"));

        open();
        QCOMPARE(field->property("text").toString(), QString());
        QCOMPARE(list->property("count").toInt(), 5);
        QCOMPARE(list->property("currentIndex").toInt(), 3);
        type(QStringLiteral("zzz"));
        QTRY_COMPARE(list->property("count").toInt(), 0);
        QVERIFY(empty->isVisible());
        QTest::keyClick(m_window, Qt::Key_Return);
        QVERIFY(popup->property("visible").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(combo->property("currentIndex").toInt(), 3);
        QCOMPARE(m_root->property("fontPicks").toInt(), 1);

        open();
        type(QStringLiteral("noto"));
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_Down);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(m_root->property("fontPicked").toString(), QStringLiteral("Noto Serif"));

        open();
        type(QStringLiteral("code"));
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QQuickItem *row = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row),
                                              Q_ARG(int, 0))
                    && row);
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(row));
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(combo->property("currentIndex").toInt(), 2);
        QCOMPARE(m_root->property("fontPicked").toString(), QStringLiteral("Fira Code"));
        QCOMPARE(m_root->property("fontPicks").toInt(), 3);
    }

    void theRowUnderAMovingPointerIsTheOneReturnPicks()
    {
        auto *combo = item("fontCombo");
        QVERIFY(combo);
        auto *popup = combo->property("popup").value<QObject *>();
        auto *content = popup->property("contentItem").value<QQuickItem *>();
        auto *field = findItem(content, QStringLiteral("comboFilterField"));
        auto *list = findItem(content, QStringLiteral("comboFilteredList"));
        QVERIFY(field && list);
        const auto rowAt = [&](int i) {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            return row;
        };

        combo->setProperty("currentIndex", 0);
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(combo));
        QTRY_VERIFY(popup->property("visible").toBool() && field->hasActiveFocus());
        QTRY_VERIFY(rowAt(4));
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::mouseMove(m_window, center(rowAt(4)));
        QTRY_COMPARE(list->property("currentIndex").toInt(), 4);
        QTest::keyClick(m_window, Qt::Key_Up);
        QCOMPARE(list->property("currentIndex").toInt(), 3);

        // The pointer stays over the list while the rows are rebuilt.
        type(QStringLiteral("o"));
        QTRY_COMPARE(list->property("count").toInt(), 4);
        QTest::qWait(50);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QCOMPARE(list->property("currentIndex").toInt(), 3);

        type(QStringLiteral("\b"));
        QTRY_COMPARE(list->property("count").toInt(), 5);
        QTRY_VERIFY(rowAt(1));
        QTest::mouseMove(m_window, center(rowAt(1)));
        QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(combo->property("currentIndex").toInt(), 1);
        QTest::mouseMove(m_window, QPoint(500, 700));
    }

    void clickingASwitchKeepsKeyboardFocus()
    {
        auto *sw = item("sw");
        QVERIFY(sw);
        const int before = m_root->property("swToggles").toInt();
        QTest::mouseClick(m_window, Qt::LeftButton, {}, center(sw));
        QTRY_COMPARE(m_root->property("swToggles").toInt(), before + 1);
        QVERIFY(sw->hasActiveFocus());
        QVERIFY(sw->property("_mouseFocus").toBool());
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(m_root->property("swToggles").toInt(), before + 2);
        QVERIFY(!sw->property("_mouseFocus").toBool());
    }

    void aLabelClickFocusesTheSwitchWithoutTheRing()
    {
        auto *sw = item("sw");
        QVERIFY(sw);
        focus(item("btn"));
        QVERIFY(!sw->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(sw, "focusFromPointer"));
        QTRY_VERIFY(sw->hasActiveFocus());
        QVERIFY(sw->property("_mouseFocus").toBool());
        focus(item("btn"));
    }

    void rightClickOnATextFieldOpensTheThemedEditMenu()
    {
        auto *tf = item("tf");
        QVERIFY(tf);
        QTest::mouseClick(m_window, Qt::RightButton, {}, center(tf));
        QObject *menu = tf->findChild<QObject *>(QStringLiteral("textFieldEditMenu"));
        QVERIFY2(menu, "AppTextField has no themed edit menu");
        QTRY_VERIFY(menu->property("visible").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTRY_VERIFY(tf->hasActiveFocus());
    }

    void aChipExposesItsLabelAsAnAccessibleName()
    {
        QCOMPARE(m_root->property("chipName").toString(), QStringLiteral("Sent"));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    BaseControlsKeyboardQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "BaseControlsKeyboardQmlTest.moc"
