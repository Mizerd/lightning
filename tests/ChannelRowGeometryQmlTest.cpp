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
};

QTEST_MAIN(ChannelRowGeometryQmlTest)
#include "ChannelRowGeometryQmlTest.moc"
