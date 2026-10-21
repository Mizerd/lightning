// Keyboard and pointer reach of the chat background dialog and editor
// (swarm chunk C66, 2026-10-20):
//
//  * Tab onto a control below the dialog's fold scrolls it into view (the
//    Flickable did not follow focus, so the ring was invisible and Space
//    toggled an unseen switch);
//  * the dialog carries the middle-click autoscroller its siblings have;
//  * the tone sliders answer Up/Down, Home/End and PageUp/PageDown, not only
//    the arrows of their own orientation.
#include <QtTest/QtTest>

#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include <memory>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "backdrop/ChatBackdropController.h"

namespace {
void collectByName(QQuickItem *p, const QString &name, QList<QQuickItem *> &out)
{
    if (!p)
        return;
    if (p->objectName() == name)
        out.append(p);
    for (QQuickItem *c : p->childItems())
        collectByName(c, name, out);
}
QQuickItem *findItem(QQuickItem *p, const QString &name)
{
    QList<QQuickItem *> hits;
    collectByName(p, name, hits);
    return hits.isEmpty() ? nullptr : hits.first();
}
void collectSliders(QQuickItem *p, QList<QQuickItem *> &out)
{
    if (!p)
        return;
    if (p->inherits("QQuickSlider"))
        out.append(p);
    for (QQuickItem *c : p->childItems())
        collectSliders(c, out);
}
} // namespace

class ChatBackgroundKeyboardQmlTest : public QObject
{
    Q_OBJECT

    static bool login(AppController &controller)
    {
        QSignalSpy spy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        return spy.wait(3000);
    }

    static void prepare(AppController &controller)
    {
        auto *b = qobject_cast<ChatBackdropController *>(controller.backdrops());
        QVERIFY(b);
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::darkGreen);
        const QVariantMap r = b->prepareImageForTesting(img);
        Q_UNUSED(r)
    }

