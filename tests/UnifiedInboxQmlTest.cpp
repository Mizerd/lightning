#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "accounts/RustBridgeApi.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "models/AccountActivityModel.h"
#include "models/UnifiedInboxModel.h"
#include "support/LabsQmlFixture.h"

namespace {

const QString kAlice = QStringLiteral("@alice:hs1.example");
const QString kBob = QStringLiteral("@bob:hs2.example");
const QString kRoom = QStringLiteral("!bobroom:hs2.example");

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 620
    height: 400
    visible: true
    color: AppTheme.background
    property int opened: 0
    UnifiedInboxSection {
        objectName: "section"
        width: 600
        onEntryOpened: win.opened++
    }
}
)QML";

QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    const auto children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *hit = findItem(child, name))
            return hit;
    }
    return nullptr;
}

}

class UnifiedInboxQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    labsqml::InertRustBridge m_bridge;

    struct Scene {
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QObject> root;
        QQuickItem *section() const
        {
            auto *window = qobject_cast<QQuickWindow *>(root.get());
            return window ? findItem(window->contentItem(), QStringLiteral("section"))
                          : nullptr;
        }
    };

    std::unique_ptr<Scene> makeScene(AppController *controller)
    {
        auto s = std::make_unique<Scene>();
        s->engine = std::make_unique<QQmlEngine>();
        s->engine->rootContext()->setContextProperty(QStringLiteral("app"), controller);
        QQmlComponent component(s->engine.get());
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("unifiedinbox.qml")));
        s->root.reset(component.create());
        if (!s->root) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        auto *window = qobject_cast<QQuickWindow *>(s->root.get());
        if (!window || !QTest::qWaitForWindowExposed(window))
            return nullptr;
        return s;
    }

    AppController *liveController()
    {
        lightning::accounts::setRustBridgeApiForTest(&m_bridge);
        auto *controller = labsqml::makeController(true, { kAlice, kBob });
        controller->switchToAccount(kAlice);
        if (!QTest::qWaitFor([controller] { return !controller->accountSwitching(); }))
            return controller;
        controller->settings()->setLabsConcurrentAccounts(true);
        QTest::qWaitFor([controller] { return controller->liveAccountsActive(); });
        return controller;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("unified-inbox-qml-test"));
    }

    void cleanup()
    {
        lightning::accounts::setRustBridgeApiForTest(nullptr);
        labsqml::setHiddenSwitch(false);
        QSettings().clear();
    }

    void withLabsOffTheSectionSaysWhereToTurnItOn()
    {
        std::unique_ptr<AppController> controller(
            labsqml::makeController(true, { kAlice, kBob }));
        QVERIFY(!controller->liveAccountsActive());
        QVERIFY(controller->unifiedInbox() == nullptr);
        auto s = makeScene(controller.get());
        QVERIFY(s);
        QQuickItem *section = s->section();
        QVERIFY(section);
        QVERIFY(section->property("hasContent").toBool());
        QQuickItem *hint = findItem(section, QStringLiteral("unifiedInboxHint"));
        QVERIFY(hint);
        QVERIFY(hint->isVisible());
        QVERIFY(!findItem(section, QStringLiteral("unifiedInboxList"))->isVisible());
    }

    void aRowShowsItsAccountAndAClickSwitchesAndOpens()
    {
        std::unique_ptr<AppController> controller(liveController());
        QVERIFY(controller->liveAccountsActive());
        QCOMPARE(controller->settings()->activeAccountUserId(), kAlice);
        AccountActivityModel *activity = controller->accountActivity();
        UnifiedInboxModel *inbox = controller->unifiedInbox();
        QVERIFY(activity && inbox);
        // The pool's epoch gate is UnifiedInboxModelTest's; this suite tests
        // presentation and routing.
        inbox->setEpochSource({});
        activity->setLiveState(kBob, AccountActivityModel::Running);

        auto s = makeScene(controller.get());
        QVERIFY(s);
        QQuickItem *section = s->section();
        QVERIFY(section);
        QVERIFY2(findItem(section, QStringLiteral("unifiedInboxHint"))->isVisible(),
                 "Labs on but only one account live: say how to add more");

        UnifiedInboxModel::Entry e;
        e.accountUserId = kBob;
        e.accountLabel = QStringLiteral("bob (home)");
        e.roomId = kRoom;
        e.roomName = QStringLiteral("Bob's room");
        e.senderId = QStringLiteral("@dave:hs2.example");
        e.senderName = QStringLiteral("Dave");
        e.preview = QStringLiteral("are you there?");
        e.eventId = QStringLiteral("$ev1");
        e.timestampMs = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(inbox->add(0, e));
        QVERIFY2(!inbox->add(0, [&] { auto a = e; a.accountUserId = kAlice;
                                      a.eventId = QStringLiteral("$a"); return a; }()),
                 "the foreground account's events are not 'other accounts'");

        QTRY_VERIFY(!findItem(section, QStringLiteral("unifiedInboxHint"))->isVisible());
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = findItem(section, QStringLiteral("unifiedInboxRow"))) != nullptr
                    && row->isVisible() && row->height() > 0);
        QQuickItem *label = findItem(row, QStringLiteral("unifiedInboxAccountLabel"));
        QVERIFY(label);
        QCOMPARE(label->property("text").toString(), QStringLiteral("bob (home)"));
        QVERIFY(label->isVisible() && label->width() > 0);

        QSignalSpy routed(controller.get(), &AppController::notificationOpenRequested);
        auto *window = qobject_cast<QQuickWindow *>(s->root.get());
        const QPointF centre = row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
        QTRY_COMPARE(s->root->property("opened").toInt(), 1);
        QVERIFY(routed.size() >= 1);
        QTRY_COMPARE(controller->settings()->activeAccountUserId(), kBob);
        QTRY_COMPARE(inbox->count(), 0);
    }

    void theForegroundAccountIsNotRoutedAgain()
    {
        std::unique_ptr<AppController> controller(liveController());
        QVERIFY(controller->liveAccountsActive());
        QVERIFY(!controller->openUnifiedInboxEntry(kAlice, kRoom, QString(), QString()));
        QVERIFY(!controller->openUnifiedInboxEntry(kBob, QString(), QString(), QString()));
    }
};

QTEST_MAIN(UnifiedInboxQmlTest)
#include "UnifiedInboxQmlTest.moc"
