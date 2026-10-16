// Room information with hostile-sized room state (break-it rerun, section 3).
// The real RoomInfoPanel against the mock backend, measured, not scanned:
//
//  * an unbroken 300-character name wrapped to ~15 lines; it is capped at
//    three, elided, with the whole name on hover;
//  * a 6.8k-character topic was shown whole and put Access settings and Leave
//    thousands of pixels down; it folds behind "Show more";
//  * the panel kept the previous room's scroll offset when opened on another
//    room (it is one instance for every room);
//  * world-readable history on an invite-only, directory-listed room raised
//    nothing; a short note explains it, never blocking;
//  * "Remove avatar" was offered for a room with no avatar (a source contract:
//    the mock grants no edit permission, so the button's own binding is what
//    can be checked).

#include <QtTest/QtTest>

#include <QFile>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtMath>
#include <QTemporaryDir>

#include <memory>

#include "app/AppController.h"
#include "auth/AuthManager.h"

namespace {

QString readQml(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                          : QString{};
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 420
    height: 800
    visible: true
    property alias panel: p
    RoomInfoPanel {
        id: p
        width: 360
        height: 800
    }
}
)QML";

// A topic of real words, ~6.8k characters, as the break-it room had.
QString longTopic()
{
    QString topic;
    while (topic.size() < 6800)
        topic += QStringLiteral("Lorem ipsum dolor sit amet, consectetur "
                                "adipiscing elit, sed do eiusmod tempor. ");
    return topic;
}

} // namespace

class RoomInfoOverflowQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;
    std::unique_ptr<AppController> m_controller;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    std::unique_ptr<QObject> m_window;
    QQuickItem *m_panel = nullptr;

    static void walk(QQuickItem *parent, QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            out.append(child);
            walk(child, out);
        }
    }
    // By displayed text, not objectName, so the case measures the same item
    // on the old panel. Only the panel's own visual tree: popups are not in it.
    QQuickItem *itemWithText(const QString &text, const char *klass) const
    {
        QList<QQuickItem *> all;
        walk(m_panel, all);
        for (QQuickItem *it : all) {
            if (QByteArray(it->metaObject()->className()).contains(klass)
                && it->property("text").toString() == text)
                return it;
        }
        return nullptr;
    }
    QQuickItem *itemNamed(const QString &name) const
    {
        QList<QQuickItem *> all;
        walk(m_panel, all);
        for (QQuickItem *it : all) {
            if (it->objectName() == name)
                return it;
        }
        return nullptr;
    }
    static QQuickItem *flickableAbove(QQuickItem *it)
    {
        for (QQuickItem *p = it ? it->parentItem() : nullptr; p;
             p = p->parentItem()) {
            if (p->inherits("QQuickFlickable"))
                return p;
        }
        return nullptr;
    }
    // y of `it` in its Flickable's content: independent of the scroll.
    static qreal contentY(QQuickItem *it)
    {
        QQuickItem *flick = flickableAbove(it);
        auto *content = flick ? flick->property("contentItem").value<QQuickItem *>()
                              : nullptr;
        return content ? it->mapToItem(content, QPointF(0, 0)).y() : -1;
    }
    void settle()
    {
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
    }
    void showRoom(const QString &name, const QString &topic)
    {
        QVariantMap data;
        data.insert(QStringLiteral("name"), name);
        data.insert(QStringLiteral("topic"), topic);
        data.insert(QStringLiteral("avatarUrl"), QString());
        // roomData is re-read from the room list on its change signals; a
        // late one from the mock's login would put the fixture back, so set
        // until it holds.
        for (int attempt = 0; attempt < 3; ++attempt) {
            m_panel->setProperty("roomData", data);
            settle();
            if (m_panel->property("roomData").toMap()
                    .value(QStringLiteral("name")).toString() == name)
                return;
        }
        QFAIL("the room list kept replacing the fixture");
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("room-info-overflow-qml-test"));

        m_controller = std::make_unique<AppController>(
            AppController::MockBackend);
        QSignalSpy spy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(spy.wait(3000));

        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->rootContext()->setContextProperty("app", m_controller.get());
        QQmlComponent component(m_engine.get());
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("roominfo-overflow.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        m_window.reset(component.create());
        QVERIFY(m_window);
        m_panel = m_window->property("panel").value<QQuickItem *>();
        QVERIFY(m_panel);
        QMetaObject::invokeMethod(m_panel, "openForRoom",
                                  Q_ARG(QVariant, QStringLiteral(
                                                      "!general:mock.local")));
        settle();
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_engine.reset();
        m_controller.reset();
    }

    // Fails on the old panel: the name wrapped to ~15 lines.
    void aLongUnbrokenNameIsCappedAtThreeLines()
    {
        const QString name(300, QLatin1Char('N'));
        showRoom(name, QString());
        QQuickItem *label = itemWithText(name, "Label");
        QVERIFY2(label, "the room name is not in the overview");
        const int lines = label->property("lineCount").toInt();
        QVERIFY2(lines >= 1 && lines <= 3,
                 qPrintable(QStringLiteral("the name takes %1 lines").arg(lines)));
        QVERIFY2(label->property("truncated").toBool(),
                 "a capped name must report itself truncated (it is what "
                 "shows the whole name on hover)");
        // A short name is untouched.
        showRoom(QStringLiteral("Lobby"), QString());
        QQuickItem *shortLabel = itemWithText(QStringLiteral("Lobby"), "Label");
        QVERIFY(shortLabel);
        QVERIFY(!shortLabel->property("truncated").toBool());
    }

    // Fails on the old panel: Leave sat ~3000 px further down with the long
    // topic than with a short one.
    void aLongTopicFoldsAndLeavesTheControlsInReach()
    {
        showRoom(QStringLiteral("Short"), QStringLiteral("A short topic."));
        QQuickItem *leave = itemWithText(QStringLiteral("Leave room"),
                                         "AppButton");
        QVERIFY2(leave, "no Leave room button in the overview");
        const qreal baseline = contentY(leave);
        QVERIFY(baseline > 0);
        auto *toggle = itemNamed(QStringLiteral("roomInfoTopicToggle"));
        QVERIFY(!toggle || !toggle->isVisible());

        const QString name(300, QLatin1Char('N'));
        showRoom(name, longTopic());
        const qreal pushed = contentY(leave) - baseline;
        // Six topic lines, the toggle and two more name lines: a couple of
        // hundred pixels, never thousands.
        QVERIFY2(pushed < 320,
                 qPrintable(QStringLiteral("Leave moved %1 px down").arg(pushed)));

        toggle = itemNamed(QStringLiteral("roomInfoTopicToggle"));
        QVERIFY2(toggle && toggle->isVisible(), "no Show more for a long topic");
        QCOMPARE(toggle->property("text").toString(), QStringLiteral("Show more"));
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        settle();
        QVERIFY2(contentY(leave) - baseline > 1000,
                 "Show more did not unfold the whole topic");
        QCOMPARE(toggle->property("text").toString(), QStringLiteral("Show less"));
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        settle();
        QVERIFY(contentY(leave) - baseline < 320);
    }

    // Fails on the old panel: openForRoom left the scroll offset (and now the
    // unfolded topic) of whichever room was shown before.
    //
    // The offset is chosen so the old panel cannot reach 0 by accident: a
    // Flickable clamps contentY when contentHeight shrinks, so an offset past
    // the NEXT room's own scroll range would be pulled back on old code too
    // (to 0, if that room fits) and the case would pass vacuously. So the
    // panel is made short, the next room's range is measured first, and the
    // offset sits inside it, where no clamp can move it (review L2).
    void anotherRoomOpensAtItsOwnTop()
    {
        const QString next = QStringLiteral("!devs:mock.local");
        const qreal fullHeight = m_panel->height();
        m_panel->setHeight(240);
        QMetaObject::invokeMethod(m_panel, "openForRoom", Q_ARG(QVariant, next));
        settle();
        QQuickItem *leave = itemWithText(QStringLiteral("Leave room"),
                                         "AppButton");
        QQuickItem *flick = flickableAbove(leave);
        QVERIFY(flick);
        QVERIFY2(flick->height() < 400,
                 qPrintable(QStringLiteral("panel not shortened: %1")
                                .arg(flick->height())));
        const qreal nextRange =
            flick->property("contentHeight").toReal() - flick->height();
        QVERIFY2(nextRange > 100,
                 qPrintable(QStringLiteral("next room scrolls only %1 px")
                                .arg(nextRange)));
        const qreal offset = qFloor(nextRange / 2);

        showRoom(QString(300, QLatin1Char('N')), longTopic());
        // Unfold where there is a fold to unfold (the old panel had none).
        if (auto *toggle = itemNamed(QStringLiteral("roomInfoTopicToggle"))) {
            QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
            settle();
        }
        QVERIFY(flick->property("contentHeight").toReal()
                > flick->height() + offset);
        flick->setProperty("contentY", offset);
        QCOMPARE(flick->property("contentY").toReal(), offset);

        // Read at once: nothing but openForRoom itself may have moved it, and
        // a clamp to the next room's range leaves `offset` where it is.
        QMetaObject::invokeMethod(m_panel, "openForRoom", Q_ARG(QVariant, next));
        const qreal after = flick->property("contentY").toReal();
        const bool folded = !m_panel->property("topicExpanded").toBool();
        m_panel->setHeight(fullHeight);
        settle();
        QCOMPARE(after, 0.0);
        QVERIFY(folded);
    }

    // Matrix allows these; a short note explains them and nothing is blocked.
    // Fails on the old panel, which had no notes (and no accessNotes()).
    void surprisingAccessCombinationsAreExplained()
    {
        auto notes = [this](const QString &rule, const QString &history,
                            int listed) {
            QVariant out;
            const bool ok = QMetaObject::invokeMethod(
                m_panel, "accessNotes", Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, rule), Q_ARG(QVariant, history),
                Q_ARG(QVariant, listed));
            if (out.metaType() == QMetaType::fromType<QJSValue>())
                out = out.value<QJSValue>().toVariant();
            return ok ? out.toStringList() : QStringList{ QStringLiteral("!") };
        };
        const QStringList all = notes(QStringLiteral("invite"),
                                      QStringLiteral("world_readable"), 1);
        QCOMPARE(all.size(), 2);
        QVERIFY(all.at(0).contains(QStringLiteral("without joining")));
        QVERIFY(all.at(1).contains(QStringLiteral("room directory")));
        // World-readable but knock: the history note only.
        QCOMPARE(notes(QStringLiteral("knock"),
                       QStringLiteral("world_readable"), 0).size(), 1);
        // A restricted room admits a Space's members, so the note must not
        // say that nobody outside can join (review L3).
        const QStringList restricted = notes(
            QStringLiteral("restricted"), QStringLiteral("world_readable"), 0);
        QCOMPARE(restricted.size(), 1);
        QVERIFY2(!restricted.at(0).contains(QStringLiteral("cannot join")),
                 qPrintable(restricted.at(0)));
        // A public room that anyone may read and is listed: nothing odd.
        QVERIFY(notes(QStringLiteral("public"),
                      QStringLiteral("world_readable"), 1).isEmpty());
        // An ordinary private room, and an unknown directory state.
        QVERIFY(notes(QStringLiteral("invite"), QStringLiteral("shared"), 0)
                    .isEmpty());
        QVERIFY(notes(QStringLiteral("invite"), QStringLiteral("shared"), -1)
                    .isEmpty());
    }

    // The mock grants no edit permission, so the group that holds the button
    // is hidden whatever its own binding says; the binding is checked at the
    // source. Fails on the old panel, where the button had no condition.
    void removeAvatarIsOfferedOnlyForARoomWithOne()
    {
        const QString qml = readQml(QStringLiteral("RoomInfoPanel.qml"));
        const qsizetype at = qml.indexOf(QStringLiteral("qsTr(\"Remove avatar\")"));
        QVERIFY(at > 0);
        const qsizetype open = qml.lastIndexOf(QStringLiteral("AppButton {"), at);
        QVERIFY(open > 0);
        const QString button = qml.mid(open, at - open);
        QVERIFY2(button.contains(QRegularExpression(
                     QStringLiteral("visible:\\s*\\(root\\.roomData\\.avatarUrl"))),
                 qPrintable(button));
    }
};

QTEST_MAIN(RoomInfoOverflowQmlTest)
#include "RoomInfoOverflowQmlTest.moc"
