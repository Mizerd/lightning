#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifResponseParser.h"
#include "gif/GifResultModel.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "gif/GifTransport.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

QVariantMap favoriteFixture(const QString &provider, const QString &id)
{
    const QString host = provider == QStringLiteral("giphy")
        ? QStringLiteral("media.giphy.com")
        : QStringLiteral("static.klipy.com");
    QVariantMap m;
    m.insert(QStringLiteral("provider"), provider);
    m.insert(QStringLiteral("gifId"), id);
    m.insert(QStringLiteral("title"), QStringLiteral("fixture %1").arg(id));
    m.insert(QStringLiteral("gifUrl"),
             QStringLiteral("https://%1/%2/original.gif").arg(host, id));
    m.insert(QStringLiteral("previewUrl"),
             QStringLiteral("https://%1/%2/preview.gif").arg(host, id));
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}

class FakeGifTransport : public GifTransport
{
    Q_OBJECT
public:
    bool up = true;
    quint64 next = 100;
    QList<QPair<quint64, QString>> issued;

    bool available() const override { return up; }
    quint64 get(const QString &url) override
    {
        if (!up)
            return 0;
        const quint64 op = next++;
        issued.append({ op, url });
        return op;
    }
    void complete(quint64 op, bool ok, int status, const QByteArray &body,
                  const QString &category)
    {
        Q_EMIT finished(op, ok, status, body, category);
    }
    quint64 lastOp() const { return issued.isEmpty() ? 0 : issued.last().first; }
};

QByteArray giphyBody(const QStringList &ids)
{
    QByteArray items;
    for (int i = 0; i < ids.size(); ++i) {
        if (i)
            items += ",";
        items += "{\"id\":\"" + ids[i].toUtf8() + "\",\"rating\":\"g\","
                 "\"title\":\"t\",\"images\":{\"original\":{"
                 "\"url\":\"https://media.giphy.com/media/" + ids[i].toUtf8()
              + "/giphy.gif\",\"width\":\"100\",\"height\":\"100\",\"size\":\"10\"}}}";
    }
    return "{\"data\":[" + items + "],\"pagination\":{\"total_count\":"
        + QByteArray::number(ids.size())
        + ",\"count\":" + QByteArray::number(ids.size()) + ",\"offset\":0}}";
}

gif::GifResult safeResult(const QString &provider, const QString &id)
{
    gif::GifResult r;
    r.provider = provider;
    r.id = id;
    r.title = QStringLiteral("fixture %1").arg(id);
    r.rating = QStringLiteral("g");
    r.gifUrl = QStringLiteral("https://media.giphy.com/media/%1/giphy.gif").arg(id);
    r.gifWidth = 100;
    r.gifHeight = 100;
    return r;
}

class FakeGifSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int gifAutoplay MEMBER gifAutoplay)
public:
    explicit FakeGifSettings(QObject *parent = nullptr) : QObject(parent) {}
    int gifAutoplay = 2;

    Q_INVOKABLE int pickerWidthShare(const QString &id) const
    { return m_sizes.value(id + QStringLiteral("/w"), 0); }
    Q_INVOKABLE int pickerHeightShare(const QString &id) const
    { return m_sizes.value(id + QStringLiteral("/h"), 0); }
    Q_INVOKABLE void setPickerShare(const QString &id, int w, int h)
    {
        m_sizes.insert(id + QStringLiteral("/w"), w);
        m_sizes.insert(id + QStringLiteral("/h"), h);
    }

private:
    QHash<QString, int> m_sizes;
};

class FakeGifApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(GifSearchController *gif READ gif CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
public:
    explicit FakeGifApp(GifSearchController *g, QObject *parent = nullptr)
        : QObject(parent), m_gif(g), m_settings(new FakeGifSettings(this))
    {
    }
    GifSearchController *gif() const { return m_gif; }
    QObject *settings() const { return m_settings; }

private:
    GifSearchController *m_gif;
    FakeGifSettings *m_settings;
};

