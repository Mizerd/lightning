#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/QuickSwitcherModel.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "spaces/SpaceManager.h"
#include "models/TimelineModel.h"

class ComposerFocusShortcutQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name) const
    {
        return m_window->findChild<QQuickItem *>(QString::fromLatin1(name));
    }

    QObject *switcher() const
    {
        const auto objects = m_window->findChildren<QObject *>();
        for (QObject *object : objects) {
            if (QByteArray(object->metaObject()->className())
                    .startsWith("QuickSwitcher"))
                return object;
        }
        return nullptr;
    }

    void openSwitcher()
    {
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTRY_VERIFY(item("quickSwitcherField")->hasActiveFocus());
    }

    int resultRow(bool invite = false, bool space = false) const
    {
        auto *model = m_controller->quickSwitcher();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariantMap result = model->resultAt(row);
            if (result.value(QStringLiteral("isInvite")).toBool() == invite
                && result.value(QStringLiteral("isSpace")).toBool() == space)
                return row;
        }
        return -1;
    }

    QObject *shortcutOn(const QString &sequence) const
    {
        const auto objects = m_window->findChildren<QObject *>();
        for (QObject *object : objects) {
            if (!object->inherits("QQuickShortcut"))
                continue;
            const QVariantList seqs = object->property("sequences").toList();
            if (seqs.size() == 1 && seqs.first().toString() == sequence)
                return object;
        }
        return nullptr;
    }

    static QString boundSequence(QObject *shortcut)
    {
        const QVariantList seqs = shortcut->property("sequences").toList();
        return seqs.isEmpty() ? QString() : seqs.first().toString();
    }

private:
    void openJoinedRoom()
    {
        openSwitcher();
        const int row = resultRow();
        QVERIFY(row >= 0);
        QVERIFY(item("quickSwitcherList")->setProperty("currentIndex", row));
        const QString roomId = m_controller->quickSwitcher()->resultAt(row)
                                   .value(QStringLiteral("roomId")).toString();
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_controller->currentRoomId(), roomId);
        QTRY_VERIFY_WITH_TIMEOUT(item("composerInput")->hasActiveFocus(), 1500);
        QTRY_VERIFY(m_controller->timeline()->realEventCount() > 2);
        QTest::qWait(300);
    }

