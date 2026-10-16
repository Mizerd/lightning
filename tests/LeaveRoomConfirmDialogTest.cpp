// The shared leave-room confirmation (qml/LeaveRoomConfirmDialog.qml), loaded
// from the real module: the wording must be honest about what leaving costs
// (a sole member empties the room; an invite-only room needs a new invite),
// never promise "invited again later" when nobody can, and keep an unknown
// count on the neutral text. Both hosts must use it, not a private copy.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 500
    height: 600
    visible: true
    LeaveRoomConfirmDialog { objectName: "dlg" }
}
)QML";

QString readQml(const QString &name)
{
    QFile f(QStringLiteral(QML_DIR "/") + name);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                       : QString();
}

} // namespace

class LeaveRoomConfirmDialogTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QObject *m_dlg = nullptr;

    QString bodyFor(const QVariant &members, const QString &rule,
                    const QVariant &invited = QVariant())
    {
        QMetaObject::invokeMethod(m_dlg, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("!r:x")),
                                  Q_ARG(QVariant, QStringLiteral("Den")),
                                  Q_ARG(QVariant, members),
                                  Q_ARG(QVariant, rule),
                                  Q_ARG(QVariant, invited));
        const QString text = m_dlg->property("bodyText").toString();
        QMetaObject::invokeMethod(m_dlg, "close");
        return text;
    }

