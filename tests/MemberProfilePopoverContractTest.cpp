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
        anchors.centerIn: parent
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

} // namespace

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

    // Opening a member card scopes Room Information to the current room, so
    // the card's moderation controls (gated on that scope, since power levels
    // belong to the scoped room) appear however the card was opened. Driven
    // through the real openFor().
    void openingAMemberCardScopesRoomInformationToTheCurrentRoom()
    {
        const QString roomId = QStringLiteral("!general:mock.local");
        m_controller->setCurrentRoomId(roomId);
        QCOMPARE(m_controller->currentRoomId(), roomId);
        // The state a timeline avatar click finds: Room Information never
        // opened for this room.
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

    void popoverIsFixed296WideAndCentredModal()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QCOMPARE(popover->property("width").toInt(), 296);
        QCOMPARE(popover->property("modal").toBool(), true);
    }

    void omittedAffordancesNeverRenderAsPlaceholders()
    {
        // Icon literals of omitted actions never appear (they do not collide
        // with prose).
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("\"call\"")));
        QVERIFY(!content.contains(QStringLiteral("\"videocam\"")));
        // Not banned here: "more_horiz" (a real overflow menu with real
        // actions, deliberately without "Set Nickname"), "block" (the real ban
        // action) and presence (the shared PresenceDot, so the popover paints
        // no presence colours of its own).
        QVERIFY(content.contains(QStringLiteral("PresenceDot")));
        QVERIFY(!content.contains(QStringLiteral("presenceOnline")));

        // No user-facing qsTr() string mentions the omitted affordances;
        // comments documenting the omission are fine.
        QRegularExpression qsTrCall(QStringLiteral("qsTr\\(\"([^\"]*)\""));
        auto it = qsTrCall.globalMatch(content);
        while (it.hasNext()) {
            const QString text = it.next().captured(1);
            QVERIFY2(!text.contains(QStringLiteral("Verified")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("SHARED")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("View full profile")),
                     qPrintable(text));
            // Ignore is a real m.ignored_user_list action now.
            QVERIFY2(!text.contains(QStringLiteral("Set Nickname")),
                     qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("Mutual Rooms")),
                     qPrintable(text));
        }
    }

    // The bio is remote free text rendered as plain text only: MSC4440's own
    // example embeds <img src="mxc://…">, which a rich renderer would fetch
    // for everyone opening the card.
    void theBioIsRenderedAsPlainTextAndNothingElse()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("profileBioText")));
        QVERIFY(content.contains(QStringLiteral("app.bio.bioFor")));
        // The request is a side effect of opening, never inside a binding.
        QVERIFY(content.contains(QStringLiteral("app.bio.request")));
        QVERIFY(!content.contains(QStringLiteral("Text.StyledText")));
        QVERIFY(!content.contains(QStringLiteral("Text.RichText")));
        QVERIFY(!content.contains(QStringLiteral("Text.MarkdownText")));
        QVERIFY(!content.contains(QStringLiteral("linkActivated")));
        // Every Text/Label rendering the bio names PlainText explicitly.
        // Anchored on expressions at both ends rather than a fixed window,
        // which comments inside the block would defeat.
        const int bioAt = content.indexOf(
            QStringLiteral("objectName: \"profileBioText\""));
        QVERIFY2(bioAt > 0, "the bio Text was not found");
        const int bioEnd = content.indexOf(
            QStringLiteral("Accessible.name: qsTr(\"Bio\")"), bioAt);
        QVERIFY2(bioEnd > bioAt, "the bio block's end anchor was not found");
        const QString bioBlock = content.mid(bioAt, bioEnd - bioAt);
        // The scan must bracket the item that renders the bio.
        QVERIFY2(bioBlock.contains(QStringLiteral("text: root.bioText")),
                 "the anchors do not bracket the item that renders the bio");
        QVERIFY2(bioBlock.contains(QStringLiteral("textFormat: Text.PlainText")),
                 qPrintable(bioBlock.left(400)));
    }

    // Presence wording lives only in PresenceDot; a second copy here would
    // drift.
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

    // A decorative badge never borrows trust or moderation vocabulary: it uses
    // the holder's identity ink, with no shield, check, lock or trust palette.
    void theBadgeNeverBorrowsATrustOrModerationSignal()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        // Anchored on expressions at both ends, not a fixed window.
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
        // ...and it explains itself.
        QVERIFY(block.contains(QStringLiteral("root.badgeDescription")));
    }

    void dmReuseMechanicsArePreserved()
    {
        // The DM-reuse call sequence (checkExistingDm -> existingDms ->
        // openRoom, else startDirectMessage) is intact, never a bare room
        // create.
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

        bool foundAdminChip = false;
        for (QObject *candidate : popover->findChildren<QObject *>()) {
            if (candidate->property("label").toString() == QStringLiteral("Administrator")
                && candidate->property("visible").toBool()) {
                foundAdminChip = true;
                break;
            }
        }
        QVERIFY(foundAdminChip);

        // "(you)" only for the own-account member.
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@alice:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Alice")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, true));
        QVERIFY(popover->property("isOwn").toBool());
        // The own-account card never shows Message (there is no self-DM
        // action).
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

    // Opening a card with only a user id (e.g. from a mention) resolves the
    // display name and avatar itself, instead of relying on every call site
    // to pass them.
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
        // ...and the card renders the display name, not the id it was handed.
        QCOMPARE(popover->property("visibleName").toString(),
                 QStringLiteral("Bob Mockworth"));
        popover->setProperty("visible", false);
    }

    // The chip row (a Flow in a ColumnLayout) occupies real space: a
    // positioner whose height depends on a width not yet assigned can settle
    // at zero while everything reads visible. Measures geometry.
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
        // ...and the row itself has real height.
        auto *row = qobject_cast<QQuickItem *>(share->parentItem());
        QVERIFY(row);
        QVERIFY2(row->height() > 0, "the chip row collapsed to zero height");

        // Share copies the public matrix.to profile link, and the notice names
        // what went to the clipboard.
        QGuiApplication::clipboard()->clear();
        QMetaObject::invokeMethod(share, "clicked");
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://matrix.to/#/%40bob%3Amock.local"));
        popover->setProperty("visible", false);
    }

    // The badge is actually rendered (it lives behind a Loader, so a table
    // row is not evidence).
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

        // An ordinary user's card shows no badge. Loader destruction is
        // deferred, so assert that nothing with that text is visible.
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

    // Nothing is invented for a user the roster does not hold (left the room,
    // members not fetched): the localpart fallback, and no avatar rather than
    // a wrong one.
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

    // The chip row shares one vertical centre, measured on laid-out items: a
    // Flow top-aligns its children, and the two chip kinds differ in height.
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

        // The content inside each button is centred too: a Control forced
        // taller than its contentItem leaves the content where it was.
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

            // Horizontally, measured on the painted children (icon and
            // label): a Control stretches its contentItem to full width, so
            // comparing that centre would always pass.
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

    // The bolt watermark stands in for a missing banner, so it yields once a
    // real banner image is ready (tied to image readiness, not the mxc
    // string, so it does not vanish early).
    void theBoltWatermarkYieldsToARealBanner()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("Storm §3.6 corner watermark"));
        QVERIFY2(at > 0, "the watermark block is gone");
        const int iconAt = src.indexOf(QStringLiteral("name: \"bolt\""), at);
        QVERIFY2(iconAt > at, "the watermark no longer draws a bolt");
        const QString block = src.mid(at, iconAt - at);
        QVERIFY2(block.contains(QStringLiteral("visible: !bannerImage.visible")),
                 "the bolt is painted over a user's own banner");
    }

    // A banner that failed to fetch recovers: wideImageSource() answers ""
    // while a transient failure mark stands, so both cache completion and
    // mark expiry must bump the re-resolve counter (as Avatar.qml does).
    void aBannerRecoversFromATransientMediaFailure()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(file.readAll());
        const int at = src.indexOf(QStringLiteral("id: bannerImage"));
        QVERIFY2(at > 0, "the banner Image is gone");
        // Bounded to the banner's own block (up to the next `id: `), so
        // handlers elsewhere in the file cannot satisfy the check.
        const int nextId = src.indexOf(QStringLiteral("id: "), at + 20);
        const QString block =
            nextId > at ? src.mid(at, nextId - at) : src.mid(at, 3000);
        QVERIFY2(block.contains(QStringLiteral("function onMediaCached(")),
                 "the banner no longer re-resolves when its bytes land");
        QVERIFY2(block.contains(QStringLiteral("function onMediaRetryable(")),
                 "a banner whose fetch failed once stays absent all session");
        // Still a counter, never an assignment to `source`, which would
        // destroy the binding.
        QVERIFY2(!block.contains(QStringLiteral("bannerImage.source =")),
                 "the banner binding is destroyed by an imperative assignment");
    }

    // Rooms in common are absent, not shown empty, until some are known: the
    // list reads only membership the store already holds (asking would cost a
    // /state per room), so an empty section would claim too much.
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

        // Opening the overflow must ask, or the list never populates.
        QFile popoverSrc(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(popoverSrc.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(popoverSrc.readAll());
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("requestMutualRooms")),
                 "the overflow never asks for the rooms it means to list");
    }

    // A display name made only of invisible characters is a real name on the
    // wire, and drew an empty title line: the card falls back to the
    // localpart as for an absent name, and the rendered label says so.
    void anInvisibleDisplayNameFallsBackToTheLocalpart()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        const QStringList invisible{
            QStringLiteral("ㅤ"),               // Hangul filler
            QStringLiteral("⠀⠀"),         // Braille blank
            QStringLiteral(" ​‍️ "), // spaces and format chars
            QString::fromUcs4(U"\U000E0020\U000E0041"), // tag characters
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

        // A name with any visible character is kept as written.
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

    // The People list's right-click menu reaches the card's own confirm step,
    // so a room kick is two clicks from the list and still confirmed.
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
                                  Q_ARG(QVariant, QStringLiteral("ban")));
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
        // Each op is gated on its own offer flag.
        QVERIFY(body.contains(QStringLiteral("op === \"kick\" && showKick")));
        QVERIFY(body.contains(QStringLiteral("op === \"ban\" && showBan")));
        QVERIFY(body.contains(QStringLiteral("op === \"unban\" && showUnban")));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MemberProfilePopoverContractTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MemberProfilePopoverContractTest.moc"
