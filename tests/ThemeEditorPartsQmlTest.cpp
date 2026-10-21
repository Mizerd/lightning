// Keyboard and focus behaviour of the theme editor's own parts (C65 round 1):
// the level switch's roving focus, the preset prompt taking and returning
// focus, the popover releasing focus on close, Return on the link buttons, the
// toast timer pausing, the Simple panel following focus, and the theme list
// revealing the current row. Each part is loaded standalone from the shipped
// MatrixClient module and driven with real key events on a shown window.

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

#include <memory>

namespace {

struct Host
{
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QObject> root;

    bool load(const QString &body, QString *error)
    {
        QQmlComponent c(&engine);
        c.setData((QStringLiteral("import QtQuick\nimport QtQuick.Controls\n"
                                  "import MatrixClient\n"
                                  "Item { width: 600; height: 500\n")
                   + body + QStringLiteral("\n}")).toUtf8(),
                  QUrl(QStringLiteral("themePartsProbe.qml")));
        root.reset(c.create());
        if (!root) {
            *error = c.errorString();
            return false;
        }
        auto *item = qobject_cast<QQuickItem *>(root.get());
        item->setParentItem(window.contentItem());
        window.resize(600, 500);
        window.show();
        window.requestActivate();
        return QTest::qWaitForWindowActive(&window);
    }
    // Repeater/ListView delegates are not QObject children of the item they
    // sit in, so the search walks the visual tree as well.
    static QQuickItem *findVisual(QQuickItem *from, const QString &name)
    {
        if (from->objectName() == name)
            return from;
        const auto kids = from->childItems();
        for (QQuickItem *k : kids)
            if (QQuickItem *f = findVisual(k, name))
                return f;
        return nullptr;
    }
    QQuickItem *item(const char *name) const
    {
        const QString n = QString::fromLatin1(name);
        QQuickItem *found = root->findChild<QQuickItem *>(n);
        if (!found)
            found = findVisual(qobject_cast<QQuickItem *>(root.get()), n);
        if (!found)
            qWarning("themeParts: no item named %s", name);
        return found;
    }
};

} // namespace

class ThemeEditorPartsQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void levelSwitchFocusFollowsSelectionWithOneTabStop()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            ThemeLevelSwitch { id: ls; level: 0
                onLevelRequested: (l) => ls.level = l }
        )QML", &err), qPrintable(err));
        QQuickItem *sw = h.item("themeLevelSwitch");
        // ONE tab stop for the group, and no segment is a stop of its own.
        QVERIFY(sw->activeFocusOnTab());
        for (const char *seg : {"themeLevel_0", "themeLevel_1", "themeLevel_2"})
            QVERIFY(!h.item(seg)->activeFocusOnTab());
        sw->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(sw->hasActiveFocus());
        auto level = [&] { return sw->property("level").toInt(); };
        QTest::keyClick(&h.window, Qt::Key_Right);
        QCOMPARE(level(), 1);
        QTest::keyClick(&h.window, Qt::Key_Right);
        QCOMPARE(level(), 2);
        QTest::keyClick(&h.window, Qt::Key_Right);   // clamps, no wrap
        QCOMPARE(level(), 2);
        QTest::keyClick(&h.window, Qt::Key_Left);
        QCOMPARE(level(), 1);
        QTest::keyClick(&h.window, Qt::Key_Home);
        QCOMPARE(level(), 0);
        QTest::keyClick(&h.window, Qt::Key_End);
        QCOMPARE(level(), 2);
        QVERIFY(sw->hasActiveFocus());   // focus stays: the ring follows the level
    }

    void clickingASegmentFocusesTheSwitchSoArrowsWork()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            Column { TextField { objectName: "other" }
              ThemeLevelSwitch { id: ls; level: 0
                onLevelRequested: (l) => ls.level = l } }
        )QML", &err), qPrintable(err));
        QQuickItem *sw = h.item("themeLevelSwitch");
        QQuickItem *seg = h.item("themeLevel_1");
        h.item("other")->forceActiveFocus();
        QVERIFY(!sw->hasActiveFocus());
        const QPointF c = seg->mapToScene(QPointF(seg->width() / 2, seg->height() / 2));
        QTest::mouseClick(&h.window, Qt::LeftButton, {}, c.toPoint());
        QCOMPARE(sw->property("level").toInt(), 1);
        QVERIFY2(sw->hasActiveFocus(), "a click must leave the switch focused");
        QTest::keyClick(&h.window, Qt::Key_Right);
        QCOMPARE(sw->property("level").toInt(), 2);
        QVERIFY(sw->property("activeFocusOnTab").toBool());
    }

    void presetPromptTakesFocusTrapsTabAndReturnsIt()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            TextInput { objectName: "before"; width: 50; height: 20
                focus: true }
            ThemePresetPrompt { id: pp; visible: false; presetName: "Storm"
                y: 100; changes: 3 }
        )QML", &err), qPrintable(err));
        h.item("before")->forceActiveFocus();
        QVERIFY(h.item("before")->hasActiveFocus());
        h.item("themePresetPrompt")->setVisible(true);
        QTRY_VERIFY(h.item("themePresetKeepButton")->hasActiveFocus());
        QTest::keyClick(&h.window, Qt::Key_Tab);
        QVERIFY(h.item("themePresetReplaceButton")->hasActiveFocus());
        QTest::keyClick(&h.window, Qt::Key_Tab);
        QVERIFY(h.item("themePresetCancelButton")->hasActiveFocus());
        QTest::keyClick(&h.window, Qt::Key_Tab);
        QVERIFY2(h.item("themePresetKeepButton")->hasActiveFocus(),
                 "Tab must wrap inside the prompt, not walk the editor");
        QTest::keyClick(&h.window, Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(h.item("themePresetCancelButton")->hasActiveFocus());
        h.item("themePresetPrompt")->setVisible(false);
        QTRY_VERIFY(h.item("before")->hasActiveFocus());
    }

    void popoverReleasesFocusWhenItCloses()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            Rectangle { id: anchorBtn; objectName: "anchorBtn"; width: 80
                height: 24 }
            ThemePopover { id: pop; objectName: "pop"; anchors.fill: parent
                anchorItem: anchorBtn; initialFocus: field
                TextInput { id: field; objectName: "field"; width: 100
                    height: 24 }
            }
        )QML", &err), qPrintable(err));
        QQuickItem *pop = h.item("pop");
        pop->setProperty("opened", true);
        QTRY_VERIFY(h.item("field")->hasActiveFocus());
        pop->setProperty("opened", false);
        QVERIFY2(!h.item("field")->hasActiveFocus(),
                 "a closed popover's field must not keep the keys");
        QVERIFY(h.item("anchorBtn")->hasActiveFocus());
    }

    void linkButtonsAnswerReturnAndEnter()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            ThemeLinkRow { id: lr; width: 400; groupLabel: "Panels"
                parentLabel: "Panels"; property int parentHits: 0
                property int unlinkHits: 0
                onParentRequested: parentHits++
                onUnlinkRequested: unlinkHits++ }
        )QML", &err), qPrintable(err));
        QObject *row = h.root->findChild<QObject *>("themeRoleLinks");
        h.item("themeLinkParent")->forceActiveFocus();
        QTest::keyClick(&h.window, Qt::Key_Return);
        QCOMPARE(row->property("parentHits").toInt(), 1);
        h.item("themeLinkUnlink")->forceActiveFocus();
        QTest::keyClick(&h.window, Qt::Key_Enter);
        QCOMPARE(row->property("unlinkHits").toInt(), 1);
    }

    void toastTimerPausesWhileTheActionIsFocused()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            ThemeToast { id: t; width: 400 }
        )QML", &err), qPrintable(err));
        QObject *t = h.root->findChild<QObject *>("themeToast");
        QMetaObject::invokeMethod(t, "show", Q_ARG(QVariant, QStringLiteral("Done")),
                                  Q_ARG(QVariant, QStringLiteral("Undo")));
        QVERIFY(t->property("dismissTimerRunning").toBool());
        h.item("themeToastAction")->forceActiveFocus();
        QVERIFY(t->property("paused").toBool());
        QVERIFY2(!t->property("dismissTimerRunning").toBool(),
                 "the timer must not run while the action has focus");
        h.item("themeToastAction")->setFocus(false);
        QVERIFY(!t->property("paused").toBool());
        QVERIFY(t->property("dismissTimerRunning").toBool());
        // With an action it stays longer than the bare message.
        QVERIFY(t->property("actionDuration").toInt()
                > t->property("duration").toInt());
    }

    void simplePanelFollowsFocusAndScrollsOnKeys()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            ThemeSimplePanel { id: sp; width: 300; height: 200
                bases: [{ id: 1, name: "A" }, { id: 2, name: "B" }]
                swatches: [{ key: "a", label: "A", color: "red" },
                           { key: "b", label: "B", color: "green" }]
                gradientRoles: { var l = []; for (var i = 0; i < 14; ++i)
                    l.push({ key: "g" + i, label: "Gradient " + i,
                             color: "blue" }); return l }
            }
        )QML", &err), qPrintable(err));
        QQuickItem *panel = h.item("themeSimplePanel");
        QVERIFY(panel->property("contentHeight").toReal()
                > panel->height() + 100);
        QVERIFY(qFuzzyIsNull(panel->property("contentY").toReal()));
        QQuickItem *last = h.item("themeSimpleGradient_g13");
        QVERIFY(last);
        last->forceActiveFocus();
        QTRY_VERIFY2(panel->property("contentY").toReal() > 0,
                     "focus below the fold must scroll into view");
        const qreal y = panel->property("contentY").toReal();
        const QPointF top = last->mapToItem(panel->property("contentItem")
                                                .value<QQuickItem *>(), {0, 0});
        QVERIFY(top.y() >= y - 1 && top.y() + last->height() <= y + panel->height() + 1);

        // Keys no control handles scroll the panel.
        panel->setProperty("contentY", 0);
        panel->forceActiveFocus();
        QTest::keyClick(&h.window, Qt::Key_End);
        const qreal maxY = panel->property("contentHeight").toReal() - panel->height();
        QVERIFY(qAbs(panel->property("contentY").toReal() - maxY) < 1);
        QTest::keyClick(&h.window, Qt::Key_Home);
        QVERIFY(qFuzzyIsNull(panel->property("contentY").toReal()));
        QTest::keyClick(&h.window, Qt::Key_PageDown);
        QVERIFY(panel->property("contentY").toReal() > 50);
    }

    void menuListRevealsTheCurrentTheme()
    {
        Host h; QString err;
        QVERIFY2(h.load(R"QML(
            ThemeMenuPanel { id: mp; objectName: "revealPanel"; width: 340
                themes: { var l = []; for (var i = 0; i < 14; ++i)
                    l.push({ id: "t" + i, name: "Theme " + i, baseTheme: 1,
                             overrideCount: 0 }); return l }
                activeId: "t13" }
        )QML", &err), qPrintable(err));
        QQuickItem *list = h.item("themeMenuList");
        QTRY_VERIFY2(list->property("contentY").toReal() > 0,
                     "the current theme's row must be scrolled into view");
        QQuickItem *row = h.item("customThemeChip_t13");
        QVERIFY(row);
        const qreal y = list->property("contentY").toReal();
        QVERIFY(row->y() >= y - 1 && row->y() + row->height() <= y + list->height() + 1);

        // A later themes change must not move a list the user scrolled away.
        list->setProperty("contentY", 0);
        QVERIFY(qFuzzyIsNull(list->property("contentY").toReal()));
        QQuickItem *panel = h.item("revealPanel");
        QVERIFY(panel);
        QVariantList more = panel->property("themes").toList();
        QVariantMap extra; extra["id"] = "t99"; extra["name"] = "Copy";
        extra["baseTheme"] = 1; extra["overrideCount"] = 0;
        more.append(extra);
        panel->setProperty("themes", more);
        QTest::qWait(150);
        QVERIFY2(qFuzzyIsNull(list->property("contentY").toReal()),
                 "a themes change must not snap the list back to the current row");
    }
};

QTEST_MAIN(ThemeEditorPartsQmlTest)
#include "ThemeEditorPartsQmlTest.moc"
