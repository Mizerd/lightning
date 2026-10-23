// The Activity Center panel, loaded offscreen against a STUB `app` (a real QML
// object, so a call the panel makes that the stub lacks is a TypeError the
// suite sees). Covers: an entry opens from the keyboard (Return, Enter and
// Space on the focused list) and from the screen-reader press action; the
// accessible name is built in ONE pass, so a reaction key or name containing
// a place-marker cannot rewrite it; unseen state and time are in the name; a
// custom-emoji (mxc) reaction never shows the raw URI; interpolated names are
// bidi-isolated; the panel has no keyword editor of its own; and a server
// that cannot list past highlights is said so, never "Nothing here yet".
//
// Run offscreen only (QT_QPA_PLATFORM=offscreen), never bare.
#include <QtTest/QtTest>

#include <memory>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>

class ActivityCenterPanelQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Fixture {
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
        f.engine = std::make_unique<QQmlApplicationEngine>();
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
        property bool historyUnavailable: false
        property var keywords: []
        property string lastOpened: ""
        property int openCalls: 0
        function open(id) { lastOpened = id; openCalls += 1 }
        function markAllSeen() {}
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

    static QQuickItem *row(const Fixture &f, int i)
    {
        QQuickItem *content = f.list->property("contentItem").value<QQuickItem *>();
        int n = 0;
        for (QQuickItem *c : content->childItems()) {
            if (c->objectName() == QLatin1String("activityRow")) {
                // children are not ordered by index; match the role.
                if (c->property("index").toInt() == i)
                    return c;
                ++n;
            }
        }
        return nullptr;
    }

private Q_SLOTS:
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
        auto *w = qobject_cast<QQuickWindow *>(f.window);
        auto *composer = f.window->findChild<QQuickItem *>(QStringLiteral("composer"));
        QVERIFY(w);
        QVERIFY(composer);
        QTest::keyClick(w, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.panel->property("opened").toBool());
        QTest::keyClick(w, Qt::Key_Down);
        QCOMPARE(f.list->property("currentIndex").toInt(), 1);

        // Tab still reaches the dialog's controls; reopening must not leave
        // arrows on whichever control had focus when the dialog was closed.
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
        // Accessible.onPressAction calls this same function.
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
        // Kept literally where the panel puts it: in the kind label
        // ("Reacted %2 to your message") and as the row's body. Never
        // substituted: the room name above appears once.
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
        // The time is in the name: a clock time "hh:mm" for today's entry.
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

    // C08-F03: Home/End/PageUp/PageDown moved nothing (keyNavigationEnabled
    // covers Up/Down) and a Tab-focused list did not follow its current row.
    // Old code: currentIndex stays 0 after End.
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
        // The panel now focuses the list on opening. Move focus away first
        // so this test still exercises a later focus transition.
        auto *control = f.panel->findChild<QQuickItem *>(QStringLiteral("activityMarkAllSeen"));
        QVERIFY(control);
        control->forceActiveFocus();
        QVERIFY(!f.list->hasActiveFocus());
        // Scroll away from row 0, then give the list focus.
        QMetaObject::invokeMethod(f.list, "positionViewAtEnd");
        settle();
        QVERIFY(f.list->property("contentY").toReal() > 100);
        f.list->setProperty("currentIndex", 0);
        f.list->forceActiveFocus();
        settle();
        QVERIFY2(f.list->property("contentY").toReal() <= 1.0,
                 "the focused list must show its current row");
    }

    // C08-F02: middle-click autoscroll exists on the Activity list.
    void theActivityListHasAMiddleClickScroller()
    {
        Fixture f;
        QVERIFY(make(f));
        QVERIFY(f.panel->findChild<QObject *>(
            QStringLiteral("activityMiddleClickScroller")));
    }

    // Break-it rerun, section 10: after a restart on a server without
    // GET /notifications the list said "Nothing here yet" while the room list
    // showed @2/@3/@5. Old code: neither label existed.
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
        // History available: no hint over the rows.
        QVERIFY(!hint->property("visible").toBool());

        f.activity->setProperty("historyUnavailable", true);
        settle();
        // Rows present: one line above them.
        QVERIFY(hint->property("visible").toBool());
        QVERIFY(!empty->property("visible").toBool());

        // Nothing listed: the empty state says why, not "Nothing here yet".
        QMetaObject::invokeMethod(f.activity, "clear");
        settle();
        QVERIFY(empty->property("visible").toBool());
        QVERIFY(!hint->property("visible").toBool());
        const QString text = empty->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("not available from this server")),
                 qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("Nothing here yet")));

        // Invites do not come from that history.
        f.activity->setProperty("filter", QStringLiteral("invites"));
        settle();
        QCOMPARE(empty->property("text").toString(),
                 QStringLiteral("No pending invites."));
        QVERIFY2(f.warnings.isEmpty(), qPrintable(f.warnings.join('\n')));
    }
};

QTEST_MAIN(ActivityCenterPanelQmlTest)
#include "ActivityCenterPanelQmlTest.moc"
