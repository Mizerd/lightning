
#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>

#include <memory>

class SpaceLobbyKeyboardQmlTest : public QObject
{
    Q_OBJECT

    struct H {
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QObject> owner;
        QQuickItem *root = nullptr;
        QQuickItem *lobby = nullptr;
    };

    bool build(H &h)
    {
        h.engine = std::make_unique<QQmlEngine>();
        QQmlComponent c(h.engine.get());
        c.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Rectangle {
    width: 640; height: 900
    color: AppTheme.background
    property string log: ""
    SpaceLobby {
        id: lobbyUnderTest
        objectName: "lobbyUnderTest"
        x: 20; width: 600
        canManage: true
        sections: [
          { sectionId: "!home:x", isRoot: true, roomId: "!home:x", name: "",
            selectable: false, roomCount: 2, matchCount: 2, collapsed: false,
            rows: [
              { roomId: "!general:x", name: "General", topic: "t",
                joined: true, isSpace: false, membership: "joined",
                selectable: true },
              { roomId: "!offer:x", name: "Unjoined", topic: "", joined: false,
                isSpace: false, membership: "", joinRule: "public",
                via: ["x"], selectable: false }
            ] },
          { sectionId: "!subA:x", isRoot: false, roomId: "!subA:x",
            name: "Comunidade", selectable: true, roomCount: 0, matchCount: 0,
            collapsed: false, rows: [] }
        ]
        onOpenRoomRequested: (id) => parent.log += "open:" + id + ";"
        onSelectionToggled: (id) => parent.log += "select:" + id + ";"
        onCollapseToggled: (id, c) => parent.log += "collapse:" + id + ";"
    }
    SpaceMemberChip {
        objectName: "chipUnderTest"
        y: 700
        modelData: ({ userId: "@bo:x", displayName: "Bo", avatarUrl: "" })
        onProfileRequested: (m) => parent.log += "profile:" + m.userId + ";"
    }
}
)"),
                  QUrl(QStringLiteral("qrc:/lobbykbtest.qml")));
        if (!c.errors().isEmpty()) {
            qWarning("%s", qPrintable(c.errorString()));
            return false;
        }
        h.owner.reset(c.create());
        h.root = qobject_cast<QQuickItem *>(h.owner.get());
        if (!h.root)
            return false;
        h.lobby = h.root->findChild<QQuickItem *>(
            QStringLiteral("lobbyUnderTest"));
        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(640, 900);
        h.root->setParentItem(h.window->contentItem());
        h.window->show();
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
            h.lobby->polish();
        }
        h.window->requestActivate();
        return QTest::qWaitForWindowExposed(h.window.get());
    }

    static void collect(QQuickItem *it, const QString &name,
                        QList<QQuickItem *> &out)
    {
        if (!it)
            return;
        if (it->objectName() == name && it->isVisible())
            out.append(it);
        for (QQuickItem *ch : it->childItems())
            collect(ch, name, out);
    }
    static QList<QQuickItem *> all(QQuickItem *root, const char *name)
    {
        QList<QQuickItem *> out;
        collect(root, QString::fromLatin1(name), out);
        return out;
    }
    static QString log(H &h) { return h.root->property("log").toString(); }
    QObject *menu(H &h)
    {
        return h.lobby->findChild<QObject *>(
            QStringLiteral("spaceLobbySectionMenu"));
    }
    static QPoint center(QQuickItem *it)
    {
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2))
            .toPoint();
    }

