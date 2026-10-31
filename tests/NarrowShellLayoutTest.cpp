// The application shell must stay usable at the smallest window size the app
// allows (Main.qml pins minimumWidth 640 / minimumHeight 360), measured
// against the real MainScreen (rail + room list + timeline + composer):
//
//   * at the minimum width the composer's text field must keep a usable
//     width, though every optional button in the input row keeps its own and
//     the field is the only Layout.fillWidth item;
//   * in a short window the composer must still grow for a multi-line draft
//     (or an edit of a long message);
//   * the Home pane, with a very long display name and Space name, keeps
//     every visible child inside its card and the viewport (B032, B054), and
//     says "No rooms match" rather than "Nothing here yet" under a room-list
//     filter (B096).

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlExpression>
#include <QQmlProperty>
#include <memory>

#include "app/AppController.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    // The application's own floor (qml/Main.qml).
    width: 640
    height: 360
    minimumWidth: 640
    minimumHeight: 360
    visible: true
    color: AppTheme.background

    MainScreen {
        objectName: "mainScreen"
        anchors.fill: parent
    }
}
)QML";

} // namespace

class NarrowShellLayoutTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
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

    // Every visible descendant of `parent` that has an area, listed by name
    // when its right or left edge is outside [0, limit] in `space`'s coordinates.
    static void collectOutside(QQuickItem *parent, QQuickItem *space,
                               qreal limit, QStringList &out)
    {
        for (QQuickItem *c : parent->childItems()) {
            if (!c->isVisible() || c->opacity() <= 0.0)
                continue;
            if (c->width() > 0 && c->height() > 0) {
                const QRectF r = space->mapRectFromItem(
                    c, QRectF(0, 0, c->width(), c->height()));
                if (r.right() > limit + 0.5 || r.left() < -0.5)
                    out << QStringLiteral("%1 [%2 .. %3]")
                               .arg(c->objectName().isEmpty()
                                        ? QString::fromLatin1(
                                              c->metaObject()->className())
                                        : c->objectName())
                               .arg(r.left()).arg(r.right());
            }
            collectOutside(c, space, limit, out);
        }
    }

    // Every visible control (button, field, combo, check, switch) whose box,
    // after the clip of each ancestor, still leaves [0, w] x [0, h] of the
    // window. A clipped ancestor is how a scrolled-away row is meant to
    // disappear, so only what is still VISIBLE after the clips counts.
    static void collectControlsOutsideWindow(QQuickItem *it, const QRectF &win,
                                             QStringList &out)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        const bool control = cls.contains("Button") || cls.contains("TextField")
            || cls.contains("ComboBox") || cls.contains("CheckBox")
            || cls.contains("Switch");
        if (control && it->width() > 0 && it->height() > 0) {
            QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
            for (QQuickItem *a = it->parentItem(); a; a = a->parentItem())
                if (a->clip())
                    r = r.intersected(
                        a->mapRectToScene(QRectF(0, 0, a->width(), a->height())));
            if (!r.isEmpty() && !win.adjusted(-1, -1, 1, 1).contains(r))
                out << QStringLiteral("%1 %2 [%3,%4 %5x%6]")
                           .arg(QString::fromLatin1(cls),
                                it->objectName(), QString::number(r.x()),
                                QString::number(r.y()), QString::number(r.width()),
                                QString::number(r.height()));
        }
        for (QQuickItem *c : it->childItems())
            collectControlsOutsideWindow(c, win, out);
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("narrowshell.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(120);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    // The text field must keep a readable width at the minimum window size.
    void theComposerInputStaysReadableAtTheMinimumWidth()
    {
        auto *flick = item("composerInputFlick");
        QVERIFY2(flick, "composer input scroll surface not found");
        qInfo() << "composer input width at 640px window" << flick->width();
        QVERIFY2(flick->width() >= 120,
                 "the composer text field collapses at the minimum window "
                 "width");
    }

    // A multi-line draft must keep growing the composer in a short window;
    // the timeline yields the space, not the field the user is typing into.
    void theComposerGrowsForAMultiLineDraftInAShortWindow()
    {
        auto *flick = item("composerInputFlick");
        auto *input = item("composerInput");
        QVERIFY(flick && input);
        input->setProperty("text", QStringLiteral("one"));
        QTest::qWait(80);
        const qreal oneLine = flick->height();
        input->setProperty("text",
                           QStringLiteral("one\ntwo\nthree\nfour\nfive"));
        QTest::qWait(120);
        const qreal fiveLines = flick->height();
        qInfo() << "composer height 1 line" << oneLine
                << "5 lines" << fiveLines;
        QVERIFY2(fiveLines >= oneLine * 2.5,
                 "the composer stops growing with the draft in a short "
                 "window");
        input->setProperty("text", QString());
        QTest::qWait(60);
    }

    // The small-window round: a window a quarter of a screen must not push a
    // control off its own edge. Measured on the real MainScreen with a room
    // open, at the application floor and at three common small sizes (the
    // send-options chevron of the composer used to sit 18 px past the edge at
    // the 640 floor).
    void everyControlStaysInsideTheWindowAtSmallSizes()
    {
        const QSize sizes[] = { QSize(640, 360), QSize(640, 720),
                                QSize(854, 480), QSize(960, 540) };
        for (const QSize &sz : sizes) {
            m_window->resize(sz);
            QTest::qWait(200);
            QStringList outside;
            collectControlsOutsideWindow(
                m_window->contentItem(),
                QRectF(0, 0, m_window->width(), m_window->height()), outside);
            QVERIFY2(outside.isEmpty(),
                     qPrintable(QStringLiteral("%1x%2: ").arg(sz.width())
                                    .arg(sz.height())
                                + outside.join(QStringLiteral("; "))));
        }
        m_window->resize(640, 360);
        QTest::qWait(100);
    }

    // Inside the window is not enough: the row's last button has to be inside
    // the CARD that draws the composer (the chevron used to hang off its edge).
    void theComposerRowStaysInsideItsCardAtTheMinimumWidth()
    {
        m_window->resize(640, 360);
        QTest::qWait(200);
        auto *card = item("composerCard");
        QVERIFY2(card, "composer card not found");
        const QRectF cr = card->mapRectToScene(
            QRectF(0, 0, card->width(), card->height()));
        for (const char *name : { "composerAttachButton", "composerSendButton",
                                  "composerSendOptionsButton" }) {
            QQuickItem *b = item(name);
            QVERIFY2(b, name);
            if (!b->isVisible())
                continue;
            const QRectF br = b->mapRectToScene(
                QRectF(0, 0, b->width(), b->height()));
            QVERIFY2(cr.adjusted(-0.5, -0.5, 0.5, 0.5).contains(br),
                     qPrintable(QStringLiteral("%1 [%2..%3] is outside the "
                                               "composer card [%4..%5]")
                                    .arg(QLatin1String(name))
                                    .arg(br.left()).arg(br.right())
                                    .arg(cr.left()).arg(cr.right())));
        }
    }

    // B032 / B054: a display name and a Space name of 300 characters at the
    // 640 px minimum. Nothing may run past the card or the pane, the name
    // elides, and the action buttons wrap instead of overflowing.
    void theHomePaneKeepsALongNameInsideItsCard()
    {
        QQmlComponent component(m_engine);
        component.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 640
    height: 360
    HomePane { objectName: "pane"; anchors.fill: parent }
}
)QML"), QUrl(QStringLiteral("homepane-narrow.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(rootItem);
        QQuickWindow win;
        win.resize(640, 360);
        rootItem->setParentItem(win.contentItem());
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QQuickItem *pane = findItem(rootItem, QStringLiteral("pane"));
        QVERIFY(pane);
        const QString longName = QString(300, QLatin1Char('W'));
        QVariantMap account;
        account.insert(QStringLiteral("displayName"), longName);
        account.insert(QStringLiteral("avatarUrl"), QString());
        pane->setProperty("activeAccount", account);
        QVariantMap sp;
        sp.insert(QStringLiteral("name"), longName);
        sp.insert(QStringLiteral("roomId"), QStringLiteral("!sp:mock.local"));
        sp.insert(QStringLiteral("avatarUrl"), QString());
        pane->setProperty("spacesModel", QVariantList{ sp });
        QTest::qWait(150);

        QQuickItem *card = findItem(pane, QStringLiteral("homeCard"));
        QVERIFY(card);
        QStringList outside;
        collectOutside(card, pane, pane->width(), outside);
        QVERIFY2(outside.isEmpty(),
                 qPrintable(QStringLiteral("outside the pane: ")
                            + outside.join(QStringLiteral("; "))));
        // Inside the CARD, not merely the pane: the card is 560 wide at most.
        const QRectF cr = pane->mapRectFromItem(
            card, QRectF(0, 0, card->width(), card->height()));
        for (const char *name : { "homeWelcomeRow", "homeActionsHolder" }) {
            QQuickItem *it = findItem(card, QLatin1String(name));
            QVERIFY2(it, name);
            const QRectF r = pane->mapRectFromItem(
                it, QRectF(0, 0, it->width(), it->height()));
            QVERIFY2(r.right() <= cr.right() + 0.5 && r.left() >= cr.left() - 0.5,
                     name);
        }
        QQuickItem *flow = findItem(card, QStringLiteral("homeActionsFlow"));
        QVERIFY(flow);
        QVERIFY2(flow->width() <= card->width() + 0.5,
                 "the action buttons are wider than the card");
        QQuickItem *name = findItem(card, QStringLiteral("homeWelcomeName"));
        QVERIFY(name);
        QVERIFY2(name->property("truncated").toBool(),
                 "the long name was not elided");
        QQuickItem *pill = findItem(card, QStringLiteral("homeSpacePillLabel"));
        QVERIFY(pill);
        QVERIFY2(pill->property("truncated").toBool(),
                 "the long Space name was not elided");
    }

    // B096: a room-list filter with no match is not an empty account.
    void theHomePaneDoesNotCallAFilteredListEmpty()
    {
        QQmlComponent component(m_engine);
        component.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 640
    height: 360
    HomePane { objectName: "pane"; anchors.fill: parent }
}
)QML"), QUrl(QStringLiteral("homepane-filter.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(rootItem);
        QQuickWindow win;
        win.resize(640, 360);
        rootItem->setParentItem(win.contentItem());
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QQuickItem *pane = findItem(rootItem, QStringLiteral("pane"));
        QVERIFY(pane);
        pane->setProperty("recentModel", QVariantList{});
        QQmlExpression clear(qmlContext(pane), nullptr,
                             QStringLiteral("app.roomList.searchQuery = ''"));
        clear.evaluate();
        QTest::qWait(400);
        QQuickItem *title = findItem(pane, QStringLiteral("homeOnboardingTitle"));
        QVERIFY(title);
        QCOMPARE(title->property("text").toString(),
                 QStringLiteral("Nothing here yet"));
        QQmlExpression set(qmlContext(pane), nullptr,
                           QStringLiteral("app.roomList.searchQuery = 'zzzz'"));
        set.evaluate();
        QVERIFY2(!set.hasError(), qPrintable(set.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(title->property("text").toString(),
                                  QStringLiteral("No rooms match"), 2000);
        clear.evaluate();
    }

    // C08-F04 / F02: the Home pane scrolls by key, by scrollbar and by
    // middle-click autoscroll when its card overflows. Old code: no
    // scrollbar, keys did nothing, no scroller.
    void theHomePaneScrollsByKeysScrollbarAndMiddleClick()
    {
        QQmlComponent component(m_engine);
        component.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 640
    height: 240
    HomePane { objectName: "pane"; anchors.fill: parent }
}
)QML"), QUrl(QStringLiteral("homepane-scroll.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(rootItem);
        QQuickWindow win;
        win.resize(640, 240);
        rootItem->setParentItem(win.contentItem());
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QQuickItem *pane = findItem(rootItem, QStringLiteral("pane"));
        QVERIFY(pane);
        QTest::qWait(900);
        QQuickItem *flick = findItem(pane, QStringLiteral("homeFlick"));
        QVERIFY(flick);
        auto maxYNow = [&]() {
            return flick->property("contentHeight").toReal() - flick->height();
        };
        QVERIFY2(maxYNow() > 100, "the fixture must overflow");
        QVERIFY2(findItem(pane, QStringLiteral("homeScrollBar")),
                 "no scrollbar on the Home pane");

        flick->forceActiveFocus();
        QTest::keyClick(&win, Qt::Key_PageDown);
        QTest::qWait(30);
        QVERIFY2(flick->property("contentY").toReal() > 50,
                 "PageDown did not scroll the Home pane");
        QTest::keyClick(&win, Qt::Key_End);
        QTest::qWait(30);
        QVERIFY(qAbs(flick->property("contentY").toReal() - maxYNow()) < 1.0);
        QTest::keyClick(&win, Qt::Key_Down);   // clamped at the end
        QTest::qWait(30);
        QVERIFY(qAbs(flick->property("contentY").toReal() - maxYNow()) < 1.0);
        QTest::keyClick(&win, Qt::Key_Home);
        QTest::qWait(30);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);

        // A click on blank space focuses the pane so the keys work there.
        flick->setProperty("focus", false);
        win.contentItem()->forceActiveFocus();
        QTest::mouseClick(&win, Qt::LeftButton, Qt::NoModifier, QPoint(6, 120));
        QTest::qWait(30);
        QVERIFY2(flick->hasActiveFocus(), "a click did not focus the pane");

        // Middle-click autoscroll: press at the blank left margin, travel
        // down, and the content follows.
        QQuickItem *scroller = findItem(pane,
                                        QStringLiteral("homeMiddleClickScroller"));
        QVERIFY2(scroller, "no middle-click scroller on the Home pane");
        QTest::mousePress(&win, Qt::MiddleButton, Qt::NoModifier, QPoint(6, 60));
        QTest::mouseMove(&win, QPoint(6, 200));
        QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentY").toReal() > 20, 2000);
        QTest::mouseRelease(&win, Qt::MiddleButton, Qt::NoModifier, QPoint(6, 200));
    }

    // C08-F06: a keyboard-focused room-updates summary shows a ring; a mouse
    // focus does not. Old code: border width 0 in both.
    void theRoomUpdatesSummaryDrawsAKeyboardFocusRing()
    {
        QQmlComponent component(m_engine);
        component.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 400
    height: 100
    RoomActivityDelegate {
        objectName: "activity"
        width: parent.width
        entries: [ { eventKind: "m.room.member", timestamp: new Date() },
                   { eventKind: "m.room.topic", timestamp: new Date() } ]
    }
}
)QML"), QUrl(QStringLiteral("activity-ring.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QQuickWindow win;
        win.resize(400, 100);
        rootItem->setParentItem(win.contentItem());
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        win.requestActivate();
        QTRY_VERIFY_WITH_TIMEOUT(win.isActive(), 2000);
        QQuickItem *row = findItem(rootItem,
                                   QStringLiteral("stateActivitySummary"));
        QVERIFY(row);
        auto *bg = row->property("background").value<QQuickItem *>();
        QVERIFY(bg);
        auto borderWidth = [&]() {
            return QQmlProperty(bg, QStringLiteral("border.width")).read().toInt();
        };
        QCOMPARE(borderWidth(), 0);
        row->forceActiveFocus(Qt::MouseFocusReason);
        QTest::qWait(30);
        QCOMPARE(borderWidth(), 0);
        row->setFocus(false, Qt::OtherFocusReason);
        QVERIFY(!row->hasActiveFocus());
        row->forceActiveFocus(Qt::TabFocusReason);
        QTest::qWait(30);
        QVERIFY2(row->property("visualFocus").toBool(),
                 "Tab focus must be a visual focus");
        QCOMPARE(borderWidth(), 2);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    NarrowShellLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "NarrowShellLayoutTest.moc"