private slots:
    void tabbingBelowTheFoldScrollsTheControlIntoView()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        prepare(controller);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 700
    height: 360
    visible: true
    property alias dlg: d
    ChatBackgroundDialog { id: d }
}
)"), QUrl(QStringLiteral("qrc:/cbk-dialog.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner);
        auto *win = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QMetaObject::invokeMethod(owner->property("dlg").value<QObject *>(),
                                  "openForDefault");
        QTest::qWait(300);

        auto *root = win->contentItem();
        auto *flick = findItem(root, QStringLiteral("chatBackgroundDialogScroll"));
        QVERIFY(flick);
        QVERIFY2(flick->property("contentHeight").toReal()
                     > flick->height() + 40,
                 "fixture must overflow, or this proves nothing");
        QCOMPARE(flick->property("contentY").toReal(), 0.0);

        auto *themes = findItem(root, QStringLiteral("chatBackgroundDialogThemes"));
        QVERIFY(themes);
        themes->forceActiveFocus(Qt::TabFocusReason);
        QTest::qWait(100);

        const QPointF topInFlick = themes->mapToItem(flick, QPointF(0, 0));
        QVERIFY2(topInFlick.y() >= 0
                     && topInFlick.y() + themes->height() <= flick->height() + 1,
                 qPrintable(QStringLiteral("focused control at y=%1 h=%2 outside "
                                           "a %3px viewport")
                                .arg(topInFlick.y()).arg(themes->height())
                                .arg(flick->height())));

        // And back up: focusing a control above the fold scrolls it down.
        auto *choose = findItem(root, QStringLiteral("chatBackgroundChoose"));
        QVERIFY(choose);
        choose->forceActiveFocus(Qt::BacktabFocusReason);
        QTest::qWait(100);
        const QPointF chooseTop = choose->mapToItem(flick, QPointF(0, 0));
        QVERIFY2(chooseTop.y() >= 0, "control above the fold stayed hidden");
    }

    void theDialogCarriesTheMiddleClickScroller()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 700
    height: 360
    visible: true
    property alias dlg: d
    ChatBackgroundDialog { id: d }
}
)"), QUrl(QStringLiteral("qrc:/cbk-mc.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *win = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QMetaObject::invokeMethod(owner->property("dlg").value<QObject *>(),
                                  "openForDefault");
        QTest::qWait(300);
        auto *scroller = findItem(
            win->contentItem(),
            QStringLiteral("chatBackgroundDialogMiddleClickScroller"));
        QVERIFY2(scroller, "no middle-click scroller in the dialog");
        auto *flick = findItem(win->contentItem(),
                               QStringLiteral("chatBackgroundDialogScroll"));
        QCOMPARE(scroller->property("view").value<QObject *>(), flick);
    }

    void toneSlidersAnswerTheFullSliderKeySet()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        prepare(controller);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 520
    height: 900
    visible: true
    property alias editor: e
    ChatBackgroundEditor { id: e; width: 480; scopeKind: "default"; showTitle: false }
}
)"), QUrl(QStringLiteral("qrc:/cbk-slider.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *win = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QTest::qWait(200);
        auto *editor = owner->property("editor").value<QQuickItem *>();
        QVERIFY(editor);
        QVERIFY2(editor->property("hasPrepared").toBool(),
                 "fixture: a prepared picture must make the sliders appear");

        QList<QQuickItem *> sliders;
        collectSliders(editor, sliders);
        QVERIFY2(sliders.size() >= 3, "expected Dim, Blur and Tint sliders");
        QQuickItem *dim = sliders.first();
        dim->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(dim->hasActiveFocus());
        const double start = dim->property("value").toDouble();

        QTest::keyClick(win, Qt::Key_Up);
        QTRY_COMPARE(dim->property("value").toDouble(), start + 0.05);
        QCOMPARE(editor->property("pDim").toDouble(), start + 0.05);
        QTest::keyClick(win, Qt::Key_Down);
        QTRY_COMPARE(dim->property("value").toDouble(), start);
        QTest::keyClick(win, Qt::Key_End);
        QTRY_COMPARE(dim->property("value").toDouble(), 1.0);
        QCOMPARE(editor->property("pDim").toDouble(), 1.0);
        QTest::keyClick(win, Qt::Key_Home);
        QTRY_COMPARE(dim->property("value").toDouble(), 0.0);
        QTest::keyClick(win, Qt::Key_PageUp);
        QTRY_COMPARE(dim->property("value").toDouble(), 0.25);
        QTest::keyClick(win, Qt::Key_PageDown);
        QTRY_COMPARE(dim->property("value").toDouble(), 0.0);
        QVERIFY(editor->property("dirty").toBool());

        // A key at a limit changes nothing and must not mark the editor dirty.
        editor->setProperty("dirty", false);
        QCOMPARE(dim->property("value").toDouble(), 0.0);
        QTest::keyClick(win, Qt::Key_Home);
        QTest::keyClick(win, Qt::Key_PageDown);
        QTest::keyClick(win, Qt::Key_Down);
        QTest::qWait(50);
        QVERIFY2(!editor->property("dirty").toBool(),
                 "a no-op key at the minimum must not set dirty");
    }
    void clickingASwitchLabelMovesFocusToItsSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 700
    height: 900
    visible: true
    property alias sec: s
    ChatBackgroundSettings { id: s; width: 640 }
}
)"), QUrl(QStringLiteral("qrc:/cbk-label.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *win = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QTest::qWait(200);
        auto *root = win->contentItem();
        auto *sw = findItem(root, QStringLiteral("showSharedBackgroundsSwitch"));
        QVERIFY(sw);
        QVERIFY2(sw->isVisible(), "fixture: the shared-backgrounds row must show");
        // Focus elsewhere first, then click the LABEL beside the switch.
        auto *other = findItem(root, QStringLiteral("surfaceDepthMakeGradientButton"));
        QVERIFY(other);
        other->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(!sw->hasActiveFocus());
        const QPointF p = sw->mapToScene(QPointF(sw->width() + 40, sw->height() / 2));
        QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier, p.toPoint());
        QTRY_VERIFY2(sw->hasActiveFocus(),
                     "clicking the label left focus on the previous control");
    }
};

QTEST_MAIN(ChatBackgroundKeyboardQmlTest)
#include "ChatBackgroundKeyboardQmlTest.moc"
