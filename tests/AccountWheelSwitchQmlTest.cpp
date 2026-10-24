// Mouse wheel over the account tile cycles accounts (GitHub #54): wheel up =
// previous, down = next, wrapping; one switch per notch; nothing while a switch
// runs; touchpad deltas accumulate to one notch; accounts needing a sign-in
// are skipped. Real AccountWheelSwitch through the "MatrixClient" module, real
// wheel events through a window.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 200
    height: 200
    visible: true

    property var list: [
        { userId: "@a:x", isActive: true },
        { userId: "@b:x", isActive: false },
        { userId: "@c:x", isActive: false, needsSignIn: true },
        { userId: "@d:x", isActive: false }
    ]
    property string active: "@a:x"
    property bool busy: false
    property var requests: []

    Item {
        x: 50; y: 50; width: 60; height: 60
        AccountWheelSwitch {
            objectName: "wheel"
            accounts: win.list
            activeUserId: win.active
            busy: win.busy
            lockMs: 60
            onSwitchRequested: (id) => {
                win.requests = win.requests.concat([id])
                win.active = id
            }
        }
    }
}
)QML";

} // namespace

class AccountWheelSwitchQmlTest : public QObject
{
    Q_OBJECT

    QQmlEngine m_engine;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QStringList requests() const
    {
        QStringList out;
        for (const QVariant &v : m_root->property("requests").toList())
            out << v.toString();
        return out;
    }
    void reset(const QString &active = QStringLiteral("@a:x"))
    {
        m_root->setProperty("requests", QVariantList());
        m_root->setProperty("active", active);
        m_root->setProperty("busy", false);
        // Let any lock from the previous case expire.
        QTest::qWait(120);
    }
    void wheel(int angleY, Qt::ScrollPhase phase = Qt::NoScrollPhase,
               QPoint pixel = QPoint())
    {
        QWheelEvent ev(QPointF(80, 80), m_window->mapToGlobal(QPointF(80, 80)),
                       pixel, QPoint(0, angleY), Qt::NoButton, Qt::NoModifier,
                       phase, false, Qt::MouseEventNotSynthesized);
        QGuiApplication::sendEvent(m_window, &ev);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QQmlComponent c(&m_engine);
        c.setData(kScene, QUrl());
        m_root = c.create();
        QVERIFY2(m_root, qPrintable(c.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }
    void cleanupTestCase() { delete m_root; }

    void wheelDownIsNextAndSkipsAnAccountThatNeedsSignIn()
    {
        reset();
        wheel(-120);
        QCOMPARE(requests(), QStringList{QStringLiteral("@b:x")});
        QTest::qWait(100);
        wheel(-120);   // @c needs a sign-in: skipped
        QCOMPARE(requests(), (QStringList{QStringLiteral("@b:x"),
                                          QStringLiteral("@d:x")}));
    }
    void wheelUpIsPreviousAndWraps()
    {
        reset();
        wheel(120);   // before the first is the last usable one
        QCOMPARE(requests(), QStringList{QStringLiteral("@d:x")});
        QTest::qWait(100);
        wheel(-120);  // and next from the last wraps to the first
        QCOMPARE(requests(), (QStringList{QStringLiteral("@d:x"),
                                          QStringLiteral("@a:x")}));
    }
    void aBurstOfNotchesIsOneSwitchAndAnInProgressSwitchBlocksAll()
    {
        reset();
        wheel(-120);
        wheel(-120);
        wheel(-120);
        QCOMPARE(requests().size(), 1);
        reset();
        m_root->setProperty("busy", true);
        wheel(-120);
        QVERIFY(requests().isEmpty());
        m_root->setProperty("busy", false);
        QTest::qWait(100);
        wheel(-120);
        QCOMPARE(requests().size(), 1);
    }
    void touchpadDeltasAccumulateToOneNotch()
    {
        reset();
        for (int i = 0; i < 5; ++i)
            wheel(-20, Qt::ScrollUpdate, QPoint(0, -3));
        QVERIFY2(requests().isEmpty(), "100 units is not a notch yet");
        wheel(-20, Qt::ScrollUpdate, QPoint(0, -3));
        QCOMPARE(requests().size(), 1);
        // The tail of the same swipe is held back by the lock.
        for (int i = 0; i < 12; ++i)
            wheel(-20, Qt::ScrollUpdate, QPoint(0, -3));
        QCOMPARE(requests().size(), 1);
    }
    void aSingleAccountHasNowhereToGo()
    {
        reset();
        const QVariant saved = m_root->property("list");
        m_root->setProperty("list", QVariantList{QVariantMap{
            {QStringLiteral("userId"), QStringLiteral("@a:x")}}});
        wheel(-120);
        QTest::qWait(100);
        wheel(120);
        QVERIFY(requests().isEmpty());
        m_root->setProperty("list", saved);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    AccountWheelSwitchQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AccountWheelSwitchQmlTest.moc"
