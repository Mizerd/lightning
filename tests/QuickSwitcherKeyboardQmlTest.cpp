// C09 round 2: behaviour of the quick switcher keyboard reach in a real
// offscreen window: the Tab wrap-around scrolls its row into view, PageUp/
// PageDown move the selection by a page, Ctrl+Home/End jump to the ends.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QClipboard>
#include <QScopeGuard>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AccountManager.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 340
    visible: true
    color: AppTheme.background

    QuickSwitcher { id: switcher; objectName: "switcher" }
    function openSwitcher() { switcher.open() }
    function openCommand() { switcher.openCommandMode() }
}
)QML";

} // namespace

class QuickSwitcherKeyboardQmlTest : public QObject
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
            QStringLiteral("quick-switcher-keyboard-test"));
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

    QQuickItem *resultList() const
    {
        return m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherList"));
    }

    // Is the row for currentIndex inside the viewport?
    bool currentRowVisible() const
    {
        QQuickItem *list = resultList();
        QMetaObject::invokeMethod(list, "forceLayout");
        const int cur = list->property("currentIndex").toInt();
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, cur));
        if (!item)
            return false;
        const qreal top = item->y() - list->property("contentY").toReal();
        return top >= -1 && top + item->height() <= list->height() + 1;
    }

    void configuredKeysNavigateWhileTheQueryKeepsFocus()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 2);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(field()->hasActiveFocus());
        QCOMPARE(field()->property("text").toString(), QString());
        QCOMPARE(m_controller->shortcuts()->setBinding(QStringLiteral("dialog.nextItem"),
                                                       QStringLiteral("Ctrl+N")), QString());
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QCOMPARE(m_controller->shortcuts()->resetToDefault(QStringLiteral("dialog.nextItem")), QString());
        closeSwitcher();

        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *commands = m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherCommandList"));
        QTRY_VERIFY(commands->property("count").toInt() > 2);
        QVERIFY(switcher()->property("commandMode").toBool());
        QVERIFY(field()->hasActiveFocus());
        QCOMPARE(m_controller->shortcuts()->dialogActionForKey(Qt::Key_J, Qt::ControlModifier),
                 QStringLiteral("dialog.nextItem"));
        commands->setProperty("currentIndex", -1);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(commands->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(commands->property("currentIndex").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(commands->property("currentIndex").toInt(), 0);
        QVERIFY(field()->hasActiveFocus());
        closeSwitcher();
    }

    void modifiedArrowsAndReturnHonorDialogBindings_data()
    {
        QTest::addColumn<bool>("commandMode");
        QTest::newRow("rooms") << false;
        QTest::newRow("commands") << true;
    }

    void modifiedArrowsAndReturnHonorDialogBindings()
    {
        QFETCH(bool, commandMode);
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("dialog.nextItem"));
            shortcuts->resetToDefault(QStringLiteral("dialog.close"));
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.nextItem"),
                                      QStringLiteral("Ctrl+Up")), QString());
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.close"),
                                      QStringLiteral("Ctrl+Return")), QString());
        QMetaObject::invokeMethod(m_root, commandMode ? "openCommand" : "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *list = commandMode
            ? m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherCommandList"))
            : resultList();
        QVERIFY(list);
        QTRY_VERIFY(list->property("count").toInt() > 2);
        list->setProperty("currentIndex", 0);
        QTest::keyClick(m_window, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(field()->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
    }

    void modifiedReturnClosesWithoutRunningTheCommand()
    {
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("dialog.close"));
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.close"),
                                      QStringLiteral("Ctrl+Return")), QString());
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        const QVariantList rows = switcher()->property("commandRows").value<QVariantList>();
        int discoverIndex = -1;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).toMap().value(QStringLiteral("id")).toString()
                == QStringLiteral("discover-rooms"))
                discoverIndex = i;
        }
        QVERIFY(discoverIndex >= 0);
        auto *list = m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherCommandList"));
        QVERIFY(list);
        list->setProperty("currentIndex", discoverIndex);
        QSignalSpy discovery(switcher(), SIGNAL(discoverRequested(QString)));
        QVERIFY(discovery.isValid());
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        QCOMPARE(discovery.count(), 0);
    }

    void closeCopiesSelectedQueryTextBeforeDismissing()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_G);
        QCOMPARE(field()->property("text").toString(), QStringLiteral("g"));
        QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(field()->property("selectedText").toString(), QStringLiteral("g"));
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("g"));
        QVERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_Right);
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());

        QCOMPARE(m_controller->shortcuts()->setBinding(QStringLiteral("dialog.close"),
                                                       QStringLiteral("Alt+L")), QString());
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        QCOMPARE(m_controller->shortcuts()->resetToDefault(QStringLiteral("dialog.close")), QString());
    }

    void tabWrapAroundScrollsTheFirstRowIntoView()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        (void) m_window->grabWindow();
        QQuickItem *list = resultList();
        QVERIFY(list);
        QTRY_VERIFY(list->property("count").toInt() > 0);
        const int count = list->property("count").toInt();
        const qreal viewH = list->height();
        qInfo() << "rows" << count << "viewport" << viewH;
        // The fixture must overflow, or nothing can scroll.
        QVERIFY2(count * 48 > viewH * 1.5, "mock list does not overflow the viewport");

        QMetaObject::invokeMethod(switcher(), "selectRow", Q_ARG(QVariant, count - 1));
        (void) m_window->grabWindow();
        QTRY_VERIFY(currentRowVisible());
        QVERIFY(list->property("contentY").toReal() > list->property("originY").toReal());

        // Tab from the last section wraps to the first row.
        QTest::keyClick(m_window, Qt::Key_Tab);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QTRY_COMPARE(list->property("contentY").toReal(),
                     list->property("originY").toReal());
        QVERIFY(currentRowVisible());
        QMetaObject::invokeMethod(switcher(), "close");
    }

    void pageKeysMoveTheSelectionAndKeepItVisible()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        (void) m_window->grabWindow();
        QQuickItem *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 0);
        const int count = list->property("count").toInt();
        QVariant stepV;
        QMetaObject::invokeMethod(switcher(), "pageRows",
                                  Q_RETURN_ARG(QVariant, stepV),
                                  Q_ARG(QVariant, QVariant::fromValue<QObject *>(list)));
        const int step = stepV.toInt();
        QVERIFY(step >= 1 && step < count - 1);

        QMetaObject::invokeMethod(switcher(), "selectRow", Q_ARG(QVariant, 0));
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QTRY_COMPARE(list->property("currentIndex").toInt(), step);
        QVERIFY(currentRowVisible());
        QTest::keyClick(m_window, Qt::Key_PageUp);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_End, Qt::ControlModifier);
        QTRY_COMPARE(list->property("currentIndex").toInt(), count - 1);
        QTRY_VERIFY(currentRowVisible());
        QTest::keyClick(m_window, Qt::Key_Home, Qt::ControlModifier);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(currentRowVisible());
        QMetaObject::invokeMethod(switcher(), "close");
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QuickSwitcherKeyboardQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "QuickSwitcherKeyboardQmlTest.moc"