private:
    // The rooms a reader can open from the list, top to bottom.
    QStringList listedRooms() const
    {
        QStringList ids;
        if (m_controller->settings()->roomNavigationLayout() == 1) {
            auto *model = m_controller->spaceChannels();
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex at = model->index(row);
                if (at.data(SpaceChannelModel::KindRole).toString() == QStringLiteral("room")
                    && !at.data(SpaceChannelModel::IsInviteRole).toBool())
                    ids.append(at.data(SpaceChannelModel::RoomIdRole).toString());
            }
            return ids;
        }
        auto *model = m_controller->roomList();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex at = model->index(row);
            if (at.data(RoomListModel::MembershipRole).toString() == QStringLiteral("joined")
                && !at.data(RoomListModel::IsSpaceRole).toBool())
                ids.append(at.data(RoomListModel::RoomIdRole).toString());
        }
        return ids;
    }

    // The next unread room after the open one in list order, wrapping; the
    // open room itself never counts.
    QString nextUnread(bool backward) const
    {
        const QStringList rooms = listedRooms();
        QSet<QString> unread;
        auto *model = m_controller->roomList();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex at = model->index(row);
            if (at.data(RoomListModel::HasUnreadRole).toBool())
                unread.insert(at.data(RoomListModel::RoomIdRole).toString());
        }
        const qsizetype n = rooms.size();
        const qsizetype here = rooms.indexOf(m_controller->currentRoomId());
        for (qsizetype step = 1; step < n; ++step) {
            const QString id = rooms.at(((here + (backward ? -step : step)) % n + n) % n);
            if (unread.contains(id))
                return id;
        }
        return {};
    }

    bool listHasInvite() const
    {
        auto *model = m_controller->roomList();
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->index(row).data(RoomListModel::MembershipRole).toString()
                == QStringLiteral("invited"))
                return true;
        }
        return false;
    }

    void ctrlTab(bool backward)
    {
        if (backward)
            QTest::keyClick(m_window, Qt::Key_Backtab,
                            Qt::ControlModifier | Qt::ShiftModifier);
        else
            QTest::keyClick(m_window, Qt::Key_Tab, Qt::ControlModifier);
    }

    // Walks the whole list from the composer in one direction and back to the start.
    void walkWholeList(bool backward)
    {
        const QString start = listedRooms().value(0);
        m_controller->openRoom(start);
        QTRY_COMPARE(m_controller->currentRoomId(), start);
        QTest::qWait(300);
        const QStringList rooms = listedRooms();
        QVERIFY2(rooms.size() >= 2 && rooms.first() == start,
                 qPrintable(rooms.join(QLatin1Char(' '))));
        item("composerInput")->forceActiveFocus();
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        for (int step = 1; step <= rooms.size(); ++step) {
            const qsizetype n = rooms.size();
            const QString want = rooms.at(backward ? (n - step % n) % n : step % n);
            ctrlTab(backward);
            QTRY_VERIFY2(m_controller->currentRoomId() == want,
                         qPrintable(QStringLiteral("at %1 want %2 in [%3] focus %4")
                                        .arg(m_controller->currentRoomId(), want,
                                             listedRooms().join(QLatin1Char(' ')),
                                             m_window->activeFocusItem()
                                                 ? m_window->activeFocusItem()->objectName()
                                                 : QStringLiteral("none"))));
            QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        }
        QCOMPARE(m_controller->currentRoomId(), start);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + "/data").toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("composer-focus-test"));
    }

    void init()
    {
        QSettings().clear();
        m_controller = new AppController(AppController::MockBackend, true);
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setScreenshotDemoMode(true);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://lightning.example"),
                                    QStringLiteral("alex"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(4000));
        QTRY_VERIFY(m_controller->roomList()->rowCount() > 0);
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_controller);
        QQmlComponent scene(m_engine);
        scene.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 1100
    height: 800
    visible: true
    MainScreen { anchors.fill: parent }
}
)QML", QUrl(QStringLiteral("composer-focus-scene.qml")));
        QObject *root = scene.create();
        QVERIFY2(root, qPrintable(scene.errorString()));
        m_window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QVERIFY(switcher());
    }

    void cleanup()
    {
        delete m_window;
        delete m_engine;
        delete m_controller;
        m_window = nullptr;
        m_engine = nullptr;
        m_controller = nullptr;
    }

    void focusShortcutCanBeReboundAndDoesNotStealSwitcherFocus()
    {
        openSwitcher();
        const int row = resultRow();
        QVERIFY(row >= 0);
        const QString roomId = m_controller->quickSwitcher()->resultAt(row)
                                   .value(QStringLiteral("roomId")).toString();
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        m_controller->openRoom(roomId);
        QVERIFY(item("timelinePane"));
        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(item("composerInput")->hasActiveFocus(), 1500);

        auto *registry = m_controller->shortcuts();
        QCOMPARE(registry->setBinding(QStringLiteral("composer.focus"),
                                      QStringLiteral("Ctrl+J")), QString());
        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(item("composerInput")->hasActiveFocus(), 1500);
        openSwitcher();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QVERIFY(item("quickSwitcherField")->hasActiveFocus());
    }

    void focusShortcutUsesActiveEditor_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("markdown") << QStringLiteral("markdown");
        QTest::newRow("rich") << QStringLiteral("rich");
    }

    void focusShortcutUsesActiveEditor()
    {
        QFETCH(QString, mode);
        openJoinedRoom();
        m_controller->settings()->setProperty("composerMode", mode);
        m_controller->composer()->setText(QStringLiteral("keep this draft"));
        auto *editor = item(mode == QStringLiteral("rich")
                                ? "composerRichInput" : "composerInput");
        QTRY_VERIFY(editor->property("text").toString().contains("keep this draft"));
        const QString draft = m_controller->composer()->text();
        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasActiveFocus(), 1500);
        QCOMPARE(m_controller->composer()->text(), draft);
    }

    void aLoneLetterMuteShortcutLeavesTypingAloneAndFiresElsewhere()
    {
        openJoinedRoom();
        QObject *mute = shortcutOn(m_controller->shortcuts()->sequenceFor(
            QStringLiteral("call.toggleMute")));
        QVERIFY(mute);
        QSignalSpy fired(mute, SIGNAL(activated()));
        QCOMPARE(m_controller->shortcuts()->setBinding(
                     QStringLiteral("call.toggleMute"), QStringLiteral("M")),
                 QString());
        QTRY_COMPARE(boundSequence(mute), QStringLiteral("M"));

        auto *composer = item("composerInput");
        QVERIFY(composer->hasActiveFocus());
        const int before = composer->property("length").toInt();
        QTest::keyClick(m_window, Qt::Key_M);
        QTRY_COMPARE(composer->property("length").toInt(), before + 1);
        QCOMPARE(fired.count(), 0);

        item("timelinePane")->forceActiveFocus();
        QVERIFY(!composer->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_M);
        QTRY_COMPARE(fired.count(), 1);
    }

    void aNumpadMuteShortcutAnswersOnlyTheNumpadAndNeverWhileTyping()
    {
        openJoinedRoom();
        QObject *mute = shortcutOn(m_controller->shortcuts()->sequenceFor(
            QStringLiteral("call.toggleMute")));
        QVERIFY(mute);
        QSignalSpy fired(mute, SIGNAL(activated()));
        QCOMPARE(m_controller->shortcuts()->setBinding(
                     QStringLiteral("call.toggleMute"), QStringLiteral("Num+.")),
                 QString());
        QTRY_COMPARE(boundSequence(mute), QStringLiteral("Num+."));

        auto *composer = item("composerInput");
        QVERIFY(composer->hasActiveFocus());
        const int before = composer->property("length").toInt();
        QTest::keyClick(m_window, Qt::Key_Period, Qt::KeypadModifier);
        QTRY_COMPARE(composer->property("length").toInt(), before + 1);
        QCOMPARE(fired.count(), 0);

        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_Period);
        QTest::qWait(50);
        QCOMPARE(fired.count(), 0);
        QTest::keyClick(m_window, Qt::Key_Period, Qt::KeypadModifier);
        QTRY_COMPARE(fired.count(), 1);
    }

    // xkb drops the keypad modifier before Qt's shortcut map sees the press, so
    // there the key press reaches the window unmatched; offscreen never does.
    void aNumpadMuteShortcutFiresFromAKeyPressTheShortcutMapMissed()
    {
        openJoinedRoom();
        ShortcutRegistry *registry = m_controller->shortcuts();
        QObject *mute = shortcutOn(registry->sequenceFor(QStringLiteral("call.toggleMute")));
        QVERIFY(mute);
        QSignalSpy fired(mute, SIGNAL(activated()));
        QCOMPARE(registry->setBinding(QStringLiteral("call.toggleMute"),
                                      QStringLiteral("Num+.")),
                 QString());
        QTRY_COMPARE(boundSequence(mute), QStringLiteral("Num+."));
        QKeyEvent numpad(QEvent::KeyPress, Qt::Key_Period, Qt::KeypadModifier,
                         QStringLiteral("."));
        QKeyEvent mainKey(QEvent::KeyPress, Qt::Key_Period, Qt::NoModifier,
                          QStringLiteral("."));

        auto *composer = item("composerInput");
        QVERIFY(composer->hasActiveFocus());
        QVERIFY(!registry->eventFilter(m_window, &numpad));
        QCOMPARE(fired.count(), 0);

        item("timelinePane")->forceActiveFocus();
        QVERIFY(!composer->hasActiveFocus());
        QVERIFY(!registry->eventFilter(m_window, &mainKey));
        QCOMPARE(fired.count(), 0);
        QVERIFY(registry->eventFilter(m_window, &numpad));
        QCOMPARE(fired.count(), 1);
    }

    void anOpenModalPopupBlocksANumpadGlobalShortcut()
    {
        openJoinedRoom();
        ShortcutRegistry *registry = m_controller->shortcuts();
        QObject *mute = shortcutOn(registry->sequenceFor(QStringLiteral("call.toggleMute")));
        QVERIFY(mute);
        QSignalSpy fired(mute, SIGNAL(activated()));
        QCOMPARE(registry->setBinding(QStringLiteral("call.toggleMute"),
                                      QStringLiteral("Num+.")),
                 QString());
        QTRY_COMPARE(boundSequence(mute), QStringLiteral("Num+."));

        QQmlComponent component(m_engine);
        component.setData(R"QML(
import QtQuick
import QtQuick.Controls
Popup {
    modal: true
    width: 200
    height: 100
    Button { text: "ok" }
}
)QML", QUrl(QStringLiteral("modal-popup.qml")));
        QObject *popup = component.create();
        QVERIFY2(popup, qPrintable(component.errorString()));
        popup->setParent(m_window->contentItem());
        popup->setProperty("parent", QVariant::fromValue(m_window->contentItem()));
        QQuickItem *timeline = item("timelinePane");
        timeline->forceActiveFocus();
        QMetaObject::invokeMethod(popup, "open");
        QTRY_VERIFY(popup->property("opened").toBool());
        QVERIFY(timeline->hasActiveFocus());

        QKeyEvent numpad(QEvent::KeyPress, Qt::Key_Period, Qt::KeypadModifier,
                         QStringLiteral("."));
        QVERIFY(!registry->eventFilter(m_window, &numpad));
        QCOMPARE(fired.count(), 0);

        QMetaObject::invokeMethod(popup, "close");
        QTRY_VERIFY(!popup->property("visible").toBool());
        timeline->forceActiveFocus();
        QVERIFY(registry->eventFilter(m_window, &numpad));
        QCOMPARE(fired.count(), 1);
    }

    void ctrlTabCyclesTheListedRoomsFromTheComposerAndWraps()
    {
        QVERIFY(listHasInvite());
        walkWholeList(false);
    }

    void ctrlShiftTabCyclesBackwardsAndWraps()
    {
        walkWholeList(true);
    }

    void plainTabInTheComposerDoesNotChangeRoom()
    {
        openJoinedRoom();
        const QString room = m_controller->currentRoomId();
        QTest::keyClick(m_window, Qt::Key_Tab);
        QTest::keyClick(m_window, Qt::Key_Backtab, Qt::ShiftModifier);
        QTest::qWait(100);
        QCOMPARE(m_controller->currentRoomId(), room);
    }

    void ctrlTabFollowsTheFilteredList()
    {
        m_controller->settings()->setRoomFilterMode(1);
        QTRY_COMPARE(m_controller->roomList()->filterMode(), 1);
        const QStringList all = listedRooms();
        walkWholeList(false);
        QCOMPARE(listedRooms(), all);
    }

    void ctrlTabStaysInsideTheActiveSpace()
    {
        const int everything = listedRooms().size();
        m_controller->spaces()->setActiveSpaceId(
            QStringLiteral("!space-studio:lightning.example"));
        QTRY_VERIFY(listedRooms().size() < everything);
        walkWholeList(false);
    }

    void ctrlTabFollowsTheChannelsLayout()
    {
        m_controller->settings()->setRoomNavigationLayout(1);
        QTRY_VERIFY(m_controller->spaceChannels()->rowCount() > 0);
        walkWholeList(false);
    }

    void cyclingScrollsTheOpenedRoomIntoView()
    {
        m_window->resize(1100, 480);
        auto *list = item("roomListView");
        QVERIFY(list);
        const QStringList rooms = listedRooms();
        m_controller->openRoom(rooms.first());
        QTRY_COMPARE(m_controller->currentRoomId(), rooms.first());
        list->setProperty("contentY", list->property("originY"));
        QTRY_VERIFY2(list->property("contentHeight").toDouble() > list->height() + 200,
                     qPrintable(QStringLiteral("content %1 view %2")
                                    .arg(list->property("contentHeight").toDouble())
                                    .arg(list->height())));
        const double topY = list->property("contentY").toDouble();

        ctrlTab(true);
        QTRY_COMPARE(m_controller->currentRoomId(), rooms.last());
        int row = -1;
        auto *model = m_controller->roomList();
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->index(r).data(RoomListModel::RoomIdRole).toString() == rooms.last())
                row = r;
        }
        QVERIFY(row >= 0);
        QQuickItem *delegate = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, delegate),
                                              Q_ARG(int, row))
                    && delegate);
        const double y = list->property("contentY").toDouble();
        QVERIFY(y > topY);
        QVERIFY(delegate->y() >= y - 0.5 && delegate->y() + delegate->height() <= y + list->height() + 0.5);

        // Already visible: the list must not move.
        ctrlTab(true);
        QTRY_COMPARE(m_controller->currentRoomId(), rooms.at(rooms.size() - 2));
        QTest::qWait(100);
        QCOMPARE(list->property("contentY").toDouble(), y);
    }

    void ctrlTabIsRebindable()
    {
        const QStringList rooms = listedRooms();
        m_controller->openRoom(rooms.first());
        QTRY_COMPARE(m_controller->currentRoomId(), rooms.first());
        QCOMPARE(m_controller->shortcuts()->setBinding(QStringLiteral("nav.nextRoom"),
                                                       QStringLiteral("Alt+Down")),
                 QString());
        QTest::keyClick(m_window, Qt::Key_Tab, Qt::ControlModifier);
        QTest::qWait(100);
        QCOMPARE(m_controller->currentRoomId(), rooms.first());
        QTest::keyClick(m_window, Qt::Key_Down, Qt::AltModifier);
        QTRY_COMPARE(m_controller->currentRoomId(), rooms.at(1));
    }

    void altShiftArrowsSkipToTheNextAndPreviousUnreadRoom()
    {
        const QStringList rooms = listedRooms();
        m_controller->openRoom(rooms.first());
        QTRY_COMPARE(m_controller->currentRoomId(), rooms.first());
        item("composerInput")->forceActiveFocus();
        QTRY_VERIFY(item("composerInput")->hasActiveFocus());
        const QString down = nextUnread(false);
        QVERIFY2(!down.isEmpty() && down != rooms.at(1),
                 qPrintable(QStringLiteral("fixture needs a read room before an unread one: [%1] -> %2")
                                .arg(rooms.join(QLatin1Char(' ')), down)));
        QTest::keyClick(m_window, Qt::Key_Down, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_COMPARE(m_controller->currentRoomId(), down);
        const QString up = nextUnread(true);
        QVERIFY(!up.isEmpty());
        QTest::keyClick(m_window, Qt::Key_Up, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_COMPARE(m_controller->currentRoomId(), up);
    }

    void accountShortcutsFireAndFollowRebinding()
    {
        QObject *next = shortcutOn(QStringLiteral("Ctrl+`"));
        QObject *previous = shortcutOn(QStringLiteral("Ctrl+Shift+`"));
        QVERIFY(next);
        QVERIFY(previous);
        QSignalSpy nextFired(next, SIGNAL(activated()));
        QSignalSpy previousFired(previous, SIGNAL(activated()));
        item("composerInput")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QTRY_COMPARE(nextFired.count(), 1);
        QTest::keyClick(m_window, Qt::Key_QuoteLeft,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(previousFired.count(), 1);
        // One account: nothing to switch to.
        QVERIFY(!m_controller->accountSwitching());

        QCOMPARE(m_controller->shortcuts()->setBinding(QStringLiteral("nav.nextAccount"),
                                                       QStringLiteral("Ctrl+Alt+Down")),
                 QString());
        QTRY_COMPARE(boundSequence(next), QStringLiteral("Ctrl+Alt+Down"));
        QTest::keyClick(m_window, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QTest::qWait(50);
        QCOMPARE(nextFired.count(), 1);
        QTest::keyClick(m_window, Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier);
        QTRY_COMPARE(nextFired.count(), 2);
    }

    void focusShortcutRefusesHiddenComposer()
    {
        openJoinedRoom();
        auto *composer = item("messageComposer");
        QVERIFY(composer);
        composer->setVisible(false);
        item("timelinePane")->forceActiveFocus();
        auto *before = m_window->activeFocusItem();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTest::qWait(50);
        QCOMPARE(m_window->activeFocusItem(), before);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ComposerFocusShortcutQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ComposerFocusShortcutQmlTest.moc"
