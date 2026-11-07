#include <QtTest/QtTest>

#include <memory>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"

class ActivityCenterPanelQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    struct Fixture {
        std::unique_ptr<SettingsManager> settings;
        std::unique_ptr<ShortcutRegistry> shortcuts;
        std::unique_ptr<QQmlApplicationEngine> engine;
        QObject *window = nullptr;
        QObject *panel = nullptr;
        QObject *app = nullptr;
        QObject *activity = nullptr;
        QQuickItem *list = nullptr;
        QStringList warnings;
    };

    bool make(Fixture &f, bool openPanel = true)
    {
        QSettings().clear();
        f.settings = std::make_unique<SettingsManager>();
        f.shortcuts = std::make_unique<ShortcutRegistry>(f.settings.get());
        f.engine = std::make_unique<QQmlApplicationEngine>();
        f.engine->rootContext()->setContextProperty("testShortcuts", f.shortcuts.get());
        connect(f.engine.get(), &QQmlEngine::warnings, this,
                [&f](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        f.warnings << e.toString();
                });
        QQmlComponent appComp(f.engine.get());
        // The raw QML below must hold no double slash (an mxc URI is split in
        // two): moc reads one as a comment, loses the string's end, and then
        // finds no class at all, so the suite links with no vtable.
        appComp.setData(R"QML(
import QtQuick
QtObject {
    property var shortcuts: testShortcuts
    property var roomList: QtObject { property bool roomFavouritesSupported: true }
    property string settingsSection: ""
    property var settings: QtObject { property string clockTimeFormat: "hh:mm" }
    property var stickers: QtObject {
        property int revision: 0
        function shortcodeForUrl(u) { return "" }
    }
    property var activity: ListModel {
        id: model
        property int unseenCount: 1
        property string filter: "all"
        property bool reactionsVisible: true
        property string searchQuery: ""
        property bool favouritesOnly: false
        property bool historyUnavailable: false
        property bool activityHasMore: true
        property bool threadPageLoading: false
        property bool threadPageFailed: false
        property int olderPageCalls: 0
        property int readHorizonDays: 60
        property var keywords: []
        property string lastOpened: ""
        property int openCalls: 0
        function open(id) { lastOpened = id; openCalls += 1 }
        function markAllSeen() {}
        function loadOlderActivity() { olderPageCalls += 1 }
        Component.onCompleted: {
            var now = Date.now()
            function add(id, kind, key, seen) {
                append({ entryId: id, kind: kind, roomId: "!r:x",
                         roomName: "Ops-Room", senderId: "@bob:x",
                         senderName: "Bob", preview: key, timestampMs: now,
                         eventId: "$" + id, threadRootId: "", seen: seen,
                         encrypted: false, reactionKey: key })
            }
            add("e1", "reaction", "%2", false)
            add("e2", "reaction", "mxc:/" + "/matrix.org/AbCdEfGhIjKlMno", true)
            add("e3", "mention", "hello", true)
            for (var i = 4; i < 40; ++i)
                add("e" + i, "mention", "m" + i, true)
        }
    }
    function showSettingsSection(s) { settingsSection = s }
    function showSettings() {}
}
)QML",
                         QUrl());
        f.app = appComp.create();
        if (!f.app) {
            qWarning() << appComp.errorString();
            return false;
        }
        f.app->setParent(f.engine.get());
        f.activity = f.app->property("activity").value<QObject *>();
        f.engine->rootContext()->setContextProperty("app", f.app);

        QQmlComponent comp(f.engine.get());
        comp.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    visible: true
    property int mainActionCalls: 0
    property int findCalls: 0
    Shortcut {
        sequence: "Ctrl+J"
        onActivated: mainActionCalls += 1
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: findCalls += 1
    }
    width: 900
    height: 800
    TextField { objectName: "composer"; focus: true; text: "draft" }
    ActivityCenterPanel { id: panel; objectName: "panel" }
    Shortcut {
        sequence: "Ctrl+I"
        enabled: !panel.visible
        onActivated: panel.openPanel()
    }
}
)QML",
                     QUrl());
        f.window = comp.create();
        if (!f.window) {
            qWarning() << comp.errorString();
            return false;
        }
        f.panel = f.window->findChild<QObject *>(QStringLiteral("panel"));
        if (!f.panel)
            return false;
        if (openPanel)
            QMetaObject::invokeMethod(f.panel, "openPanel");
        settle();
        f.list = f.panel->findChild<QQuickItem *>(QStringLiteral("activityList"));
        return f.list != nullptr;
    }

    static void settle()
    {
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents();
            QTest::qWait(20);
        }
    }

    static QQuickItem *findVisualItem(QQuickItem *item, const QString &name)
    {
        if (!item)
            return nullptr;
        if (item->objectName() == name)
            return item;
        for (QQuickItem *child : item->childItems()) {
            if (auto *found = findVisualItem(child, name))
                return found;
        }
        return nullptr;
    }

    static QQuickItem *row(const Fixture &f, int i)
    {
        QQuickItem *content = f.list->property("contentItem").value<QQuickItem *>();
        int n = 0;
        for (QQuickItem *c : content->childItems()) {
            if (c->objectName() == QLatin1String("activityRow")) {
                if (c->property("index").toInt() == i)
                    return c;
                ++n;
            }
        }
        return nullptr;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("activity-keyboard-test"));
    }

    void searchFieldFitsAndForwardsEdits()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *field = f.panel->findChild<QQuickItem *>(QStringLiteral("activitySearch"));
        QVERIFY(field);
        QVERIFY(field->width() > 100);
        QVERIFY(field->height() > 0);
        QVERIFY(field->width() <= f.panel->property("width").toReal());
        field->forceActiveFocus();
        for (const auto key : {Qt::Key_N, Qt::Key_E, Qt::Key_E, Qt::Key_D, Qt::Key_L, Qt::Key_E})
            QTest::keyClick(qobject_cast<QQuickWindow *>(f.window), key);
        QCOMPARE(f.activity->property("searchQuery").toString(), QStringLiteral("needle"));
        QMetaObject::invokeMethod(f.panel, "close");
        settle();
        QCOMPARE(f.activity->property("searchQuery").toString(), QString());
    }

    void typingFromTheListLandsInTheSearchField()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *field = f.panel->findChild<QQuickItem *>(QStringLiteral("activitySearch"));
        QVERIFY(w);
        QVERIFY(field);
        QVERIFY(f.list->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_B);
        QTest::keyClick(w, Qt::Key_O);
        QVERIFY(field->hasActiveFocus());
        QCOMPARE(f.activity->property("searchQuery").toString(), QStringLiteral("bo"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("bo"));
        QTest::keyClick(w, Qt::Key_Down);
        QVERIFY(f.list->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_Space);
        QCOMPARE(f.activity->property("openCalls").toInt(), 1);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void findKeyFocusesTheSearchFieldInsideTheModal()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *field = f.panel->findChild<QQuickItem *>(QStringLiteral("activitySearch"));
        QVERIFY(w);
        QVERIFY(field);
        QVERIFY(f.list->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(field->hasActiveFocus());
        QCOMPARE(f.window->property("findCalls").toInt(), 0);
        QCOMPARE(f.shortcuts->setBinding(QStringLiteral("room.find"),
                                         QStringLiteral("Ctrl+Alt+F")), QString());
        f.list->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(field->hasActiveFocus());
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void escapeClearsTheSearchBeforeItClosesTheDialog()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *field = f.panel->findChild<QQuickItem *>(QStringLiteral("activitySearch"));
        QVERIFY(w);
        QVERIFY(field);
        QTest::keyClick(w, Qt::Key_X);
        QVERIFY(field->hasActiveFocus());
        QCOMPARE(f.activity->property("searchQuery").toString(), QStringLiteral("x"));
        QTest::keyClick(w, Qt::Key_Escape);
        settle();
        QVERIFY(f.panel->property("opened").toBool());
        QCOMPARE(f.activity->property("searchQuery").toString(), QString());
        QCOMPARE(field->property("text").toString(), QString());
        QTest::keyClick(w, Qt::Key_Escape);
        QTRY_VERIFY(!f.panel->property("visible").toBool());

        QVERIFY(QMetaObject::invokeMethod(f.panel, "openPanel"));
        QTRY_VERIFY(f.panel->property("opened").toBool());
        f.activity->setProperty("searchQuery", QStringLiteral("ops"));
        f.list->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_Escape);
        settle();
        QVERIFY(f.panel->property("opened").toBool());
        QCOMPARE(f.activity->property("searchQuery").toString(), QString());
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void theFavouritesStarTogglesTheScopeWhereFavouritesExist()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);
        QQuickItem *star = findVisualItem(w->contentItem(),
                                          QStringLiteral("activityFavouritesOnly"));
        QVERIFY(star);
        QVERIFY(star->isVisible());
        QVERIFY(QMetaObject::invokeMethod(star, "clicked"));
        QCOMPARE(f.activity->property("favouritesOnly").toBool(), true);
        QVERIFY(star->property("active").toBool());
        QVERIFY(QMetaObject::invokeMethod(star, "clicked"));
        QCOMPARE(f.activity->property("favouritesOnly").toBool(), false);
        f.app->property("roomList").value<QObject *>()
            ->setProperty("roomFavouritesSupported", false);
        QTRY_VERIFY(!star->isVisible());
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void searchFieldStaysPutWhenNothingMatches()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *field = f.panel->findChild<QQuickItem *>(QStringLiteral("activitySearch"));
        QVERIFY(field);
        const qreal before = field->mapToScene(QPointF(0, 0)).y();
        f.activity->setProperty("searchQuery", QStringLiteral("zzzzqqq-no-match"));
        settle();
        QCOMPARE(field->mapToScene(QPointF(0, 0)).y(), before);
        auto *searchRow = f.panel->findChild<QQuickItem *>(QStringLiteral("activitySearchRow"));
        QVERIFY(searchRow);
        QCOMPARE(field->parentItem(), searchRow);
        for (const char *name : {"activityEmptyState", "activityHistoryUnavailable"}) {
            auto *line = f.panel->findChild<QQuickItem *>(QString::fromLatin1(name));
            QVERIFY(line);
            QVERIFY(line->parentItem() == searchRow->parentItem());
            const auto siblings = searchRow->parentItem()->childItems();
            QVERIFY2(siblings.indexOf(searchRow) < siblings.indexOf(line), name);
        }
        f.activity->setProperty("searchQuery", QString());
        settle();
        QCOMPARE(field->mapToScene(QPointF(0, 0)).y(), before);
    }

    void olderHighlightsCanBeLoadedFromTheMainActivityList()
    {
        Fixture f;
        QVERIFY(make(f));
        QObject *button = f.panel->findChild<QObject *>(QStringLiteral("activityLoadOlder"));
        QVERIFY(button);
        QVERIFY(button->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(f.activity->property("olderPageCalls").toInt(), 1);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void dialogKeysNavigateCloseAndStayInsideTheModal()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY(f.activity->setProperty("activityHasMore", false));
        QCoreApplication::processEvents();
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);
        QTest::keyClick(w, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);
        QTest::keyClick(w, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(f.list->property("currentIndex").toInt(), 0);
        QCOMPARE(f.window->property("mainActionCalls").toInt(), 0);
        QTest::keyClick(w, Qt::Key_Tab);
        QVERIFY(!f.list->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);
        QVERIFY(f.list->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY(!f.panel->property("visible").toBool());
        QCOMPARE(f.activity->property("openCalls").toInt(), 0);
        QTest::keyClick(w, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(f.window->property("mainActionCalls").toInt(), 1);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void dialogKeysUseLiveRebindingsAndHandleAnEmptyList()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);
        QCOMPARE(f.shortcuts->setBinding(QStringLiteral("dialog.nextItem"),
                                         QStringLiteral("Ctrl+N")), QString());
        QCOMPARE(f.shortcuts->setBinding(QStringLiteral("dialog.close"),
                                         QStringLiteral("Alt+L")), QString());
        QTest::keyClick(w, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(f.list->property("currentIndex").toInt(), 0);
        QTest::keyClick(w, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);
        QVERIFY(QMetaObject::invokeMethod(f.activity, "clear"));
        QTest::keyClick(w, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(f.list->property("count").toInt(), 0);
        QCOMPARE(f.activity->property("openCalls").toInt(), 0);
        QTest::keyClick(w, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(!f.panel->property("visible").toBool());
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void reactionsTabFollowsTheReactionRule()
    {
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);
        QVERIFY(findVisualItem(w->contentItem(),
                               QStringLiteral("activityFilter_reactions")));
        f.activity->setProperty("reactionsVisible", false);
        QTRY_VERIFY(!findVisualItem(w->contentItem(),
                                    QStringLiteral("activityFilter_reactions")));
        QVERIFY(findVisualItem(w->contentItem(),
                               QStringLiteral("activityFilter_invites")));
        f.activity->setProperty("reactionsVisible", true);
        QTRY_VERIFY(findVisualItem(w->contentItem(),
                                   QStringLiteral("activityFilter_reactions")));
    }

    void reboundKeysPreemptFocusedControls_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<QString>("focusName");
        QTest::addColumn<bool>("navigate");
        QTest::newRow("return-list") << int(Qt::Key_Return)
            << QStringLiteral("activityList") << false;
        QTest::newRow("enter-list") << int(Qt::Key_Enter)
            << QStringLiteral("activityList") << false;
        QTest::newRow("space-list") << int(Qt::Key_Space)
            << QStringLiteral("activityList") << false;
        QTest::newRow("return-button") << int(Qt::Key_Return)
            << QStringLiteral("activityMarkAllSeen") << false;
        QTest::newRow("return-filter") << int(Qt::Key_Return)
            << QStringLiteral("activityFilter_mentions") << false;
        QTest::newRow("page-list") << int(Qt::Key_PageDown)
            << QStringLiteral("activityList") << true;
    }

    void reboundKeysPreemptFocusedControls()
    {
        QFETCH(int, key);
        QFETCH(QString, focusName);
        QFETCH(bool, navigate);
        Fixture f;
        QVERIFY(make(f));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *focus = findVisualItem(w ? w->contentItem() : nullptr, focusName);
        QVERIFY(w);
        QVERIFY(focus);
        const QString action = navigate ? QStringLiteral("dialog.nextItem")
                                        : QStringLiteral("dialog.close");
        const QString sequence = f.shortcuts->sequenceFromKeyEvent(key, Qt::ControlModifier);
        QCOMPARE(f.shortcuts->setBinding(action, sequence), QString());
        focus->forceActiveFocus();
        QVERIFY(focus->hasActiveFocus());
        QTest::keyClick(w, static_cast<Qt::Key>(key), Qt::ControlModifier);
        if (navigate) {
            QCOMPARE(f.list->property("currentIndex").toInt(), 1);
            QVERIFY(f.panel->property("visible").toBool());
        } else {
            QTRY_VERIFY(!f.panel->property("visible").toBool());
        }
        QCOMPARE(f.activity->property("openCalls").toInt(), 0);
        QCOMPARE(f.activity->property("filter").toString(), QStringLiteral("all"));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void openingFromTheKeyboardFocusesTheList()
    {
        Fixture f;
        QVERIFY(make(f, false));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *composer = f.window->findChild<QQuickItem *>(QStringLiteral("composer"));
        QVERIFY(w);
        QVERIFY(composer);
        QVERIFY(composer->hasActiveFocus());

        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QVERIFY(f.list->hasActiveFocus());
        QCOMPARE(f.list->property("currentIndex").toInt(), 0);
        QTest::keyClick(w, Qt::Key_Down);
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);
        QTest::keyClick(w, Qt::Key_Up);
        QCOMPARE(f.list->property("currentIndex").toInt(), 0);
        QTest::keyClick(w, Qt::Key_Down);
        QTest::keyClick(w, Qt::Key_Return);
        QCOMPARE(f.activity->property("lastOpened").toString(), QStringLiteral("e2"));
        QCOMPARE(f.activity->property("openCalls").toInt(), 1);
        QTRY_VERIFY(!f.panel->property("visible").toBool());
        QCOMPARE(composer->property("text").toString(), QStringLiteral("draft"));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void reopeningReturnsFocusFromControlsToTheCurrentRow()
    {
        Fixture f;
        QVERIFY(make(f, false));
        QVERIFY(f.activity->setProperty("activityHasMore", false));
        QCoreApplication::processEvents();
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *composer = f.window->findChild<QQuickItem *>(QStringLiteral("composer"));
        QVERIFY(w);
        QVERIFY(composer);
        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QTest::keyClick(w, Qt::Key_Down);
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);

        QTest::keyClick(w, Qt::Key_Tab);
        QVERIFY(!f.list->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_Escape);
        QTRY_VERIFY(!f.panel->property("visible").toBool());
        QVERIFY(composer->hasActiveFocus());
        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QVERIFY(f.list->hasActiveFocus());
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);
        QTest::keyClick(w, Qt::Key_Down);
        QCOMPARE(f.list->property("currentIndex").toInt(), 2);
        QCOMPARE(f.activity->property("openCalls").toInt(), 0);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void thePlainOpenerLandsOnAllAfterTheThreadsOpener()
    {
        Fixture f;
        QVERIFY(make(f, false));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);

        QVERIFY(QMetaObject::invokeMethod(f.panel, "openThreads"));
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QCOMPARE(f.activity->property("filter").toString(), QStringLiteral("threads"));
        QVERIFY(f.panel->property("showingThreads").toBool());
        QTest::keyClick(w, Qt::Key_Escape);
        QTRY_VERIFY(!f.panel->property("visible").toBool());

        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QCOMPARE(f.activity->property("filter").toString(), QStringLiteral("all"));
        QVERIFY(!f.panel->property("showingThreads").toBool());

        QVERIFY(f.activity->setProperty("filter", QStringLiteral("replies")));
        QTest::keyClick(w, Qt::Key_Escape);
        QTRY_VERIFY(!f.panel->property("visible").toBool());
        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QCOMPARE(f.activity->property("filter").toString(), QStringLiteral("all"));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void anEmptyListCanBeDismissedWithoutOpeningAnEntry()
    {
        Fixture f;
        QVERIFY(make(f, false));
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *composer = f.window->findChild<QQuickItem *>(QStringLiteral("composer"));
        QVERIFY(w);
        QVERIFY(composer);
        QVERIFY(QMetaObject::invokeMethod(f.activity, "clear"));
        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QVERIFY(f.list->hasActiveFocus());
        QCOMPARE(f.list->property("count").toInt(), 0);
        QTest::keyClick(w, Qt::Key_Down);
        QTest::keyClick(w, Qt::Key_Return);
        QCOMPARE(f.activity->property("openCalls").toInt(), 0);
        QVERIFY(f.panel->property("opened").toBool());
        QTest::keyClick(w, Qt::Key_Escape);
        QTRY_VERIFY(!f.panel->property("visible").toBool());
        QVERIFY(composer->hasActiveFocus());
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void returnEnterAndSpaceOpenTheCurrentEntry()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
        f.list->forceActiveFocus();
        settle();
        QVERIFY(f.list->hasActiveFocus());
        QQuickWindow *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);

        const Qt::Key keys[] = {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Space};
        int expected = 0;
        for (Qt::Key k : keys) {
            f.list->setProperty("currentIndex", 2);
            QMetaObject::invokeMethod(f.panel, "open");
            settle();
            f.list->forceActiveFocus();
            f.activity->setProperty("lastOpened", QString());
            QTest::keyClick(w, k);
            settle();
            ++expected;
            QCOMPARE(f.activity->property("lastOpened").toString(),
                     QStringLiteral("e3"));
            QCOMPARE(f.activity->property("openCalls").toInt(), expected);
            QVERIFY2(!f.panel->property("opened").toBool(),
                     "opening an entry closes the panel, as a tap does");
        }
    }

    void thePressActionPathOpensTheEntry()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY(QMetaObject::invokeMethod(
            f.panel, "openEntry", Q_ARG(QVariant, QStringLiteral("e2"))));
        QCOMPARE(f.activity->property("lastOpened").toString(),
                 QStringLiteral("e2"));
    }

    void aReactionKeyOfPercentTwoCannotRewriteTheAccessibleName()
    {
        Fixture f;
        QVERIFY(make(f));
        QQuickItem *r = row(f, 0);
        QVERIFY2(r, "row 0 was not built");
        const QVariant v = r->property("a11yName");
        QVERIFY2(v.isValid(), "the row exposes no accessible name text");
        const QString name = v.toString();
        QCOMPARE(name.count(QStringLiteral("Ops-Room")), 1);
        QVERIFY2(name.contains(QStringLiteral("Reacted %2 to your message")),
                 qPrintable(name));
        QCOMPARE(name.count(QStringLiteral("%2")), 2);
        QVERIFY2(name.contains(QStringLiteral("Bob")), qPrintable(name));
    }

    void theNameCarriesUnseenStateAndTime()
    {
        Fixture f;
        QVERIFY(make(f));
        const QString unseen = row(f, 0)->property("a11yName").toString();
        const QString seen = row(f, 2)->property("a11yName").toString();
        QVERIFY2(unseen != seen, "seen and unseen rows read the same");
        QVERIFY2(unseen.startsWith(QStringLiteral("Unread")), qPrintable(unseen));
        QVERIFY2(!seen.startsWith(QStringLiteral("Unread")), qPrintable(seen));
        QVERIFY2(QRegularExpression(QStringLiteral("\\d\\d:\\d\\d")).match(seen).hasMatch(),
                 qPrintable(seen));
    }

    void aCustomEmojiReactionNeverShowsTheRawMxc()
    {
        Fixture f;
        QVERIFY(make(f));
        const QString name = row(f, 1)->property("a11yName").toString();
        QVERIFY2(!name.contains(QStringLiteral("mxc://")), qPrintable(name));
        QVariant label;
        QVERIFY(QMetaObject::invokeMethod(
            f.panel, "kindLabel", Q_RETURN_ARG(QVariant, label),
            Q_ARG(QVariant, QStringLiteral("reaction")),
            Q_ARG(QVariant, QStringLiteral("mxc://matrix.org/x"))));
        QVERIFY2(!label.toString().contains(QStringLiteral("mxc://")),
                 qPrintable(label.toString()));
    }

    void namesAreBidiIsolatedAndCannotCloseTheIsolate()
    {
        Fixture f;
        QVERIFY(make(f));
        QVariant out;
        QVERIFY(QMetaObject::invokeMethod(
            f.panel, "isolate", Q_RETURN_ARG(QVariant, out),
            Q_ARG(QVariant, QString(QChar(0x202E)) + QStringLiteral("evil")
                                + QChar(0x2069) + QStringLiteral("x"))));
        const QString s = out.toString();
        QVERIFY(s.startsWith(QChar(0x2068)));
        QVERIFY(s.endsWith(QChar(0x2069)));
        QCOMPARE(s.count(QChar(0x2069)), 1);
        QVERIFY(!s.contains(QChar(0x202E)));
        const QString name = row(f, 2)->property("a11yName").toString();
        QVERIFY2(name.contains(QChar(0x2068)), "sender/room are not isolated");
    }

    void thePanelHasNoKeywordEditorAndLinksToSettings()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY(!f.panel->findChild<QObject *>(
            QStringLiteral("activityKeywordsField")));
        QObject *btn = f.panel->findChild<QObject *>(
            QStringLiteral("activityOpenKeywordSettings"));
        QVERIFY(btn);
        QMetaObject::invokeMethod(btn, "clicked");
        QCOMPARE(f.app->property("settingsSection").toString(),
                 QStringLiteral("notifications"));
    }

    void homeEndAndPageKeysMoveTheCurrentRow()
    {
        Fixture f;
        QVERIFY(make(f));
        QQuickWindow *w = qobject_cast<QQuickWindow *>(f.window);
        QVERIFY(w);
        f.list->forceActiveFocus();
        settle();
        QTest::keyClick(w, Qt::Key_End);
        settle();
        QCOMPARE(f.list->property("currentIndex").toInt(),
                 f.list->property("count").toInt() - 1);
        QTest::keyClick(w, Qt::Key_PageUp);
        settle();
        const int afterPage = f.list->property("currentIndex").toInt();
        QVERIFY2(afterPage < f.list->property("count").toInt() - 2 && afterPage > 0, qPrintable(QString::number(afterPage)));
        QTest::keyClick(w, Qt::Key_Home);
        settle();
        QCOMPARE(f.list->property("currentIndex").toInt(), 0);
        QVERIFY(f.list->property("contentY").toReal() <= 1.0);
    }

    void focusingTheListBringsItsCurrentRowIntoView()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY(f.list->property("contentHeight").toReal()
                > f.list->height());
        auto *control = f.panel->findChild<QQuickItem *>(QStringLiteral("activityMarkAllSeen"));
        QVERIFY(control);
        control->forceActiveFocus();
        QVERIFY(!f.list->hasActiveFocus());
        QMetaObject::invokeMethod(f.list, "positionViewAtEnd");
        settle();
        QVERIFY(f.list->property("contentY").toReal() > 100);
        f.list->setProperty("currentIndex", 0);
        f.list->forceActiveFocus();
        settle();
        QVERIFY2(f.list->property("contentY").toReal() <= 1.0,
                 "the focused list must show its current row");
    }

    void theActivityListHasAMiddleClickScroller()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY(f.panel->findChild<QObject *>(
            QStringLiteral("activityMiddleClickScroller")));
    }

    void theOptionsMenuSetsTheReadHorizon()
    {
        Fixture f;
        QVERIFY(make(f));
        QObject *button = f.panel->findChild<QObject *>(
            QStringLiteral("activityOptionsButton"));
        QVERIFY(button);
        QObject *menu = f.panel->findChild<QObject *>(
            QStringLiteral("activityOptionsMenu"));
        QVERIFY(menu);
        auto rowAt = [menu](int i) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, i));
            return item;
        };
        QObject *d7 = rowAt(1);
        QObject *d60 = rowAt(4);
        QObject *never = rowAt(8);
        QVERIFY(d60 && d7 && never);
        QCOMPARE(d7->objectName(), QStringLiteral("activityHorizon7"));
        QCOMPARE(d60->objectName(), QStringLiteral("activityHorizon60"));
        QCOMPARE(never->objectName(), QStringLiteral("activityHorizon0"));
        QVERIFY(d60->property("radioSelected").toBool());
        QVERIFY(!d7->property("radioSelected").toBool());
        QVERIFY(QMetaObject::invokeMethod(d7, "triggered"));
        QCOMPARE(f.activity->property("readHorizonDays").toInt(), 7);
        settle();
        QVERIFY(d7->property("radioSelected").toBool());
        QVERIFY(QMetaObject::invokeMethod(never, "triggered"));
        QCOMPARE(f.activity->property("readHorizonDays").toInt(), 0);
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }

    void aServerWithoutHistoryIsSaidSoNotNothingHereYet()
    {
        Fixture f;
        QVERIFY(make(f));
        QObject *hint = f.panel->findChild<QObject *>(
            QStringLiteral("activityHistoryUnavailable"));
        QObject *empty = f.panel->findChild<QObject *>(
            QStringLiteral("activityEmptyState"));
        QVERIFY(hint);
        QVERIFY(empty);
        QVERIFY(!hint->property("visible").toBool());

        f.activity->setProperty("historyUnavailable", true);
        settle();
        QVERIFY(hint->property("visible").toBool());
        QVERIFY(!empty->property("visible").toBool());

        QMetaObject::invokeMethod(f.activity, "clear");
        settle();
        QVERIFY(empty->property("visible").toBool());
        QVERIFY(!hint->property("visible").toBool());
        const QString text = empty->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("not available from this server")),
                 qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("Nothing here yet")));

        f.activity->setProperty("filter", QStringLiteral("invites"));
        settle();
        QCOMPARE(empty->property("text").toString(),
                 QStringLiteral("No pending invites."));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }
};

QTEST_MAIN(ActivityCenterPanelQmlTest)
#include "ActivityCenterPanelQmlTest.moc"
