// The Channels navigation layout's rows, in real instantiated geometry.
//
// Why a geometry suite and not another source scan: the Channels list was
// reported as "I can't left click room names, and favorite / mute / all the
// other actions are unavailable", and neither half of that is visible in the
// source. Both had the same cause — the presenter's row chooser named only two
// of the model's THREE row kinds, so a group label ("Direct messages",
// "Favourites") fell through to the channel-row component and rendered as a
// room row with an empty room id: clicking it opened nothing, and the row it
// impersonated had no context menu at all.
//
// So the contracts here are the two that were actually broken, plus the
// geometry they depend on:
//   * each row kind reports a real height inside a width-assigned Loader, and
//     the Loader adopts it (rows stack at y=0 otherwise);
//   * a channel row is clickable across that height;
//   * a channel row carries the SHARED RoomActionsMenu, the same one the
//     Classic row uses — not a second copy that can drift, and not nothing.
//
// The presenter's own chooser is pinned in NavigationLayoutContractTest, which
// can read it directly — and it now has FIVE kinds to name, because the layout
// gained Lobby and Message Search rows and lost the plain section label.

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QDir>
#include <QQmlExpression>
#include <QImage>

#include <algorithm>
#include <memory>

class ChannelRowGeometryQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Harness {
        // Declared FIRST so it is destroyed LAST: the warnings lambda holds a
        // reference and the engine can still emit during its own teardown.
        QStringList warnings;
        // Same reason, one layer down: the engine's root context holds this as
        // a context property, so it must outlive the engine.
        std::unique_ptr<QObject> appStub;
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QObject> rootOwner;
        QQuickItem *root = nullptr;
        QQuickItem *loader = nullptr;

