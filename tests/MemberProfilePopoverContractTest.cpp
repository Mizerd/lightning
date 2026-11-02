// The member profile popover, rendered offscreen from the real
// MemberProfilePopover.qml against a logged-in AppController on the mock
// backend: openFor(), startOrOpenDm() and Copy ID are driven, omitted
// affordances never render (not even as disabled placeholders), and the
// DM-reuse and Copy ID mechanics are unchanged.

#include <QtTest/QtTest>

#include <limits>

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/RoomInfoController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 500
    height: 600
    visible: true
    color: AppTheme.background

    MemberProfilePopover {
        id: popover
        objectName: "popover"
        parent: Overlay.overlay
    }

    // The timeline's shape: a surface whose TapHandler takes focus, with an
    // affordance inside whose own TapHandler opens the card. Both see a tap.
    Item {
        id: focusSurface
        objectName: "focusSurface"
        width: 200
        height: 60
        TapHandler { onTapped: focusSurface.forceActiveFocus() }
        Rectangle {
            objectName: "cardOpener"
            anchors.fill: parent
            color: "transparent"
            TapHandler {
                onTapped: popover.openFor({ userId: "@carol:mock.local",
                                            displayName: "Carol", avatarUrl: "" })
            }
        }
    }

    // An anchored open: the rect is in overlay coordinates.
    function openAt(userId, x, y, w, h) {
        popover.openFor({ userId: userId, displayName: "", avatarUrl: "" },
                        { x: x, y: y, width: w, height: h })
    }

    function openFor(userId, displayName, membership, role, isOwn) {
        popover.openFor({
            userId: userId,
            displayName: displayName,
            membership: membership,
            role: role,
            avatarUrl: "",
            isOwn: isOwn
        })
    }

    // EXACTLY what qml/MessageDelegate.qml's `mention:` branch hands over:
    // a user id and two empty strings. Keep it byte-identical in shape — the
    // whole point of the case below is that this caller knows nothing else.
    function openFromMentionLink(userId) {
        popover.openFor({
            userId: userId,
            displayName: "",
            avatarUrl: ""
        })
    }
}
)QML";

}

class MemberProfilePopoverContractTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QObject *find(const QString &name) const
    {
        return m_root->findChild<QObject *>(name);
    }

