
#include <QFile>
#include <memory>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

class InvitePreviewCardQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void keyboardContract()
    {
        QQuickWindow window;
        window.resize(640, 520);
        auto *before = new QQuickItem(window.contentItem());
        before->setSize(QSizeF(10, 10));
        before->setActiveFocusOnTab(true);
        auto *covered = new QQuickItem(window.contentItem());
        covered->setSize(QSizeF(10, 10));
        covered->setActiveFocusOnTab(true);
        QQmlApplicationEngine engine;
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("InvitePreviewCard"));
        QVERIFY(created.count() > 0 || created.wait(5000));
        auto *card = qobject_cast<QQuickItem *>(created.at(0).at(0).value<QObject *>());
        QVERIFY(card);
        card->setParentItem(window.contentItem());
        card->setSize(QSizeF(600, 500));
        card->setProperty("roomId", QStringLiteral("!r:test"));
        auto *after = new QQuickItem(window.contentItem());
        after->setSize(QSizeF(10, 10));
        after->setActiveFocusOnTab(true);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, 2000));

        auto *accept = card->findChild<QQuickItem *>(QStringLiteral("invitePreviewAccept"));
        auto *decline = card->findChild<QQuickItem *>(QStringLiteral("invitePreviewDecline"));
        QVERIFY(accept && decline);

        before->forceActiveFocus();
        card->setVisible(false);
        card->setVisible(true);
        QTest::qWait(50);
        QVERIFY2(before->hasActiveFocus(), "showing the card must not take focus");

        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY2(covered->hasActiveFocus(), "an enabled covered item is the next Tab stop");
        before->forceActiveFocus();
        covered->setEnabled(false);
        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY2(card->hasActiveFocus(), "Tab reaches the card once the covered content is disabled");
        auto *ring = card->findChild<QQuickItem *>(QStringLiteral("invitePreviewFocusRing"));
        QVERIFY(ring);
        QVERIFY2(ring->isVisible(), "the card's own Tab stop shows a focus ring");
        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY2(accept->hasActiveFocus(), "Tab from the card reaches Accept");
        QVERIFY(accept->property("visualFocus").toBool());
        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY2(decline->hasActiveFocus(), "Tab from Accept reaches Decline");
        QVERIFY(decline->property("visualFocus").toBool());
        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY2(!card->isAncestorOf(window.activeFocusItem()) && window.activeFocusItem() != card,
                 "Tab from Decline must leave the card (no keyboard trap)");
        QTest::keyClick(&window, Qt::Key_Backtab);
        QVERIFY2(decline->hasActiveFocus(), "Shift+Tab re-enters at Decline");

        card->setProperty("pending", true);
        QTRY_VERIFY_WITH_TIMEOUT(card->hasActiveFocus(), 2000);
        card->setProperty("pending", false);
        before->forceActiveFocus();
        card->setProperty("errorText", QStringLiteral("nope"));
        QTest::qWait(100);
        QVERIFY2(before->hasActiveFocus(), "an error must not steal focus from elsewhere");
        card->setProperty("pending", true);
        QTest::qWait(100);
        QVERIFY2(before->hasActiveFocus(), "pending set elsewhere must not steal focus");
    }

    void theInviteAvatarFollowsTheAccountChoice()
    {
        QQmlApplicationEngine engine;
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("InvitePreviewCard"));
        QVERIFY(created.count() > 0 || created.wait(5000));
        auto *card = created.at(0).at(0).value<QObject *>();
        QVERIFY(card);
        card->setProperty("roomId", QStringLiteral("!r:test"));
        card->setProperty("avatarUrl", QStringLiteral("mxc://evil.example/pic"));
        auto *avatar = card->findChild<QObject *>(QStringLiteral("invitePreviewAvatar"));
        QVERIFY(avatar);
        QCOMPARE(avatar->property("mxc").toString(), QString());
        card->setProperty("showAvatar", true);
        QCOMPARE(avatar->property("mxc").toString(),
                 QStringLiteral("mxc://evil.example/pic"));
        card->setProperty("showAvatar", false);
        QCOMPARE(avatar->property("mxc").toString(), QString());
        QFile pane(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/TimelinePane.qml"));
        QVERIFY(pane.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(pane.readAll());
        const int at = src.indexOf(QStringLiteral("id: invitePreviewCard"));
        QVERIFY(at > 0);
        const QString block = src.mid(at, 1500);
        QVERIFY(block.contains(QStringLiteral("app.accountPrefs.inviteAvatarsAllowed")));
        QVERIFY(src.contains(QStringLiteral("readonly property bool inviteAvatarWithheld:")));
        const int header = src.indexOf(QStringLiteral("objectName: \"roomHeaderAvatar\""));
        QVERIFY(header > 0);
        QVERIFY(src.mid(header, 400).contains(QStringLiteral("mxc: root.inviteAvatarWithheld")));
        delete card;
    }

    void theInviterOpensTheirProfileAndSharedRoomsAreShown()
    {
        QQuickWindow window;
        window.resize(640, 520);
        QQmlApplicationEngine engine;
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("InvitePreviewCard"));
        QVERIFY(created.count() > 0 || created.wait(5000));
        auto *card = qobject_cast<QQuickItem *>(created.at(0).at(0).value<QObject *>());
        QVERIFY(card);
        card->setParentItem(window.contentItem());
        card->setSize(QSizeF(600, 500));
        card->setProperty("roomId", QStringLiteral("!r:test"));
        card->setProperty("inviter", QStringLiteral("Stranger"));
        card->setProperty("inviterUserId", QStringLiteral("@stranger:evil.example"));
        card->setProperty("commonRoomsText", QStringLiteral("No rooms in common"));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, 2000));

        auto *hint = card->findChild<QQuickItem *>(QStringLiteral("invitePreviewCommonRooms"));
        QVERIFY2(hint, "the invite preview says nothing about shared rooms");
        QVERIFY(hint->isVisible());
        QCOMPARE(hint->property("text").toString(), QStringLiteral("No rooms in common"));
        auto *area = card->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewInviterProfileArea"));
        QVERIFY2(area, "the inviter cannot be opened from the invite preview");
        QSignalSpy asked(card, SIGNAL(inviterProfileRequested(QQuickItem*)));
        QVERIFY(asked.isValid());
        QTest::mouseClick(&window, Qt::LeftButton, {},
                          area->mapToScene(QPointF(area->width() / 2,
                                                   area->height() / 2)).toPoint());
        QCOMPARE(asked.count(), 1);

        auto *inviterLine = card->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewInviter"));
        QVERIFY(inviterLine);
        card->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY2(inviterLine->hasActiveFocus(),
                 "the inviter is not in the keyboard tab chain");
        QTest::keyClick(&window, Qt::Key_Return);
        QTest::keyClick(&window, Qt::Key_Space);
        QCOMPARE(asked.count(), 3);
        QTest::keyClick(&window, Qt::Key_Tab);
        QVERIFY(card->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewAccept"))->hasActiveFocus());

        QVERIFY(area->property("enabled").toBool());
        card->setProperty("inviterUserId", QStringLiteral(""));
        QVERIFY(!area->property("enabled").toBool());
        card->setProperty("commonRoomsText", QString());
        QVERIFY(!hint->isVisible());

        QFile pane(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/TimelinePane.qml"));
        QVERIFY(pane.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(pane.readAll());
        const int at = src.indexOf(QStringLiteral("id: invitePreviewCard"));
        QVERIFY(at > 0);
        const QString block = src.mid(at, 3000);
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.mutualRoomHint(inviterUserId)")));
        QVERIFY(block.contains(QStringLiteral("senderProfilePopover.openFor(")));
        QVERIFY(block.contains(QStringLiteral("roomScoped: false")));
        delete card;
    }

    void aWithheldInvitersCardLoadsNoPictureOrBanner()
    {
        for (const bool withhold : {false, true}) {
            QQmlEngine engine;
            QQmlComponent stubComponent(&engine);
            stubComponent.setData(QByteArrayLiteral(R"(
import QtQuick
QtObject {
    property string currentRoomId: ""
    property QtObject banners: QtObject {
        property int revision: 0
        property int requests: 0
        function request(id) { requests += 1 }
        function bannerFor(id) { return "mxc://evil.example/banner" }
    }
    property QtObject mediaBridge: QtObject { property bool supported: false }
    property QtObject moderation: QtObject {
        property bool supported: false
        property bool busy: false
    }
    property QtObject conversations: QtObject { property bool supported: false }
    function textFontWithEmoji(f) { return f }
}
)"),
                                  QUrl(QStringLiteral("qrc:/withheld-appstub.qml")));
            std::unique_ptr<QObject> stub(stubComponent.create());
            QVERIFY2(stub, qPrintable(stubComponent.errorString()));
            engine.rootContext()->setContextProperty(QStringLiteral("app"), stub.get());
            QQmlComponent scene(&engine);
            scene.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 500
    height: 600
    visible: true
    MemberProfilePopover { objectName: "popover"; parent: Overlay.overlay }
}
)"),
                          QUrl(QStringLiteral("qrc:/withheld-scene.qml")));
            std::unique_ptr<QObject> root(scene.create());
            QVERIFY2(root, qPrintable(scene.errorString()));
            auto *popover = root->findChild<QObject *>(QStringLiteral("popover"));
            QVERIFY(popover);
            QVariantMap member{
                {QStringLiteral("userId"), QStringLiteral("@stranger:evil.example")},
                {QStringLiteral("displayName"), QStringLiteral("Stranger")},
                {QStringLiteral("avatarUrl"), QStringLiteral("mxc://evil.example/face")},
                {QStringLiteral("roomScoped"), false},
                {QStringLiteral("withholdMedia"), withhold}};
            QMetaObject::invokeMethod(popover, "openFor", Q_ARG(QVariant, member),
                                      Q_ARG(QVariant, QVariant()));
            QTRY_VERIFY(popover->property("opened").toBool());
            auto *avatar = root->findChild<QObject *>(QStringLiteral("profileCardAvatar"));
            auto *banner = root->findChild<QObject *>(QStringLiteral("profileBannerImage"));
            QVERIFY(avatar && banner);
            auto *banners = stub->property("banners").value<QObject *>();
            if (withhold) {
                QCOMPARE(avatar->property("mxc").toString(), QString());
                QCOMPARE(banner->property("mxc").toString(), QString());
                QCOMPARE(banners->property("requests").toInt(), 0);
            } else {
                QCOMPARE(avatar->property("mxc").toString(),
                         QStringLiteral("mxc://evil.example/face"));
                QCOMPARE(banner->property("mxc").toString(),
                         QStringLiteral("mxc://evil.example/banner"));
                QCOMPARE(banners->property("requests").toInt(), 1);
            }
        }
    }
};
QTEST_MAIN(InvitePreviewCardQmlTest)
#include "InvitePreviewCardQmlTest.moc"
