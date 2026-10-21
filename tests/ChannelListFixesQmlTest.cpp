// Channels layout list fixes (swarm C05 round 1), on the real components:
//   * F02 rows, category headers and nav rows are in the Tab order and
//     Return/Enter activates them;
//   * F03 under right-to-left mirroring the icon/avatar sit at the RIGHT edge
//     and the label never runs off the left edge or under the avatar;
//   * F05 a truncated channel name and the mute bell have tooltips;
//   * F01/F04 the presenter carries a MiddleClickScroller on its ListView and
//     the ListView stops at its bounds (contract scan: the presenter needs the
//     whole `app` object, so these two are source scans, stated as such).

#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QtQml/qqml.h>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include <memory>

class ChannelListFixesQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Harness {
        std::unique_ptr<QObject> appStub;
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QObject> rootOwner;
        QQuickItem *row = nullptr;
    };

    bool build(Harness &h, const QString &body, bool rtl = false)
    {
        h.engine = std::make_unique<QQmlEngine>();
        QQmlComponent stub(h.engine.get());
        stub.setData(QByteArrayLiteral(R"(
import QtQuick
QtObject {
    property QtObject roomList: QtObject {
        property bool roomFavouritesSupported: false
    }
    property QtObject roomInfo: QtObject {
        property string roomId: ""
        property bool canInvite: false
    }
}
)"), QUrl(QStringLiteral("qrc:/c05-appstub.qml")));
        h.appStub.reset(stub.create());
        if (!h.appStub)
            return false;
        h.engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    h.appStub.get());
        QQmlComponent component(h.engine.get());
        const QString src = QStringLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient
Item {
    id: outer
    width: 300
    height: 200
    LayoutMirroring.enabled: %2
    LayoutMirroring.childrenInherit: true
    %1
}
)").arg(body, rtl ? QStringLiteral("true") : QStringLiteral("false"));
        component.setData(src.toUtf8(), QUrl(QStringLiteral("qrc:/c05.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        h.rootOwner.reset(component.create());
        auto *root = qobject_cast<QQuickItem *>(h.rootOwner.get());
        if (!root)
            return false;
        h.row = root->findChild<QQuickItem *>(QStringLiteral("subject"));
        if (!h.row)
            return false;
        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(400, 300);
        root->setParentItem(h.window->contentItem());
        h.window->show();
        if (!QTest::qWaitForWindowExposed(h.window.get()))
            return false;
        QCoreApplication::processEvents();
        return true;
    }

    static QRectF sceneRect(QQuickItem *i)
    {
        return i->mapRectToScene(QRectF(0, 0, i->width(), i->height()));
    }
    static QQuickItem *named(QQuickItem *root, const char *name)
    {
        return root->findChild<QQuickItem *>(QString::fromLatin1(name));
    }
    static QQuickItem *loaded(QQuickItem *loader)
    {
        return loader ? loader->property("item").value<QQuickItem *>() : nullptr;
    }
    static bool tooltipVisible(QQuickItem *item)
    {
        QQmlExpression e(qmlContext(item), item,
                         QStringLiteral("ToolTip.visible"));
        return e.evaluate().toBool();
    }
    static QString readSource(const QString &rel)
    {
        QFile f(QStringLiteral(QML_DIR "/") + rel);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    static QString rowBody(const QString &type, const QString &props)
    {
        const QString w = props.contains(QLatin1String("width:"))
            ? QString() : QStringLiteral("width: 300; ");
        return QStringLiteral("%1 { objectName: \"subject\"; %3%2 }")
            .arg(type, props, w);
    }

private slots:
    // ---- F02 -----------------------------------------------------------
    void everyChannelRowKindIsInTheTabOrder_data()
    {
        QTest::addColumn<QString>("body");
        QTest::newRow("nav") << rowBody("ChannelNavRow",
                                        "label: \"Lobby\"; iconName: \"home\"");
        QTest::newRow("header") << rowBody("ChannelCategoryHeader",
                                           "headerName: \"Rooms\"");
        QTest::newRow("channel") << rowBody(
            "ChannelDelegate", "roomId: \"!r:x\"; channelName: \"general\"");
    }
    void everyChannelRowKindIsInTheTabOrder()
    {
        QFETCH(QString, body);
        Harness h;
        QVERIFY(build(h, body));
        QVERIFY2(h.row->property("activeFocusOnTab").toBool(),
                 "a Channels-layout row is not reachable with Tab");
        h.row->forceActiveFocus();
        QVERIFY(h.row->hasActiveFocus());
        QSignalSpy clicked(h.row, SIGNAL(clicked()));
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QCOMPARE(clicked.count(), 1);
        QTest::keyClick(h.window.get(), Qt::Key_Enter);
        QCOMPARE(clicked.count(), 2);
    }

    // ---- F03 -----------------------------------------------------------
    void aNavRowMirrorsInRightToLeft()
    {
        Harness h;
        QVERIFY(build(h, rowBody("ChannelNavRow",
                                 "label: \"Create Room\"; iconName: \"add\""),
                      /*rtl=*/true));
        QQuickItem *glyph = named(h.row, "channelNavGlyph");
        QQuickItem *name = loaded(named(h.row, "channelNavName"));
        QVERIFY(glyph);
        QVERIFY(name);
        const QRectF g = sceneRect(glyph), n = sceneRect(name), r = sceneRect(h.row);
        QVERIFY2(g.center().x() > r.center().x(),
                 "the nav icon did not move to the right edge");
        QVERIFY2(n.left() >= r.left() - 0.5,
                 qPrintable(QStringLiteral("label starts at %1, off the left edge")
                                .arg(n.left())));
        QVERIFY2(!n.intersects(g), "the label overlaps the icon");
        QVERIFY2(n.width() > 20, "the label has no usable width");
    }
    void aCategoryHeaderMirrorsInRightToLeft()
    {
        Harness h;
        QVERIFY(build(h, rowBody("ChannelCategoryHeader",
                                 "headerName: \"Rooms\"; showsAvatar: false"),
                      true));
        QQuickItem *chev = named(h.row, "channelCategoryChevron");
        QQuickItem *name = loaded(named(h.row, "channelCategoryName"));
        QVERIFY(chev);
        QVERIFY(name);
        const QRectF c = sceneRect(chev), n = sceneRect(name), r = sceneRect(h.row);
        QVERIFY(c.center().x() > r.center().x());
        QVERIFY(n.left() >= r.left() - 0.5);
        QVERIFY(!n.intersects(c));
        QVERIFY(n.width() > 20);
    }
    void aChannelRowMirrorsInRightToLeft()
    {
        Harness h;
        QVERIFY(build(h, rowBody("ChannelDelegate",
                                 "roomId: \"!r:x\"; channelName: \"general\""),
                      true));
        QQuickItem *av = named(h.row, "channelAvatar");
        QQuickItem *name = loaded(named(h.row, "channelNameLoader"));
        QVERIFY(av);
        QVERIFY(name);
        const QRectF a = sceneRect(av), n = sceneRect(name), r = sceneRect(h.row);
        QVERIFY2(a.center().x() > r.center().x(),
                 "the avatar did not move to the right edge");
        QVERIFY2(n.left() >= r.left() - 0.5, "the name runs off the left edge");
        QVERIFY2(!n.intersects(a), "the name is drawn under the avatar");
        QVERIFY(n.width() > 20);
    }

    // ---- F05 -----------------------------------------------------------
    void aTruncatedNameHasATooltipAndAShortOneDoesNot()
    {
        Harness h;
        QVERIFY(build(h, rowBody("ChannelDelegate",
                                 "width: 120; roomId: \"!r:x\"; channelName: "
                                 "\"A channel name much too long for this column\"")));
        QQuickItem *name = loaded(named(h.row, "channelNameLoader"));
        QVERIFY(name);
        QVERIFY2(name->property("truncated").toBool(),
                 "precondition: the name must be elided");
        QTest::mouseMove(h.window.get(), QPoint(60, 16));
        QTest::qWait(50);
        QTest::mouseMove(h.window.get(), QPoint(62, 16));
        QTRY_VERIFY2(tooltipVisible(h.row),
                     "a truncated channel name shows no tooltip on hover");

        Harness s;
        QVERIFY(build(s, rowBody("ChannelDelegate",
                                 "roomId: \"!r:x\"; channelName: \"ok\"")));
        QTest::mouseMove(s.window.get(), QPoint(60, 16));
        QTest::qWait(50);
        QTest::mouseMove(s.window.get(), QPoint(62, 16));
        QTest::qWait(900);
        QVERIFY2(!tooltipVisible(s.row), "a whole name must not get a tooltip");
    }
    void theMuteBellHasATooltip()
    {
        Harness h;
        QVERIFY(build(h, rowBody("ChannelDelegate",
                                 "roomId: \"!r:x\"; channelName: \"general\"; "
                                 "notificationMode: 2")));
        QQuickItem *bellLoader = named(h.row, "channelMutedGlyph");
        QVERIFY(bellLoader);
        QVERIFY(bellLoader->property("active").toBool());
        QQuickItem *bell = loaded(bellLoader);
        QVERIFY(bell);
        const QPointF p = sceneRect(bell).center();
        QTest::mouseMove(h.window.get(), (p + QPointF(-3, 0)).toPoint());
        QTest::qWait(50);
        QTest::mouseMove(h.window.get(), p.toPoint());
        QTRY_VERIFY2(tooltipVisible(bell), "the mute bell shows no tooltip");
    }

    // ---- F01 / F04: source contracts -----------------------------------
    void theChannelsListScrollsLikeTheClassicList()
    {
        const QString s = readSource(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY2(!s.isEmpty(), "presenter source not found");
        QVERIFY2(s.contains(QRegularExpression(
                     QStringLiteral(R"(MiddleClickScroller\s*\{[\s\S]{0,400}?view:\s*channelList)"))),
                 "no MiddleClickScroller on the channel list (F01)");
        QVERIFY2(s.contains(QStringLiteral("middleClickScroller.stop()")),
                 "a room switch does not end the autoscroll");
        QVERIFY2(s.contains(QStringLiteral("boundsBehavior: Flickable.StopAtBounds")),
                 "the channel list overshoots its bounds (F04)");
    }

    // Review r1 2b: no tooltip beside an open context menu (source contract).
    void tooltipsYieldToTheContextMenu()
    {
        const QString s = readSource(QStringLiteral("ChannelDelegate.qml"));
        QVERIFY2(!s.isEmpty(), "delegate source not found");
        QVERIFY2(s.contains(QStringLiteral("ToolTip.visible: root.hovered && !root.contextMenuOpen")),
                 "the name tooltip ignores the open context menu");
        QVERIFY2(s.contains(QStringLiteral("muteHover.hovered && !root.contextMenuOpen")),
                 "the mute tooltip ignores the open context menu");
    }
};

QTEST_MAIN(ChannelListFixesQmlTest)
#include "ChannelListFixesQmlTest.moc"
