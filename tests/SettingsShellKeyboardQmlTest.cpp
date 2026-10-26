// Settings shell reach and keyboard rules (swarm chunk C63): the navigation
// column scrolls and its rows take Tab focus, a destructive confirm dialog
// starts on Cancel and hands focus back, Escape leaves a field before it
// leaves Settings, and the page scrolls and opens its copy-link menu from the
// keyboard. Production Main window, mock backend.
#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QClipboard>
#include <QScopeGuard>
#include "app/ShortcutRegistry.h"
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

    void cleanup()
    {
        m_controller->showMain();
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

    void enterAndSpaceOnABranchRowSelectAndToggleIt()
    {
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        auto *row = item("settingsNavRow_privacy");
        auto *outline = item("settingsNavOutline_privacy");
        QVERIFY(screen && row && outline);
        screen->setProperty("expandedOutlines",
                            QVariantMap{{QStringLiteral("appearance"), true}});
        QCoreApplication::processEvents();
        QVERIFY(!outline->isVisible());
        row->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(row->hasActiveFocus());

        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        QTRY_VERIFY(outline->isVisible());
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        QTRY_VERIFY(!outline->isVisible());
        QTest::keyClick(m_window, Qt::Key_Space);
        QTRY_VERIFY(outline->isVisible());
        QTest::keyClick(m_window, Qt::Key_Space);
        QTRY_VERIFY(!outline->isVisible());
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

    void dialogKeysSwitchSectionsAndKeepTabAvailable()
    {
        openSection(QStringLiteral("account"));
        auto *screen = item("settingsScreenRoot");
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        search->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        auto *privacy = item("settingsNavRow_privacy");
        QVERIFY(privacy);
        QVERIFY(privacy->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Tab);
        QVERIFY(!privacy->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("account"));
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("account"));
        screen->setProperty("section", QStringLiteral("about"));
        item("settingsNavRow_about")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("about"));
    }

    void sectionNavigationSkipsFilteredRows()
    {
        openSection(QStringLiteral("account"));
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        search->setProperty("text", QStringLiteral("cursor blinking"));
        search->forceActiveFocus();
        QTRY_VERIFY(!item("settingsNavRow_privacy")->isVisible());
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(item("settingsScreenRoot")->property("section").toString(),
                 QStringLiteral("appearance"));
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(item("settingsScreenRoot")->property("section").toString(),
                 QStringLiteral("appearance"));
        search->setProperty("text", QString());
    }

    void closePreservesCopyAndPeelsSearchBeforeLeaving()
    {
        openSection(QStringLiteral("appearance"));
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        search->setProperty("text", QStringLiteral("cursor"));
        search->forceActiveFocus();
        QVERIFY(QMetaObject::invokeMethod(search, "selectAll"));
        QGuiApplication::clipboard()->clear();
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("cursor"));
        QCOMPARE(search->property("text").toString(), QStringLiteral("cursor"));
        QVERIFY(QMetaObject::invokeMethod(search, "deselect"));
        QTRY_COMPARE(search->property("selectedText").toString(), QString());
        QVERIFY(search->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(search->property("text").toString(), QString());
        QCOMPARE(m_controller->currentScreen(), AppController::SettingsScreen);
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(m_controller->currentScreen(), AppController::MainScreen);
    }

    void closeLeavesAContentEditorBeforeSettings()
    {
        openSection(QStringLiteral("appearance"));
        auto *box = item("textScalePercentBox");
        QVERIFY(box);
        const QString before = box->property("text").toString();
        box->forceActiveFocus();
        QTRY_VERIFY(box->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_7);
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(box->property("text").toString(), before);
        QCOMPARE(m_controller->currentScreen(), AppController::SettingsScreen);
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(m_controller->currentScreen(), AppController::MainScreen);
    }

    void reboundDialogKeysWorkAndCaptureKeepsTheChord()
    {
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("dialog.nextItem"));
            shortcuts->resetToDefault(QStringLiteral("dialog.close"));
        });
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.nextItem"),
                                      QStringLiteral("Ctrl+Down")), QString());
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.close"),
                                      QStringLiteral("Ctrl+Return")), QString());
        openSection(QStringLiteral("account"));
        item("settingsSearchField")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(item("settingsScreenRoot")->property("section").toString(),
                 QStringLiteral("privacy"));
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(m_controller->currentScreen(), AppController::MainScreen);

        openSection(QStringLiteral("shortcuts"));
        auto *change = item("shortcutChange_dialog.close");
        QVERIFY(change);
        QQuickItem *row = change;
        while (row && row->property("actionId").toString() != QStringLiteral("dialog.close"))
            row = row->parentItem();
        QVERIFY(row);
        QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
        QTRY_VERIFY(item("shortcutCaptureSink_dialog.close")->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(shortcuts->sequenceFor(QStringLiteral("dialog.close")),
                 QStringLiteral("Ctrl+C"));
        QCOMPARE(m_controller->currentScreen(), AppController::SettingsScreen);
    }

    void pageTabsAndSubsectionKeysRevealTheirTarget()
    {
        auto *shortcuts = m_controller->shortcuts();
        const QStringList ids = {QStringLiteral("settings.nextPage"), QStringLiteral("settings.previousPage"),
            QStringLiteral("settings.nextSubsection"), QStringLiteral("settings.previousSubsection")};
        const auto restore = qScopeGuard([&] {
            for (const QString &id : ids)
                shortcuts->resetToDefault(id);
        });
        const QStringList keys = {QStringLiteral("Tab"), QStringLiteral("Shift+Tab"),
                                   QStringLiteral("Ctrl+J"), QStringLiteral("Ctrl+K")};
        for (int i = 0; i < ids.size(); ++i)
            QCOMPARE(shortcuts->setBinding(ids[i], keys[i]), QString());
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        auto *search = item("settingsSearchField");
        search->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_Tab);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("shortcuts"));
        QTest::keyClick(m_window, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(screen->property("section").toString(), QStringLiteral("appearance"));
        auto *flick = item("settingsContentFlick");
        QVERIFY(flick);
        flick->setProperty("contentY", 0.0);
        flick->forceActiveFocus();
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(screen, "refreshOutlineActive"));
        const int before = screen->property("outlineActive").toInt();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        const int next = screen->property("outlineActive").toInt();
        QVERIFY(next > before);
        const QString anchor = screen->property("searchRevealAnchor").toString();
        QVERIFY(!anchor.isEmpty());
        QTRY_VERIFY(flick->property("contentY").toReal() > 0);
        QVERIFY(m_window->activeFocusItem()->objectName().startsWith(QStringLiteral("settingsNavOutlineEntry_")));
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(screen->property("outlineActive").toInt(), before);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(screen->property("outlineActive").toInt(), before);
        QTest::keyClick(m_window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(search->hasActiveFocus());
        search->setProperty("text", QStringLiteral("cursor"));
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QTRY_COMPARE(search->property("text").toString(), QString());
        QTRY_VERIFY(m_window->activeFocusItem()->objectName().startsWith(QStringLiteral("settingsNavOutlineEntry_")));
    }

    void settingsPageTabsCanBeCapturedFromTheConfigMenu()
    {
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("settings.nextPage"));
            shortcuts->resetToDefault(QStringLiteral("settings.previousPage"));
        });
        openSection(QStringLiteral("shortcuts"));
        for (const QString &id : {QStringLiteral("settings.nextPage"), QStringLiteral("settings.previousPage")}) {
            const QByteArray name = (QStringLiteral("shortcutChange_") + id).toUtf8();
            auto *change = item(name.constData());
            QVERIFY(change);
            QQuickItem *row = change;
            while (row && row->property("actionId").toString() != id)
                row = row->parentItem();
            QVERIFY(row);
            QVERIFY(QMetaObject::invokeMethod(row, "beginCapture"));
            const bool previous = id == QStringLiteral("settings.previousPage");
            QTest::keyClick(m_window, previous ? Qt::Key_Backtab : Qt::Key_Tab,
                            previous ? Qt::ShiftModifier : Qt::NoModifier);
            QCOMPARE(shortcuts->sequenceFor(id), previous ? QStringLiteral("Shift+Tab") : QStringLiteral("Tab"));
            QVERIFY(!row->property("capturing").toBool());
            QCOMPARE(m_controller->currentScreen(), AppController::SettingsScreen);
        }
    }

    void manualHalfPagesScrollContentWithoutMovingFocus()
    {
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("settings.halfPageUp"));
            shortcuts->resetToDefault(QStringLiteral("settings.halfPageDown"));
        });
        QCOMPARE(shortcuts->setBinding(QStringLiteral("settings.halfPageUp"), QStringLiteral("Ctrl+U")), QString());
        QCOMPARE(shortcuts->setBinding(QStringLiteral("settings.halfPageDown"), QStringLiteral("Ctrl+D")), QString());
        openSection(QStringLiteral("appearance"));
        auto *flick = item("settingsContentFlick");
        auto *nav = item("settingsNavFlick");
        auto *row = item("settingsNavRow_appearance");
        QVERIFY(flick);
        QVERIFY(nav);
        QVERIFY(row);
        row->forceActiveFocus();
        flick->setProperty("contentY", 0.0);
        const qreal navY = nav->property("contentY").toReal();
        const qreal half = flick->height() / 2;
        QTest::keyClick(m_window, Qt::Key_D, Qt::ControlModifier);
        QVERIFY(qAbs(flick->property("contentY").toReal() - half) < 2);
        QVERIFY(row->hasActiveFocus());
        QCOMPARE(nav->property("contentY").toReal(), navY);
        QCOMPARE(item("settingsScreenRoot")->property("section").toString(), QStringLiteral("appearance"));
        QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
        QVERIFY(qAbs(flick->property("contentY").toReal()) < 2);
        QTest::keyClick(m_window, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        const qreal maxY = flick->property("contentHeight").toReal() - flick->height();
        flick->setProperty("contentY", maxY);
        QTest::keyClick(m_window, Qt::Key_D, Qt::ControlModifier);
        QVERIFY(qAbs(flick->property("contentY").toReal() - maxY) < 2);
        QVERIFY(row->hasActiveFocus());
    }

    void nestedDialogKeepsSettingsKeys()
    {
        openSection(QStringLiteral("account"));
        QObject *dialog = nullptr;
        for (QObject *o : m_window->findChildren<QObject *>()) {
            if (o->property("title").toString() == QLatin1String("Clear your display name?")) {
                dialog = o;
                break;
            }
        }
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(item("confirmDialogCancel")->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(item("settingsScreenRoot")->property("section").toString(),
                 QStringLiteral("account"));
        QCOMPARE(m_controller->currentScreen(), AppController::SettingsScreen);
        QVERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
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