        QQuickItem *item() const
        {
            return loader ? loader->property("item").value<QQuickItem *>()
                          : nullptr;
        }
    };

    // `body` is the component declaration the Loader loads, exactly as the
    // presenter writes it: the row type plus a width binding and NO height.
    // `withAppStub` is OPT-IN, and deliberately so: every case here predates it
    // and runs with `app` undefined, which the delegate guards for on purpose
    // (a row built from inside a property-change handler can see it missing).
    // Introducing the stub globally would change what those cases exercise.
    //
    // The stub carries `roomList` and NOTHING else. `roomFavouritesSupported`
    // is what gates the favourite star, and the ABSENCE of `app.settings` is
    // load-bearing in the other direction: `refreshNotificationMode()` returns
    // early without it, so a literal `notificationMode:` set by a case
    // survives instead of being overwritten from a settings lookup that would
    // answer 0 for a room no settings object knows about.
    bool build(Harness &h, const QString &body, bool withAppStub = false)
    {
        h.engine = std::make_unique<QQmlEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        if (withAppStub) {
            QQmlComponent stub(h.engine.get());
            stub.setData(QByteArrayLiteral(R"(
import QtQuick
QtObject {
    property QtObject roomList: QtObject {
        property bool roomFavouritesSupported: true
    }
}
)"),
                         QUrl(QStringLiteral("qrc:/channelrowtest-appstub.qml")));
            if (!stub.errors().isEmpty()) {
                qWarning("%s", qPrintable(stub.errorString()));
                return false;
            }
            h.appStub.reset(stub.create());
            if (!h.appStub)
                return false;
            h.engine->rootContext()->setContextProperty(
                QStringLiteral("app"), h.appStub.get());
        }
        QQmlComponent component(h.engine.get());
        const QString source = QStringLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient
Item {
    id: outer
    objectName: "outer"
    width: 300
    height: 600
    // The presenter's delegate shape: a Loader given the list's WIDTH and no
    // height at all, so its height is whatever the loaded row reports.
    Loader {
        id: rowLoader
        objectName: "rowLoader"
        width: outer.width
        sourceComponent: rowComponent
    }
    Component {
        id: rowComponent
        %1
    }
}
)").arg(body);
        component.setData(source.toUtf8(),
                          QUrl(QStringLiteral("qrc:/channelrowtest.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        h.rootOwner.reset(component.create());
        h.root = qobject_cast<QQuickItem *>(h.rootOwner.get());
        if (!h.root)
            return false;
        h.loader = h.root->findChild<QQuickItem *>(QStringLiteral("rowLoader"));
        if (!h.loader)
            return false;

        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(400, 700);
        h.root->setParentItem(h.window->contentItem());
        h.window->show();
        QCoreApplication::processEvents();
        h.loader->polish();
        QCoreApplication::processEvents();
        return true;
    }

    // The one assertion that matters, applied to each row kind: the row has a
    // real height, the Loader adopted it (so the ListView lays rows out one
    // below another rather than stacking them all at y=0), and the row filled
    // the width it was given.
    void checkRowHasGeometry(const QString &body, const char *label)
    {
        Harness h;
        QVERIFY2(build(h, body), label);
        QQuickItem *row = h.item();
        QVERIFY2(row != nullptr, label);
        QVERIFY2(row->height() > 0,
                 qPrintable(QStringLiteral("%1: row height is %2 — a row with "
                                           "no height stacks every other row "
                                           "on top of it at y=0")
                                .arg(QLatin1String(label))
                                .arg(row->height())));
        QCOMPARE(h.loader->height(), row->height());
        QCOMPARE(row->width(), 300.0);
        for (const QString &warning : h.warnings)
            QVERIFY2(!warning.contains(QStringLiteral("Unable to assign")),
                     qPrintable(warning));
    }


    // ---- The Space Home lobby (2026-09-23) --------------------------------
    //
    // SpaceLobby.qml draws what SpaceManager::lobbySections builds: a root
    // "Rooms" section, then one section per subspace. These cases load the
    // REAL component with a fixture shaped like that output and measure it,
    // because the defects this family has had — a header drawn over its own
    // rows, a badge colliding with a name — pass every source scan.
    struct Lobby {
        QStringList warnings;
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QObject> rootOwner;
        QQuickItem *root = nullptr;
        QQuickItem *lobby = nullptr;
    };

    static QString lobbyFixture(bool subspaceCollapsed, bool withVoz = true)
    {
        // Topics include markup on purpose: it must render as TEXT.
        // The href is unquoted and has no scheme ON PURPOSE. moc 6.11 does
        // not parse C++ raw strings: an escaped double quote, or a double
        // slash (read as a comment), inside this one made it lose the whole
        // class (No relevant classes found, an empty .moc). Measured
        // 2026-09-23.
        return QStringLiteral(R"([
          { sectionId: "!home:x", isRoot: true, roomId: "!home:x", name: "",
            selectable: false, roomCount: 3, spaceCount: 0, matchCount: 3,
            unreadTotal: 2, highlightTotal: 0, hasUnread: true,
            collapsed: false,
            rows: [
              { roomId: "!general:x", parentId: "!home:x", name: "General",
                topic: "Say hello, introduce yourself and read the rules first",
                joined: true, isSpace: false, suggested: true, members: 128,
                hasUnread: true, unreadCount: 2, highlightCount: 0,
                membership: "joined", selectable: true },
              { roomId: "!rules:x", parentId: "!home:x",
                name: "A rather long room name that should elide before the badges",
                topic: "<b>bold</b> <a href=evil.example>link</a> <img src=x>",
                joined: true, isSpace: false, suggested: false, members: 12,
                membership: "joined", selectable: true },
              { roomId: "!offer:x", parentId: "!home:x", name: "Unjoined room",
                topic: "Visible before joining", joined: false, isSpace: false,
                members: 7, membership: "", joinRule: "public",
                via: ["x"], selectable: true }
            ] },
          { sectionId: "!subA:x", isRoot: false, roomId: "!subA:x",
            name: "Comunidade", selectable: true, suggested: true,
            roomCount: 2, spaceCount: 1, matchCount: 3, unreadTotal: 4,
            highlightTotal: 1, hasUnread: true, collapsed: %1,
            rows: %2 }%3
        ])")
            .arg(subspaceCollapsed ? QStringLiteral("true")
                                   : QStringLiteral("false"),
                 subspaceCollapsed ? QStringLiteral("[]") : QStringLiteral(R"([
              { roomId: "!a1:x", parentId: "!subA:x", name: "Off topic",
                topic: "Anything goes", joined: true, isSpace: false,
                members: 40, membership: "joined", selectable: false },
              { roomId: "!deep:x", parentId: "!subA:x", name: "Deep",
                topic: "", joined: true, isSpace: true, childCount: 3,
                membership: "joined", selectable: false },
              { roomId: "!a3:x", parentId: "!subA:x", name: "Voice lounge",
                topic: "Only via hierarchy", joined: false, isSpace: false,
                membership: "knocked", joinRule: "knock", selectable: false }
            ])"),
                 withVoz ? QStringLiteral(R"(,
          { sectionId: "!subB:x", isRoot: false, roomId: "!subB:x",
            name: "Voz", selectable: true, roomCount: 0, spaceCount: 0,
            matchCount: 0, unreadTotal: 0, highlightTotal: 0,
            hasUnread: false, collapsed: false, rows: [] })")
                         : QString());
    }

    // `extra` is QML inserted into the SpaceLobby block (more inputs). With
    // `rebuildOnFold` the harness answers a fold the way TimelinePane does:
    // a NEW sections array — folded, and without the Voz section, so the
    // section COUNT changes too.
    bool buildLobby(Lobby &h, bool subspaceCollapsed, bool canManage = true,
                    int width = 600, const QString &extra = QString(),
                    bool rebuildOnFold = false)
    {
        h.engine = std::make_unique<QQmlEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        QQmlComponent component(h.engine.get());
        const QString source = QStringLiteral(R"(
import QtQuick
import MatrixClient
Rectangle {
    width: %1 + 40
    height: 1100
    color: AppTheme.background
    property string log: ""
    property bool rebuildOnFold: %4
    property var expandedData: %5
    property var foldedData: %6
    SpaceLobby {
        id: lobbyUnderTest
        objectName: "lobbyUnderTest"
        x: 20
        width: %1
        canManage: %2
        selectedIds: ({ "!general:x": true })
        sections: %3
        %7
        onOpenRoomRequested: (id) => parent.log += "open:" + id + ";"
        onOpenSpaceRequested: (id) => parent.log += "space:" + id + ";"
        onSelectionToggled: (id) => parent.log += "select:" + id + ";"
        onCollapseToggled: (id, c) => {
            parent.log += "fold:" + id + ":" + c + ";"
            if (parent.rebuildOnFold)
                lobbyUnderTest.sections = JSON.parse(JSON.stringify(
                    c ? parent.foldedData : parent.expandedData))
        }
        onJoinRequested: (id, via, sp) => parent.log += "join:" + id + ";"
    }
}
)")
            .arg(QString::number(width),
                 canManage ? QStringLiteral("true") : QStringLiteral("false"),
                 lobbyFixture(subspaceCollapsed),
                 rebuildOnFold ? QStringLiteral("true")
                               : QStringLiteral("false"),
                 lobbyFixture(false, true), lobbyFixture(true, false), extra);
        component.setData(source.toUtf8(),
                          QUrl(QStringLiteral("qrc:/lobbytest.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        h.rootOwner.reset(component.create());
        h.root = qobject_cast<QQuickItem *>(h.rootOwner.get());
        if (!h.root)
            return false;
        h.lobby = h.root->findChild<QQuickItem *>(
            QStringLiteral("lobbyUnderTest"));
        if (!h.lobby)
            return false;
        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(int(h.root->width()), int(h.root->height()));
        h.root->setParentItem(h.window->contentItem());
        h.window->show();
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
            h.lobby->polish();
        }
        h.window->requestActivate();
        return QTest::qWaitForWindowExposed(h.window.get());
    }

    // Repeater delegates are not findChild-reachable in every shape (the
    // memory note on that); walk the VISUAL tree instead.
    static void collect(QQuickItem *item, const QString &name,
                        QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name && item->isVisible())
            out.append(item);
        for (QQuickItem *child : item->childItems())
            collect(child, name, out);
    }
    static QList<QQuickItem *> all(QQuickItem *root, const char *name)
    {
        QList<QQuickItem *> out;
        collect(root, QString::fromLatin1(name), out);
        std::sort(out.begin(), out.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y();
        });
        return out;
    }
    static QRectF sceneRect(QQuickItem *it)
    {
        return it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
    }
    static bool contains(const QRectF &outer, const QRectF &inner)
    {
        const qreal e = 0.5; // sub-pixel layout rounding
        return inner.left() >= outer.left() - e
               && inner.right() <= outer.right() + e
               && inner.top() >= outer.top() - e
               && inner.bottom() <= outer.bottom() + e;
    }
    static QQuickItem *ancestorNamed(QQuickItem *it, const char *name)
    {
        for (QQuickItem *p = it ? it->parentItem() : nullptr; p;
             p = p->parentItem()) {
            if (p->objectName() == QLatin1String(name))
                return p;
        }
        return nullptr;
    }
    static QString attached(QQuickItem *it, const char *what)
    {
        QQmlExpression expr(qmlContext(it), it,
                            QStringLiteral("Accessible.%1")
                                .arg(QLatin1String(what)));
        return expr.evaluate().toString();
    }
    static QString nameOf(QQuickItem *row)
    {
        QList<QQuickItem *> names;
        collect(row, QStringLiteral("spaceLobbyRowName"), names);
        return names.isEmpty() ? QString()
                               : names.first()->property("text").toString();
    }
    static QQuickItem *sectionWithId(QQuickItem *root, const QString &id)
    {
        for (QQuickItem *sec : all(root, "spaceLobbySection")) {
            if (sec->property("modelData").toMap()
                    .value(QStringLiteral("sectionId")).toString() == id)
                return sec;
        }
        return nullptr;
    }
    static QQuickItem *firstIn(QQuickItem *root, const char *name)
    {
        QList<QQuickItem *> found;
        collect(root, QString::fromLatin1(name), found);
        return found.isEmpty() ? nullptr : found.first();
    }
    static bool runJs(Lobby &h, const QString &js)
    {
        QQmlExpression expr(qmlContext(h.root), h.root, js);
        expr.evaluate();
        QCoreApplication::processEvents();
        return !expr.hasError();
    }
    static QQuickItem *rowNamed(QQuickItem *root, const QString &name)
    {
        for (QQuickItem *row : all(root, "spaceUnifiedChildRow")) {
            if (nameOf(row) == name)
                return row;
        }
        return nullptr;
    }

