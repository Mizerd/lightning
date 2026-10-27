// Offscreen behaviour test for the two-mode quick switcher: mode transitions
// ('>' prefix in, lone backspace out, openCommandMode()), the command action
// list (settings/sections/accounts/themes only, never leave/mute/files/
// sign-out verbs), a real end-to-end theme action against settings.theme,
// Escape dismissal, and a source pin that navigate-mode invite routing opens
// (never accepts) invites.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTranslator>

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

namespace {

// A German-like translator with deliberately FREE word choices: the hint must
// be built from the real labels, so no translation may break its examples.
class FreeGermanTranslator : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation, int n) const override
    {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (qstrcmp(context, "QuickSwitcher") != 0)
            return {};
        const QString src = QString::fromUtf8(sourceText);
        if (src == QLatin1String("Theme: %1"))
            return QStringLiteral("Design: %1");
        if (src == QLatin1String("Open %1"))
            return QStringLiteral("%1 \u00f6ffnen");
        if (src == QLatin1String("Switch to %1"))
            return QStringLiteral("Zu %1 wechseln");
        if (src == QLatin1String("Privacy & security"))
            return QStringLiteral("Datenschutz & Sicherheit");
        if (src == QLatin1String("Try %1"))
            return QStringLiteral("Versuche %1");
        return {};
    }
    bool isEmpty() const override { return false; }
};

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

        // Offscreen nothing requests a frame after the popup opens, and
        // ListView delegate creation rides a frame's polish pass, so force one.
        // Rows materialize a tick later.
        (void) m_window->grabWindow();
        // Row items materialize a tick after count updates — harvest until
        // the first action is visible.
        QTRY_VERIFY(commandTitles().contains(QStringLiteral("Open Settings")));
        const QStringList titles = commandTitles();
        QVERIFY(titles.contains(QStringLiteral("Saved messages")));
        QVERIFY(titles.contains(QStringLiteral("Threads activity")));
        QVERIFY(titles.contains(QStringLiteral("Use compact layout"))
                || titles.contains(QStringLiteral("Use Lightning layout")));
        QVERIFY(titles.contains(QStringLiteral("Return to room")));
        QVERIFY(titles.contains(QStringLiteral("Open thread root in room")));
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
        // The 11 real themes (incl. Storm) plus "Match system" — sourced
        // from AppTheme.themeList, so a new theme joins automatically.
        QCOMPARE(themeActions, 12);
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

private:
    int rowsFor(const QString &query)
    {
        field()->setProperty("text", query);
        return switcher()->property("commandRows").value<QVariantList>().size();
    }

    // Every example the footer shows, typed verbatim, must find an action.
    void checkHintExamplesWork(int minExamples)
    {
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        QTRY_VERIFY(commandCount() > 0);
        const QStringList examples =
            switcher()->property("commandHintExamples").toStringList();
        QVERIFY2(examples.size() >= minExamples, qPrintable(examples.join('|')));
        const QString hint = switcher()->property("commandHintText").toString();
        for (const QString &example : examples) {
            QVERIFY2(hint.contains(example), qPrintable(example));
            QVERIFY2(rowsFor(example) >= 1,
                     qPrintable(QStringLiteral("no action for hint example: ")
                                + example));
        }
        field()->setProperty("text", QString());
        closeSwitcher();
    }

private slots:
    void everyWordMatchesInAnyOrderIgnoringCasePunctuationAndAccents()
    {
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        QTRY_VERIFY(commandCount() > 0);
        QVERIFY(rowsFor(QStringLiteral("theme indigo night")) >= 1);
        QVERIFY(rowsFor(QStringLiteral("night indigo theme")) >= 1);
        QVERIFY(rowsFor(QStringLiteral("THEME: Indigo-Night")) >= 1);
        QVERIFY(rowsFor(QStringLiteral("indigo-night")) >= 1);
        QVERIFY(rowsFor(QStringLiteral("open  privacy")) >= 1);
        QVERIFY(rowsFor(QStringLiteral("security privacy")) >= 1);
        QCOMPARE(rowsFor(QStringLiteral("indigo zzzz")), 0);
        // Single word keeps substring behaviour.
        QVERIFY(rowsFor(QStringLiteral("priv")) >= 1);
        field()->setProperty("text", QString());
        closeSwitcher();
    }

    void hintExamplesWorkVerbatimInEnglish()
    {
        checkHintExamplesWork(2);
    }

    void switchExampleIsOfferedAndWorksWithAnotherAccount()
    {
        m_controller->settings()->saveSession(
            QStringLiteral("https://one.example.org"),
            QStringLiteral("@bob:example.org"), QStringLiteral("BOBDEV"),
            QStringLiteral("bob-token-fixture"));
        m_controller->settings()->saveSession(
            QStringLiteral("https://two.example.org"),
            QStringLiteral("@alice:example.org"), QStringLiteral("ALICEDEV"),
            QStringLiteral("alice-token-fixture"));
        m_controller->accounts()->setActiveUser(QStringLiteral("@bob:example.org"));
        m_controller->accounts()->updateProfile(
            QStringLiteral("@alice:example.org"), QStringLiteral("alice"),
            QString());
        QTRY_VERIFY(switcher()->property("hasOtherAccount").toBool());
        FreeGermanTranslator de;
        QVERIFY(QCoreApplication::installTranslator(&de));
        m_engine->retranslate();
        checkHintExamplesWork(3);
        QVERIFY(switcher()->property("commandHintText").toString()
                    .contains(QStringLiteral("Zu alice wechseln")));
        QCoreApplication::removeTranslator(&de);
        m_engine->retranslate();
        checkHintExamplesWork(3);
    }

    void hintExamplesWorkVerbatimInAFreelyTranslatedGerman()
    {
        FreeGermanTranslator de;
        QVERIFY(QCoreApplication::installTranslator(&de));
        m_engine->retranslate();
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("commandMode").toBool());
        const QString hint = switcher()->property("commandHintText").toString();
        QVERIFY2(hint.startsWith(QStringLiteral("Versuche")), qPrintable(hint));
        closeSwitcher();
        checkHintExamplesWork(2);
        QCoreApplication::removeTranslator(&de);
        m_engine->retranslate();
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
