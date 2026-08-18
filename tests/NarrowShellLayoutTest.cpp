// 2026-08-18 tester report: the application shell must stay usable at the
// smallest window size the app itself allows (Main.qml pins minimumWidth 640
// / minimumHeight 420).
//
// Two reported defects, both measured here against the REAL MainScreen (rail
// + room list + timeline + composer), not against the composer in isolation:
//
//   * "kai sushrinkini app iki max net nematai pilnos vienos raides ka
//     typini" — at the minimum width the composer's text field collapsed to
//     a few pixels, because every optional button in the input row keeps its
//     full width and the field is the only Layout.fillWidth item left to
//     absorb the deficit.
//   * "kai darai shift+enter max praleidzia tik viena eilute" / "kai padalini
//     app i dvi dalis ... negali editinti zinuciu per sita ui" — in a short
//     window the composer could not grow past one extra line, so a multi-line
//     draft (and an edit of a long message) was invisible while typing.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

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
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    NarrowShellLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "NarrowShellLayoutTest.moc"