private slots:
    void initTestCase()
    {
        m_engine = new QQmlEngine;
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("leave.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_dlg = m_root->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(m_dlg);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
    }

    void aSoleMemberIsToldTheRoomWillBeEmpty()
    {
        // Closed rule, nobody invited: the one case where it is true.
        const QString t = bodyFor(1, QStringLiteral("invite"), 0);
        QVERIFY2(t.contains(QStringLiteral("only member")), qPrintable(t));
        QVERIFY2(t.contains(QStringLiteral("empty")), qPrintable(t));
        QVERIFY2(!t.contains(QStringLiteral("invited again later")),
                 "promised a re-invite nobody can send");
        QVERIFY(bodyFor(1, QStringLiteral("private"), 0)
                    .contains(QStringLiteral("empty")));
    }

    void noClaimOfAnEmptyRoomWhereAWayBackExists()
    {
        const QString e = QStringLiteral("empty");
        // Public and restricted rooms can be joined again.
        QVERIFY(!bodyFor(1, QStringLiteral("public"), 0).contains(e));
        QVERIFY(!bodyFor(1, QStringLiteral("restricted"), 0).contains(e));
        // Pending invitees can still join: the room is not empty.
        QVERIFY(!bodyFor(1, QStringLiteral("invite"), 2).contains(e));
        // Unknown rule or unknown invite count: neutral, never a promise.
        QVERIFY(!bodyFor(1, QString(), 0).contains(e));
        QVERIFY(!bodyFor(1, QStringLiteral("invite")).contains(e));
        QVERIFY(!bodyFor(1, QStringLiteral("invite"), -1).contains(e));
    }

    void anInviteOnlyRoomNeedsANewInvitation()
    {
        const QString t = bodyFor(5, QStringLiteral("invite"));
        QVERIFY2(t.contains(QStringLiteral("new invitation")), qPrintable(t));
        QVERIFY(!t.contains(QStringLiteral("invited again later")));
    }

    void anUnknownCountGetsTheNeutralTextNotAClaim()
    {
        const QString t = bodyFor(QVariant(), QString());
        QVERIFY2(t.contains(QStringLiteral("invited again later")),
                 qPrintable(t));
        QVERIFY(!t.contains(QStringLiteral("only member")));
        QVERIFY(bodyFor(-1, QString()).contains(QStringLiteral("invited again")));
    }

    // break-it s03: a public room said "you can be invited again later",
    // when anyone can simply join it again. Fails on the old dialog, which
    // gave every rule but invite/knock/private the re-invite text.
    void aPublicRoomSaysItCanBeJoinedAgain()
    {
        const QString t = bodyFor(5, QStringLiteral("public"));
        QVERIFY2(t.contains(QStringLiteral("join it again at any time")),
                 qPrintable(t));
        QVERIFY2(!t.contains(QStringLiteral("invited again")), qPrintable(t));
        // Not over-applied: restricted and unknown rules keep the neutral
        // text, invite-only keeps its own.
        QVERIFY(!bodyFor(5, QStringLiteral("restricted"))
                     .contains(QStringLiteral("join it again")));
        QVERIFY(!bodyFor(5, QString()).contains(QStringLiteral("join it again")));
        QVERIFY(bodyFor(5, QStringLiteral("invite"))
                    .contains(QStringLiteral("new invitation")));
    }

    // break-it s03: a 300-character unbroken name did not wrap (WordWrap
    // cannot break one "word") and ran past the dialog and the window. Fails
    // on the old dialog: the body's content was wider than the body, and the
    // whole name was quoted.
    void aLongUnbrokenNameStaysInsideTheDialog()
    {
        const QString name(300, QLatin1Char('N'));
        QMetaObject::invokeMethod(m_dlg, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("!r:x")),
                                  Q_ARG(QVariant, name),
                                  Q_ARG(QVariant, QVariant(5)),
                                  Q_ARG(QVariant, QStringLiteral("public")),
                                  Q_ARG(QVariant, QVariant(0)));
        QTRY_VERIFY(m_dlg->property("opened").toBool());
        auto *body = m_dlg->findChild<QQuickItem *>(
            QStringLiteral("leaveRoomConfirmBody"));
        QVERIFY(body);
        QCoreApplication::processEvents();
        const qreal contentWidth = body->property("contentWidth").toReal();
        QVERIFY2(contentWidth <= body->width() + 0.5,
                 qPrintable(QStringLiteral("body content %1 px in a %2 px body")
                                .arg(contentWidth).arg(body->width())));
        auto *window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(window);
        const QPointF right = body->mapToScene(
            QPointF(body->width(), body->height()));
        QVERIFY2(right.x() <= window->width() && right.y() <= window->height(),
                 "the body runs past the window");
        // The room is identified, not reproduced.
        const QString text = m_dlg->property("bodyText").toString();
        QVERIFY2(!text.contains(name), "the whole 300-character name is quoted");
        QVERIFY(text.contains(QString(80, QLatin1Char('N')) + QChar(0x2026)));
        QMetaObject::invokeMethod(m_dlg, "close");
        QTRY_VERIFY(!m_dlg->property("visible").toBool());
    }

    void confirmEmitsTheRoomIdAfterClosing()
    {
        QSignalSpy spy(m_dlg, SIGNAL(confirmed(QString)));
        QMetaObject::invokeMethod(m_dlg, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("!r:x")),
                                  Q_ARG(QVariant, QStringLiteral("Den")),
                                  Q_ARG(QVariant, 3),
                                  Q_ARG(QVariant, QStringLiteral("public")),
                                  // openFor takes five arguments since the
                                  // invited count; a call with four matches
                                  // nothing and opens nothing.
                                  Q_ARG(QVariant, QVariant(0)));
        QTRY_VERIFY(m_dlg->property("opened").toBool());
        auto *btn = m_dlg->findChild<QObject *>(
            QStringLiteral("leaveRoomConfirmButton"));
        QVERIFY(btn);
        QVERIFY(QMetaObject::invokeMethod(btn, "clicked"));
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("!r:x"));
    }

    // The two hosts share this component; a private copy would let the
    // wording drift again.
    void bothHostsUseTheSharedDialog()
    {
        const QString rooms = readQml(QStringLiteral("RoomsPanel.qml"));
        const QString info = readQml(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(rooms.contains(QStringLiteral("LeaveRoomConfirmDialog {")));
        QVERIFY(info.contains(QStringLiteral("LeaveRoomConfirmDialog {")));
        QVERIFY(!rooms.contains(QStringLiteral("you can be invited again later")));
        QVERIFY(!info.contains(QStringLiteral("invited again later")));
    }
};

QTEST_MAIN(LeaveRoomConfirmDialogTest)
#include "LeaveRoomConfirmDialogTest.moc"