const char *kTwoPickerScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    GifPicker {
        id: roomPicker
        objectName: "roomPicker"
        target: "room"
    }
    GifPicker {
        id: threadPicker
        objectName: "threadPicker"
        target: "thread"
    }
}
)QML";

const char *kAnchoredPickerScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    Rectangle {
        id: composerBar
        objectName: "composerBar"
        color: "#202030"
        height: 60
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
    }

    GifPicker {
        id: picker
        objectName: "picker"
        target: "room"
        anchorItem: composerBar
    }
}
)QML";

const char *kAncestorMoveScene = R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            objectName: "roomColumn"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Item {
            id: sidePanel
            objectName: "sidePanel"
            Layout.preferredWidth: 340
            Layout.fillHeight: true

            Rectangle {
                id: panelComposer
                objectName: "panelComposer"
                color: "#202030"
                height: 50
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
            }
        }
    }

    GifPicker {
        id: picker
        objectName: "picker"
        target: "room"
        anchorItem: panelComposer
    }
}
)QML";

const char *kPointAnchoredScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true

    GifPicker {
        id: picker
        objectName: "picker"
        target: "room"
        // Deliberately no anchorItem: placed once from this point, then only
        // ever clamped.
        anchorPoint: Qt.point(860, 300)
    }
}
)QML";

}

class GifPickerSelectionQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("gif-picker-selection-test"));
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void savedClickChoosesExactSavedRowNotTrending()
    {
        AppController controller(AppController::MockBackend);
        auto *gif = controller.gif();
        QVERIFY(gif != nullptr);

        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("aaa1"))));
        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("klipy"), QStringLiteral("bbb2"))));
        QCOMPARE(gif->favorites()->rowCount(), 2);
        QCOMPARE(gif->results()->rowCount(), 0);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("saved"));

        const QVariantMap expected = gif->saved()->get(1);
        QVERIFY(!expected.value(QStringLiteral("gifId")).toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(picker, "choose",
                                          Q_ARG(QVariant, 1)));

        QCOMPARE(chosen.count(), 1);
        const QVariantMap result = chosen.at(0).at(0).toMap();
        QCOMPARE(result.value(QStringLiteral("provider")).toString(),
                 expected.value(QStringLiteral("provider")).toString());
        QCOMPARE(result.value(QStringLiteral("gifId")).toString(),
                 expected.value(QStringLiteral("gifId")).toString());
        QCOMPARE(result.value(QStringLiteral("gifUrl")).toString(),
                 expected.value(QStringLiteral("gifUrl")).toString());
        QVERIFY(!result.value(QStringLiteral("provider")).toString().isEmpty());

        QVERIFY(QMetaObject::invokeMethod(picker, "choose",
                                          Q_ARG(QVariant, 99)));
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(warnings, QStringList{});
    }

    void staleCurrentIndexAfterModelResetCannotSendWrongItem()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        gif.results()->reset({ safeResult(QStringLiteral("giphy"), QStringLiteral("cat1")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("cat2")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("cat3")) });
        QCOMPARE(gif.results()->count(), 3);

        auto *gridObj =
            picker->findChild<QObject *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);

        QQmlProperty::write(gridObj, QStringLiteral("currentIndex"), 2);
        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("currentIndex"))
                     .toInt(),
                 2);

        gif.results()->reset({ safeResult(QStringLiteral("giphy"), QStringLiteral("dog1")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("dog2")),
                               safeResult(QStringLiteral("giphy"), QStringLiteral("dog3")) });
        QCOMPARE(gif.results()->count(), 3);
        QVERIFY(gif.results()->get(2).value(QStringLiteral("gifId")).toString()
                != QStringLiteral("cat3"));

        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("currentIndex"))
                     .toInt(),
                 -1);
        QCOMPARE(warnings, QStringList{});
    }

    void searchFieldEnterWithPendingDebounceSendsNothing()
    {
        FakeGifTransport transport;
        GifSearchController gif;
        gif.setDebounceMs(60'000);
        gif.setApiKey(QStringLiteral("giphy"), QStringLiteral("GKEY"));
        gif.setTransport(&transport);
        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        gif.results()->reset({ safeResult(QStringLiteral("giphy"), QStringLiteral("trend1")) });
        QCOMPARE(gif.results()->count(), 1);

        auto *searchFieldItem = picker->findChild<QQuickItem *>(
            QStringLiteral("gifSearchField"));
        QVERIFY(searchFieldItem != nullptr);
        QVERIFY(QMetaObject::invokeMethod(searchFieldItem,
                                          "forceActiveFocus"));
        searchFieldItem->setProperty("text", QStringLiteral("dogs"));

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        const int issuedCountBeforeEnter = transport.issued.size();
        QTest::keyClick(&window, Qt::Key_Return);
        QCoreApplication::processEvents();

        QCOMPARE(chosen.count(), 0);
        QVERIFY(transport.issued.size() > issuedCountBeforeEnter);
        QVERIFY(transport.issued.last().second.contains(QStringLiteral("dogs")));

        transport.complete(transport.lastOp(), true, 200,
                           giphyBody({}), QStringLiteral("ok"));
        QCOMPARE(chosen.count(), 0);
        QCOMPARE(warnings, QStringList{});
    }

    void openingOnePickerClosesTheOther()
    {
        AppController controller(AppController::MockBackend);
        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kTwoPickerScene),
                          QUrl(QStringLiteral("twopickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCoreApplication::processEvents();

        auto *roomPicker =
            root->findChild<QObject *>(QStringLiteral("roomPicker"));
        auto *threadPicker =
            root->findChild<QObject *>(QStringLiteral("threadPicker"));
        QVERIFY(roomPicker != nullptr);
        QVERIFY(threadPicker != nullptr);

        QVERIFY(QMetaObject::invokeMethod(roomPicker, "open"));
        QTRY_VERIFY(roomPicker->property("opened").toBool());
        QVERIFY(!threadPicker->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(threadPicker, "open"));
        QTRY_VERIFY(threadPicker->property("opened").toBool());
        QTRY_VERIFY(!roomPicker->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(roomPicker, "open"));
        QTRY_VERIFY(roomPicker->property("opened").toBool());
        QTRY_VERIFY(!threadPicker->property("opened").toBool());

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void savedTabBindsTheMergedModelNotResults()
    {
        QTemporaryDir starredDir;
        QVERIFY(starredDir.isValid());
        GifSearchController gif;
        gif.favorites()->clearAll();
        gif.openStarredStoreFor(starredDir.path());
        QVERIFY(gif.starredStore()->isOpen());
        QCOMPARE(gif.saved()->count(), 0);

        QByteArray starredBytes = QByteArrayLiteral("GIF89a");
        starredBytes.append(char(10)); starredBytes.append(char(0));
        starredBytes.append(char(10)); starredBytes.append(char(0));
        QSignalSpy starFinished(gif.starredStore(),
                                &GifStarredStore::starFinished);
        gif.starredStore()->starBytes(QStringLiteral("mk1"), starredBytes);
        QCOMPARE(starFinished.count(), 1);
        QVERIFY(starFinished.at(0).at(1).toBool());
        QCOMPARE(gif.starredStore()->count(), 1);

        gif.results()->reset(
            { safeResult(QStringLiteral("giphy"), QStringLiteral("trend1")),
              safeResult(QStringLiteral("giphy"), QStringLiteral("trend2")) });
        QCOMPARE(gif.results()->count(), 2);

        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("saved"));
        QCoreApplication::processEvents();
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("tab")).toString(),
                 QStringLiteral("saved"));
        QVERIFY(!QQmlProperty::read(picker, QStringLiteral("providerTab"))
                     .toBool());

        auto *gridObj =
            picker->findChild<QObject *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);

        QObject *activeModel =
            QQmlProperty::read(picker, QStringLiteral("activeModel"))
                .value<QObject *>();
        QCOMPARE(activeModel, static_cast<QObject *>(gif.saved()));
        QVERIFY(activeModel != static_cast<QObject *>(gif.results()));

        QObject *gridModel =
            QQmlProperty::read(gridObj, QStringLiteral("model")).value<QObject *>();
        QCOMPARE(gridModel, static_cast<QObject *>(gif.saved()));

        QCOMPARE(QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        QCOMPARE(warnings, QStringList{});
    }

    void pickerIsPinnedToItsAnchorAtEveryWindowSize()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAnchoredPickerScene),
                          QUrl(QStringLiteral("anchoredpickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        auto *bar = root->findChild<QQuickItem *>(QStringLiteral("composerBar"));
        QVERIFY(picker != nullptr && bar != nullptr);

        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        QCOMPARE(picker->property("parent").value<QQuickItem *>(), bar);

        const qreal gap = picker->property("anchorGap").toReal();
        const auto pinned = [&](const char *whenLabel) {
            const qreal x = picker->property("x").toReal();
            const qreal y = picker->property("y").toReal();
            const qreal w = picker->property("width").toReal();
            const qreal h = picker->property("height").toReal();
            QVERIFY2(qAbs((x + w) - bar->width()) < 1.5,
                     qPrintable(QStringLiteral("%1: right edge %2 vs anchor %3")
                                    .arg(QString::fromUtf8(whenLabel))
                                    .arg(x + w).arg(bar->width())));
            QVERIFY2(x + w <= bar->width() + 0.5,
                     qPrintable(QStringLiteral("%1: overhangs the anchor")
                                    .arg(QString::fromUtf8(whenLabel))));
            QVERIFY2(qAbs((y + h) + gap) < 1.5,
                     qPrintable(QStringLiteral("%1: bottom %2, expected %3")
                                    .arg(QString::fromUtf8(whenLabel))
                                    .arg(y + h).arg(-gap)));
        };

        pinned("initial");
        const qreal barWidthBefore = bar->width();

        window->resize(1300, 900);
        QTRY_COMPARE(int(window->width()), 1300);
        QTRY_VERIFY2(bar->width() > barWidthBefore + 300,
                     "anchor did not grow: scene no longer exercises a resize");
        pinned("after grow");

        window->resize(620, 520);
        QTRY_COMPARE(int(window->width()), 620);
        QTRY_VERIFY(bar->width() < barWidthBefore);
        pinned("after shrink");
        QVERIFY(picker->property("width").toReal() <= bar->width() + 0.5);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void pickerFollowsAnAnchorMovedByItsAncestor()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAncestorMoveScene),
                          QUrl(QStringLiteral("ancestormovescene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        auto *bar = root->findChild<QQuickItem *>(QStringLiteral("panelComposer"));
        auto *sidePanel = root->findChild<QQuickItem *>(QStringLiteral("sidePanel"));
        QVERIFY(picker != nullptr && bar != nullptr && sidePanel != nullptr);
        auto *overlayForScene = window->contentItem();
        QVERIFY(overlayForScene != nullptr);

        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        const qreal panelXBefore = sidePanel->x();
        const qreal barLocalXBefore = bar->x();
        const qreal barSceneXBefore =
            bar->mapToItem(overlayForScene, QPointF(0, 0)).x();
        const qreal pickerXBefore = picker->property("x").toReal();

        window->resize(1300, 900);
        QTRY_COMPARE(int(window->width()), 1300);
        QTRY_VERIFY2(sidePanel->x() > panelXBefore + 300,
                     qPrintable(QStringLiteral("panel did not slide: %1 -> %2")
                                    .arg(panelXBefore).arg(sidePanel->x())));
        QCOMPARE(bar->x(), barLocalXBefore);
        QVERIFY(bar->mapToItem(overlayForScene, QPointF(0, 0)).x()
                > barSceneXBefore + 300);

        QCOMPARE(picker->property("x").toReal(), pickerXBefore);
        const qreal x = picker->property("x").toReal();
        const qreal w = picker->property("width").toReal();
        QVERIFY(qAbs((x + w) - bar->width()) < 1.5);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void pointAnchoredPopupIsPlacedOnceThenOnlyClamped()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kPointAnchoredScene),
                          QUrl(QStringLiteral("pointanchoredscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        QVERIFY(picker != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());
        auto *overlay = picker->property("parent").value<QQuickItem *>();
        QVERIFY(overlay != nullptr);

        const qreal w = picker->property("width").toReal();
        const qreal openedX = picker->property("x").toReal();
        const qreal rightLimit = overlay->width() - w - 8;
        QTRY_COMPARE(picker->property("x").toReal(), rightLimit);

        window->resize(1400, 900);
        QTRY_COMPARE(int(window->width()), 1400);
        QTRY_COMPARE(int(overlay->width()), 1400);
        QCoreApplication::processEvents();
        const qreal centredIfReplaced = 860 - w / 2;
        QVERIFY2(qAbs(centredIfReplaced - openedX) > 50,
                 "scene no longer distinguishes clamp from re-place");
        QCOMPARE(picker->property("x").toReal(), openedX);

        window->resize(500, 700);
        QTRY_COMPARE(int(overlay->width()), 500);
        QTRY_VERIFY(picker->property("x").toReal()
                    + picker->property("width").toReal()
                    <= overlay->width());
        QVERIFY(picker->property("x").toReal() >= 8);

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void resizeGrowsFromThePinnedCornerAndPersists()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAnchoredPickerScene),
                          QUrl(QStringLiteral("anchoredpickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        auto *bar = root->findChild<QQuickItem *>(QStringLiteral("composerBar"));
        QVERIFY(picker != nullptr && bar != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        const qreal fraction = picker->property("widthFraction").toReal();
        QVERIFY2(qAbs(picker->property("width").toReal()
                      - bar->width() * fraction) < 1.5,
                 qPrintable(QStringLiteral("auto width %1 vs share %2")
                                .arg(picker->property("width").toReal())
                                .arg(bar->width() * fraction)));
        const qreal gap = picker->property("anchorGap").toReal();
        const qreal rightEdgeBefore = picker->property("x").toReal()
                                      + picker->property("width").toReal();

        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 460),
                                          Q_ARG(QVariant, 600)));
        QTRY_COMPARE(picker->property("width").toReal(), 460.0);
        QCOMPARE(picker->property("height").toReal(), 600.0);
        QCOMPARE(picker->property("x").toReal()
                     + picker->property("width").toReal(), rightEdgeBefore);
        QVERIFY(qAbs((picker->property("y").toReal()
                      + picker->property("height").toReal()) + gap) < 1.5);

        const qreal floorW = picker->property("minWidth").toReal();
        const qreal floorH = picker->property("minHeight").toReal();
        QVERIFY(floorW > 0 && floorH > 0);
        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 50),
                                          Q_ARG(QVariant, 50)));
        QCOMPARE(picker->property("width").toReal(), floorW);
        QCOMPARE(picker->property("height").toReal(), floorH);

        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 99999),
                                          Q_ARG(QVariant, 99999)));
        QCOMPARE(picker->property("width").toReal(), bar->width());
        QCOMPARE(picker->property("x").toReal(), 0.0);
        QVERIFY(picker->property("height").toReal()
                <= window->height() - bar->height());

        QVERIFY(QMetaObject::invokeMethod(picker, "resizeTo",
                                          Q_ARG(QVariant, 420),
                                          Q_ARG(QVariant, 560)));
        QVERIFY(QMetaObject::invokeMethod(picker, "endResize"));

        auto *settings = fakeApp.property("settings").value<QObject *>();
        QVERIFY(settings != nullptr);
        int storedShare = 0;
        QVERIFY(QMetaObject::invokeMethod(settings, "pickerWidthShare",
                                          Q_RETURN_ARG(int, storedShare),
                                          Q_ARG(QString, QStringLiteral("picker"))));
        const int expectedShare = qRound(420.0 / bar->width() * 1000);
        QCOMPARE(storedShare, expectedShare);

        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QTRY_VERIFY(!picker->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());
        const qreal restored = picker->property("width").toReal();
        QVERIFY2(qAbs(restored - 420.0) < 2.0,
                 qPrintable(QStringLiteral("restored width %1").arg(restored)));
        QVERIFY(qAbs((picker->property("x").toReal() + restored) - bar->width()) < 1.5);

        window->resize(1500, 900);
        QTRY_VERIFY(bar->width() > 1200);
        QTRY_VERIFY2(picker->property("width").toReal() > restored + 100,
                     "picker did not scale up with the window");

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void recentTileIsNotSavedMerelyBecauseItWasSent()
    {
        GifSearchController gif;
        gif.favorites()->clearAll();
        gif.recent()->clearAll();

        const QVariantMap sent =
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("sent1"));
        gif.recordSent(sent);
        QCOMPARE(gif.recent()->rowCount(), 1);
        QCOMPARE(gif.favorites()->rowCount(), 0);

        FakeGifApp fakeApp(&gif);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);

        QQmlComponent component(&engine);
        component.setData(QByteArray(kAnchoredPickerScene),
                          QUrl(QStringLiteral("anchoredpickerscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QObject *>(QStringLiteral("picker"));
        QVERIFY(picker != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTRY_VERIFY(picker->property("opened").toBool());

        QQmlProperty::write(picker, QStringLiteral("tab"),
                            QStringLiteral("recent"));
        auto *gridObj =
            picker->findChild<QQuickItem *>(QStringLiteral("gifResultGrid"));
        QVERIFY(gridObj != nullptr);
        QTRY_COMPARE(QQmlProperty::read(gridObj, QStringLiteral("count")).toInt(), 1);

        QQuickItem *tile = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(
                        gridObj, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tile),
                        Q_ARG(int, 0))
                    && tile != nullptr);

        QVERIFY2(!QQmlProperty::read(tile, QStringLiteral("saved")).toBool(),
                 "a recents row reported itself as saved");

        QVERIFY(gif.toggleFavorite(sent));
        QTRY_VERIFY2(QQmlProperty::read(tile, QStringLiteral("saved")).toBool(),
                     "tile did not pick up the new saved state");

        QVERIFY(!gif.toggleFavorite(sent));
        QTRY_VERIFY(!QQmlProperty::read(tile, QStringLiteral("saved")).toBool());

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void secondActivationBeforeCloseCompletesSendsExactlyOne()
    {
        GifSearchController gif;
        FakeGifApp fakeApp(&gif);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &fakeApp);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        gif.results()->reset(
            { safeResult(QStringLiteral("giphy"),
                        QStringLiteral("dupactivation1")) });
        QCOMPARE(gif.results()->count(), 1);

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        QVERIFY(QMetaObject::invokeMethod(picker, "choose", Q_ARG(QVariant, 0)));
        QVERIFY(QMetaObject::invokeMethod(picker, "choose", Q_ARG(QVariant, 0)));
        QCOMPARE(chosen.count(), 1);
        const QVariantMap result = chosen.at(0).at(0).toMap();
        QCOMPARE(result.value(QStringLiteral("provider")).toString(),
                 QStringLiteral("giphy"));
        QCOMPARE(result.value(QStringLiteral("gifId")).toString(),
                 QStringLiteral("dupactivation1"));
        QCOMPARE(warnings, QStringList{});
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(GifPickerSelectionQmlTest)
#include "GifPickerSelectionQmlTest.moc"