private Q_SLOTS:
    void channelRowHasRealHeightInsideAWidthAssignedLoader()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelDelegate {
            width: 300
            roomId: "!room:example.org"
            channelName: "general"
        })"),
                            "channel row");
    }

    void spaceFolderHeaderHasRealHeightInsideAWidthAssignedLoader()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelCategoryHeader {
            width: 300
            headerId: "!space:example.org"
            headerName: "Engineering"
            showsAvatar: true
        })"),
                            "space folder header");
    }

    void groupHeaderHasRealHeightInsideAWidthAssignedLoader()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelCategoryHeader {
            width: 300
            headerId: "@rooms"
            headerName: "Rooms"
        })"),
                            "group header");
    }

    // Lobby and Message Search are navigation, not rooms — a different
    // component, and it has to occupy its row like every other one.
    void navigationRowsHaveRealHeightInsideAWidthAssignedLoader()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelNavRow {
            width: 300
            label: "Lobby"
            iconName: "home"
        })"),
                            "lobby row");
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelNavRow {
            width: 300
            label: "Message Search"
            iconName: "search"
        })"),
                            "message search row");
    }

    // A navigation row that could not be clicked would be exactly the dead
    // decorative row this layout was told not to have.
    void aNavigationRowIsClickableAcrossItsHeight()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(R"(
        ChannelNavRow {
            width: 300
            label: "Message Search"
            iconName: "search"
            property int clicks: 0
            onClicked: clicks += 1
        })")));
        QQuickItem *row = h.item();
        QVERIFY(row);
        QVERIFY(row->height() > 0);
        const QPointF centre =
            row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QCoreApplication::processEvents();
        QCOMPARE(row->property("clicks").toInt(), 1);
    }

    // A row with no name yet is the state the delegate is CREATED in (the
    // room's state has not resolved), and it must still occupy its row —
    // otherwise the list collapses exactly while it is being populated.
    void aChannelRowWithNoNameYetStillOccupiesItsRow()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelDelegate {
            width: 300
            roomId: "!room:example.org"
        })"),
                            "nameless channel row");
    }

    // A press on the row's centre must reach the row. This is the assertion
    // that distinguishes a real channel row from the group label that used to
    // impersonate one: the label carried no click target at all.
    void aChannelRowIsClickableAcrossItsHeight()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(R"(
        ChannelDelegate {
            width: 300
            roomId: "!room:example.org"
            channelName: "general"
            property int clicks: 0
            onClicked: clicks += 1
        })")));
        QQuickItem *row = h.item();
        QVERIFY(row);
        QVERIFY(row->height() > 0);
        const QPointF centre =
            row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QCoreApplication::processEvents();
        QCOMPARE(row->property("clicks").toInt(), 1);
    }

    // The row menu is the Channels layout's whole action set — favourite,
    // mark read/unread, notification mode, copy link, leave — and it had none
    // at all. It must be the SHARED component, not a second copy.
    void aChannelRowCarriesTheSharedActionsMenu()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(R"(
        ChannelDelegate {
            width: 300
            roomId: "!room:example.org"
            channelName: "general"
        })")));
        QQuickItem *row = h.item();
        QVERIFY(row);
        // The menu is built on FIRST USE, not declared inline: it is a Popup
        // with a submenu and ten items, and building that for every row made
        // a filter change (which rebuilds every delegate) visibly laggy. So
        // this drives the real entry point rather than looking for a child
        // that should not exist yet — which also makes it a test of
        // REACHABILITY instead of of a declaration.
        QVERIFY2(row->findChild<QObject *>(QStringLiteral("channelContextMenu"))
                     == nullptr,
                 "the row builds its context menu eagerly");
        QVERIFY(QMetaObject::invokeMethod(row, "openContextMenu"));
        QCoreApplication::processEvents();
        QObject *menu =
            row->findChild<QObject *>(QStringLiteral("channelContextMenu"));
        QVERIFY2(menu != nullptr,
                 "the Channels row has no context menu, so favourite / mute / "
                 "mark read / copy link / leave are unreachable in that "
                 "layout");
        // The shared component, so the two layouts' menus cannot drift.
        QVERIFY(QString::fromUtf8(menu->metaObject()->className())
                    .contains(QStringLiteral("RoomActionsMenu")));
    }

    // A MUTED FAVOURITE DREW ITS STAR THROUGH THE BELL (reported 2026-09-17).
    //
    // The marks on this row's right edge are a chain — pill, call glyph, mute
    // glyph, star — but the bell was not in it: it pinned itself to
    // `parent.right` at a 14px margin while the star anchored to
    // `callGlyph.left`, and a collapsed call glyph sits at that same 14px
    // margin. So with no pill and no call the two landed on the same pixels.
    //
    // Only geometry can see this. Every one of these marks is declared
    // correctly in isolation and a source scan reads the file as fine; what is
    // wrong is the relationship between two anchor chains, which exists only
    // once both items are instantiated and laid out. Same defect and same
    // cause as the timeline row's right rail (CLAUDE.md §16).
    void aMutedFavouriteDrawsItsStarClearOfTheBell()
    {
        Harness h;
        QVERIFY(build(h, QStringLiteral(R"(
        ChannelDelegate {
            width: 300
            roomId: "!room:example.org"
            channelName: "general"
            isFavourite: true
            notificationMode: 2
        })"),
                         /*withAppStub=*/true));
        QQuickItem *row = h.item();
        QVERIFY(row);

        QQuickItem *star =
            row->findChild<QQuickItem *>(QStringLiteral("channelFavouriteStar"));
        QQuickItem *bell =
            row->findChild<QQuickItem *>(QStringLiteral("channelMutedGlyph"));
        QVERIFY2(star != nullptr, "the row has no favourite star at all");
        QVERIFY2(bell != nullptr, "the row has no mute glyph at all");

        // PRECONDITIONS, NOT DECORATION. If either mark is not actually shown
        // the overlap assertion below is vacuously true, and this suite would
        // then pass on the very code it was written to catch — the failure
        // mode CLAUDE.md records three separate times. Assert both are live
        // before asserting anything about where they are.
        QVERIFY2(star->property("active").toBool(),
                 "the favourite star is not shown on a favourited row, so the "
                 "overlap assertion below would prove nothing");
        QVERIFY2(bell->property("active").toBool(),
                 "the mute glyph is not shown on a muted row, so the overlap "
                 "assertion below would prove nothing");
        QVERIFY(star->width() > 0);
        QVERIFY(bell->width() > 0);

        const QRectF starRect = star->mapRectToScene(
            QRectF(0, 0, star->width(), star->height()));
        const QRectF bellRect = bell->mapRectToScene(
            QRectF(0, 0, bell->width(), bell->height()));

        QVERIFY2(!starRect.intersects(bellRect),
                 qPrintable(
                     QStringLiteral(
                         "the favourite star and the mute glyph overlap: star "
                         "x=[%1,%2] bell x=[%3,%4]. A muted favourite draws "
                         "one mark on top of the other.")
                         .arg(starRect.left())
                         .arg(starRect.right())
                         .arg(bellRect.left())
                         .arg(bellRect.right())));

        // And the star belongs to the LEFT of the bell, not merely beside it:
        // the bell owns the rightmost slot whenever it is shown.
        QVERIFY2(starRect.right() <= bellRect.left(),
                 "the favourite star is not left of the mute glyph");
    }

    // Sections stack, headers sit above their own rows, every row is inside
    // its section's card, and nothing in a row is drawn over anything else.
    void theLobbySectionsAndRowsDoNotOverlap()
    {
        Lobby h;
        QVERIFY(buildLobby(h, /*subspaceCollapsed=*/false));
        const auto sections = all(h.lobby, "spaceLobbySection");
        QCOMPARE(sections.size(), 3);
        for (int i = 1; i < sections.size(); ++i) {
            QVERIFY2(sceneRect(sections[i - 1]).bottom()
                         <= sceneRect(sections[i]).top() + 0.5,
                     qPrintable(QStringLiteral("section %1 overlaps section %2")
                                    .arg(i - 1).arg(i)));
        }
        const auto headers = all(h.lobby, "spaceLobbySectionHeader");
        const auto cards = all(h.lobby, "spaceLobbySectionCard");
        QCOMPARE(headers.size(), 3);
        QCOMPARE(cards.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(headers[i]->height() > 0);
            QVERIFY(contains(sceneRect(sections[i]), sceneRect(headers[i])));
            QVERIFY(contains(sceneRect(sections[i]), sceneRect(cards[i])));
            QVERIFY2(sceneRect(headers[i]).bottom()
                         <= sceneRect(cards[i]).top() + 0.5,
                     qPrintable(QStringLiteral(
                         "section %1's header is drawn over its own rows")
                                    .arg(i)));
        }
        const auto rows = all(h.lobby, "spaceUnifiedChildRow");
        // 3 root rows + 3 in Comunidade; Voz is empty.
        QCOMPARE(rows.size(), 6);
        for (int i = 0; i < rows.size(); ++i) {
            QQuickItem *card = ancestorNamed(rows[i], "spaceLobbySectionCard");
            QVERIFY2(card, "a lobby row outside any section card");
            QVERIFY2(contains(sceneRect(card), sceneRect(rows[i])),
                     qPrintable(QStringLiteral("row %1 (%2) escapes its card")
                                    .arg(i).arg(nameOf(rows[i]))));
            if (i > 0 && ancestorNamed(rows[i - 1], "spaceLobbySectionCard")
                             == card) {
                QVERIFY2(sceneRect(rows[i - 1]).bottom()
                             <= sceneRect(rows[i]).top() + 0.5,
                         "two rows in one section overlap");
            }
            // The name line and the topic line are both inside the row, one
            // above the other.
            QList<QQuickItem *> name, topic;
            collect(rows[i], QStringLiteral("spaceLobbyRowName"), name);
            collect(rows[i], QStringLiteral("spaceLobbyRowTopic"), topic);
            QCOMPARE(name.size(), 1);
            QVERIFY(contains(sceneRect(rows[i]), sceneRect(name.first())));
            if (!topic.isEmpty()) {
                QVERIFY(contains(sceneRect(rows[i]), sceneRect(topic.first())));
                QVERIFY2(sceneRect(name.first()).bottom()
                             <= sceneRect(topic.first()).top() + 0.5,
                         "a row's topic is drawn over its name");
            }
        }
        // A long name elides BEFORE the Join / open affordances rather than
        // running under them.
        QQuickItem *longRow = rowNamed(h.lobby,
            QStringLiteral("A rather long room name that should elide before "
                           "the badges"));
        QVERIFY(longRow);
        QList<QQuickItem *> longName, glyph;
        collect(longRow, QStringLiteral("spaceLobbyRowName"), longName);
        collect(longRow, QStringLiteral("spaceLobbyOpenGlyph"), glyph);
        QCOMPARE(glyph.size(), 1);
        QVERIFY(sceneRect(longName.first()).right()
                <= sceneRect(glyph.first()).left());
        for (const QString &w : h.warnings)
            QVERIFY2(!w.contains(QStringLiteral("Binding loop"))
                         && !w.contains(QStringLiteral("recursive rearrange"))
                         && !w.contains(QStringLiteral("Unable to assign")),
                     qPrintable(w));
    }

    // A topic is attacker-writable room state: it renders as the characters
    // it contains, on one elided line, never as rich text.
    void aLobbyTopicIsPlainTextOnOneLine()
    {
        Lobby h;
        QVERIFY(buildLobby(h, false));
        QQuickItem *row = rowNamed(h.lobby,
            QStringLiteral("A rather long room name that should elide before "
                           "the badges"));
        QVERIFY(row);
        QList<QQuickItem *> topic;
        collect(row, QStringLiteral("spaceLobbyRowTopic"), topic);
        QCOMPARE(topic.size(), 1);
        QCOMPARE(topic.first()->property("textFormat").toInt(),
                 int(Qt::PlainText));
        QVERIFY(topic.first()->property("text").toString().contains(
            QStringLiteral("<a href=")));
        QCOMPARE(topic.first()->property("lineCount").toInt(), 1);
        // A row with no topic draws no empty second line.
        QQuickItem *deep = rowNamed(h.lobby, QStringLiteral("Deep"));
        QVERIFY(deep);
        QList<QQuickItem *> none;
        collect(deep, QStringLiteral("spaceLobbyRowTopic"), none);
        QVERIFY(none.isEmpty());
        // An unjoined room shows its topic too.
        QQuickItem *offer = rowNamed(h.lobby, QStringLiteral("Unjoined room"));
        QVERIFY(offer);
        QList<QQuickItem *> offerTopic;
        collect(offer, QStringLiteral("spaceLobbyRowTopic"), offerTopic);
        QCOMPARE(offerTopic.size(), 1);
        QCOMPARE(offerTopic.first()->property("text").toString(),
                 QStringLiteral("Visible before joining"));
    }

    // Rows and headers say what they are to a screen reader — including the
    // membership the removed "Joined" chip used to show.
    void lobbyRowsAndHeadersHaveAccessibleNames()
    {
        Lobby h;
        QVERIFY(buildLobby(h, /*subspaceCollapsed=*/true));
        QQuickItem *general = rowNamed(h.lobby, QStringLiteral("General"));
        QQuickItem *offer = rowNamed(h.lobby, QStringLiteral("Unjoined room"));
        QVERIFY(general && offer);
        QCOMPARE(attached(general, "name"), QStringLiteral("General"));
        QVERIFY(attached(general, "description").contains(
            QStringLiteral("Say hello")));
        QVERIFY2(attached(offer, "name").contains(QStringLiteral("not joined")),
                 qPrintable(attached(offer, "name")));
        const auto headers = all(h.lobby, "spaceLobbySectionHeader");
        QCOMPARE(headers.size(), 3);
        QVERIFY(attached(headers[0], "name").contains(QStringLiteral("Rooms")));
        QVERIFY(attached(headers[1], "name").contains(QStringLiteral("Comunidade")));
        QVERIFY(attached(headers[1], "name").contains(QStringLiteral("collapsed")));
        QVERIFY(attached(headers[2], "name").contains(QStringLiteral("expanded")));
        // The selection box names its room.
        QList<QQuickItem *> boxes;
        collect(general, QStringLiteral("spaceChildSelectBox"), boxes);
        QCOMPARE(boxes.size(), 1);
        QVERIFY(attached(boxes.first(), "name").contains(QStringLiteral("General")));
    }

    // Folding is DATA: a folded section is handed no rows, so no row is
    // built for it — and its header still carries the activity inside.
    void aFoldedLobbySectionBuildsNoRows()
    {
        Lobby h;
        QVERIFY(buildLobby(h, /*subspaceCollapsed=*/true));
        const auto sections = all(h.lobby, "spaceLobbySection");
        QCOMPARE(sections.size(), 3);
        QList<QQuickItem *> rowsInFolded;
        collect(sections[1], QStringLiteral("spaceUnifiedChildRow"),
                rowsInFolded);
        QVERIFY(rowsInFolded.isEmpty());
        QCOMPARE(all(h.lobby, "spaceUnifiedChildRow").size(), 3);
        // Sections below still stack under the folded header.
        QVERIFY(sceneRect(sections[1]).bottom()
                <= sceneRect(sections[2]).top() + 0.5);
        // ...and the folded header says there is unread inside.
        QVERIFY(firstIn(sections[1], "spaceLobbyFoldedUnread"));
        QVERIFY(!firstIn(sections[2], "spaceLobbyFoldedUnread"));
    }

    // Every sync hands the lobby a NEW array. That must not close an open
    // section menu, and the menu must follow its section by id.
    void aRebuildKeepsTheSectionMenuOpen()
    {
        Lobby h;
        QVERIFY(buildLobby(h, false));
        QObject *menu = h.lobby->findChild<QObject *>(
            QStringLiteral("spaceLobbySectionMenu"));
        QVERIFY(menu);
        QQuickItem *button = firstIn(sectionWithId(h.lobby,
                                                   QStringLiteral("!subA:x")),
                                     "spaceLobbySectionMenuButton");
        QVERIFY(button);
        QTest::mouseClick(h.window.get(), Qt::LeftButton, {},
                          button->mapToScene(QPointF(button->width() / 2,
                                                     button->height() / 2))
                              .toPoint());
        QTRY_VERIFY(menu->property("opened").toBool());

        // Same content, new array (what every sync does).
        QVERIFY(runJs(h, QStringLiteral(
            "lobbyUnderTest.sections = JSON.parse(JSON.stringify(expandedData))")));
        QVERIFY2(menu->property("opened").toBool(),
                 "a rebuild closed the open section menu");
        // A different COUNT (Voz gone, Comunidade folded): still open, still
        // aimed at Comunidade.
        QVERIFY(runJs(h, QStringLiteral(
            "lobbyUnderTest.sections = JSON.parse(JSON.stringify(foldedData))")));
        QVERIFY(menu->property("opened").toBool());
        QCOMPARE(menu->property("target").toMap()
                     .value(QStringLiteral("sectionId")).toString(),
                 QStringLiteral("!subA:x"));
        QObject *open = menu->findChild<QObject *>(
            QStringLiteral("spaceLobbyOpenSpace"));
        QVERIFY(open);
        QMetaObject::invokeMethod(open, "triggered");
        QVERIFY2(h.root->property("log").toString()
                     .contains(QStringLiteral("space:!subA:x;")),
                 qPrintable(h.root->property("log").toString()));

        // Its section gone: the menu closes rather than act on nothing.
        QVERIFY(runJs(h, QStringLiteral(
            "lobbyUnderTest.sections = JSON.parse(JSON.stringify(expandedData))")));
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(runJs(h, QStringLiteral(
            "lobbyUnderTest.sections = [expandedData[0]]")));
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    // Folding from the keyboard, twice: focus stays on the header although
    // each fold hands the lobby a new array with a different section COUNT.
    void keyboardFoldTwiceKeepsFocus()
    {
        Lobby h;
        QVERIFY(buildLobby(h, false, true, 600, QString(),
                           /*rebuildOnFold=*/true));
        QVERIFY(QTest::qWaitForWindowActive(h.window.get()));
        auto header = [&] {
            return firstIn(sectionWithId(h.lobby, QStringLiteral("!subA:x")),
                           "spaceLobbySectionHeader");
        };
        QVERIFY(header());
        header()->forceActiveFocus();
        QVERIFY(header()->hasActiveFocus());
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QTRY_VERIFY(header() && header()->hasActiveFocus());
        QCOMPARE(all(h.lobby, "spaceLobbySection").size(), 2);
        QTest::keyClick(h.window.get(), Qt::Key_Return);
        QTRY_VERIFY(header() && header()->hasActiveFocus());
        QCOMPARE(all(h.lobby, "spaceLobbySection").size(), 3);
        QCOMPARE(h.root->property("log").toString(),
                 QStringLiteral("fold:!subA:x:true;fold:!subA:x:false;"));
    }

    // While /hierarchy has not answered, "No rooms yet" would be a false
    // claim: the lobby says it is loading instead.
    void aLobbyWaitingForHierarchySaysLoading()
    {
        Lobby h;
        QVERIFY(buildLobby(h, false, true, 600,
                           QStringLiteral("loadingIds: ({ \"!subB:x\": true })")));
        QQuickItem *voz = sectionWithId(h.lobby, QStringLiteral("!subB:x"));
        QVERIFY(voz);
        QQuickItem *empty = firstIn(voz, "spaceLobbySectionEmpty");
        QVERIFY(empty);
        QCOMPARE(empty->property("text").toString(),
                 QStringLiteral("Loading rooms…"));
        // A section that HAS answered still says it is empty.
        QVERIFY(runJs(h, QStringLiteral("lobbyUnderTest.loadingIds = ({})")));
        QCOMPARE(empty->property("text").toString(),
                 QStringLiteral("No rooms yet"));
        // And the whole-lobby empty card, for the Home.
        QVERIFY(runJs(h, QStringLiteral(
            "lobbyUnderTest.sections = []; lobbyUnderTest.homeLoading = true")));
        QQuickItem *title = nullptr;
        QList<QQuickItem *> t;
        collect(h.lobby, QStringLiteral("spaceLobbyEmptyTitle"), t);
        QVERIFY(!t.isEmpty());
        title = t.first();
        QCOMPARE(title->property("text").toString(),
                 QStringLiteral("Loading rooms…"));
    }

    // Taps: a joined row opens; its selection box selects WITHOUT opening; an
    // unjoined row acts only through Join; a header folds its section; only
    // the Home's direct children offer a selection box.
    void lobbyTapsReachTheRightTarget()
    {
        Lobby h;
        QVERIFY(buildLobby(h, false));
        auto click = [&](QQuickItem *it, QPointF local) {
            QTest::mouseClick(h.window.get(), Qt::LeftButton, {},
                              it->mapToScene(local).toPoint());
            QCoreApplication::processEvents();
        };
        auto log = [&] { return h.root->property("log").toString(); };
        QQuickItem *general = rowNamed(h.lobby, QStringLiteral("General"));
        QVERIFY(general);
        click(general, QPointF(80, general->height() / 2));
        QCOMPARE(log(), QStringLiteral("open:!general:x;"));

        QList<QQuickItem *> boxes;
        collect(general, QStringLiteral("spaceChildSelectBox"), boxes);
        QCOMPARE(boxes.size(), 1);
        click(boxes.first(), QPointF(boxes.first()->width() / 2,
                                     boxes.first()->height() / 2));
        QCOMPARE(log(), QStringLiteral("open:!general:x;select:!general:x;"));

        QQuickItem *offer = rowNamed(h.lobby, QStringLiteral("Unjoined room"));
        QVERIFY(offer);
        click(offer, QPointF(80, offer->height() / 2));
        QCOMPARE(log(), QStringLiteral("open:!general:x;select:!general:x;"));

        QQuickItem *deep = rowNamed(h.lobby, QStringLiteral("Deep"));
        QVERIFY(deep);
        click(deep, QPointF(80, deep->height() / 2));
        QVERIFY(log().endsWith(QStringLiteral("space:!deep:x;")));
        // A SUBSPACE's room is not the Home's to remove: no selection box.
        QList<QQuickItem *> deepBoxes;
        collect(deep, QStringLiteral("spaceChildSelectBox"), deepBoxes);
        QVERIFY(deepBoxes.isEmpty());

        const auto headers = all(h.lobby, "spaceLobbySectionHeader");
        // The header's own bands: the select box selects the SUBSPACE and
        // the menu button opens the menu — neither may also fold it.
        QQuickItem *headerBox = firstIn(headers[1], "spaceChildSelectBox");
        QVERIFY(headerBox);
        click(headerBox, QPointF(headerBox->width() / 2,
                                 headerBox->height() / 2));
        QVERIFY2(log().endsWith(QStringLiteral("select:!subA:x;")),
                 qPrintable(log()));
        QQuickItem *menuButton =
            firstIn(headers[1], "spaceLobbySectionMenuButton");
        QVERIFY(menuButton);
        click(menuButton, QPointF(menuButton->width() / 2,
                                  menuButton->height() / 2));
        QObject *menu = h.lobby->findChild<QObject *>(
            QStringLiteral("spaceLobbySectionMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY2(!log().contains(QStringLiteral("fold:")), qPrintable(log()));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());

        click(headers[1], QPointF(headers[1]->width() * 0.6,
                                  headers[1]->height() / 2));
        QVERIFY2(log().endsWith(QStringLiteral("fold:!subA:x:true;")),
                 qPrintable(log()));
    }

    // Writes the lobby as the offscreen renderer draws it, for review. Opt-in
    // (LIGHTNING_LOBBY_SHOT_DIR) — a picture is not an assertion.
    void lobbySnapshotForReview()
    {
        const QString dir = qEnvironmentVariable("LIGHTNING_LOBBY_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set LIGHTNING_LOBBY_SHOT_DIR to write the lobby snapshot");
        QDir().mkpath(dir);
        for (const bool manage : { true, false }) {
            Lobby h;
            QVERIFY(buildLobby(h, false, manage, 720));
            const QImage img = h.window->grabWindow();
            QVERIFY(!img.isNull());
            QVERIFY(img.save(QDir(dir).filePath(
                manage ? QStringLiteral("lobby-after-manager.png")
                       : QStringLiteral("lobby-after-member.png"))));
        }
    }
};

QTEST_MAIN(ChannelRowGeometryQmlTest)
#include "ChannelRowGeometryQmlTest.moc"
