// The invite preview card's keyboard contract (C83-F01): it is a Tab stop
// followed by Accept and Decline with a visible ring, Tab can LEAVE it (no
// keyboard trap), showing it never steals focus, and focus returns to it when
// its buttons disable under the user.

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
        // TimelinePane's covered timeline: earlier in the tree than the card,
        // so Tab reaches it first unless it is disabled (the TimelinePane
        // hunk, modelled here by setEnabled(false)).
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

        // Showing the card must not steal focus from the list that opened it.
        before->forceActiveFocus();
        card->setVisible(false);
        card->setVisible(true);
        QTest::qWait(50);
        QVERIFY2(before->hasActiveFocus(), "showing the card must not take focus");

        // Without the TimelinePane hunk Tab walks the covered content first.
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

        // Buttons disabling under the user must not drop focus out of the card.
        card->setProperty("pending", true);
        QTRY_VERIFY_WITH_TIMEOUT(card->hasActiveFocus(), 2000);
        card->setProperty("pending", false);
        // Focus the user moved elsewhere must NOT be pulled back (async error,
        // or the list's own Accept/Reject setting pending).
        before->forceActiveFocus();
        card->setProperty("errorText", QStringLiteral("nope"));
        QTest::qWait(100);
        QVERIFY2(before->hasActiveFocus(), "an error must not steal focus from elsewhere");
        card->setProperty("pending", true);
        QTest::qWait(100);
        QVERIFY2(before->hasActiveFocus(), "pending set elsewhere must not steal focus");
    }
};
QTEST_MAIN(InvitePreviewCardQmlTest)
#include "InvitePreviewCardQmlTest.moc"
