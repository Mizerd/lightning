// v0.6.5 (SPEC 1j+1k): offscreen behavior proof for the two-mode quick
// switcher — the Wave-1 audit's G2 gap (this surface previously had zero
// behavioural coverage). Covers the mode transitions ('>' prefix in, lone
// backspace out, openCommandMode()), the honesty of the command action list
// (settings/sections/accounts/themes ONLY — never leave/mute/files/sign-out
// verbs), a real end-to-end theme action against the existing writable
// settings.theme property, Escape dismissal, and a source-level pin that
// navigate-mode invite routing still opens (never accepts) invites.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    QuickSwitcher { id: switcher; objectName: "switcher" }
    function openSwitcher() { switcher.open() }
    function openCommand() { switcher.openCommandMode() }
}
)QML";

} // namespace

class QuickSwitcherModesQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QObject *switcher() const
    {
        return m_root->findChild<QObject *>(QStringLiteral("switcher"));
    }

    QQuickItem *field() const
    {
        return m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherField"));
    }

    QQuickItem *commandRow(int index) const
    {
        auto *list = m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        if (!list)
            return nullptr;
        // Offscreen popups may never get a polish pass scheduled for a
        // list that became fillable after open — force the layout so
        // delegates actually materialize before lookup.
        QMetaObject::invokeMethod(list, "forceLayout");
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }

    int commandCount() const
    {
        auto *list = m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        return list ? list->property("count").toInt() : -1;
    }

    // Full action-list titles from the model property (the ListView only
    // instantiates the viewport, so delegate-walking would miss the tail).
    QStringList commandTitles() const
    {
        QStringList titles;
        const QVariantList rows =
            switcher()->property("commandRows").value<QVariantList>();
        for (const QVariant &row : rows) {
            const QVariantMap map = row.toMap();
            const QString label = map.value(QStringLiteral("label"))
                                      .toString();
            titles << (label.isEmpty()
                           ? map.value(QStringLiteral("name")).toString()
                           : label);
        }
        return titles;
    }

    void closeSwitcher()
    {
        QMetaObject::invokeMethod(switcher(), "close");
        QTRY_VERIFY(!switcher()->property("visible").toBool());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("quick-switcher-modes-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("quickswitcherscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    void prefixEntersCommandModeAndLoneBackspaceReturns()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QVERIFY(!switcher()->property("commandMode").toBool());

        QVERIFY(field());
        QTest::keyClick(m_window, Qt::Key_Greater);
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        // The trigger character never becomes query text.
        QCOMPARE(field()->property("text").toString(), QString());

        // Backspacing the (already-stripped) lone '>' returns to navigate.
        QTest::keyClick(m_window, Qt::Key_Backspace);
        QTRY_VERIFY(!switcher()->property("commandMode").toBool());
        QVERIFY(switcher()->property("opened").toBool());
        closeSwitcher();
    }

    void openCommandModeFunctionLandsInCommandMode()
    {
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        closeSwitcher();
    }

    void commandListOffersOnlyHonestActions()
    {
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        QTRY_VERIFY(commandCount() > 0);

        // Offscreen, nothing requests a frame after the popup opens, and
        // ListView delegate creation rides the polish pass a frame runs —
        // force one exactly like the pixel suites do. Rows materialize a
        // tick later; harvest until the first action is visible.
        (void) m_window->grabWindow();
        // Row items materialize a tick after count updates — harvest until
        // the first action is visible.
        QTRY_VERIFY(commandTitles().contains(QStringLiteral("Open Settings")));
        const QStringList titles = commandTitles();
        int themeActions = 0;
        for (const QString &title : titles) {
            if (title.startsWith(QStringLiteral("Theme:")))
                ++themeActions;
            // Nothing without a backend may appear as a verb.
            QVERIFY2(!title.contains(QStringLiteral("Mute"),
                                     Qt::CaseInsensitive),
                     qPrintable(title));
            QVERIFY2(!title.contains(QStringLiteral("Leave"),
                                     Qt::CaseInsensitive),
                     qPrintable(title));
            QVERIFY2(!title.contains(QStringLiteral("Sign out"),
                                     Qt::CaseInsensitive),
                     qPrintable(title));
            QVERIFY2(!title.contains(QStringLiteral("file"),
                                     Qt::CaseInsensitive),
                     qPrintable(title));
        }
        // The 10 real themes plus "Match system" — sourced from
        // AppTheme.themeList, so a new theme joins automatically.
        QCOMPARE(themeActions, 11);
        closeSwitcher();
    }

    void themeActionAppliesTheExistingWritableProperty()
    {
        const SettingsManager::Theme before = m_controller->settings()->theme();
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        QTRY_VERIFY(commandCount() > 0);

        // Filter down to the Deep Teal action and activate it.
        QVERIFY(field());
        field()->setProperty("text", QStringLiteral("deep teal"));
        QTRY_VERIFY(commandCount() >= 1);
        auto findDeepTeal = [this]() -> QQuickItem * {
            for (int i = 0; i < commandCount(); ++i) {
                QQuickItem *candidate = commandRow(i);
                if (!candidate)
                    continue;
                auto *title = candidate->findChild<QQuickItem *>(
                    QStringLiteral("commandRowTitle"));
                if (title && title->property("text").toString().contains(
                        QStringLiteral("Deep Teal")))
                    return candidate;
            }
            return nullptr;
        };
        QTRY_VERIFY2(findDeepTeal() != nullptr,
                     "no Deep Teal theme action after filtering");
        QQuickItem *row = findDeepTeal();
        const QPointF center = row->mapToScene(
            QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QTRY_COMPARE(int(m_controller->settings()->theme()), 10);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        m_controller->settings()->setTheme(before);
    }

    void escapeDismissesTheModal()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
    }

    void navigateInviteRoutingStillOpensNeverAccepts()
    {
        QFile file(QStringLiteral(QML_DIR "/QuickSwitcher.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("app.openRoom(r.roomId)")));
        QVERIFY(!content.contains(QStringLiteral("acceptInvite")));
        // The scope chips never offer a Files scope (no backend).
        QVERIFY(!content.contains(QStringLiteral("qsTr(\"Files\")")));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QuickSwitcherModesQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "QuickSwitcherModesQmlTest.moc"