private slots:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(5000));
        QTRY_VERIFY(m_controller->loggedIn());

        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                     m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("memberprofilescene.qml")));
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

    void openingAMemberCardScopesRoomInformationToTheCurrentRoom()
    {
        const QString roomId = QStringLiteral("!general:mock.local");
        m_controller->setCurrentRoomId(roomId);
        QCOMPARE(m_controller->currentRoomId(), roomId);
        m_controller->roomInfo()->setRoomId(QString{});
        QVERIFY(m_controller->roomInfo()->roomId().isEmpty());

        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@carol:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Carol")),
                                  Q_ARG(QVariant, QStringLiteral("join")),
                                  Q_ARG(QVariant, QString{}),
                                  Q_ARG(QVariant, false));

        QVERIFY2(m_controller->roomInfo()->roomId() == roomId,
                 "opening a member card left Room Information scoped to "
                 "another room, so every admin control on the card refuses "
                 "itself: an admin who reached the card from an avatar or a "
                 "mention sees no Kick, no Ban and no Set role at all");
    }

    void popoverIsFixed340WideAndNonModal()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QCOMPARE(popover->property("width").toInt(), 340);
        QCOMPARE(popover->property("modal").toBool(), false);
    }

    void anAnchoredCardOpensBesideItsAnchorAndStaysInsideTheWindow()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const auto open = [&](int x, int y, int w, int h) {
            QVERIFY(QMetaObject::invokeMethod(
                m_root, "openAt", Q_ARG(QVariant, QStringLiteral("@bob:mock.local")),
                Q_ARG(QVariant, x), Q_ARG(QVariant, y), Q_ARG(QVariant, w),
                Q_ARG(QVariant, h)));
            QTRY_VERIFY(popover->property("opened").toBool());
        };
        const auto rect = [&] {
            return QRectF(popover->property("x").toReal(),
                          popover->property("y").toReal(),
                          popover->property("width").toReal(),
                          popover->property("height").toReal());
        };
        const auto close = [&] {
            QMetaObject::invokeMethod(popover, "close");
            QTRY_VERIFY(!popover->property("visible").toBool());
        };

        open(20, 100, 40, 20);
        QRectF r = rect();
        QCOMPARE(r.x(), 68.0);
        QVERIFY2(r.y() >= 8 && r.bottom() <= m_window->height() - 8,
                 qPrintable(QStringLiteral("y=%1 bottom=%2")
                                .arg(r.y()).arg(r.bottom())));
        QVERIFY(qAbs(r.y() - 100) < 1.0 || r.bottom() >= m_window->height() - 9);
        close();

        open(440, 100, 40, 20);
        r = rect();
        QVERIFY2(r.right() <= 440 - 8 + 0.5,
                 qPrintable(QStringLiteral("right=%1").arg(r.right())));
        QVERIFY(r.x() >= 8);
        close();

        open(200, 580, 100, 20);
        r = rect();
        QVERIFY(r.x() >= 8 && r.right() <= m_window->width() - 8 + 0.5);
        QVERIFY2(r.bottom() <= m_window->height() - 8 + 0.5 && r.y() >= 8,
                 qPrintable(QStringLiteral("y=%1 bottom=%2")
                                .arg(r.y()).arg(r.bottom())));
        close();

        open(0, 0, 4, 4);
        r = rect();
        QVERIFY(r.x() >= 8 && r.y() >= 8);
        close();
    }

    void aSecondClickReplacesTheCardInsteadOfStackingAnother()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QStringLiteral("@bob:mock.local")),
            Q_ARG(QVariant, 20), Q_ARG(QVariant, 80), Q_ARG(QVariant, 40),
            Q_ARG(QVariant, 20)));
        QTRY_VERIFY(popover->property("opened").toBool());
        const qreal firstY = popover->property("y").toReal();
        QCOMPARE(popover->property("userId").toString(),
                 QStringLiteral("@bob:mock.local"));
        QVERIFY(QMetaObject::invokeMethod(
            m_root, "openAt", Q_ARG(QVariant, QStringLiteral("@carol:mock.local")),
            Q_ARG(QVariant, 20), Q_ARG(QVariant, 200), Q_ARG(QVariant, 40),
            Q_ARG(QVariant, 20)));
        QTRY_COMPARE(popover->property("userId").toString(),
                     QStringLiteral("@carol:mock.local"));
        QVERIFY(popover->property("opened").toBool());
        QVERIFY2(popover->property("y").toReal() > firstY,
                 "the card did not move to the second anchor");
        QMetaObject::invokeMethod(popover, "close");
        QTRY_VERIFY(!popover->property("visible").toBool());
    }

    void escapeAndAnOutsidePressCloseTheCard()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const auto open = [&] {
            QVERIFY(QMetaObject::invokeMethod(
                m_root, "openAt",
                Q_ARG(QVariant, QStringLiteral("@bob:mock.local")),
                Q_ARG(QVariant, 20), Q_ARG(QVariant, 100), Q_ARG(QVariant, 40),
                Q_ARG(QVariant, 20)));
            QTRY_VERIFY(popover->property("opened").toBool());
            QTest::qWait(30);
        };
        open();
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY2(!popover->property("visible").toBool(),
                     "Escape did not close the anchored card");

        open();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          QPoint(10, m_window->height() - 10));
        QTRY_VERIFY2(!popover->property("visible").toBool(),
                     "an outside press did not close the anchored card");
    }

    void omittedAffordancesNeverRenderAsPlaceholders()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("\"call\"")));
        QVERIFY(!content.contains(QStringLiteral("\"videocam\"")));
        QVERIFY(content.contains(QStringLiteral("PresenceDot")));
        QVERIFY(!content.contains(QStringLiteral("presenceOnline")));

        QRegularExpression qsTrCall(QStringLiteral("qsTr\\(\"([^\"]*)\""));
        auto it = qsTrCall.globalMatch(content);
        while (it.hasNext()) {
            const QString text = it.next().captured(1);
            QVERIFY2(!text.contains(QStringLiteral("Verified")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("SHARED")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("View full profile")),
                     qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("Set Nickname")),
                     qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("Mutual Rooms")),
                     qPrintable(text));
        }
    }

    void theBioIsRenderedAsPlainTextAndNothingElse()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("profileBioText")));
        QVERIFY(content.contains(QStringLiteral("app.bio.bioFor")));
        QVERIFY(content.contains(QStringLiteral("app.bio.request")));
        QVERIFY(!content.contains(QStringLiteral("Text.StyledText")));
        QVERIFY(!content.contains(QStringLiteral("Text.RichText")));
        QVERIFY(!content.contains(QStringLiteral("Text.MarkdownText")));
        QVERIFY(!content.contains(QStringLiteral("linkActivated")));
        const int bioAt = content.indexOf(
            QStringLiteral("objectName: \"profileBioText\""));
        QVERIFY2(bioAt > 0, "the bio Text was not found");
        const int bioEnd = content.indexOf(
            QStringLiteral("Accessible.name: qsTr(\"Bio\")"), bioAt);
        QVERIFY2(bioEnd > bioAt, "the bio block's end anchor was not found");
        const QString bioBlock = content.mid(bioAt, bioEnd - bioAt);
        QVERIFY2(bioBlock.contains(QStringLiteral("text: root.bioText")),
                 "the anchors do not bracket the item that renders the bio");
        QVERIFY2(bioBlock.contains(QStringLiteral("textFormat: Text.PlainText")),
                 qPrintable(bioBlock.left(400)));
        QVERIFY2(bioBlock.contains(QStringLiteral("readOnly: true")),
                 qPrintable(bioBlock.left(600)));
        QVERIFY2(bioBlock.contains(QStringLiteral("selectByMouse: true")),
                 "the bio cannot be selected with the mouse");
        QVERIFY2(bioBlock.contains(QStringLiteral("selectByKeyboard: true")),
                 "the bio cannot be selected with the keyboard");
    }

    void thePresenceSentenceLivesOnlyInTheSharedDot()
    {
        QFile popover(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(popover.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(popover.readAll());
        QVERIFY(!content.contains(QStringLiteral("presenceLine")));
        QVERIFY(!content.contains(QStringLiteral("active %1 min ago")));
        QVERIFY(content.contains(QStringLiteral("hoverStatus: true")));

        QFile dot(QStringLiteral(QML_DIR "/PresenceDot.qml"));
        QVERIFY(dot.open(QIODevice::ReadOnly));
        const QString dotSrc = QString::fromUtf8(dot.readAll());
        QVERIFY(dotSrc.contains(QStringLiteral("statusText")));
        QVERIFY(dotSrc.contains(QStringLiteral("active %1 min ago")));
        QVERIFY(dotSrc.contains(QStringLiteral("ToolTip.text")));
    }

    void theBadgeNeverBorrowsATrustOrModerationSignal()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        const int at = content.indexOf(QStringLiteral("root.badgeLabel.length > 0"));
        QVERIFY2(at > 0, "the badge Loader was not found");
        const int end = content.indexOf(QStringLiteral("root.bioText.length > 0"), at);
        QVERIFY2(end > at, "the badge block's end anchor was not found");
        const QString block = content.mid(at, end - at);
        QVERIFY(block.contains(QStringLiteral("AppTheme.userColor(root.userId)")));
        QVERIFY2(!block.contains(QStringLiteral("verified_user")), "badge shield");
        QVERIFY2(!block.contains(QStringLiteral("\"shield\"")), "badge shield");
        QVERIFY2(!block.contains(QStringLiteral("\"check\"")), "badge check");
        QVERIFY2(!block.contains(QStringLiteral("\"lock\"")), "badge lock");
        QVERIFY2(!block.contains(QStringLiteral("trust")), "badge trust palette");
        QVERIFY(block.contains(QStringLiteral("root.badgeDescription")));
    }

    void dmReuseMechanicsArePreserved()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("checkExistingDm")));
        QVERIFY(content.contains(QStringLiteral("existingDms")));
        QVERIFY(content.contains(QStringLiteral("startDirectMessage")));
        QVERIFY(content.contains(QStringLiteral("app.openRoom")));
    }

    void roleRendersAsAStatusChipAndYouIndicatorIsKept()
    {
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@carol:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Carol")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("administrator")),
                                  Q_ARG(QVariant, false));
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QTRY_VERIFY(popover->property("opened").toBool());

        // In the open room the live power level outranks the snapshot the
        // caller passed (C35): the mock's Carol is an ordinary member there,
        // so drive the live role the way _refreshRole would for a level-100
        // member, and check the chip follows it both ways.
        const auto adminChipShown = [popover] {
            for (QObject *candidate : popover->findChildren<QObject *>()) {
                if (candidate->property("label").toString() == QStringLiteral("Administrator")
                    && candidate->property("visible").toBool()) {
                    return true;
                }
            }
            return false;
        };
        const QString live = popover->property("liveRole").toString();
        if (live.isEmpty())
            QVERIFY(adminChipShown());
        popover->setProperty("liveRole", QStringLiteral("administrator"));
        QVERIFY(adminChipShown());
        popover->setProperty("liveRole", QStringLiteral("member"));
        QVERIFY(!adminChipShown());
        popover->setProperty("liveRole", live);

        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@alice:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Alice")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, true));
        QVERIFY(popover->property("isOwn").toBool());
        QObject *messageButton = nullptr;
        for (QObject *candidate : popover->findChildren<QObject *>()) {
            if (candidate->property("text").toString() == QStringLiteral("Message")) {
                messageButton = candidate;
                break;
            }
        }
        if (messageButton)
            QVERIFY(!messageButton->property("visible").toBool());
        popover->setProperty("visible", false);
    }

    void openingWithOnlyAUserIdResolvesTheNameAndTheFace()
    {
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        const QString room = QStringLiteral("!general:mock.local");
        const QString bob = QStringLiteral("@bob:mock.local");

        // The mock's seeded members have no avatar; give this one a face.
        MemberInfo member;
        member.userId = bob;
        member.displayName = QStringLiteral("Bob Mockworth");
        member.avatarMxcUrl = QStringLiteral("mxc://mock.local/bob-face");
        mock->setRoomMemberForTest(room, member);

        m_controller->openRoom(room);
        auto *roomInfo = m_controller->roomInfo();
        QVERIFY(roomInfo);
        roomInfo->setRoomId(room);
        roomInfo->refreshMembers();
        QTRY_VERIFY(!roomInfo->memberFor(bob).isEmpty());

        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QMetaObject::invokeMethod(m_root, "openFromMentionLink",
                                  Q_ARG(QVariant, bob));
        QTRY_VERIFY(popover->property("opened").toBool());

        QCOMPARE(popover->property("displayName").toString(),
                 QStringLiteral("Bob Mockworth"));
        QCOMPARE(popover->property("avatarMxc").toString(),
                 QStringLiteral("mxc://mock.local/bob-face"));
        QCOMPARE(popover->property("visibleName").toString(),
                 QStringLiteral("Bob Mockworth"));
        popover->setProperty("visible", false);
    }

    void theChipRowActuallyOccupiesSpace()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@bob:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Bob")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, false));
        QTRY_VERIFY(popover->property("opened").toBool());

        auto *share = qobject_cast<QQuickItem *>(
            find(QStringLiteral("profileShareButton")));
        QVERIFY(share);
        auto *overflow = qobject_cast<QQuickItem *>(
            find(QStringLiteral("profileOverflowButton")));
        QVERIFY(overflow);
        QTRY_VERIFY(share->isVisible());
        QVERIFY2(share->width() > 0 && share->height() > 0,
                 qPrintable(QStringLiteral("share chip collapsed: %1x%2")
                                .arg(share->width())
                                .arg(share->height())));
        QVERIFY2(overflow->width() > 0 && overflow->height() > 0,
                 qPrintable(QStringLiteral("overflow chip collapsed: %1x%2")
                                .arg(overflow->width())
                                .arg(overflow->height())));
        auto *row = qobject_cast<QQuickItem *>(share->parentItem());
        QVERIFY(row);
        QVERIFY2(row->height() > 0, "the chip row collapsed to zero height");

        QGuiApplication::clipboard()->clear();
        QMetaObject::invokeMethod(share, "clicked");
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://matrix.to/#/%40bob%3Amock.local"));
        popover->setProperty("visible", false);
    }

    void theBadgeHoldersCardActuallyRendersTheBadge()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const QString holder =
            QStringLiteral("@romanticanimegerl:cutefunny.art");
        QMetaObject::invokeMethod(m_root, "openFromMentionLink",
                                  Q_ARG(QVariant, holder));
        QTRY_VERIFY(popover->property("opened").toBool());
        QCOMPARE(popover->property("badgeLabel").toString(),
                 QStringLiteral("idea master"));

        bool rendered = false;
        for (QObject *candidate : popover->findChildren<QObject *>()) {
            if (candidate->property("text").toString()
                    == QStringLiteral("idea master")
                && candidate->property("visible").toBool()) {
                rendered = true;
                break;
            }
        }
        QVERIFY2(rendered, "the badge pill never reached the card");

        QMetaObject::invokeMethod(m_root, "openFromMentionLink",
                                  Q_ARG(QVariant,
                                        QStringLiteral("@bob:mock.local")));
        QCOMPARE(popover->property("badgeLabel").toString(), QString());
        auto badgeStillShowing = [popover]() {
            for (QObject *candidate : popover->findChildren<QObject *>()) {
                if (candidate->property("text").toString()
                        == QStringLiteral("idea master")
                    && candidate->property("visible").toBool()) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY(!badgeStillShowing());
        popover->setProperty("visible", false);
    }

    void aUserTheRosterDoesNotHoldIsNeverFabricated()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const QString stranger = QStringLiteral("@nobody:elsewhere.example");
        QMetaObject::invokeMethod(m_root, "openFromMentionLink",
                                  Q_ARG(QVariant, stranger));
        QTRY_VERIFY(popover->property("opened").toBool());

        QCOMPARE(popover->property("displayName").toString(), QString());
        QCOMPARE(popover->property("avatarMxc").toString(), QString());
        QCOMPARE(popover->property("visibleName").toString(),
                 QStringLiteral("nobody"));
        popover->setProperty("visible", false);
    }

    void copyIdShowsAndClearsTheClipboardNotice()
    {
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@dave:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Dave")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, false));
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QTRY_VERIFY(popover->property("opened").toBool());

        auto *copyButton = find(QStringLiteral("profileCopyIdButton"));
        QVERIFY(copyButton);
        QMetaObject::invokeMethod(copyButton, "clicked");
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("@dave:mock.local"));
    }

    void theChipRowSharesOneVerticalCentre()
    {
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@dave:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Dave")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, false));
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QTRY_VERIFY(popover->property("opened").toBool());

        QStringList report;
        QList<qreal> centres;
        for (const QString &name : { QStringLiteral("profileHomeserverChip"),
                                     QStringLiteral("profileShareButton"),
                                     QStringLiteral("profileOverflowButton") }) {
            auto *item = qobject_cast<QQuickItem *>(find(name));
            QVERIFY2(item != nullptr, qPrintable(name));
            if (!item->isVisible())
                continue;
            const QPointF scene = item->mapToScene(QPointF(0, 0));
            const qreal centre = scene.y() + item->height() / 2.0;
            centres.append(centre);
            report << QStringLiteral("%1 y=%2 h=%3 centre=%4")
                          .arg(name).arg(scene.y()).arg(item->height()).arg(centre);
        }
        QVERIFY2(centres.size() >= 2, qPrintable(report.join(QLatin1String("; "))));
        for (const qreal c : centres) {
            QVERIFY2(qAbs(c - centres.first()) < 1.5,
                     qPrintable(QStringLiteral("chips are not on one centre: ")
                                + report.join(QLatin1String("; "))));
        }

        for (const QString &name : { QStringLiteral("profileShareButton"),
                                     QStringLiteral("profileOverflowButton") }) {
            auto *item = qobject_cast<QQuickItem *>(find(name));
            QVERIFY(item);
            if (!item->isVisible())
                continue;
            auto *content = item->property("contentItem").value<QQuickItem *>();
            QVERIFY2(content != nullptr, qPrintable(name + QStringLiteral(" has no contentItem")));
            const qreal itemCentre = item->height() / 2.0;
            const qreal contentCentre = content->y() + content->height() / 2.0;
            QVERIFY2(qAbs(itemCentre - contentCentre) < 1.5,
                     qPrintable(QStringLiteral(
                         "%1 content is off centre vertically: item h=%2 "
                         "centre=%3, content y=%4 h=%5 centre=%6")
                             .arg(name).arg(item->height()).arg(itemCentre)
                             .arg(content->y()).arg(content->height())
                             .arg(contentCentre)));

            qreal minX = std::numeric_limits<qreal>::max();
            qreal maxX = std::numeric_limits<qreal>::lowest();
            const auto kids = content->childItems();
            for (QQuickItem *kid : kids) {
                if (!kid->isVisible() || kid->width() <= 0)
                    continue;
                const QPointF topLeft = kid->mapToItem(item, QPointF(0, 0));
                minX = qMin(minX, topLeft.x());
                maxX = qMax(maxX, topLeft.x() + kid->width());
            }
            QVERIFY2(maxX > minX,
                     qPrintable(name + QStringLiteral(" has no painted content")));
            const qreal paintedCentreX = (minX + maxX) / 2.0;
            const qreal itemCentreX = item->width() / 2.0;
            QVERIFY2(qAbs(itemCentreX - paintedCentreX) < 1.5,
                     qPrintable(QStringLiteral(
                         "%1 content is off centre horizontally: item w=%2 "
                         "centre=%3, painted %4..%5 centre=%6")
                             .arg(name).arg(item->width()).arg(itemCentreX)
                             .arg(minX).arg(maxX).arg(paintedCentreX)));
        }
    }

    void theBoltWatermarkYieldsToARealBanner()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        QVERIFY(!src.isEmpty());
        const int iconAt = src.indexOf(QStringLiteral("name: \"bolt\""));
        QVERIFY2(iconAt > 0, "the watermark no longer draws a bolt");
        const int at = src.lastIndexOf(QStringLiteral("Item {"), iconAt);
        QVERIFY2(at > 0, "the watermark block is gone");
        const QString block = src.mid(at, iconAt - at);
        QVERIFY2(block.contains(QStringLiteral("visible: !bannerImage.visible")),
                 "the bolt is painted over a user's own banner");
    }

    void aBannerRecoversFromATransientMediaFailure()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        const int at = src.indexOf(QStringLiteral("id: bannerImage"));
        QVERIFY2(at > 0, "the banner Image is gone");
        const int nextId = src.indexOf(QStringLiteral("id: "), at + 20);
        const QString block =
            nextId > at ? src.mid(at, nextId - at) : src.mid(at, 3000);
        QVERIFY2(block.contains(QStringLiteral("function onMediaCached(")),
                 "the banner no longer re-resolves when its bytes land");
        QVERIFY2(block.contains(QStringLiteral("function onMediaRetryable(")),
                 "a banner whose fetch failed once stays absent all session");
        QVERIFY2(!block.contains(QStringLiteral("bannerImage.source =")),
                 "the banner binding is destroyed by an imperative assignment");
    }

    void mutualRoomsAreAbsentUntilAnyAreKnown()
    {
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@dave:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Dave")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, false));
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QTRY_VERIFY(popover->property("opened").toBool());

        // The mock has no mutual rooms: no header, no rows.
        auto *header = find(QStringLiteral("profileMutualRoomsHeader"));
        if (header)
            QVERIFY2(!header->property("visible").toBool(),
                     "the section claims rooms in common that are not known");
        QVERIFY2(find(QStringLiteral("profileMutualRoom")) == nullptr,
                 "a mutual-room row exists with no mutual rooms");

        QFile popoverSrc(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(popoverSrc.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(popoverSrc.readAll());
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("requestMutualRooms")),
                 "the overflow never asks for the rooms it means to list");
    }

    void anInvisibleDisplayNameFallsBackToTheLocalpart()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const QStringList invisible{
            QStringLiteral("ㅤ"),
            QStringLiteral("⠀⠀"),
            QStringLiteral(" ​‍️ "),
            QString::fromUcs4(U"\U000E0020\U000E0041"),
        };
        for (const QString &name : invisible) {
            QMetaObject::invokeMethod(
                m_root, "openFor",
                Q_ARG(QVariant, QStringLiteral("@bram:mock.local")),
                Q_ARG(QVariant, name), Q_ARG(QVariant, QStringLiteral("join")),
                Q_ARG(QVariant, QString{}), Q_ARG(QVariant, false));
            QTRY_VERIFY(popover->property("opened").toBool());
            QCOMPARE(popover->property("visibleName").toString(),
                     QStringLiteral("bram"));
            auto *label = find(QStringLiteral("profileDisplayName"));
            QVERIFY(label);
            QCOMPARE(label->property("text").toString(), QStringLiteral("bram"));
            popover->setProperty("visible", false);
            QTRY_VERIFY(!popover->property("opened").toBool());
        }

        QMetaObject::invokeMethod(
            m_root, "openFor",
            Q_ARG(QVariant, QStringLiteral("@bram:mock.local")),
            Q_ARG(QVariant, QStringLiteral("ㅤBram")),
            Q_ARG(QVariant, QStringLiteral("join")),
            Q_ARG(QVariant, QString{}), Q_ARG(QVariant, false));
        QTRY_VERIFY(popover->property("opened").toBool());
        QCOMPARE(popover->property("visibleName").toString(),
                 QStringLiteral("ㅤBram"));
        popover->setProperty("visible", false);
    }

    void openForActionLandsOnTheConfirmStepOnlyWhenAllowed()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QVariantMap member{
            { QStringLiteral("userId"), QStringLiteral("@carol:mock.local") },
            { QStringLiteral("displayName"), QStringLiteral("Carol") },
        };
        // The mock roster reports no moderation power, so the card opens
        // without the confirm step rather than offering a doomed action.
        QMetaObject::invokeMethod(popover, "openForAction",
                                  Q_ARG(QVariant, member),
                                  Q_ARG(QVariant, QStringLiteral("ban")),
                                  Q_ARG(QVariant, QVariant()));
        QTRY_VERIFY(popover->property("opened").toBool());
        QCOMPARE(popover->property("showBan").toBool(), false);
        QCOMPARE(popover->property("modAction").toString(), QString());
        popover->setProperty("visible", false);
        QTRY_VERIFY(!popover->property("opened").toBool());

        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        const int at = src.indexOf(QStringLiteral("function openForAction("));
        QVERIFY2(at > 0, "openForAction was not found");
        const QString body = src.mid(at, src.indexOf(QLatin1Char('}'), at) - at);
        QVERIFY(body.contains(QStringLiteral("op === \"kick\" && showKick")));
        QVERIFY(body.contains(QStringLiteral("op === \"ban\" && showBan")));
        QVERIFY(body.contains(QStringLiteral("op === \"unban\" && showUnban")));
    }

    void loweringYourOwnRoleIsWarned()
    {
        auto *popover = find(QStringLiteral("popover"));
        auto *confirm = find(QStringLiteral("profileRoleConfirmDialog"));
        auto *warning = find(QStringLiteral("profileSelfDemotionWarning"));
        QVERIFY(popover);
        QVERIFY(confirm);
        QVERIFY2(warning, "no self-demotion warning in the role confirmation");
        const int own = int(m_controller->roomInfo()->ownPowerLevel());

        popover->setProperty("isOwn", true);
        confirm->setProperty("pendingLevel", own - 10);
        QCOMPARE(confirm->property("selfDemotion").toBool(), true);
        QCOMPARE(confirm->property("irreversible").toBool(), false);
        QCOMPARE(confirm->property("title").toString(),
                 QStringLiteral("Lower your own role?"));
        QVERIFY(warning->property("text").toString()
                    .contains(QStringLiteral("raise it back")));

        popover->setProperty("isOwn", false);
        QCOMPARE(confirm->property("selfDemotion").toBool(), false);
        confirm->setProperty("pendingLevel", own);
        QCOMPARE(confirm->property("irreversible").toBool(), true);
        QCOMPARE(confirm->property("title").toString(),
                 QStringLiteral("Change role?"));
        confirm->setProperty("pendingLevel", 0);

        // A v12 creator (2^53 from the bridge) cannot be lowered at all, so
        // the warning never offers it.
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        const int at = src.indexOf(QStringLiteral("readonly property bool selfDemotion:"));
        QVERIFY(at > 0);
        QVERIFY(src.mid(at, 300).contains(
            QStringLiteral("app.roomInfo.ownPowerLevel < 9007199254740992")));
    }

    void aBannedMemberIsOfferedNoRoleButtons()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        popover->setProperty("roleOptions",
                             QVariantList{ QVariantMap{ { "level", 50 } } });
        popover->setProperty("membership", QStringLiteral("banned"));
        QVERIFY(QMetaObject::invokeMethod(popover, "_refreshRole",
                                          Q_ARG(QVariant, true)));
        QCOMPARE(popover->property("roleOptions").toList().size(), 0);
        popover->setProperty("membership", QStringLiteral("left"));
        popover->setProperty("roleOptions",
                             QVariantList{ QVariantMap{ { "level", 50 } } });
        QVERIFY(QMetaObject::invokeMethod(popover, "_refreshRole",
                                          Q_ARG(QVariant, true)));
        QCOMPARE(popover->property("roleOptions").toList().size(), 0);
        popover->setProperty("membership", QString());

        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        const int fn = src.indexOf(QStringLiteral("function _refreshRole("));
        QVERIFY(fn > 0);
        const int guard = src.indexOf(QStringLiteral("membership === \"banned\""), fn);
        const int build = src.indexOf(QStringLiteral("powerLevelFor(userId)"), fn);
        QVERIFY(guard > 0 && build > 0 && guard < build);
    }

    void theRoleChipFollowsTheLivePowerLevel()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        popover->setProperty("role", QStringLiteral("moderator"));
        popover->setProperty("liveRole", QString());
        QCOMPARE(popover->property("badgeRole").toString(),
                 QStringLiteral("moderator"));
        popover->setProperty("liveRole", QStringLiteral("administrator"));
        QCOMPARE(popover->property("badgeRole").toString(),
                 QStringLiteral("administrator"));
        popover->setProperty("role", QString());
        popover->setProperty("liveRole", QString());
    }

    void theCreatorPillSaysCreatorNotAdministrator()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const auto chipShown = [popover](const QString &label) {
            for (QObject *candidate : popover->findChildren<QObject *>()) {
                if (candidate->property("label").toString() == label
                    && candidate->property("visible").toBool()) {
                    return true;
                }
            }
            return false;
        };
        popover->setProperty("role", QString());
        popover->setProperty("liveRole", QStringLiteral("creator"));
        QCOMPARE(popover->property("badgeRole").toString(),
                 QStringLiteral("creator"));
        QVERIFY(chipShown(QStringLiteral("Creator")));
        QVERIFY(!chipShown(QStringLiteral("Administrator")));
        popover->setProperty("liveRole", QStringLiteral("administrator"));
        QVERIFY(chipShown(QStringLiteral("Administrator")));
        QVERIFY(!chipShown(QStringLiteral("Creator")));
        popover->setProperty("liveRole", QString());
    }

    void theLiveRoleMappingMatchesTheRosterTiers()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const QList<QPair<double, QString>> cases = {
            { 0, "member" }, { 49, "member" }, { 50, "moderator" },
            { 75, "moderator" }, { 99, "moderator" }, { 100, "administrator" },
            { 150, "administrator" }, { 9007199254740992.0, "creator" } };
        for (const auto &c : cases) {
            QVariant out;
            QVERIFY(QMetaObject::invokeMethod(
                popover, "_roleForLevel", Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, c.first)));
            QCOMPARE(out.toString(), c.second);
        }
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        QVERIFY(src.contains(QStringLiteral("liveRole = _roleForLevel(current)")));
    }

    void theCardsButtonsShowFocusAndAnswerEnter()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        for (const char *id : { "roleButton", "modButton" }) {
            const QString name = QString::fromLatin1(id);
            QVERIFY2(src.contains(name + QStringLiteral(".visualFocus")),
                     qPrintable(name + QStringLiteral(" draws no focus ring")));
        }
        for (const char *anchor : { "id: roleButton", "id: modButton" }) {
            const QString a = QString::fromLatin1(anchor);
            const int at = src.indexOf(a);
            QVERIFY2(at > 0, qPrintable(a + QStringLiteral(" not found")));
            int open = at, depth = 0;
            while (open > 0) {
                --open;
                if (src[open] == QLatin1Char('}'))
                    ++depth;
                else if (src[open] == QLatin1Char('{') && depth-- == 0)
                    break;
            }
            int close = open;
            for (int d = 0; close < src.size(); ++close) {
                if (src[close] == QLatin1Char('{'))
                    ++d;
                else if (src[close] == QLatin1Char('}') && --d == 0)
                    break;
            }
            const QString block = src.mid(open, close - open + 1);
            QVERIFY2(block.contains(QStringLiteral("Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }"))
                         && block.contains(QStringLiteral("Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }")),
                     qPrintable(a + QStringLiteral(" does not activate on Return/Enter")));
        }
        {
            const int at = src.indexOf(QStringLiteral("id: modConfirmButton"));
            QVERIFY(at > 0);
            const int open = src.lastIndexOf(QStringLiteral("AppButton {"), at);
            QVERIFY2(open > 0 && at - open < 200, "modConfirmButton is not an AppButton");
            const QString block = src.mid(at, 1800);
            const int next = block.indexOf(QStringLiteral("Button {"));
            const QString own = next > 0 ? block.left(next) : block;
            QVERIFY2(!own.contains(QStringLiteral("Keys.onReturnPressed")),
                     "modConfirmButton has its own Return handler");
        }
        for (const char *name : { "profileIgnoreConfirmCancel", "profileIgnoreConfirmAccept",
                                  "profileRoleConfirmCancel", "profileRoleConfirmAccept" }) {
            const QString a = QStringLiteral("objectName: \"%1\"").arg(QLatin1String(name));
            const int at = src.indexOf(a);
            QVERIFY2(at > 0, name);
            const int open = src.lastIndexOf(QStringLiteral("AppButton {"), at);
            QVERIFY2(open > 0 && at - open < 120, qPrintable(QStringLiteral("%1 is not an AppButton").arg(QLatin1String(name))));
            const int next = src.indexOf(QStringLiteral("Button {"), at);
            QVERIFY2(!src.mid(at, next - at).contains(QStringLiteral("Keys.onReturnPressed")), name);
        }
        QVERIFY(src.count(QStringLiteral("Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }")) >= 4);
        QVERIFY(src.count(QStringLiteral("Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }")) >= 4);
        QVERIFY(src.contains(QStringLiteral("onAccepted: if (modConfirmButton.enabled) modConfirmButton.clicked()")));
        QVERIFY(src.contains(QStringLiteral("contentItem.forceActiveFocus()")));
    }

    void aReopenedCardHoldsNoFocusedButton()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        for (int i = 0; i < 2; ++i) {
            QMetaObject::invokeMethod(m_root, "openFor",
                                      Q_ARG(QVariant, QStringLiteral("@carol:mock.local")),
                                      Q_ARG(QVariant, QStringLiteral("Carol")),
                                      Q_ARG(QVariant, QStringLiteral("join")),
                                      Q_ARG(QVariant, QString{}),
                                      Q_ARG(QVariant, false));
            QTRY_VERIFY(popover->property("opened").toBool());
            auto *focus = m_window->activeFocusItem();
            QVERIFY(focus);
            QVERIFY2(!focus->inherits("QQuickAbstractButton")
                         && !QString::fromLatin1(focus->metaObject()->className())
                                .contains(QStringLiteral("Button")),
                     "a button holds focus on open");
            auto *btn = popover->findChild<QQuickItem *>(
                QStringLiteral("profileShareButton"));
            QVERIFY(btn);
            QVERIFY(btn->isVisible());
            btn->forceActiveFocus();
            QVERIFY2(btn->hasActiveFocus(), "stale-focus setup did not take");
            QMetaObject::invokeMethod(popover, "close");
            QTRY_VERIFY(!popover->property("visible").toBool());
        }
    }

    void escapeClosesTheCard()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        if (popover->property("visible").toBool()) {
            QMetaObject::invokeMethod(popover, "close");
            QTRY_VERIFY(!popover->property("visible").toBool());
        }

        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@carol:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Carol")),
                                  Q_ARG(QVariant, QStringLiteral("join")),
                                  Q_ARG(QVariant, QString{}),
                                  Q_ARG(QVariant, false));
        QTRY_VERIFY(popover->property("opened").toBool());

        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY2_WITH_TIMEOUT(!popover->property("visible").toBool(),
                                  "Escape left the member card open", 2000);
    }

    void escapeClosesACardOpenedByATapAFocusTakerAlsoSaw()
    {
        auto *popover = find(QStringLiteral("popover"));
        auto *opener = qobject_cast<QQuickItem *>(find(QStringLiteral("cardOpener")));
        QVERIFY(popover);
        QVERIFY(opener);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        if (popover->property("visible").toBool()) {
            QMetaObject::invokeMethod(popover, "close");
            QTRY_VERIFY(!popover->property("visible").toBool());
        }

        const QPoint at = opener->mapToScene(QPointF(opener->width() / 2,
                                                     opener->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_VERIFY(popover->property("opened").toBool());
        QTest::qWait(50);

        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY2_WITH_TIMEOUT(!popover->property("visible").toBool(),
                                  "Escape left a tap-opened member card open", 2000);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MemberProfilePopoverContractTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MemberProfilePopoverContractTest.moc"