private Q_SLOTS:
    void joinedRowsAreTabStopsThatOpenFromTheKeyboard()
    {
        H h;
        QVERIFY(build(h));
        const auto rows = all(h.lobby, "spaceUnifiedChildRow");
        QCOMPARE(rows.size(), 2);
        QVERIFY(rows[0]->property("activeFocusOnTab").toBool());
        QVERIFY2(!rows[1]->property("activeFocusOnTab").toBool(),
                 "an unjoined row opens nothing; its Join button is the stop");
        for (Qt::Key k : {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Space}) {
            h.root->setProperty("log", QString());
            rows[0]->forceActiveFocus();
            QVERIFY(rows[0]->hasActiveFocus());
            QTest::keyClick(h.window.get(), k);
            QCOMPARE(log(h), QStringLiteral("open:!general:x;"));
        }
        h.root->setProperty("log", QString());
        rows[1]->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QVERIFY(log(h).isEmpty());
    }

    void aFocusedRowDrawsARing()
    {
        H h;
        QVERIFY(build(h));
        QQuickItem *row = all(h.lobby, "spaceUnifiedChildRow")[0];
        QCOMPARE(row->property("border").value<QObject *>()
                     ->property("width").toInt(), 0);
        row->forceActiveFocus();
        QTRY_COMPARE(row->property("border").value<QObject *>()
                         ->property("width").toInt(), 2);
    }

    void aMemberChipIsANamedTabStopThatOpensItsProfile()
    {
        H h;
        QVERIFY(build(h));
        QQuickItem *chip = h.root->findChild<QQuickItem *>(
            QStringLiteral("chipUnderTest"));
        QVERIFY(chip);
        QVERIFY(chip->property("activeFocusOnTab").toBool());
        QQmlExpression name(qmlContext(chip), chip,
                            QStringLiteral("Accessible.name"));
        QCOMPARE(name.evaluate().toString(), QStringLiteral("Bo"));
        chip->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QCOMPARE(log(h), QStringLiteral("profile:@bo:x;"));
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("width").toInt(), 2);
    }

    void aSelectionBoxIsATabStopThatSpaceToggles()
    {
        H h;
        QVERIFY(build(h));
        const auto boxes = all(h.lobby, "spaceChildSelectBox");
        QVERIFY(!boxes.isEmpty());
        QVERIFY(boxes[0]->property("activeFocusOnTab").toBool());
        boxes[0]->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_Space);
        QVERIFY2(log(h).startsWith(QStringLiteral("select:")),
                 qPrintable(log(h)));
    }

    void aFocusedHeaderDrawsARingNotOnlyATint()
    {
        H h;
        QVERIFY(build(h));
        const auto headers = all(h.lobby, "spaceLobbySectionHeader");
        QCOMPARE(headers.size(), 2);
        const auto fills = all(h.lobby, "spaceLobbyHeaderFill");
        QVERIFY(fills.size() >= 2);
        auto bw = [&](QQuickItem *f) {
            return f->property("border").value<QObject *>()
                ->property("width").toInt();
        };
        QCOMPARE(headers[1]->property("sectionId").toString(),
                 QStringLiteral("!subA:x"));
        QCOMPARE(bw(fills[1]), 0);
        headers[1]->forceActiveFocus();
        QTRY_COMPARE(bw(fills[1]), 2);
    }

    void returnOnTheMenuButtonDoesNotFoldTheSection()
    {
        H h;
        QVERIFY(build(h));
        const auto btns = all(h.lobby, "spaceLobbySectionMenuButton");
        QVERIFY(!btns.isEmpty());
        btns.last()->forceActiveFocus();
        QVERIFY(btns.last()->hasActiveFocus());
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QVERIFY2(!log(h).contains(QStringLiteral("collapse:")),
                 qPrintable(log(h)));
        auto *hdr = all(h.lobby, "spaceLobbySectionHeader")[1];
        QCOMPARE(hdr->property("sectionId").toString(),
                 QStringLiteral("!subA:x"));
        hdr->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QVERIFY(log(h).contains(QStringLiteral("collapse:!subA:x")));
    }

    void rightClickOnTheMenuButtonDoesNotOpenTheHeaderMenuAtTheClick()
    {
        H h;
        QVERIFY(build(h));
        const auto btns = all(h.lobby, "spaceLobbySectionMenuButton");
        QVERIFY(!btns.isEmpty());
        QObject *m = menu(h);
        QTest::mouseClick(h.window.get(), Qt::RightButton, {},
                          center(btns.last()));
        QTest::qWait(150);
        QVERIFY(!m->property("opened").toBool());
    }

    void aSubspaceHeaderOpensTheSectionMenuFromRightClickAndKeys()
    {
        H h;
        QVERIFY(build(h));
        const auto headers = all(h.lobby, "spaceLobbySectionHeader");
        QCOMPARE(headers.size(), 2);
        QObject *m = menu(h);
        QVERIFY(m);

        QTest::mouseClick(h.window.get(), Qt::RightButton, {},
                          center(headers[1]) + QPoint(-100, 0));
        QTRY_VERIFY(m->property("opened").toBool());
        QCOMPARE(m->property("target").toMap()
                     .value(QStringLiteral("sectionId")).toString(),
                 QStringLiteral("!subA:x"));
        QMetaObject::invokeMethod(m, "close");
        QTRY_VERIFY(!m->property("opened").toBool());

        headers[1]->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_Menu);
        QTRY_VERIFY(m->property("opened").toBool());
        QMetaObject::invokeMethod(m, "close");
        QTRY_VERIFY(!m->property("opened").toBool());
        headers[1]->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(m->property("opened").toBool());
        QMetaObject::invokeMethod(m, "close");
        QTRY_VERIFY(!m->property("opened").toBool());

        QTest::mouseClick(h.window.get(), Qt::RightButton, {},
                          center(headers[0]) + QPoint(-100, 0));
        headers[0]->forceActiveFocus();
        QTest::keyClick(h.window.get(), Qt::Key_Menu);
        QTest::qWait(150);
        QVERIFY(!m->property("opened").toBool());
    }
};

QTEST_MAIN(SpaceLobbyKeyboardQmlTest)
#include "SpaceLobbyKeyboardQmlTest.moc"
