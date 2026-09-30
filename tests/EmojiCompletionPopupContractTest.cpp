// EmojiCompletionPopup: a standard Unicode emoji and a custom pack emoticon
// share one popup and must be visually distinguishable (glyph vs image), and
// accepting either must hand the caller the whole row so it can tell which
// kind it picked. Loads the real EmojiCompletionPopup.qml offscreen, the same
// recipe as MentionPopupContractTest.

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Minimal `app` stand-in: the popup's Image resolves app.mediaBridge for its
// custom-pack rows. Signatures mirror MediaBridge; never emitted here.
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

// Defined after the class: moc loses its place inside this raw string
// and would not see the test class at all.
const char *sceneText();


} // namespace

class EmojiCompletionPopupContractTest : public QObject
{
    Q_OBJECT

private:
    QQuickItem *rowAt(QObject *root, int index) const
    {
        // ListView delegates are model-owned, not QObject-parented: resolve
        // through itemAtIndex on the (static, findable) list.
        auto *list = root->findChild<QQuickItem *>(
            QStringLiteral("emojiCompletionList"));
        if (!list)
            return nullptr;
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }

private Q_SLOTS:
    // Row 0 (custom pack) shows its remote image and pack name, never the
    // Unicode glyph; row 1 (Unicode) shows the glyph and the catalogue name,
    // never the image — proving the two kinds are visually distinguishable
    // in the one merged popup. On the pre-merge code this test cannot even
    // compile: `kind`, `emoji` and the glyph Label did not exist, and
    // completions carried only custom-pack rows.
    void customAndUnicodeRowsAreDistinguishable()
    {
        QQmlApplicationEngine engine;
        FakeAppContext appContext;
        engine.rootContext()->setContextProperty("app", &appContext);
        QQmlComponent component(&engine);
        component.setData(QByteArray(sceneText()),
                          QUrl(QStringLiteral("emojicompletionscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCoreApplication::processEvents();

        auto *popup = root->findChild<QObject *>(QStringLiteral("popup"));
        QVERIFY(popup != nullptr);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QCOMPARE(popup->property("count").toInt(), 2);

        QTRY_VERIFY(rowAt(root, 0) != nullptr);
        QTRY_VERIFY(rowAt(root, 1) != nullptr);

        auto *row0 = rowAt(root, 0);
        auto *image0 = row0->findChild<QQuickItem *>(QStringLiteral("emojiCompletionImage"));
        auto *glyph0 = row0->findChild<QQuickItem *>(QStringLiteral("emojiCompletionGlyph"));
        auto *meta0 = row0->findChild<QQuickItem *>(QStringLiteral("emojiCompletionMeta"));
        auto *code0 = row0->findChild<QQuickItem *>(QStringLiteral("emojiCompletionShortcode"));
        QVERIFY(image0 && glyph0 && meta0 && code0);
        QVERIFY2(image0->property("visible").toBool(), "custom row must show its image");
        QVERIFY2(!glyph0->property("visible").toBool(), "custom row must not show a glyph");
        QCOMPARE(code0->property("text").toString(), QStringLiteral(":blob_wave:"));
        QCOMPARE(meta0->property("text").toString(), QStringLiteral("My Pack"));

        auto *row1 = rowAt(root, 1);
        auto *image1 = row1->findChild<QQuickItem *>(QStringLiteral("emojiCompletionImage"));
        auto *glyph1 = row1->findChild<QQuickItem *>(QStringLiteral("emojiCompletionGlyph"));
        auto *meta1 = row1->findChild<QQuickItem *>(QStringLiteral("emojiCompletionMeta"));
        auto *code1 = row1->findChild<QQuickItem *>(QStringLiteral("emojiCompletionShortcode"));
        QVERIFY(image1 && glyph1 && meta1 && code1);
        QVERIFY2(!image1->property("visible").toBool(), "Unicode row must not show an image");
        QVERIFY2(glyph1->property("visible").toBool(), "Unicode row must show its glyph");
        QCOMPARE(glyph1->property("text").toString(), QStringLiteral("👍"));
        QCOMPARE(code1->property("text").toString(), QStringLiteral(":thumbsup:"));
        QCOMPARE(meta1->property("text").toString(), QStringLiteral("thumbs up"));

        // accept() hands the caller the whole row, so it can branch on `kind`
        // instead of losing that distinction to a bare shortcode string.
        QSignalSpy chosen(popup, SIGNAL(chosen(QVariant)));
        popup->setProperty("currentIndex", 1);
        QVERIFY(QMetaObject::invokeMethod(popup, "accept"));
        QCOMPARE(chosen.count(), 1);
        const QVariantMap entry = chosen.at(0).at(0).toMap();
        QCOMPARE(entry.value(QStringLiteral("kind")).toString(), QStringLiteral("unicode"));
        QCOMPARE(entry.value(QStringLiteral("emoji")).toString(), QStringLiteral("👍"));

        delete root;
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    EmojiCompletionPopupContractTest test;
    return QTest::qExec(&test, argc, argv);
}

namespace {
const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 400
    height: 320
    visible: true

    EmojiCompletionPopup {
        id: popup
        objectName: "popup"
        anchorInputTop: Qt.point(10, 300)
        anchorWidth: 320
        completions: [
            { shortcode: "blob_wave", url: "mxc://example.org/abc",
              packName: "My Pack" },
            { kind: "unicode", emoji: "👍", name: "thumbs up",
              shortcode: "thumbsup" }
        ]
    }
}
)QML";
const char *sceneText() { return kScene; }
} // namespace

#include "EmojiCompletionPopupContractTest.moc"
