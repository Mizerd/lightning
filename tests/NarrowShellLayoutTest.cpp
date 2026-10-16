// The application shell must stay usable at the smallest window size the app
// allows (Main.qml pins minimumWidth 640 / minimumHeight 420), measured
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
    height: 420
    minimumWidth: 640
    minimumHeight: 420
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
    height: 420
    HomePane { objectName: "pane"; anchors.fill: parent }
}
)QML"), QUrl(QStringLiteral("homepane-narrow.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(rootItem);
        QQuickWindow win;
        win.resize(640, 420);
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
    height: 420
    HomePane { objectName: "pane"; anchors.fill: parent }
}
)QML"), QUrl(QStringLiteral("homepane-filter.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(rootItem);
        QQuickWindow win;
        win.resize(640, 420);
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
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    NarrowShellLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "NarrowShellLayoutTest.moc"
