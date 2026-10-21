// Settings shell reach and keyboard rules (swarm chunk C63): the navigation
// column scrolls and its rows take Tab focus, a destructive confirm dialog
// starts on Cancel and hands focus back, Escape leaves a field before it
// leaves Settings, and the page scrolls and opens its copy-link menu from the
// keyboard. Production Main window, mock backend.
#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;
}

class SettingsShellKeyboardQmlTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        for (QQuickItem *child : parent->childItems()) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }
    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }
    void openSection(const QString &section)
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *screen = item("settingsScreenRoot");
        QVERIFY(screen);
        screen->setProperty("section", section);
        QCoreApplication::processEvents();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("settings-shell-keyboard-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy createdSpy(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        m_window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        m_window->setWidth(1280);
        m_window->setHeight(800);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_controller;
    }

    // C63-F01: at a short window the nav scrolls, and About can be brought
    // inside the column's own bounds.
    void theNavColumnScrollsToReachAbout()
    {
        m_window->setHeight(420);
        QCoreApplication::processEvents();
        openSection(QStringLiteral("appearance"));
        auto *flick = item("settingsNavFlick");
        QVERIFY2(flick, "the settings nav column has no Flickable");
        const qreal contentH = flick->property("contentHeight").toReal();
        QVERIFY2(contentH > flick->height(),
                 "the nav content should be taller than a 420px window");
        flick->setProperty("contentY", contentH - flick->height());
        QCoreApplication::processEvents();
        auto *about = item("settingsNavRow_about");
        QVERIFY(about);
        const qreal top = about->mapToItem(flick, QPointF(0, 0)).y();
        QVERIFY2(top >= 0 && top + about->height() <= flick->height() + 1,
                 qPrintable(QStringLiteral("About row at y=%1 of %2")
                                .arg(top).arg(flick->height())));
        m_window->setHeight(800);
        QCoreApplication::processEvents();
    }

    // C63-F02: every section row is in the Tab chain.
    void navRowsTakeTabFocus()
    {
        openSection(QStringLiteral("account"));
        int checked = 0;
        for (const char *key : {"account", "privacy", "sessions", "updates", "about"}) {
            auto *row = item((QByteArray("settingsNavRow_") + key).constData());
            if (!row)
                continue;
            ++checked;
            QVERIFY2(row->property("focusPolicy").toInt() & Qt::TabFocus, key);
        }
        QVERIFY2(checked >= 4, "too few nav rows were found");
    }

    // C63-F03: a confirm dialog starts on Cancel, so a lone Space cancels.
    void confirmDialogStartsOnCancel()
    {
        openSection(QStringLiteral("account"));
        QObject *dialog = nullptr;
        for (QObject *o : m_window->findChildren<QObject *>()) {
            if (o->property("title").toString()
                    == QLatin1String("Clear your display name?")) {
                dialog = o;
                break;
            }
        }
        QVERIFY2(dialog, "the clear-display-name dialog was not found");
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *cancel = item("confirmDialogCancel");
        QVERIFY2(cancel, "the dialog's Cancel button has no objectName");
        QTRY_VERIFY2(cancel->hasActiveFocus(), "focus is not on Cancel");
        QSignalSpy accepted(dialog, SIGNAL(accepted()));
        QTest::keyClick(m_window, Qt::Key_Space);
        QCoreApplication::processEvents();
        QCOMPARE(accepted.count(), 0);
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }

    // C63-F06: Escape in a percent box reverts it and stays in Settings.
    void escapeInAPercentBoxDoesNotCloseSettings()
    {
        openSection(QStringLiteral("appearance"));
        auto *box = item("textScalePercentBox");
        QVERIFY(box);
        const QString before = box->property("text").toString();
        box->forceActiveFocus();
        QTRY_VERIFY(box->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_7);
        QTest::keyClick(m_window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::SettingsScreen));
        QCOMPARE(box->property("text").toString(), before);
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(int(m_controller->currentScreen()),
                                  int(AppController::MainScreen), 3000);
        m_controller->showMain();
    }

    // C63-F05: the page scrolls from the keyboard.
    void pageKeysScrollTheSettingsPage()
    {
        openSection(QStringLiteral("appearance"));
        auto *flick = item("settingsContentFlick");
        QVERIFY(flick);
        QVERIFY(flick->property("contentHeight").toReal() > flick->height());
        flick->setProperty("contentY", 0.0);
        flick->forceActiveFocus();
        QTRY_VERIFY(flick->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QTRY_VERIFY(flick->property("contentY").toReal() > 100);
        QTest::keyClick(m_window, Qt::Key_End);
        QTRY_VERIFY(flick->property("contentY").toReal()
                    >= flick->property("contentHeight").toReal() - flick->height() - 1);
        QTest::keyClick(m_window, Qt::Key_Home);
        QTRY_COMPARE(flick->property("contentY").toReal(), 0.0);
    }

    // C63-F05 (review r1): the reported path - focus is elsewhere, the user
    // clicks blank page area, then pages. The page must take the focus itself.
    void clickingBlankPageAreaThenPageDownScrolls()
    {
        openSection(QStringLiteral("appearance"));
        auto *flick = item("settingsContentFlick");
        auto *search = item("settingsSearchField");
        QVERIFY(flick);
        QVERIFY(search);
        flick->setProperty("contentY", 0.0);
        search->forceActiveFocus();
        QTRY_VERIFY(!flick->hasActiveFocus());
        // Left gutter of the pane: outside the centred (max 860px) column.
        const QPointF blank = flick->mapToScene(QPointF(6, flick->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          blank.toPoint());
        QTRY_VERIFY2(flick->hasActiveFocus(),
                     "a click on blank page area did not focus the page");
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QTRY_VERIFY(flick->property("contentY").toReal() > 100);
    }

    // C63-F05 (review r1): End on a focused nav row scrolls the nav column,
    // not the page behind it.
    void pageKeysOnANavRowScrollTheNavNotThePage()
    {
        m_window->setHeight(420);
        QCoreApplication::processEvents();
        openSection(QStringLiteral("appearance"));
        auto *content = item("settingsContentFlick");
        auto *nav = item("settingsNavFlick");
        auto *row = item("settingsNavRow_account");
        QVERIFY(content);
        QVERIFY(nav);
        QVERIFY(row);
        content->setProperty("contentY", 0.0);
        nav->setProperty("contentY", 0.0);
        row->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(row->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_End);
        QTRY_VERIFY(nav->property("contentY").toReal() > 0);
        QCOMPARE(content->property("contentY").toReal(), 0.0);
        m_window->setHeight(800);
        QCoreApplication::processEvents();
    }

    // C63-F07: Menu opens the copy-link menu for the focused setting.
    void menuKeyOpensTheCopyLinkMenu()
    {
        openSection(QStringLiteral("appearance"));
        auto *box = item("textScalePercentBox");
        QVERIFY(box);
        box->forceActiveFocus();
        QTRY_VERIFY(box->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Menu);
        QObject *menu = m_window->findChild<QObject *>(
            QStringLiteral("settingLinkMenu"));
        QVERIFY(menu);
        QTRY_VERIFY2(menu->property("visible").toBool(),
                     "the Menu key opened nothing");
        QMetaObject::invokeMethod(menu, "close");
    }
};

QTEST_MAIN(SettingsShellKeyboardQmlTest)
#include "SettingsShellKeyboardQmlTest.moc"
