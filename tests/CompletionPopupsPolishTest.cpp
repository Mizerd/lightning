// C38 round 1: the three composer completion popups (slash, mention, emoji).
// Behavior, on the real QML loaded offscreen:
//  - hovering a row highlights it, so the row a click accepts is the row that
//    is drawn selected (Slash and Emoji had no hover at all);
//  - the wheel handler is the timeline's SmoothWheelArea on all three lists;
//  - a popup whose anchor is below a shrunken window stays inside it;
//  - a long mention display name elides (RichText ignores elide);
//  - the emoji popup is named for assistive technology and carries the shared
//    header and Return keycap.

#include "models/MentionSuggestionModel.h"
#include "matrix/MockMatrixClient.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest/QtTest>

namespace {

class FakeMediaBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
public:
    explicit FakeMediaBridge(QObject *parent = nullptr) : QObject(parent) {}
    bool supported() const { return false; }
    Q_INVOKABLE QString mxcImageSource(const QString &, int) const { return {}; }
Q_SIGNALS:
    void mediaCached(const QString &cacheKey);
    void mediaFetchFailed(const QString &cacheKey, const QString &category);
    void mediaRetryable(const QString &cacheKey);
    void animatedMediaReady(const QString &cacheKey);
    void motionSlotFreed();
};

class FakeAppContext : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *mediaBridge READ mediaBridge CONSTANT)
public:
    explicit FakeAppContext(QObject *parent = nullptr)
        : QObject(parent), m_bridge(new FakeMediaBridge(this)) {}
    QObject *mediaBridge() const { return m_bridge; }
private:
    FakeMediaBridge *m_bridge;
};

class MemberMock : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 requestRoomMembers(const QString &) override { return ++m_op; }
    void deliver(const QString &roomId, const QVariantList &members)
    {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"), true);
        snapshot.insert(QStringLiteral("members"), members);
        Q_EMIT roomMembersReceived(m_op, roomId, snapshot);
    }
    quint64 m_op = 0;
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 500
    height: 420
    visible: true

    SlashCommandPopup {
        objectName: "slash"
        anchorInputTop: Qt.point(20, 400)
        anchorWidth: 320
        completions: [
            { name: "a", description: "one", enabled: true },
            { name: "b", description: "two", enabled: true },
            { name: "c", description: "three", enabled: true },
            { name: "d", description: "four", enabled: true },
            { name: "e", description: "five", enabled: true },
            { name: "f", description: "six", enabled: true },
            { name: "g", description: "seven", enabled: true },
            { name: "h", description: "eight", enabled: true }
        ]
    }
    EmojiCompletionPopup {
        objectName: "emoji"
        anchorInputTop: Qt.point(20, 400)
        anchorWidth: 320
        completions: [
            { kind: "unicode", emoji: "A", name: "first", shortcode: "one" },
            { kind: "unicode", emoji: "B", name: "second", shortcode: "two" },
            { kind: "unicode", emoji: "C", name: "third", shortcode: "three" }
        ]
    }
    MentionPopup {
        objectName: "mention"
        suggestions: mentionModel
        anchorInputTop: Qt.point(20, 400)
        anchorWidth: 320
    }
}
)QML";

} // namespace

class CompletionPopupsPolishTest : public QObject
{
    Q_OBJECT

    struct Scene {
        QQmlApplicationEngine engine;
        QObject *root = nullptr;
        QQuickWindow *window = nullptr;
    };

