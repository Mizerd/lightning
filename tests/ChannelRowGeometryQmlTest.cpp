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
// The presenter's own three-way chooser is pinned in
// NavigationLayoutContractTest, which can read it directly.

#include <QtTest/QtTest>

#include <QQmlComponent>
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
    bool build(Harness &h, const QString &body)
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

    void categoryHeaderHasRealHeightInsideAWidthAssignedLoader()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelCategoryHeader {
            width: 300
            categoryId: "!space:example.org"
            categoryName: "Engineering"
        })"),
                            "category header");
    }

    void sectionHeaderHasRealHeightInsideAWidthAssignedLoader()
    {
        checkRowHasGeometry(QStringLiteral(R"(
        ChannelSectionHeader {
            width: 300
            label: "Direct messages"
        })"),
                            "section header");
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
};

QTEST_MAIN(ChannelRowGeometryQmlTest)
#include "ChannelRowGeometryQmlTest.moc"
