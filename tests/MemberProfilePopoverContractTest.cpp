// v0.6.5 (SPEC 1p): offscreen proof for the redesigned member profile
// popover content. Loads the real MemberProfilePopover.qml against a real
// AppController on the mock backend (logged in, so app.conversations is
// actually usable), drives openFor()/startOrOpenDm()/Copy ID, and asserts
// the omitted affordances (call/videocam/more_horiz/Ignore/Verified
// chip/SHARED rooms/View full profile) never rendered — no disabled
// placeholders in their place — while the DM-reuse and Copy ID mechanics
// are preserved byte-for-byte.

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

    void popoverIsFixed296WideAndCentredModal()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QCOMPARE(popover->property("width").toInt(), 296);
        QCOMPARE(popover->property("modal").toBool(), true);
    }

    void omittedAffordancesNeverRenderAsPlaceholders()
    {
        // Icon literals for the omitted actions must never appear at all —
        // these strings do not collide with prose (unlike the words below).
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("\"call\"")));
        QVERIFY(!content.contains(QStringLiteral("\"videocam\"")));
        // "more_horiz" left this list on 2026-08-28, the same way "block"
        // did: it is now the glyph of a REAL overflow menu carrying only
        // actions that exist (Copy user ID, and the account-wide ignore
        // through ModerationController). It is not an empty affordance, and
        // the menu deliberately has no "Set Nickname" row because Lightning
        // has no per-user nickname to set — which the qsTr sweep below pins.
        // "block" left this list on 2026-08-14: it is now the icon of the
        // REAL ban action (SDK Room::ban_user through RoomInfoController),
        // not an Ignore placeholder. Ignore itself remains omitted below.
        //
        // Presence left this list with the v0.7.x presence round: the
        // popover now carries the REAL shared indicator (PresenceDot +
        // status line backed by PresenceManager polling), so the contract
        // flipped from "never present" to "present via the shared
        // component" — the popover must not paint its own presence colours.
        QVERIFY(content.contains(QStringLiteral("PresenceDot")));
        QVERIFY(!content.contains(QStringLiteral("presenceOnline")));

        // No qsTr() user-facing string ever mentions the omitted
        // affordances — explanatory source comments documenting the
        // omission are not user-facing wording, so this checks qsTr() call
        // sites specifically rather than banning the words file-wide.
        QRegularExpression qsTrCall(QStringLiteral("qsTr\\(\"([^\"]*)\""));
        auto it = qsTrCall.globalMatch(content);
        while (it.hasNext()) {
            const QString text = it.next().captured(1);
            QVERIFY2(!text.contains(QStringLiteral("Verified")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("SHARED")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("View full profile")),
                     qPrintable(text));
            // "Ignore" left this list in v0.7.x: the account-wide ignore is
            // now a REAL m.ignored_user_list action on the popover, not an
            // omitted mock affordance.
            QVERIFY2(!text.contains(QStringLiteral("Set Nickname")),
                     qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("Mutual Rooms")),
                     qPrintable(text));
        }
    }

    // The bio is remote free text. It may be rendered as PLAIN TEXT and
    // nothing else — MSC4440's own example embeds an <img src="mxc://…">,
    // so a StyledText/RichText renderer here would fetch remote media of the
    // profile owner's choosing for everyone who opened the card (§6).
    void theBioIsRenderedAsPlainTextAndNothingElse()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("profileBioText")));
        QVERIFY(content.contains(QStringLiteral("app.bio.bioFor")));
        // The ASK is a side effect on the open edge, never inside a binding.
        QVERIFY(content.contains(QStringLiteral("app.bio.request")));
        QVERIFY(!content.contains(QStringLiteral("Text.StyledText")));
        QVERIFY(!content.contains(QStringLiteral("Text.RichText")));
        QVERIFY(!content.contains(QStringLiteral("Text.MarkdownText")));
        QVERIFY(!content.contains(QStringLiteral("linkActivated")));
        // Every Text/Label that renders the bio must name PlainText
        // explicitly rather than relying on the default.
        //
        // Anchored on the EXPRESSION at BOTH ends, never on a fixed window
        // after a name. A `content.mid(at, N)` scan is defeated the moment
        // somebody adds a comment inside the block — the assertion then
        // measures the wrong text and starts passing on broken code (or,
        // as here, failing on correct code). CLAUDE.md records that trap
        // four times over; this file will not make it a fifth.
        const int bioAt = content.indexOf(
            QStringLiteral("objectName: \"profileBioText\""));
        QVERIFY2(bioAt > 0, "the bio Text was not found");
        const int bioEnd = content.indexOf(
            QStringLiteral("Accessible.name: qsTr(\"Bio\")"), bioAt);
        QVERIFY2(bioEnd > bioAt, "the bio block's end anchor was not found");
        const QString bioBlock = content.mid(bioAt, bioEnd - bioAt);
        // The scan must actually be looking at the item that renders the
        // bio, or "it contains PlainText" is a claim about some other Text.
        QVERIFY2(bioBlock.contains(QStringLiteral("text: root.bioText")),
                 "the anchors do not bracket the item that renders the bio");
        QVERIFY2(bioBlock.contains(QStringLiteral("textFormat: Text.PlainText")),
                 qPrintable(bioBlock.left(400)));
    }

    // The standalone presence status line is GONE, and the state is
    // formatted in exactly one place — PresenceDot. A second copy of the
    // wording here is how the two drift apart.
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

    // A decorative badge must never borrow the vocabulary of a trust or a
    // moderation signal. The tint is the holder's own identity ink and the
    // treatment carries no shield, check, lock or trust palette.
    void theBadgeNeverBorrowsATrustOrModerationSignal()
    {
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        // Anchored on the EXPRESSION at both ends, never on a fixed window
        // after a name: a source scan cut at N characters is defeated the
        // moment somebody adds a comment, and this file has recorded that
        // trap four times.
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
        // ...and it explains itself rather than leaving the treatment to
        // imply what it is.
        QVERIFY(block.contains(QStringLiteral("root.badgeDescription")));
    }

    void dmReuseMechanicsArePreserved()
    {
        // The exact existing DM-reuse call sequence must still be present
        // (checkExistingDm -> existingDms -> openRoom, else
        // startDirectMessage), never a bare/duplicate room create.
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
        // The own-account row never shows the Message button (there is no
        // real self-DM action).
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

    // Reported 2026-08-28: clicking a user from a MENTION in message text
    // opened their card with no picture and their MXID where the display
    // name should be, while clicking the same person's avatar one line above
    // was correct. The two call sites in MessageDelegate differ only in what
    // they PASS — the mention branch has nothing but a user id — so the fix
    // is that the card resolves what it was not given, rather than a sixth
    // call site being told to remember.
    void openingWithOnlyAUserIdResolvesTheNameAndTheFace()
    {
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        const QString room = QStringLiteral("!general:mock.local");
        const QString bob = QStringLiteral("@bob:mock.local");

        // The mock's seeded members carry no avatar, and an avatar is half
        // of the report — so give this one a face before asking for it.
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
        // ...and the name the card actually renders is the display name, not
        // the id it was handed.
        QCOMPARE(popover->property("visibleName").toString(),
                 QStringLiteral("Bob Mockworth"));
        popover->setProperty("visible", false);
    }

    // The chip row is a Flow inside a ColumnLayout, which is a shape with a
    // known Qt hazard: a positioner whose implicit height depends on a width
    // the layout has not given it yet can settle at zero and take the whole
    // row off the card while every `visible` still reads true. So this
    // measures GEOMETRY, not visibility.
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
        // ...and the row itself has real height, not just its children.
        auto *row = qobject_cast<QQuickItem *>(share->parentItem());
        QVERIFY(row);
        QVERIFY2(row->height() > 0, "the chip row collapsed to zero height");

        // Share copies the PUBLIC matrix.to profile link, and the notice
        // names what went to the clipboard — "Share" must not be a control
        // whose effect the user cannot see.
        QGuiApplication::clipboard()->clear();
        QMetaObject::invokeMethod(share, "clicked");
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://matrix.to/#/%40bob%3Amock.local"));
        popover->setProperty("visible", false);
    }

    // The badge, actually RENDERED. It lives behind a Loader (a Label born
    // holding "" keeps ItemObservesViewport for its whole life — §16), so
    // "the table has a row" is not evidence that anything reaches the card.
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

        // ...and an ordinary user's card shows no badge. The Loader's item
        // is destroyed rather than emptied, but destruction is DEFERRED, so
        // the honest assertion is that nothing carrying that text is still
        // VISIBLE — not that the object has already gone.
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

    // The other half of the same rule: nothing may be INVENTED for a user
    // the roster does not hold — somebody who has left, or a room whose
    // members were never fetched. The localpart fallback every surface
    // already shares is the honest answer, and the avatar stays empty
    // because a wrong face is worse than no face.
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

    // The chip row shares ONE vertical centre.
    //
    // Two attempts at this failed, so it MEASURES the laid-out items rather
    // than reading the source. A Flow lays its children out top-aligned and
    // the two chip kinds disagree on height — StatusChip is
    // max(chipHeight, content+4), ProfileChipButton is chipHeight+6 — so
    // without a common height the shorter ones hang off a common top edge.
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

        // AND THE CONTENT INSIDE THEM. The items sharing a centre is not the
        // same claim as the label inside each one being centred: a Control
        // whose height is forced larger than its contentItem's leaves that
        // content wherever the control put it, and what a reader sees is the
        // TEXT, not the box around it.
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

            // HORIZONTALLY, MEASURED ON THE PAINTED CHILDREN.
            //
            // Comparing the contentItem's centre to the button's is VACUOUS:
            // a Control stretches its contentItem across the full available
            // width, so that centre always matches while the children inside
            // it are packed at x=0. This case passed twice against a button
            // whose dots were visibly left of centre, which is exactly the
            // shape of a measurement that cannot fail.
            //
            // So it measures the union of the CHILDREN — the icon and the
            // label — which is what a reader actually sees.
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

    // The bolt watermark belongs to the GRADIENT, not to somebody's banner.
    //
    // It stands in for a banner nobody has set. Once a user has one, the
    // banner is their picture and stamping a logo across it is neither ours
    // to do nor what they chose. Tied to the IMAGE'S READINESS rather than to
    // the mxc string, so it does not blink out before the picture it is
    // making way for has arrived.
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

    // Rooms in common, as Sable lists them — and ABSENT rather than shown
    // empty when none are known.
    //
    // That distinction is the point: the list reads only membership the
    // store already holds, because asking would cost one /state per room
    // every time a card opened (the rule that governs the room-list call
    // glyph). So "no rooms known" and "no rooms in common" are different
    // claims, and an empty section would assert the stronger one.
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

        // The mock backend answers no mutual rooms, so the header must not
        // be drawn and no room row may exist.
        auto *header = find(QStringLiteral("profileMutualRoomsHeader"));
        if (header)
            QVERIFY2(!header->property("visible").toBool(),
                     "the section claims rooms in common that are not known");
        QVERIFY2(find(QStringLiteral("profileMutualRoom")) == nullptr,
                 "a mutual-room row exists with no mutual rooms");

        // Opening the overflow must ASK, or the list can never populate.
        QFile popoverSrc(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(popoverSrc.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(popoverSrc.readAll());
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("requestMutualRooms")),
                 "the overflow never asks for the rooms it means to list");
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MemberProfilePopoverContractTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MemberProfilePopoverContractTest.moc"
