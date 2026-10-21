// Room information panel: keyboard operation, Escape, empty states, scrolling
// (swarm C33). The real RoomInfoPanel against the mock backend, driven with
// real key events; nothing here is a text scan.
//
//  * Pinned rows are tab stops, Enter and Space jump to the message, a Tab
//    onto a row below the fold scrolls it into view, the list has a scrollbar;
//  * member rows open on Enter, and Shift+F10 / Menu opens the member menu;
//  * Escape inside a text field leaves the field and does NOT reach the host's
//    close-the-panel shortcut (a ShortcutOverride case);
//  * a filter that matches nobody says so;
//  * an open combo popup freezes the Overview's wheel scrolling.

#include <QtTest/QtTest>

#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

#include "app/AppController.h"
#include "app/PinnedMessagesController.h"
#include "app/RoomInfoController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 420
    height: 600
    visible: true
    property alias panel: p
    RoomInfoPanel {
        id: p
        width: 360
        height: 600
    }
}
)QML";

const QString kRoom = QStringLiteral("!general:mock.local");

// A mock whose pinned-events and roster reads are answered by the test (the
// stock mock's rooms have no members and it supports no pins).
class TestMock final : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;
    quint64 nextOp = 1000;
    quint64 lastOp = 0;
    QVariantList roster;
    quint64 requestRoomMembers(const QString &roomId) override
    {
        const quint64 op = nextOp++;
        QVariantMap snap;
        snap.insert(QStringLiteral("ok"), true);
        snap.insert(QStringLiteral("members"), roster);
        QTimer::singleShot(0, this, [this, op, roomId, snap] {
            Q_EMIT roomMembersReceived(op, roomId, snap);
        });
        return op;
    }
    bool supportsPinnedMessages() const override { return true; }
    bool supportsRoomManagement() const override { return true; }
    quint64 requestPinnedMessages(const QString &, bool) override
    {
        lastOp = nextOp++;
        return lastOp;
    }
};

QVariantMap pin(const QString &eventId, bool available)
{
    QVariantMap e;
    e.insert(QStringLiteral("eventId"), eventId);
    e.insert(QStringLiteral("available"), available);
    if (available) {
        e.insert(QStringLiteral("sender"), QStringLiteral("@bob:mock.local"));
        e.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Bob"));
        e.insert(QStringLiteral("senderAvatarUrl"), QString());
        e.insert(QStringLiteral("timestampMs"), qlonglong(1700000000000LL));
        e.insert(QStringLiteral("kind"), QStringLiteral("text"));
        e.insert(QStringLiteral("preview"), QStringLiteral("note ") + eventId);
    }
    return e;
}

} // namespace

class RoomInfoKeyboardQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;
    std::unique_ptr<AppController> m_controller;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    std::unique_ptr<QObject> m_window;
    std::unique_ptr<TestMock> m_mock;
    QQuickItem *m_panel = nullptr;
    QQuickWindow *m_quick = nullptr;

    static void walk(QQuickItem *parent, QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            out.append(child);
            walk(child, out);
        }
    }
    QList<QQuickItem *> all() const
    {
        QList<QQuickItem *> out;
        walk(m_panel, out);
        return out;
    }
    QQuickItem *itemNamed(const QString &name) const
    {
        for (QQuickItem *it : all())
            if (it->objectName() == name)
                return it;
        return nullptr;
    }
    QList<QQuickItem *> itemsNamed(const QString &name) const
    {
        QList<QQuickItem *> out;
        for (QQuickItem *it : all())
            if (it->objectName() == name)
                out.append(it);
        return out;
    }
    QQuickItem *textFieldWithPlaceholder(const QString &text) const
    {
        for (QQuickItem *it : all())
            if (it->property("placeholderText").toString() == text)
                return it;
        return nullptr;
    }
    static void settle()
    {
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
    }
    void showSection(const QString &section)
    {
        m_panel->setProperty("section", section);
        settle();
    }
    void reopen()
    {
        QMetaObject::invokeMethod(m_panel, "openForRoom", Q_ARG(QVariant, kRoom));
        settle();
    }
    bool focusOn(QQuickItem *item)
    {
        item->forceActiveFocus();
        return QTest::qWaitFor([&] { return item->hasActiveFocus(); }, 2000);
    }
    void sendPins(const QVariantList &entries)
    {
        QVariantMap snap;
        snap.insert(QStringLiteral("ok"), true);
        snap.insert(QStringLiteral("canPin"), false);
        snap.insert(QStringLiteral("total"), int(entries.size()));
        snap.insert(QStringLiteral("truncated"), false);
        QStringList ids;
        for (const QVariant &e : entries)
            ids << e.toMap().value(QStringLiteral("eventId")).toString();
        snap.insert(QStringLiteral("ids"), ids);
        snap.insert(QStringLiteral("entries"), entries);
        snap.insert(QStringLiteral("category"), QString());
        // Point the pin controller at a client whose read this test answers,
        // on the room the panel shows.
        m_mock->lastOp = 0;
        m_controller->pinned()->setRoomId(QString());
        m_controller->pinned()->setRoomId(kRoom);
        m_controller->roomInfo()->setRoomId(kRoom);
        QTRY_VERIFY(m_mock->lastOp != 0);
        Q_EMIT m_mock->pinnedReceived(m_mock->lastOp, kRoom, snap);
        settle();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("room-info-keyboard-qml-test"));

        m_controller = std::make_unique<AppController>(AppController::MockBackend);
        QSignalSpy spy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(spy.wait(3000));
        m_mock = std::make_unique<TestMock>();
        m_controller->pinned()->setClient(m_mock.get());
        m_controller->roomInfo()->setClient(m_mock.get());

        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->rootContext()->setContextProperty("app", m_controller.get());
        QQmlComponent component(m_engine.get());
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("roominfo-keyboard.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        m_window.reset(component.create());
        QVERIFY(m_window);
        m_quick = qobject_cast<QQuickWindow *>(m_window.get());
        QVERIFY(m_quick);
        m_panel = m_window->property("panel").value<QQuickItem *>();
        QVERIFY(m_panel);
        m_quick->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_quick));
        reopen();
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_engine.reset();
        m_controller.reset();
        m_mock.reset();
    }

    // Fails on the old panel: the list was bare, so a pin list taller than
    // the panel had no scrollbar at all.
    void thePinnedListHasAScrollBar()
    {
        QVariantList pins;
        for (int i = 0; i < 40; ++i)
            pins << pin(QStringLiteral("$p%1").arg(i), true);
        sendPins(pins);
        showSection(QStringLiteral("pinned"));
        QQuickItem *list = itemNamed(QStringLiteral("pinnedMessagesList"));
        QVERIFY(list);
        bool hasBar = false;
        for (QQuickItem *child : list->childItems())
            hasBar = hasBar || child->inherits("QQuickScrollBar");
        QVERIFY2(hasBar, "the pinned list has no scrollbar");
    }

    // Fails on the old panel: the rows were Items with a TapHandler, so no
    // row took focus and Enter did nothing.
    void pinnedRowsAreTabStopsAndEnterJumps()
    {
        sendPins({pin(QStringLiteral("$ok"), true),
                  pin(QStringLiteral("$gone"), false)});
        showSection(QStringLiteral("pinned"));
        QTRY_COMPARE(itemsNamed(QStringLiteral("pinnedMessageRow")).size(), 2);
        QQuickItem *gone = nullptr;
        QQuickItem *ok = nullptr;
        for (QQuickItem *row : itemsNamed(QStringLiteral("pinnedMessageRow")))
            (row->property("resolved").toBool() ? ok : gone) = row;
        QVERIFY(ok && gone);
        QVERIFY2(ok->activeFocusOnTab(), "a resolved pin must be a tab stop");
        QVERIFY2(!gone->activeFocusOnTab(), "an unavailable pin goes nowhere");

        QSignalSpy jump(m_panel, SIGNAL(jumpToEventRequested(QString)));
        QVERIFY(focusOn(ok));
        QTest::keyClick(m_quick, Qt::Key_Return);
        QCOMPARE(jump.count(), 1);
        QCOMPARE(jump.last().at(0).toString(), QStringLiteral("$ok"));
        QTest::keyClick(m_quick, Qt::Key_Space);
        QCOMPARE(jump.count(), 2);
    }

    // Fails on the old panel: a focused row below the fold stayed clipped.
    void aFocusedPinBelowTheFoldScrollsIntoView()
    {
        QVariantList pins;
        for (int i = 0; i < 40; ++i)
            pins << pin(QStringLiteral("$s%1").arg(i), true);
        sendPins(pins);
        showSection(QStringLiteral("pinned"));
        QQuickItem *list = itemNamed(QStringLiteral("pinnedMessagesList"));
        QVERIFY(list);
        QTRY_VERIFY(!itemsNamed(QStringLiteral("pinnedMessageRow")).isEmpty());
        list->setProperty("contentY", 0.0);
        settle();
        QQuickItem *below = nullptr;
        for (QQuickItem *row : itemsNamed(QStringLiteral("pinnedMessageRow"))) {
            const qreal bottom = row->y() + row->height();
            if (bottom > list->height() + list->property("contentY").toReal()
                && (!below || row->y() < below->y()))
                below = row;
        }
        QVERIFY2(below, "no instantiated row lies past the viewport");
        QVERIFY(focusOn(below));
        QTRY_VERIFY2(below->y() + below->height()
                         <= list->property("contentY").toReal() + list->height()
                                + 1.0,
                     "the focused row is still clipped");
    }

    // Fails on the old panel: nothing said why the list was blank.
    void anEmptyListSaysWhy()
    {
        m_mock->roster.clear();
        reopen();
        showSection(QStringLiteral("people"));
        QQuickItem *search = textFieldWithPlaceholder(QStringLiteral("Type name…"));
        QVERIFY(search);
        QQuickItem *empty = itemNamed(QStringLiteral("memberEmptyState"));
        QVERIFY(empty);
        auto text = [&]() -> QString {
            auto *label = empty->property("item").value<QQuickItem *>();
            return label ? label->property("text").toString() : QString();
        };
        QTRY_VERIFY(!m_controller->roomInfo()->loading());
        QTRY_COMPARE(text(), QStringLiteral("No members to show."));
        search->setProperty("text", QStringLiteral("zzz-nobody-has-this-name"));
        QTRY_COMPARE(text(), QStringLiteral("No members match this name."));
        search->setProperty("text", QString());
        QTRY_COMPARE(text(), QStringLiteral("No members to show."));
        // With people in the room the message is gone.
        QVariantMap m;
        m.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        m.insert(QStringLiteral("displayName"), QStringLiteral("Bob"));
        m.insert(QStringLiteral("avatarUrl"), QString());
        m.insert(QStringLiteral("membership"), QStringLiteral("joined"));
        m.insert(QStringLiteral("role"), QStringLiteral("default"));
        m.insert(QStringLiteral("ambiguous"), false);
        m.insert(QStringLiteral("isOwn"), false);
        m_mock->roster = {m};
        reopen();
        QTRY_VERIFY(!m_controller->roomInfo()->loading());
        QTRY_VERIFY(!empty->property("active").toBool());
        search->setProperty("text", QStringLiteral("zzz-nobody-has-this-name"));
        QTRY_COMPARE(text(), QStringLiteral("No members match this name."));
    }

    // Fails on the old panel: Escape in a field was never claimed, so the
    // host's close-the-panel shortcut fired and the draft went with it.
    void escapeInATextFieldLeavesTheFieldNotThePanel()
    {
        reopen();
        showSection(QStringLiteral("people"));
        QQuickItem *search = textFieldWithPlaceholder(QStringLiteral("Type name…"));
        QVERIFY(search);
        QVERIFY(focusOn(search));

        // The host's Shortcut only fires when nothing accepts this first.
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Escape,
                           Qt::NoModifier);
        override.setAccepted(false);
        QCoreApplication::sendEvent(m_quick, &override);
        QVERIFY2(override.isAccepted(),
                 "a focused text field must claim Escape before the host "
                 "shortcut can close the panel");

        QTest::keyClick(m_quick, Qt::Key_Escape);
        QTRY_VERIFY(!search->hasActiveFocus());

        // Focus is out of the field now: the next Escape belongs to the host.
        QKeyEvent second(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
        second.setAccepted(false);
        QCoreApplication::sendEvent(m_quick, &second);
        QVERIFY(!second.isAccepted());
    }

    // Fails on the old panel: Menu / Shift+F10 did nothing on a member row.
    void memberRowsOpenFromTheKeyboard()
    {
        QVariantMap m;
        m.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        m.insert(QStringLiteral("displayName"), QStringLiteral("Bob"));
        m.insert(QStringLiteral("avatarUrl"), QString());
        m.insert(QStringLiteral("membership"), QStringLiteral("joined"));
        m.insert(QStringLiteral("role"), QStringLiteral("default"));
        m.insert(QStringLiteral("ambiguous"), false);
        m.insert(QStringLiteral("isOwn"), false);
        m_mock->roster = {m};
        reopen();
        showSection(QStringLiteral("people"));
        QTRY_VERIFY(!m_controller->roomInfo()->loading());
        QQuickItem *list = itemNamed(QStringLiteral("memberRoleList"));
        QVERIFY(list);
        auto findRow = [&]() -> QQuickItem * {
            auto *content = list->property("contentItem").value<QQuickItem *>();
            for (QQuickItem *loader : content->childItems()) {
                auto *item = loader->property("item").value<QQuickItem *>();
                if (item && item->metaObject()->indexOfProperty("member") >= 0
                    && !item->property("member").toMap()
                            .value(QStringLiteral("userId")).toString().isEmpty())
                    return item;
            }
            return nullptr;
        };
        QTRY_VERIFY(findRow() != nullptr);
        QQuickItem *row = findRow();
        QVERIFY2(row->property("focusPolicy").toInt() & Qt::TabFocus,
                 "a member row must be a tab stop");

        QSignalSpy clicked(row, SIGNAL(clicked()));
        QVERIFY(focusOn(row));
        QTest::keyClick(m_quick, Qt::Key_Return);
        QCOMPARE(clicked.count(), 1);
        // The profile card is open now; close it before the menu check.
        QTest::keyClick(m_quick, Qt::Key_Escape);
        settle();

        auto *menu = m_panel->findChild<QObject *>(QStringLiteral("roomMemberMenu"));
        QVERIFY(menu);
        QVERIFY(!menu->property("visible").toBool());
        QVERIFY(focusOn(row));
        QTest::keyClick(m_quick, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(menu->property("visible").toBool());
    }

    // Fails on the old panel: a wheel over an open (detached, non-modal)
    // combo popup scrolled the page away from it.
    void anOpenComboFreezesTheOverviewWheel()
    {
        reopen();
        showSection(QStringLiteral("overview"));
        QQuickItem *flick = itemNamed(QStringLiteral("roomInfoOverviewFlick"));
        QQuickItem *combo = itemNamed(QStringLiteral("roomNotificationModeCombo"));
        QVERIFY(flick && combo);
        QVERIFY(flick->property("interactive").toBool());
        QObject *popup = combo->property("popup").value<QObject *>();
        QVERIFY(popup);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("visible").toBool());
        QVERIFY2(!flick->property("interactive").toBool(),
                 "the Overview must not scroll under an open combo popup");
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTRY_VERIFY(flick->property("interactive").toBool());
    }
};

QTEST_MAIN(RoomInfoKeyboardQmlTest)
#include "RoomInfoKeyboardQmlTest.moc"