    static QQuickItem *rowAt(QObject *root, const char *list, int index)
    {
        auto *l = root->findChild<QQuickItem *>(QString::fromLatin1(list));
        if (!l)
            return nullptr;
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(l, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }
    static void hoverCentre(QQuickWindow *w, QQuickItem *item)
    {
        const QPointF p = item->mapToScene(
            QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseMove(w, p.toPoint());
        QTest::mouseMove(w, p.toPoint() + QPoint(1, 0));
    }

    MemberMock m_mock;
    MentionSuggestionModel m_model;

    bool load(Scene &s, FakeAppContext &app)
    {
        s.engine.rootContext()->setContextProperty("app", &app);
        s.engine.rootContext()->setContextProperty("mentionModel", &m_model);
        QQmlComponent c(&s.engine);
        c.setData(QByteArray(kScene), QUrl(QStringLiteral("scene.qml")));
        s.root = c.create();
        if (!s.root) {
            qWarning() << c.errorString();
            return false;
        }
        s.window = qobject_cast<QQuickWindow *>(s.root);
        return s.window && QTest::qWaitForWindowExposed(s.window);
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_model.setClient(&m_mock);
        m_model.setRoomId(QStringLiteral("!r:hs"));
        QVariantMap m;
        m.insert(QStringLiteral("userId"), QStringLiteral("@long:hs"));
        m.insert(QStringLiteral("displayName"), QStringLiteral(
            "Dimitrios Very-Long-Display-Name Papadopoulos-Smith the Third "
            "of the Many Names and Even More Names Besides"));
        m.insert(QStringLiteral("avatarUrl"), QString());
        m.insert(QStringLiteral("membership"), QStringLiteral("join"));
        m.insert(QStringLiteral("role"), QStringLiteral("user"));
        m.insert(QStringLiteral("ambiguous"), false);
        m.insert(QStringLiteral("isOwn"), false);
        m_mock.deliver(QStringLiteral("!r:hs"), {m});
        m_model.setRoomMentionAllowed(false);
    }

    void hoverHighlightsTheRowAClickWouldAccept()
    {
        FakeAppContext app;
        Scene s;
        QVERIFY(load(s, app));
        for (const char *name : {"slash", "emoji"}) {
            auto *popup = s.root->findChild<QObject *>(QString::fromLatin1(name));
            QVERIFY(popup);
            QVERIFY(QMetaObject::invokeMethod(popup, "open"));
            QTRY_VERIFY(popup->property("opened").toBool());
            const char *list = name[0] == 's' ? "slashCommandPopupList"
                                              : "emojiCompletionList";
            QTRY_VERIFY(rowAt(s.root, list, 2) != nullptr);
            QCOMPARE(popup->property("currentIndex").toInt(), 0);
            hoverCentre(s.window, rowAt(s.root, list, 2));
            QTRY_COMPARE_WITH_TIMEOUT(popup->property("currentIndex").toInt(),
                                      2, 3000);
            QMetaObject::invokeMethod(popup, "close");
        }
        delete s.root;
    }

    // Review r1 item 2: a pointer parked on the popup must not steal the
    // keyboard's selection when the list scrolls under it.
    void keyboardAdvancesMonotonicallyUnderAParkedPointer()
    {
        FakeAppContext app;
        Scene s;
        QVERIFY(load(s, app));
        for (const char *name : {"slash", "emoji"}) {
            auto *popup = s.root->findChild<QObject *>(QString::fromLatin1(name));
            QVERIFY(popup);
            QVERIFY(QMetaObject::invokeMethod(popup, "open"));
            QTRY_VERIFY(popup->property("opened").toBool());
            const bool slash = name[0] == 's';
            const char *list = slash ? "slashCommandPopupList" : "emojiCompletionList";
            QTRY_VERIFY(rowAt(s.root, list, 1) != nullptr);
            hoverCentre(s.window, rowAt(s.root, list, 1));
            QTRY_COMPARE_WITH_TIMEOUT(popup->property("currentIndex").toInt(), 1, 3000);
            const int rows = slash ? 8 : 3;
            int last = 1;
            // Exactly one row per key press down to the last row (no wrap).
            for (int i = 1; i < rows - 1; ++i) {
                QVERIFY(QMetaObject::invokeMethod(popup, "moveDown"));
                QTest::qWait(60); // let a re-issued hover land
                QCOMPARE(popup->property("currentIndex").toInt(), last + 1);
                ++last;
            }
            QCOMPARE(last, rows - 1);
            for (int i = rows - 1; i > 0; --i) {
                QVERIFY(QMetaObject::invokeMethod(popup, "moveUp"));
                QTest::qWait(60);
                QCOMPARE(popup->property("currentIndex").toInt(), last - 1);
                --last;
            }
            QCOMPARE(last, 0);
            QMetaObject::invokeMethod(popup, "close");
        }
        delete s.root;
    }

    void everyListUsesTheSharedWheelHandler()
    {
        FakeAppContext app;
        Scene s;
        QVERIFY(load(s, app));
        for (const char *name : {"slash", "emoji", "mention"}) {
            auto *popup = s.root->findChild<QObject *>(QString::fromLatin1(name));
            QVERIFY(popup);
            QVERIFY(QMetaObject::invokeMethod(popup, "open"));
            QTRY_VERIFY(popup->property("opened").toBool());
            const char *list = name[0] == 's' ? "slashCommandPopupList"
                              : name[0] == 'e' ? "emojiCompletionList"
                                               : "mentionListView";
            auto *l = s.root->findChild<QQuickItem *>(QString::fromLatin1(list));
            QVERIFY2(l, list);
            bool found = false;
            for (QObject *o : l->findChildren<QObject *>()) {
                if (QByteArray(o->metaObject()->className())
                        .startsWith("SmoothWheelArea"))
                    found = true;
            }
            QVERIFY2(found, list);
            QMetaObject::invokeMethod(popup, "close");
        }
        delete s.root;
    }

    void aPopupStaysInsideAShrunkWindow()
    {
        FakeAppContext app;
        Scene s;
        QVERIFY(load(s, app));
        for (const char *name : {"slash", "emoji", "mention"}) {
            auto *popup = s.root->findChild<QObject *>(QString::fromLatin1(name));
            QVERIFY(popup);
            // The composer's anchor was recorded at a taller window.
            popup->setProperty("anchorInputTop", QPointF(20, 900));
            QVERIFY(QMetaObject::invokeMethod(popup, "open"));
            QTRY_VERIFY(popup->property("opened").toBool());
            const qreal y = popup->property("y").toReal();
            const qreal h = popup->property("height").toReal();
            QVERIFY2(y >= 0 && y + h <= s.window->height(), name);
            QMetaObject::invokeMethod(popup, "close");
        }
        delete s.root;
    }

    void aLongMentionNameElides()
    {
        FakeAppContext app;
        Scene s;
        QVERIFY(load(s, app));
        auto *popup = s.root->findChild<QObject *>(QStringLiteral("mention"));
        QVERIFY(popup);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QTRY_VERIFY(rowAt(s.root, "mentionListView", 0) != nullptr);
        auto *name = rowAt(s.root, "mentionListView", 0)
                         ->findChild<QQuickItem *>(QStringLiteral("mentionName"));
        QVERIFY(name);
        QTRY_VERIFY2(name->property("truncated").toBool(),
                     "a name wider than the row must end in an ellipsis");
        delete s.root;
    }

    void emojiPopupIsNamedAndSharesTheFamilyChrome()
    {
        FakeAppContext app;
        Scene s;
        QVERIFY(load(s, app));
        auto *popup = s.root->findChild<QObject *>(QStringLiteral("emoji"));
        QVERIFY(popup);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QTRY_VERIFY(rowAt(s.root, "emojiCompletionList", 1) != nullptr);
        QVERIFY(s.root->findChild<QQuickItem *>(
            QStringLiteral("emojiCompletionPopupHeader")));
        auto *row1 = rowAt(s.root, "emojiCompletionList", 1);
        // Accessible.* attached properties are not readable as properties;
        // the row is named through QAccessible instead.
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(row1);
        QVERIFY(iface);
        QVERIFY2(iface->text(QAccessible::Name).contains(QStringLiteral(":two:")),
                 qPrintable(iface->text(QAccessible::Name)));
        auto *key0 = rowAt(s.root, "emojiCompletionList", 0)
                         ->findChild<QQuickItem *>(QStringLiteral("emojiCompletionKeycap"));
        auto *key1 = row1->findChild<QQuickItem *>(QStringLiteral("emojiCompletionKeycap"));
        QVERIFY(key0 && key1);
        QVERIFY(key0->isVisible());
        QVERIFY(!key1->isVisible());
        delete s.root;
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QAccessible::setActive(true);
    CompletionPopupsPolishTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "CompletionPopupsPolishTest.moc"
